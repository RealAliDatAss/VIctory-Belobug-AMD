#include "xmem_core.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

#ifndef MEM_WRITE_WATCH
#define MEM_WRITE_WATCH 0x00200000
#endif

namespace victory::xmem {

namespace {

std::size_t align_up(std::size_t v, std::size_t a) noexcept {
    return (v + a - 1) & ~(a - 1);
}

} // namespace

AddressRange compute_address_range(std::uint64_t flags) noexcept {
    if ((flags & kFlagAddress32Bit) != 0) {
        return AddressRange{0, 0xFFFF'FFFFULL};
    }
    const std::uint64_t field = (flags >> 12) & 7ULL;
    const std::uint64_t bits = (field == 0) ? 40ULL : (47ULL - field);
    return AddressRange{
        static_cast<std::uintptr_t>(1ULL << 32),
        static_cast<std::uintptr_t>((1ULL << bits) - 1ULL),
    };
}

void* virtual_alloc(
    void* base,
    std::size_t size,
    std::uint32_t kind,
    std::uint64_t flags,
    std::uint32_t protect,
    bool enable_write_watch) {
    if (size == 0) {
        return nullptr;
    }

    const bool is_placeholder =
        ((kind & kKindPages64K) == kKindPages64K) && ((kind & MEM_COMMIT) == 0);
    if (is_placeholder && ((kind & MEM_REPLACE_PLACEHOLDER) != 0)) {
        return base;
    }

    const AddressRange range = compute_address_range(flags);
    MEM_ADDRESS_REQUIREMENTS req{};
    req.LowestStartingAddress = reinterpret_cast<PVOID>(range.lowest);
    req.HighestEndingAddress = reinterpret_cast<PVOID>(range.highest);
    req.Alignment = ((kind & (kKindPages2M | kKindPages64KBit)) == kKindPages2M) ? kMib2 : 0;

    DWORD win_protect = (protect & kHostProtectMask);
    if (win_protect == 0) {
        win_protect = PAGE_NOACCESS;
    }
    if (is_placeholder) {
        win_protect = PAGE_NOACCESS;
    }

    DWORD win_kind = kind & ~(kKindPages2M | kKindPages64KBit);
    if (is_placeholder) {
        win_kind |= MEM_RESERVE | MEM_RESERVE_PLACEHOLDER;
    }

    const bool constrained = (base == nullptr);
    if (constrained) {
        win_kind |= MEM_RESERVE;
        if (enable_write_watch && ((win_kind & MEM_RESERVE_PLACEHOLDER) == 0)) {
            win_kind |= MEM_WRITE_WATCH;
        }
    }

    MEM_EXTENDED_PARAMETER param{};
    param.Type = MemExtendedParameterAddressRequirements;
    param.Pointer = &req;

    return VirtualAlloc2(
        GetCurrentProcess(),
        base,
        size,
        win_kind,
        win_protect,
        constrained ? &param : nullptr,
        constrained ? 1UL : 0UL);
}

bool virtual_free(void* base, std::size_t size, std::uint32_t free_type) noexcept {
    if (!base) {
        return false;
    }
    return VirtualFree(base, size, free_type) != 0;
}

bool split_placeholder(void* split_addr, std::size_t split_size) noexcept {
    if (!split_addr || split_size == 0) {
        return false;
    }
    return VirtualFree(split_addr, split_size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != 0;
}

PhysicalSection::PhysicalSection(HANDLE handle, std::size_t page_count, bool precommitted)
    : handle_(handle),
      page_count_(page_count),
      precommitted_(precommitted),
      bitmap_words_((page_count + 63) / 64) {
    committed_bits_ = std::make_unique<std::atomic<std::uint64_t>[]>(bitmap_words_);
    const std::uint64_t init_val = precommitted ? ~0ULL : 0ULL;
    for (std::size_t i = 0; i < bitmap_words_; ++i) {
        committed_bits_[i].store(init_val, std::memory_order_relaxed);
    }
}

PhysicalSection::~PhysicalSection() {
    if (window_base_ != 0) {
        UnmapViewOfFile(reinterpret_cast<LPCVOID>(window_base_));
        window_base_ = 0;
    }
    if (handle_) {
        CloseHandle(handle_);
        handle_ = nullptr;
    }
}

bool PhysicalSection::ensure_committed_pages(
    std::uintptr_t view_base,
    std::size_t first_page,
    std::size_t count) {
    if (first_page + count > page_count_) {
        return false;
    }
    if (precommitted_) {
        return true;
    }
    std::size_t page = first_page;
    const std::size_t end = first_page + count;
    while (page < end) {
        const std::uint64_t bit = 1ULL << (page % 64);
        if ((committed_bits_[page / 64].load(std::memory_order_acquire) & bit) != 0) {
            ++page;
            continue;
        }
        std::size_t run = 0;
        while (page + run < end) {
            const std::size_t p = page + run;
            const std::uint64_t b = 1ULL << (p % 64);
            if ((committed_bits_[p / 64].load(std::memory_order_acquire) & b) != 0) {
                break;
            }
            ++run;
        }
        if (run == 0) {
            continue;
        }
        void* commit_addr = reinterpret_cast<void*>(view_base + page * kPage64K);
        if (!VirtualAlloc(commit_addr, run * kPage64K, MEM_COMMIT, PAGE_READWRITE)) {
            return false;
        }
        for (std::size_t p = page; p < page + run; ++p) {
            committed_bits_[p / 64].fetch_or(1ULL << (p % 64), std::memory_order_acq_rel);
        }
        page += run;
    }
    return true;
}

void* PhysicalSection::ensure_window_slice(std::size_t first_page, std::size_t count) {
    if (!handle_ || first_page + count > page_count_) {
        return nullptr;
    }
    std::call_once(window_once_, [this]() {
        void* view = MapViewOfFile(handle_, FILE_MAP_WRITE, 0, 0, byte_size());
        window_base_ = reinterpret_cast<std::uintptr_t>(view);
    });
    if (window_base_ == 0) {
        return nullptr;
    }
    if (!ensure_committed_pages(window_base_, first_page, count)) {
        return nullptr;
    }
    return reinterpret_cast<void*>(window_base_ + first_page * kPage64K);
}

void* PhysicalSection::map_into_placeholder(
    void* target_va,
    std::size_t first_page,
    std::size_t count,
    std::uint32_t protect) {
    if (!handle_ || !target_va || first_page + count > page_count_ || count == 0) {
        return nullptr;
    }
    const ULONG64 offset = static_cast<ULONG64>(first_page) * kPage64K;
    const SIZE_T len = count * kPage64K;
    const DWORD page_prot = (protect & kHostProtectMask) ? (protect & kHostProtectMask) : PAGE_READWRITE;
    void* mapped = MapViewOfFile3(
        handle_,
        GetCurrentProcess(),
        target_va,
        offset,
        len,
        MEM_REPLACE_PLACEHOLDER,
        page_prot,
        nullptr,
        0);
    if (!mapped) {
        return nullptr;
    }
    if (!ensure_committed_pages(reinterpret_cast<std::uintptr_t>(mapped), first_page, count)) {
        UnmapViewOfFile2(GetCurrentProcess(), mapped, MEM_PRESERVE_PLACEHOLDER);
        return nullptr;
    }
    return mapped;
}

bool PhysicalAllocator::allocate_pages(
    std::uint64_t /*flags*/,
    std::size_t count,
    bool precommit_section,
    std::vector<std::uintptr_t>& out_page_numbers) {
    out_page_numbers.clear();
    if (count == 0) {
        return false;
    }
    const std::uint64_t total_bytes = static_cast<std::uint64_t>(count) * kPage64K;
    const DWORD sec_attr = PAGE_READWRITE | (precommit_section ? SEC_COMMIT : SEC_RESERVE);
    HANDLE handle = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        sec_attr,
        static_cast<DWORD>(total_bytes >> 32),
        static_cast<DWORD>(total_bytes & 0xFFFF'FFFFULL),
        nullptr);
    if (!handle) {
        return false;
    }

    auto section = std::make_shared<PhysicalSection>(handle, count, precommit_section);
    std::uintptr_t first = 0;
    {
        std::lock_guard lock(mutex_);
        first = next_page_num_;
        next_page_num_ += count * kFramesPerPage;
        for (std::size_t i = 0; i < count; ++i) {
            pages_.emplace(first + i * kFramesPerPage, PageEntry{section, i});
        }
    }

    out_page_numbers.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        out_page_numbers.push_back(first + i * kFramesPerPage);
    }
    return true;
}

bool PhysicalAllocator::free_pages(const std::uintptr_t* page_numbers, std::size_t count) {
    if (!page_numbers && count > 0) {
        return false;
    }
    std::vector<PageEntry> dropped;
    dropped.reserve(count);
    {
        std::lock_guard lock(mutex_);
        for (std::size_t i = 0; i < count; ++i) {
            auto it = pages_.find(page_numbers[i]);
            if (it != pages_.end()) {
                dropped.push_back(std::move(it->second));
                pages_.erase(it);
            }
        }
    }
    return true;
}

void* PhysicalAllocator::map_pages(
    void* base,
    std::size_t count,
    const std::uintptr_t* page_numbers,
    std::uint32_t protect) {
    if (!base || !page_numbers || count == 0) {
        return nullptr;
    }

    struct Run {
        std::shared_ptr<PhysicalSection> section;
        std::size_t first_page = 0;
        std::size_t page_count = 0;
    };
    std::vector<Run> runs;
    {
        std::lock_guard lock(mutex_);
        for (std::size_t i = 0; i < count; ++i) {
            auto it = pages_.find(page_numbers[i]);
            if (it == pages_.end()) {
                return nullptr;
            }
            const auto& entry = it->second;
            if (!runs.empty() &&
                runs.back().section.get() == entry.section.get() &&
                runs.back().first_page + runs.back().page_count == entry.page_index) {
                runs.back().page_count += 1;
            } else {
                runs.push_back(Run{entry.section, entry.page_index, 1});
            }
        }
    }

    auto addr = reinterpret_cast<std::uintptr_t>(base);
    for (std::size_t r = 0; r < runs.size(); ++r) {
        const std::size_t run_bytes = runs[r].page_count * kPage64K;
        if (r + 1 < runs.size()) {
            // Split off the prefix placeholder when multiple discontinuous runs map into one reservation.
            split_placeholder(reinterpret_cast<void*>(addr), run_bytes);
        }
        if (!runs[r].section->map_into_placeholder(
                reinterpret_cast<void*>(addr), runs[r].first_page, runs[r].page_count, protect)) {
            return nullptr;
        }
        addr += run_bytes;
    }
    return base;
}

bool PhysicalAllocator::unmap_preserving_placeholder(void* base, std::size_t /*bytes*/) {
    if (!base) {
        return false;
    }
    return UnmapViewOfFile2(GetCurrentProcess(), base, MEM_PRESERVE_PLACEHOLDER) != 0;
}

std::size_t PhysicalAllocator::tracked_page_count() const {
    std::lock_guard lock(mutex_);
    return pages_.size();
}

bool run_self_tests(std::string& report) {
    std::ostringstream oss;

    // 1. Verify 32-bit console graphics reservation constraint (< 4 GiB).
    void* low32 = virtual_alloc(nullptr, 2 * kPage64K, MEM_RESERVE | MEM_COMMIT,
                                kFlagGraphics | kFlagAddress32Bit, PAGE_READWRITE, false);
    const auto low_addr = reinterpret_cast<std::uintptr_t>(low32);
    if (!low32 || low_addr + 2 * kPage64K > 0x1'0000'0000ULL) {
        if (low32) {
            virtual_free(low32, 0, MEM_RELEASE);
        }
        report += "xmem_core: 32-bit constraint check failed\n";
        return false;
    }
    virtual_free(low32, 0, MEM_RELEASE);

    // 2. Verify 40-bit console address constraint ([4 GiB, 1 TiB)).
    void* gpu40 = virtual_alloc(nullptr, 2 * kPage64K, MEM_RESERVE | MEM_COMMIT,
                                kFlagGraphics, PAGE_READWRITE, false);
    const auto gpu40_addr = reinterpret_cast<std::uintptr_t>(gpu40);
    if (!gpu40 || gpu40_addr < 0x1'0000'0000ULL || gpu40_addr + 2 * kPage64K > (1ULL << 40)) {
        if (gpu40) {
            virtual_free(gpu40, 0, MEM_RELEASE);
        }
        report += "xmem_core: 40-bit constraint check failed\n";
        return false;
    }
    virtual_free(gpu40, 0, MEM_RELEASE);

    // 3. Verify physical SEC_COMMIT section + placeholder split + multi-view aliasing.
    PhysicalAllocator alloc;
    std::vector<std::uintptr_t> phys;
    if (!alloc.allocate_pages(kFlagGraphics, 2, true, phys) || phys.size() != 2) {
        report += "xmem_core: allocate_pages failed\n";
        return false;
    }

    const std::size_t res_bytes = 2 * kPage64K;
    void* ph = virtual_alloc(nullptr, res_bytes, kKindPages64K, kFlagGraphics | kFlagAddress32Bit, PAGE_NOACCESS);
    if (!ph) {
        alloc.free_pages(phys.data(), phys.size());
        report += "xmem_core: placeholder virtual_alloc failed\n";
        return false;
    }
    if (!split_placeholder(ph, kPage64K)) {
        virtual_free(ph, 0, MEM_RELEASE);
        alloc.free_pages(phys.data(), phys.size());
        report += "xmem_core: split_placeholder failed\n";
        return false;
    }

    void* view_a = alloc.map_pages(ph, 1, &phys[0], PAGE_READWRITE);
    void* view_b = alloc.map_pages(static_cast<std::uint8_t*>(ph) + kPage64K, 1, &phys[0], PAGE_READWRITE);
    if (!view_a || !view_b) {
        if (view_a) {
            alloc.unmap_preserving_placeholder(view_a, kPage64K);
        }
        if (view_b) {
            alloc.unmap_preserving_placeholder(view_b, kPage64K);
        }
        virtual_free(ph, 0, MEM_RELEASE);
        alloc.free_pages(phys.data(), phys.size());
        report += "xmem_core: alias map_pages failed\n";
        return false;
    }

    auto* a_u32 = static_cast<volatile std::uint32_t*>(view_a);
    auto* b_u32 = static_cast<volatile std::uint32_t*>(view_b);
    a_u32[0] = 0xCAFE'BABEu;
    const std::uint32_t seen = b_u32[0];

    alloc.unmap_preserving_placeholder(view_a, kPage64K);
    alloc.unmap_preserving_placeholder(view_b, kPage64K);
    // Reclaim split placeholders.
    virtual_free(ph, 0, MEM_RELEASE);
    virtual_free(static_cast<std::uint8_t*>(ph) + kPage64K, 0, MEM_RELEASE);
    alloc.free_pages(phys.data(), phys.size());

    if (seen != 0xCAFE'BABEu) {
        oss << "xmem_core: physical alias mismatch (0x" << std::hex << seen << ")\n";
        report += oss.str();
        return false;
    }

    oss << "xmem_core: PASS (32b=0x" << std::hex << low_addr
        << ", 40b=0x" << gpu40_addr << ", SEC_COMMIT alias verified)\n";
    report += oss.str();
    return true;
}

} // namespace victory::xmem
