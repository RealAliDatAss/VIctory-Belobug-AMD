#include "hook_x64.hpp"
#include "mem_guard.hpp"

#include <windows.h>

#include <cstring>
#include <limits>
#include <sstream>

namespace victory::hook {

namespace {

constexpr std::size_t kStubArenaBytes = 64u << 10;

bool emit_rel32_jmp(std::uintptr_t from, std::uintptr_t to, std::uint8_t (&out)[5]) noexcept {
    const std::intptr_t disp =
        static_cast<std::intptr_t>(to) - static_cast<std::intptr_t>(from + 5);
    if (disp < std::numeric_limits<std::int32_t>::min() ||
        disp > std::numeric_limits<std::int32_t>::max()) {
        return false;
    }
    out[0] = 0xE9;
    const auto rel32 = static_cast<std::int32_t>(disp);
    std::memcpy(&out[1], &rel32, sizeof(rel32));
    return true;
}

std::size_t align_up(std::size_t v, std::size_t a) noexcept {
    return (v + a - 1) & ~(a - 1);
}

struct OrByteDecoded {
    std::size_t len = 0;
    std::uint8_t rex = 0;
    std::uint8_t modrm = 0;
    std::size_t addr_start = 0;
    std::size_t addr_end = 0;
    std::uint8_t imm8 = 0;
};

bool decode_or_byte(const std::uint8_t* code, std::size_t avail, OrByteDecoded& out) noexcept {
    if (!code || avail < 5) {
        return false;
    }
    std::size_t at = 0;
    std::uint8_t rex = 0;
    if ((code[0] & 0xF0) == 0x40) {
        rex = code[0];
        at = 1;
    }
    if (at + 2 >= avail) {
        return false;
    }
    const std::uint8_t op = code[at];
    const std::uint8_t modrm = code[at + 1];
    if (op != 0x80 || ((modrm >> 3) & 7) != 1) {
        return false;
    }
    const std::uint8_t mode = modrm >> 6;
    const std::uint8_t rm = modrm & 7;
    if (mode == 3 || (mode == 0 && rm == 5)) {
        return false;
    }

    const std::size_t rest = at + 2;
    std::size_t end = rest;
    if (rm == 4) {
        if (end >= avail) {
            return false;
        }
        const std::uint8_t sib = code[end];
        if (((sib & 7) == 4 && (rex & 1) == 0) || ((sib & 7) == 5 && mode == 0)) {
            return false;
        }
        end += 1;
    }
    if (mode == 1) {
        end += 1;
    } else if (mode == 2) {
        end += 4;
    }
    if (end >= avail) {
        return false;
    }
    out.len = end + 1;
    out.rex = rex;
    out.modrm = modrm;
    out.addr_start = rest;
    out.addr_end = end;
    out.imm8 = code[end];
    return true;
}

} // namespace

void* alloc_near_executable(std::uintptr_t near_addr, std::size_t bytes) {
    constexpr std::intptr_t kMaxSpan = 0x7000'0000LL;
    constexpr std::size_t kStep = 64u << 10;

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const auto min_addr = reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress);
    const auto max_addr = reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress);

    const std::uintptr_t lo = (near_addr > static_cast<std::uintptr_t>(kMaxSpan) + min_addr)
                                  ? (near_addr - static_cast<std::uintptr_t>(kMaxSpan))
                                  : min_addr;
    const std::uintptr_t hi = (near_addr + static_cast<std::uintptr_t>(kMaxSpan) < max_addr)
                                  ? (near_addr + static_cast<std::uintptr_t>(kMaxSpan))
                                  : max_addr;

    for (std::uintptr_t probe = (near_addr & ~(kStep - 1)); probe >= lo; probe -= kStep) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(probe), &mbi, sizeof(mbi))) {
            break;
        }
        if (mbi.State == MEM_FREE && mbi.RegionSize >= bytes) {
            if (void* p = VirtualAlloc(reinterpret_cast<LPVOID>(probe), bytes,
                                       MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) {
                return p;
            }
        }
        if (probe < kStep) {
            break;
        }
    }

    for (std::uintptr_t probe = ((near_addr + kStep - 1) & ~(kStep - 1)); probe <= hi; probe += kStep) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(probe), &mbi, sizeof(mbi))) {
            break;
        }
        if (mbi.State == MEM_FREE && mbi.RegionSize >= bytes) {
            if (void* p = VirtualAlloc(reinterpret_cast<LPVOID>(probe), bytes,
                                       MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) {
                return p;
            }
        }
        probe = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (probe > hi) {
            break;
        }
    }
    return nullptr;
}

bool patch_rel32_jump(
    std::uintptr_t site,
    const std::vector<std::uint8_t>& expected,
    std::uintptr_t target,
    PatchRecord& out_record,
    std::string& err) {
    if (site == 0 || target == 0 || expected.size() < 5) {
        err = "invalid patch site or expected length < 5";
        return false;
    }

    std::uint8_t jmp[5]{};
    if (!emit_rel32_jmp(site, target, jmp)) {
        err = "jump target outside +/-2 GiB rel32 range";
        return false;
    }

    auto* code = reinterpret_cast<std::uint8_t*>(site);
    if (std::memcmp(code, expected.data(), expected.size()) != 0) {
        err = "original bytes mismatch (different title build)";
        return false;
    }

    DWORD old_prot = 0;
    if (!VirtualProtect(code, expected.size(), PAGE_EXECUTE_READWRITE, &old_prot)) {
        err = "VirtualProtect failed before patch";
        return false;
    }

    out_record.site = site;
    out_record.original_bytes = expected;
    out_record.applied = true;

    std::vector<std::uint8_t> patch(expected.size(), 0x90);
    std::memcpy(patch.data(), jmp, sizeof(jmp));
    std::memcpy(code, patch.data(), patch.size());

    DWORD unused = 0;
    VirtualProtect(code, expected.size(), old_prot, &unused);
    FlushInstructionCache(GetCurrentProcess(), code, expected.size());
    return true;
}

bool restore_patch(PatchRecord& record, std::string& err) {
    if (!record.applied || record.site == 0 || record.original_bytes.empty()) {
        return true;
    }
    auto* code = reinterpret_cast<std::uint8_t*>(record.site);
    DWORD old_prot = 0;
    if (!VirtualProtect(code, record.original_bytes.size(), PAGE_EXECUTE_READWRITE, &old_prot)) {
        err = "VirtualProtect failed during restore";
        return false;
    }
    std::memcpy(code, record.original_bytes.data(), record.original_bytes.size());
    DWORD unused = 0;
    VirtualProtect(code, record.original_bytes.size(), old_prot, &unused);
    FlushInstructionCache(GetCurrentProcess(), code, record.original_bytes.size());
    record.applied = false;
    return true;
}

bool parse_vmovups_lea(const std::uint8_t* store, std::size_t len, std::uint8_t (&out_lea)[4]) noexcept {
    if (!store || len < 5) {
        return false;
    }
    bool x = false;
    bool b = false;
    std::size_t op_at = 0;
    if (store[0] == 0xC5) {
        op_at = 2;
    } else if (store[0] == 0xC4) {
        const std::uint8_t p = store[1];
        if ((p & 0x1F) != 1) {
            return false;
        }
        x = (p & 0x40) == 0;
        b = (p & 0x20) == 0;
        op_at = 3;
    } else {
        return false;
    }
    if (len != op_at + 3) {
        return false;
    }
    const std::uint8_t op = store[op_at];
    const std::uint8_t modrm = store[op_at + 1];
    const std::uint8_t sib = store[op_at + 2];
    if (op != 0x11 || modrm != 0x04) {
        return false;
    }
    if ((sib & 7) == 5 || ((sib & 7) == 4 && !b) || (((sib >> 3) & 7) == 4 && !x)) {
        return false;
    }
    out_lea[0] = static_cast<std::uint8_t>(0x48 | (x ? 2 : 0) | (b ? 1 : 0));
    out_lea[1] = 0x8D;
    out_lea[2] = 0x04;
    out_lea[3] = sib;
    return true;
}

bool build_descriptor_mark_stub(
    const std::uint8_t* store,
    std::size_t store_len,
    std::uintptr_t site_addr,
    std::uintptr_t stub_addr,
    const VictoryDescriptorMarks* marks,
    std::vector<std::uint8_t>& out_stub) {
    static_assert(sizeof(VictoryDescriptorMarks) == 32);
    static_assert(offsetof(VictoryDescriptorMarks, start) == 0);
    static_assert(offsetof(VictoryDescriptorMarks, len) == 8);
    static_assert(offsetof(VictoryDescriptorMarks, slots) == 16);
    static_assert(offsetof(VictoryDescriptorMarks, chunks) == 24);

    if (!marks) {
        return false;
    }
    std::uint8_t lea[4]{};
    if (!parse_vmovups_lea(store, store_len, lea)) {
        return false;
    }

    out_stub.clear();
    out_stub.insert(out_stub.end(), store, store + store_len);
    out_stub.push_back(0x50); // push rax
    out_stub.insert(out_stub.end(), std::begin(lea), std::end(lea)); // lea rax, [base+index]
    out_stub.push_back(0x52); // push rdx
    out_stub.push_back(0x51); // push rcx

    const auto marks_val = reinterpret_cast<std::uint64_t>(marks);
    out_stub.push_back(0x48);
    out_stub.push_back(0xBA); // mov rdx, marks
    const std::size_t imm_at = out_stub.size();
    out_stub.resize(imm_at + 8);
    std::memcpy(&out_stub[imm_at], &marks_val, 8);

    const std::uint8_t body[] = {
        0x48, 0x2B, 0x02,             // sub rax, [rdx]
        0x48, 0x3B, 0x42, 0x08,       // cmp rax, [rdx+8]
        0x73, 0x18,                   // jae +24 (past marks)
        0x48, 0xC1, 0xE8, 0x05,       // shr rax, 5 (slot index)
        0x48, 0x8B, 0x4A, 0x10,       // mov rcx, [rdx+16] (slots ptr)
        0xC6, 0x04, 0x01, 0x01,       // mov byte ptr [rcx+rax], 1
        0x48, 0xC1, 0xE8, 0x07,       // shr rax, 7 (chunk index)
        0x48, 0x8B, 0x4A, 0x18,       // mov rcx, [rdx+24] (chunks ptr)
        0xC6, 0x04, 0x01, 0x01,       // mov byte ptr [rcx+rax], 1
        0x59, 0x5A, 0x58,             // pop rcx; pop rdx; pop rax
    };
    out_stub.insert(out_stub.end(), std::begin(body), std::end(body));

    std::uint8_t back[5]{};
    if (!emit_rel32_jmp(stub_addr + out_stub.size(), site_addr + store_len, back)) {
        return false;
    }
    out_stub.insert(out_stub.end(), std::begin(back), std::end(back));
    return true;
}

const std::vector<DescriptorSiteDef>& known_descriptor_sites() {
    static const std::vector<DescriptorSiteDef> kSites = {
        {0x4d9f04f, 10, 5, {0x81, 0xe1, 0xe0, 0x7f, 0x00, 0x00, 0xc5, 0xfc, 0x10, 0x00, 0xc5, 0xfc, 0x11, 0x04, 0x31, 0x0f, 0xb6, 0x47, 0x0b, 0x39, 0xc5}},
        {0x4d9f10d, 10, 5, {0x81, 0xe1, 0xe0, 0x7f, 0x00, 0x00, 0xc5, 0xfc, 0x10, 0x00, 0xc5, 0xfc, 0x11, 0x04, 0x31, 0x0f, 0xb6, 0x47, 0x0f, 0x41, 0x39, 0xc5}},
        {0x4d9f19c, 12, 6, {0x41, 0x81, 0xe1, 0xe0, 0x7f, 0x00, 0x00, 0xc4, 0xc1, 0x7c, 0x10, 0x02, 0xc4, 0xc1, 0x7c, 0x11, 0x04, 0x31, 0x44, 0x0f, 0xb6, 0x4f, 0x0d, 0x44, 0x39, 0xc8}},
        {0x4eec95d, 11, 6, {0x41, 0x81, 0xe1, 0xe0, 0x7f, 0x00, 0x00, 0xc5, 0xfc, 0x10, 0x00, 0xc4, 0x81, 0x7c, 0x11, 0x04, 0x01, 0x0f, 0xb6, 0x42, 0x0b, 0x39, 0xc5}},
        {0x4eeca08, 10, 6, {0x81, 0xe7, 0xe0, 0x7f, 0x00, 0x00, 0xc5, 0xfc, 0x10, 0x06, 0xc4, 0xa1, 0x7c, 0x11, 0x04, 0x07, 0x0f, 0xb6, 0x72, 0x0d, 0x39, 0xf0}},
        {0x4eeca82, 11, 6, {0x41, 0x81, 0xe2, 0xe0, 0x7f, 0x00, 0x00, 0xc5, 0xfc, 0x10, 0x00, 0xc4, 0x81, 0x7c, 0x11, 0x04, 0x02, 0x0f, 0xb6, 0x42, 0x0f, 0x41, 0x39, 0xc6}},
    };
    return kSites;
}

std::size_t apply_descriptor_sites(
    std::uintptr_t image_base,
    std::size_t image_size,
    const VictoryDescriptorMarks* marks,
    const std::vector<DescriptorSiteDef>& sites,
    std::vector<PatchRecord>* out_records) {
    if (image_base == 0 || image_size == 0 || !marks) {
        return 0;
    }
    void* arena = nullptr;
    std::size_t used = 0;
    std::size_t patched = 0;

    for (const auto& site : sites) {
        if (site.window_rva + site.expected_window.size() > image_size ||
            site.store_offset + site.store_len > site.expected_window.size() ||
            site.store_len < 5) {
            continue;
        }
        const std::uintptr_t window_va = image_base + site.window_rva;
        if (std::memcmp(reinterpret_cast<const void*>(window_va),
                        site.expected_window.data(),
                        site.expected_window.size()) != 0) {
            continue;
        }

        const std::uintptr_t site_va = window_va + site.store_offset;
        if (!arena) {
            arena = alloc_near_executable(site_va, kStubArenaBytes);
            if (!arena) {
                break;
            }
        }
        const std::uintptr_t stub_va = reinterpret_cast<std::uintptr_t>(arena) + used;
        std::vector<std::uint8_t> stub_bytes;
        if (!build_descriptor_mark_stub(
                site.expected_window.data() + site.store_offset,
                site.store_len,
                site_va,
                stub_va,
                marks,
                stub_bytes)) {
            continue;
        }
        if (used + stub_bytes.size() > kStubArenaBytes) {
            break;
        }

        std::memcpy(reinterpret_cast<void*>(stub_va), stub_bytes.data(), stub_bytes.size());
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(stub_va), stub_bytes.size());

        std::vector<std::uint8_t> expected_store(
            site.expected_window.begin() + static_cast<std::ptrdiff_t>(site.store_offset),
            site.expected_window.begin() + static_cast<std::ptrdiff_t>(site.store_offset + site.store_len));
        PatchRecord rec;
        std::string err;
        if (!patch_rel32_jump(site_va, expected_store, stub_va, rec, err)) {
            continue;
        }
        used += align_up(stub_bytes.size(), 16);
        ++patched;
        if (out_records) {
            rec.trampoline = arena;
            rec.trampoline_size = kStubArenaBytes;
            out_records->push_back(std::move(rec));
        }
    }
    return patched;
}

bool build_or_byte_guard_stub(
    const std::uint8_t* code,
    std::size_t avail_len,
    std::uintptr_t site_addr,
    std::uintptr_t stub_addr,
    std::size_t& out_insn_len,
    std::vector<std::uint8_t>& out_stub) {
    OrByteDecoded d{};
    if (!decode_or_byte(code, avail_len, d) || d.len < 5) {
        return false;
    }

    out_insn_len = d.len;
    out_stub.clear();
    out_stub.push_back(0x50); // push rax
    out_stub.push_back(0x51); // push rcx
    if ((d.rex & 0x43) != 0) {
        out_stub.push_back(static_cast<std::uint8_t>(d.rex & 0x43));
    }
    out_stub.push_back(0x0F);
    out_stub.push_back(0xB6);
    out_stub.push_back(static_cast<std::uint8_t>(d.modrm & 0xC7));
    out_stub.insert(out_stub.end(), code + d.addr_start, code + d.addr_end);

    const std::uint8_t check[] = {
        0x89, 0xC1,                         // mov ecx, eax
        0xF7, 0xD1,                         // not ecx
        0x81, 0xE1, d.imm8, 0x00, 0x00, 0x00, // and ecx, imm8
        0x0C, d.imm8,                       // or al, imm8 (sets ZF/SF/PF/CF=0/OF=0)
    };
    out_stub.insert(out_stub.end(), std::begin(check), std::end(check));

    std::vector<std::uint8_t> write_path = {0x59, 0x58}; // pop rcx; pop rax
    write_path.insert(write_path.end(), code, code + d.len);
    const std::size_t skip_len = write_path.size() + 5;
    if (skip_len > 255) {
        return false;
    }

    out_stub.push_back(0x67);
    out_stub.push_back(0xE3);
    out_stub.push_back(static_cast<std::uint8_t>(skip_len)); // jecxz skip_write

    out_stub.insert(out_stub.end(), write_path.begin(), write_path.end());
    std::uint8_t back1[5]{};
    if (!emit_rel32_jmp(stub_addr + out_stub.size(), site_addr + d.len, back1)) {
        return false;
    }
    out_stub.insert(out_stub.end(), std::begin(back1), std::end(back1));

    out_stub.push_back(0x59); // pop rcx
    out_stub.push_back(0x58); // pop rax
    std::uint8_t back2[5]{};
    if (!emit_rel32_jmp(stub_addr + out_stub.size(), site_addr + d.len, back2)) {
        return false;
    }
    out_stub.insert(out_stub.end(), std::begin(back2), std::end(back2));
    return true;
}

bool run_self_tests(std::string& report) {
    // 1. Basic rel32 trampoline + restore test.
    void* fn_mem = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!fn_mem) {
        report += "hook_x64: VirtualAlloc failed\n";
        return false;
    }
    const std::vector<std::uint8_t> orig = {0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3};
    std::memcpy(fn_mem, orig.data(), orig.size());
    FlushInstructionCache(GetCurrentProcess(), fn_mem, orig.size());

    using Fn = int (*)();
    auto fn = reinterpret_cast<Fn>(fn_mem);
    if (fn() != 42) {
        VirtualFree(fn_mem, 0, MEM_RELEASE);
        report += "hook_x64: baseline call failed\n";
        return false;
    }

    void* tramp = alloc_near_executable(reinterpret_cast<std::uintptr_t>(fn_mem), 4096);
    if (!tramp) {
        VirtualFree(fn_mem, 0, MEM_RELEASE);
        report += "hook_x64: alloc_near_executable failed\n";
        return false;
    }
    const std::uint8_t tramp_code[] = {0xB8, 0x63, 0x00, 0x00, 0x00, 0xC3};
    std::memcpy(tramp, tramp_code, sizeof(tramp_code));
    FlushInstructionCache(GetCurrentProcess(), tramp, sizeof(tramp_code));

    PatchRecord rec;
    std::string err;
    if (!patch_rel32_jump(reinterpret_cast<std::uintptr_t>(fn_mem), orig,
                          reinterpret_cast<std::uintptr_t>(tramp), rec, err)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        VirtualFree(fn_mem, 0, MEM_RELEASE);
        report += "hook_x64: patch failed: " + err + "\n";
        return false;
    }

    const int hooked_val = fn();
    const bool restored = restore_patch(rec, err);
    const int restored_val = fn();

    VirtualFree(tramp, 0, MEM_RELEASE);
    VirtualFree(fn_mem, 0, MEM_RELEASE);

    if (hooked_val != 99 || !restored || restored_val != 42) {
        std::ostringstream oss;
        oss << "hook_x64: FAIL (hooked=" << hooked_val << " restored=" << restored_val << ")\n";
        report += oss.str();
        return false;
    }

    // 2. Descriptor-site stub self-test using SITES[0] machine-code pattern:
    //    fn(uint64_t offset /*rcx*/, uint8_t* table /*rdx*/, const uint8_t* rec /*r8*/)
    //    Body:
    //      mov rsi<-r8 / rcx<-offset / rdx<-table? Simpler: match SITES[0] exact window with rcx=offset, rdx->rax?
    //      In SITES[0]: `and ecx, 7FE0h; vmovups ymm0, [rax]; vmovups [rcx+rsi], ymm0; movzx eax, byte ptr [rdi+0Bh]; cmp ebp, eax`
    //      Let's allocate a minimal synthetic function using `vmovups ymm0, [r8]; vmovups [rcx+rdx], ymm0; vzeroupper; ret`.
    void* desc_fn_mem = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    void* desc_heap = VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    void* desc_stub_mem = desc_fn_mem ? alloc_near_executable(reinterpret_cast<std::uintptr_t>(desc_fn_mem), 4096) : nullptr;
    if (!desc_fn_mem || !desc_heap || !desc_stub_mem) {
        if (desc_stub_mem) VirtualFree(desc_stub_mem, 0, MEM_RELEASE);
        if (desc_heap) VirtualFree(desc_heap, 0, MEM_RELEASE);
        if (desc_fn_mem) VirtualFree(desc_fn_mem, 0, MEM_RELEASE);
        report += "hook_x64: descriptor test alloc failed\n";
        return false;
    }
    // `c4 c1 7c 10 00` = vmovups ymm0, [r8]
    // `c5 fc 11 04 11` = vmovups [rcx+rdx], ymm0 (5 bytes at offset 5)
    // `c5 f8 77 c3`    = vzeroupper; ret
    const std::vector<std::uint8_t> desc_fn_bytes = {
        0xC4, 0xC1, 0x7C, 0x10, 0x00,
        0xC5, 0xFC, 0x11, 0x04, 0x11,
        0xC5, 0xF8, 0x77, 0xC3,
    };
    std::memcpy(desc_fn_mem, desc_fn_bytes.data(), desc_fn_bytes.size());
    std::vector<std::uint8_t> slots(1024, 0);
    std::vector<std::uint8_t> chunks(8, 0);
    VictoryDescriptorMarks marks{};
    marks.start = reinterpret_cast<std::uint64_t>(desc_heap) + 0x1000;
    marks.len = 1024 * 32;
    marks.slots = slots.data();
    marks.chunks = chunks.data();

    const std::uintptr_t store_va = reinterpret_cast<std::uintptr_t>(desc_fn_mem) + 5;
    std::vector<std::uint8_t> d_stub;
    if (!build_descriptor_mark_stub(desc_fn_bytes.data() + 5, 5, store_va,
                                    reinterpret_cast<std::uintptr_t>(desc_stub_mem), &marks, d_stub)) {
        VirtualFree(desc_stub_mem, 0, MEM_RELEASE);
        VirtualFree(desc_heap, 0, MEM_RELEASE);
        VirtualFree(desc_fn_mem, 0, MEM_RELEASE);
        report += "hook_x64: build_descriptor_mark_stub failed\n";
        return false;
    }
    std::memcpy(desc_stub_mem, d_stub.data(), d_stub.size());
    FlushInstructionCache(GetCurrentProcess(), desc_stub_mem, d_stub.size());
    PatchRecord d_rec;
    if (!patch_rel32_jump(store_va, {0xC5, 0xFC, 0x11, 0x04, 0x11},
                          reinterpret_cast<std::uintptr_t>(desc_stub_mem), d_rec, err)) {
        VirtualFree(desc_stub_mem, 0, MEM_RELEASE);
        VirtualFree(desc_heap, 0, MEM_RELEASE);
        VirtualFree(desc_fn_mem, 0, MEM_RELEASE);
        report += "hook_x64: descriptor site patch failed: " + err + "\n";
        return false;
    }

    std::uint8_t rec_data[32]{};
    for (int k = 0; k < 32; ++k) rec_data[k] = static_cast<std::uint8_t>(k ^ 0x5A);
    using DescFn = void (*)(std::uint64_t, std::uint8_t*, const std::uint8_t*);
    auto dfn = reinterpret_cast<DescFn>(desc_fn_mem);
    // Write slot 131 (in chunk 1) inside marks range.
    dfn(32, reinterpret_cast<std::uint8_t*>(marks.start) + 130 * 32, rec_data);
    // Also write outside marks range (below marks.start) — must store data without marking.
    dfn(0, static_cast<std::uint8_t*>(desc_heap), rec_data);

    const bool desc_ok =
        slots[131] == 1 && chunks[1] == 1 && slots[0] == 0 &&
        std::memcmp(reinterpret_cast<void*>(marks.start + 131 * 32), rec_data, 32) == 0 &&
        std::memcmp(desc_heap, rec_data, 32) == 0;
    VirtualFree(desc_stub_mem, 0, MEM_RELEASE);
    VirtualFree(desc_heap, 0, MEM_RELEASE);
    VirtualFree(desc_fn_mem, 0, MEM_RELEASE);
    if (!desc_ok) {
        report += "hook_x64: descriptor-site mark verification failed\n";
        return false;
    }

    // 3. Write-site look-first guard stub combined with GuardedRegion!
    //    Synthesize `fn(uint8_t* obj /*rcx*/) -> int` containing:
    //      80 49 10 40    (or byte ptr [rcx+10h], 40h -> 4 bytes; pad? Use disp32 `80 89 10 00 00 00 40` = 7 bytes!)
    //      0F 95 C0       (setnz al)
    //      0F B6 C0       (movzx eax, al)
    //      C3             (ret)
    void* ws_fn_mem = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    void* ws_page = VirtualAlloc(nullptr, victory::mem::kConsolePageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    void* ws_stub_mem = ws_fn_mem ? alloc_near_executable(reinterpret_cast<std::uintptr_t>(ws_fn_mem), 4096) : nullptr;
    if (!ws_fn_mem || !ws_page || !ws_stub_mem) {
        if (ws_stub_mem) VirtualFree(ws_stub_mem, 0, MEM_RELEASE);
        if (ws_page) VirtualFree(ws_page, 0, MEM_RELEASE);
        if (ws_fn_mem) VirtualFree(ws_fn_mem, 0, MEM_RELEASE);
        report += "hook_x64: write-site test alloc failed\n";
        return false;
    }
    const std::vector<std::uint8_t> ws_code = {
        0x80, 0x89, 0x10, 0x00, 0x00, 0x00, 0x40, // or byte ptr [rcx+10h], 40h (7 bytes)
        0x0F, 0x95, 0xC0,                         // setnz al
        0x0F, 0xB6, 0xC0,                         // movzx eax, al
        0xC3,                                     // ret
    };
    std::memcpy(ws_fn_mem, ws_code.data(), ws_code.size());
    std::size_t insn_len = 0;
    std::vector<std::uint8_t> ws_stub;
    if (!build_or_byte_guard_stub(
            ws_code.data(), ws_code.size(),
            reinterpret_cast<std::uintptr_t>(ws_fn_mem),
            reinterpret_cast<std::uintptr_t>(ws_stub_mem),
            insn_len, ws_stub) || insn_len != 7) {
        VirtualFree(ws_stub_mem, 0, MEM_RELEASE);
        VirtualFree(ws_page, 0, MEM_RELEASE);
        VirtualFree(ws_fn_mem, 0, MEM_RELEASE);
        report += "hook_x64: build_or_byte_guard_stub failed\n";
        return false;
    }
    std::memcpy(ws_stub_mem, ws_stub.data(), ws_stub.size());
    FlushInstructionCache(GetCurrentProcess(), ws_stub_mem, ws_stub.size());
    PatchRecord ws_rec;
    if (!patch_rel32_jump(reinterpret_cast<std::uintptr_t>(ws_fn_mem),
                          std::vector<std::uint8_t>(ws_code.begin(), ws_code.begin() + 7),
                          reinterpret_cast<std::uintptr_t>(ws_stub_mem), ws_rec, err)) {
        VirtualFree(ws_stub_mem, 0, MEM_RELEASE);
        VirtualFree(ws_page, 0, MEM_RELEASE);
        VirtualFree(ws_fn_mem, 0, MEM_RELEASE);
        report += "hook_x64: write-site patch failed: " + err + "\n";
        return false;
    }

    auto* obj = static_cast<std::uint8_t*>(ws_page);
    obj[0x10] = 0x40; // Bit already set BEFORE arming guard.
    obj[0x20] = 0x00; // Different offset without bit set.
    victory::mem::GuardedRegion ws_guard(reinterpret_cast<std::uintptr_t>(ws_page),
                                         victory::mem::kConsolePageSize);
    ws_guard.arm_all();
    using WsFn = int (*)(std::uint8_t*);
    auto wfn = reinterpret_cast<WsFn>(ws_fn_mem);
    // Call 1: bit already set -> MUST NOT fault on PAGE_READONLY and MUST return non-zero ZF=0 -> 1!
    const int r1 = wfn(obj);
    const auto faults_after_noop = ws_guard.fault_count();
    // Call 2: target at +0x20-0x10=+0x10 -> offset 0x20 has 0x00 -> MUST fault once and set 0x40!
    const int r2 = wfn(obj + 0x10);
    const auto faults_after_write = ws_guard.fault_count();
    const bool ws_ok = (r1 == 1 && faults_after_noop == 0 && r2 == 1 &&
                        faults_after_write == 1 && obj[0x20] == 0x40);

    ws_guard.disarm_all();
    VirtualFree(ws_stub_mem, 0, MEM_RELEASE);
    VirtualFree(ws_page, 0, MEM_RELEASE);
    VirtualFree(ws_fn_mem, 0, MEM_RELEASE);
    if (!ws_ok) {
        std::ostringstream oss;
        oss << "hook_x64: write-site guard failed (r1=" << r1 << " f0=" << faults_after_noop
            << " r2=" << r2 << " f1=" << faults_after_write << ")\n";
        report += oss.str();
        return false;
    }

    report += "hook_x64: PASS (trampoline + descriptor-site stub + write-site look-first guard verified)\n";
    return true;
}

} // namespace victory::hook
