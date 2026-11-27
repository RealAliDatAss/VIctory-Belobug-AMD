#include "mem_guard.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace victory::mem {

namespace {

struct VehSlot {
    std::atomic<GuardedRegion*> region{nullptr};
    std::atomic<std::uint32_t> active_faults{0};
};

std::mutex g_registry_write_mutex;
std::array<VehSlot, kMaxGuardedRegions> g_veh_slots;
std::atomic<std::size_t> g_veh_high_water{0};
PVOID g_veh_handle = nullptr;
std::once_flag g_veh_once;

std::size_t register_veh_region(GuardedRegion* region) {
    std::lock_guard lock(g_registry_write_mutex);
    for (std::size_t i = 0; i < kMaxGuardedRegions; ++i) {
        if (g_veh_slots[i].region.load(std::memory_order_relaxed) == nullptr) {
            g_veh_slots[i].region.store(region, std::memory_order_release);
            const std::size_t hw = g_veh_high_water.load(std::memory_order_relaxed);
            if (i + 1 > hw) {
                g_veh_high_water.store(i + 1, std::memory_order_release);
            }
            return i;
        }
    }
    return kMaxGuardedRegions;
}

void unregister_veh_region(std::size_t slot, GuardedRegion* expected) {
    if (slot >= kMaxGuardedRegions) {
        return;
    }
    std::lock_guard lock(g_registry_write_mutex);
    if (g_veh_slots[slot].region.load(std::memory_order_relaxed) == expected) {
        g_veh_slots[slot].region.store(nullptr, std::memory_order_release);
        // Drain any concurrent VEH readers on this slot without taking a mutex inside VEH.
        while (g_veh_slots[slot].active_faults.load(std::memory_order_acquire) != 0) {
            YieldProcessor();
        }
    }
}

LONG CALLBACK veh_handler(PEXCEPTION_POINTERS info) {
    if (!info || !info->ExceptionRecord) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto* rec = info->ExceptionRecord;
    // Access violation (0xC0000005), write fault (ExceptionInformation[0] == 1).
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2 ||
        rec->ExceptionInformation[0] != 1) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto fault_addr = static_cast<std::uintptr_t>(rec->ExceptionInformation[1]);
    std::uintptr_t rip = 0;
#if defined(_M_X64) || defined(__x86_64__)
    if (info->ContextRecord) {
        rip = static_cast<std::uintptr_t>(info->ContextRecord->Rip);
    }
#endif

    // Mutex-free traversal of active VEH slots to prevent lock inversion/deadlock.
    const std::size_t limit = g_veh_high_water.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < limit; ++i) {
        VehSlot& slot = g_veh_slots[i];
        slot.active_faults.fetch_add(1, std::memory_order_acq_rel);
        GuardedRegion* region = slot.region.load(std::memory_order_acquire);
        bool handled = false;
        if (region && fault_addr >= region->base() && fault_addr - region->base() < region->size()) {
            handled = region->handle_write_fault(fault_addr, rip);
        }
        slot.active_faults.fetch_sub(1, std::memory_order_release);
        if (handled) {
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

std::size_t align_up(std::size_t val, std::size_t align) noexcept {
    return (val + align - 1) & ~(align - 1);
}

} // namespace

DirtyBitset::DirtyBitset(std::size_t bit_count) {
    reset(bit_count);
}

void DirtyBitset::reset(std::size_t bit_count) {
    bit_count_ = bit_count;
    word_count_ = (bit_count + 63) / 64;
    if (word_count_ == 0) {
        words_.reset();
        return;
    }
    words_ = std::make_unique<std::atomic<std::uint64_t>[]>(word_count_);
    for (std::size_t i = 0; i < word_count_; ++i) {
        words_[i].store(0, std::memory_order_relaxed);
    }
}

void DirtyBitset::set(std::size_t idx) noexcept {
    if (idx >= bit_count_) {
        return;
    }
    words_[idx / 64].fetch_or(1ULL << (idx % 64), std::memory_order_acq_rel);
}

bool DirtyBitset::test_and_clear(std::size_t idx) noexcept {
    if (idx >= bit_count_) {
        return false;
    }
    const std::uint64_t bit = 1ULL << (idx % 64);
    auto& word = words_[idx / 64];
    if ((word.load(std::memory_order_acquire) & bit) == 0) {
        return false;
    }
    return (word.fetch_and(~bit, std::memory_order_acq_rel) & bit) != 0;
}

void DirtyBitset::drain_set_bits(std::vector<std::size_t>& out_indices) noexcept {
    out_indices.clear();
    for (std::size_t w = 0; w < word_count_; ++w) {
        if (words_[w].load(std::memory_order_acquire) == 0) {
            continue;
        }
        std::uint64_t bits = words_[w].exchange(0, std::memory_order_acq_rel);
        while (bits != 0) {
            unsigned long tz = 0;
            _BitScanForward64(&tz, bits);
            const std::size_t idx = w * 64 + static_cast<std::size_t>(tz);
            if (idx < bit_count_) {
                out_indices.push_back(idx);
            }
            bits &= (bits - 1);
        }
    }
}

void ensure_veh_installed() {
    std::call_once(g_veh_once, []() {
        g_veh_handle = AddVectoredExceptionHandler(1, veh_handler);
    });
}

GuardedRegion::GuardedRegion(std::uintptr_t base, std::size_t size)
    : base_(base),
      size_(size),
      block_count_((size + kConsolePageSize - 1) / kConsolePageSize),
      states_(std::make_unique<std::atomic<std::uint8_t>[]>(block_count_)),
      dirty_epochs_(std::make_unique<std::atomic<std::uint64_t>[]>(block_count_)) {
    for (std::size_t i = 0; i < block_count_; ++i) {
        states_[i].store(kWritableDirty, std::memory_order_relaxed);
        dirty_epochs_[i].store(0, std::memory_order_relaxed);
    }
    ensure_veh_installed();
    veh_slot_ = register_veh_region(this);
}

GuardedRegion::~GuardedRegion() {
    disarm_all();
    unregister_veh_region(veh_slot_, this);
}

std::size_t GuardedRegion::arm_all() {
    std::size_t armed = 0;
    for (std::size_t i = 0; i < block_count_; ++i) {
        std::uint8_t expected = kWritableDirty;
        if (!states_[i].compare_exchange_strong(expected, kTransitioning, std::memory_order_acq_rel)) {
            continue;
        }
        const std::uintptr_t addr = base_ + i * kConsolePageSize;
        const std::size_t len = std::min(kConsolePageSize, size_ - i * kConsolePageSize);
        DWORD old_prot = 0;
        if (VirtualProtect(reinterpret_cast<LPVOID>(addr), len, PAGE_READONLY, &old_prot)) {
            states_[i].store(kGuardedClean, std::memory_order_release);
            ++armed;
        } else {
            states_[i].store(kWritableDirty, std::memory_order_release);
        }
    }
    return armed;
}

void GuardedRegion::disarm_all() {
    for (std::size_t i = 0; i < block_count_; ++i) {
        const auto cur = states_[i].load(std::memory_order_acquire);
        if (cur == kWritableDirty) {
            continue;
        }
        const std::uintptr_t addr = base_ + i * kConsolePageSize;
        const std::size_t len = std::min(kConsolePageSize, size_ - i * kConsolePageSize);
        DWORD old_prot = 0;
        VirtualProtect(reinterpret_cast<LPVOID>(addr), len, PAGE_READWRITE, &old_prot);
        states_[i].store(kWritableDirty, std::memory_order_release);
    }
}

bool GuardedRegion::consume_dirty(std::size_t index) noexcept {
    if (index >= block_count_) {
        return false;
    }
    return dirty_epochs_[index].exchange(0, std::memory_order_acq_rel) != 0;
}

bool GuardedRegion::handle_write_fault(std::uintptr_t fault_addr, std::uintptr_t rip) noexcept {
    if (fault_addr < base_ || fault_addr - base_ >= size_) {
        return false;
    }
    const std::size_t idx = (fault_addr - base_) / kConsolePageSize;
    for (;;) {
        const std::uint8_t state = states_[idx].load(std::memory_order_acquire);
        if (state == kWritableDirty) {
            return true;
        }
        if (state == kTransitioning) {
            YieldProcessor();
            continue;
        }
        std::uint8_t expected = kGuardedClean;
        if (states_[idx].compare_exchange_weak(expected, kTransitioning, std::memory_order_acq_rel)) {
            const std::uintptr_t addr = base_ + idx * kConsolePageSize;
            const std::size_t len = std::min(kConsolePageSize, size_ - idx * kConsolePageSize);
            DWORD old_prot = 0;
            const BOOL ok = VirtualProtect(reinterpret_cast<LPVOID>(addr), len, PAGE_READWRITE, &old_prot);
            dirty_epochs_[idx].fetch_add(1, std::memory_order_acq_rel);
            states_[idx].store(kWritableDirty, std::memory_order_release);
            if (ok) {
                fault_count_.fetch_add(1, std::memory_order_relaxed);
                last_fault_ip_.store(rip, std::memory_order_relaxed);
                return true;
            }
            return false;
        }
    }
}

WriteWatchRegion::WriteWatchRegion(std::size_t size, std::uintptr_t existing_base)
    : size_(align_up(size, kConsolePageSize)),
      block_count_(size_ / kConsolePageSize),
      dirty_blocks_(block_count_) {
    if (size_ == 0) {
        return;
    }
    if (existing_base != 0) {
        base_ = existing_base;
        owns_memory_ = false;
    } else {
        void* mem = VirtualAlloc(nullptr, size_, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH,
                                 PAGE_READWRITE);
        if (!mem) {
            return;
        }
        base_ = reinterpret_cast<std::uintptr_t>(mem);
        owns_memory_ = true;
        ResetWriteWatch(mem, size_);
    }
    scratch_addrs_.resize((size_ + kOsPageSize - 1) / kOsPageSize);
}

WriteWatchRegion::~WriteWatchRegion() {
    if (owns_memory_ && base_ != 0) {
        VirtualFree(reinterpret_cast<LPVOID>(base_), 0, MEM_RELEASE);
        base_ = 0;
    }
}

bool WriteWatchRegion::reset_watch() noexcept {
    if (base_ == 0 || size_ == 0) {
        return false;
    }
    return ResetWriteWatch(reinterpret_cast<LPVOID>(base_), size_) == 0;
}

bool WriteWatchRegion::poll_dirty_64k(std::vector<std::size_t>& out_blocks, bool reset_flag) {
    out_blocks.clear();
    if (base_ == 0 || size_ == 0 || scratch_addrs_.empty()) {
        return false;
    }
    ULONG_PTR count = scratch_addrs_.size();
    DWORD granularity = 0;
    const DWORD flags = reset_flag ? WRITE_WATCH_FLAG_RESET : 0;
    const UINT rc = GetWriteWatch(flags, reinterpret_cast<PVOID>(base_), size_,
                                  scratch_addrs_.data(), &count, &granularity);
    if (rc != 0) {
        return false;
    }
    for (ULONG_PTR i = 0; i < count; ++i) {
        const auto addr = reinterpret_cast<std::uintptr_t>(scratch_addrs_[i]);
        if (addr >= base_ && addr - base_ < size_) {
            dirty_blocks_.set((addr - base_) / kConsolePageSize);
        }
    }
    dirty_blocks_.drain_set_bits(out_blocks);
    return true;
}

bool safe_read_bytes(void* dst, const void* src, std::size_t bytes) noexcept {
    if (!dst || !src) {
        return false;
    }
    if (bytes == 0) {
        return true;
    }
    __try {
        std::memcpy(dst, src, bytes);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool run_self_tests(std::string& report) {
    std::ostringstream oss;
    constexpr std::size_t kBlocks = 8;
    constexpr std::size_t kTotal = kBlocks * kConsolePageSize;

    void* raw = VirtualAlloc(nullptr, kTotal, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!raw) {
        report += "mem_guard: VirtualAlloc failed\n";
        return false;
    }

    auto* bytes = static_cast<volatile std::uint8_t*>(raw);
    GuardedRegion region(reinterpret_cast<std::uintptr_t>(raw), kTotal);
    const std::size_t armed = region.arm_all();
    if (armed != kBlocks) {
        oss << "mem_guard: expected " << kBlocks << " armed blocks, got " << armed << "\n";
        report += oss.str();
        VirtualFree(raw, 0, MEM_RELEASE);
        return false;
    }

    // Exercise multi-threaded concurrent write faults on distinct blocks (0, 2, 4, 6).
    std::vector<std::thread> workers;
    for (std::size_t b : {0u, 2u, 4u, 6u}) {
        workers.emplace_back([bytes, b]() {
            bytes[b * kConsolePageSize + 64] = static_cast<std::uint8_t>(0x30 + b);
            bytes[b * kConsolePageSize + 128] = static_cast<std::uint8_t>(0x50 + b);
        });
    }
    for (auto& t : workers) {
        t.join();
    }

    bool ok_dirty = true;
    for (std::size_t b = 0; b < kBlocks; ++b) {
        const bool expected = (b % 2 == 0);
        if (region.consume_dirty(b) != expected) {
            ok_dirty = false;
        }
    }
    if (!ok_dirty || region.fault_count() != 4 || region.last_fault_ip() == 0) {
        oss << "mem_guard: MT dirty tracking mismatch (faults=" << region.fault_count() << ")\n";
        report += oss.str();
        VirtualFree(raw, 0, MEM_RELEASE);
        return false;
    }

    // Verify SEH safe_read_bytes catches unmapped memory cleanly without crashing.
    std::uint8_t buf[16]{};
    const bool ok_good = safe_read_bytes(buf, raw, sizeof(buf));
    const bool ok_bad = safe_read_bytes(buf, reinterpret_cast<const void*>(0x1000), sizeof(buf));
    VirtualFree(raw, 0, MEM_RELEASE);

    if (!ok_good || ok_bad) {
        oss << "mem_guard: safe_read_bytes mismatch (good=" << ok_good << " bad=" << ok_bad << ")\n";
        report += oss.str();
        return false;
    }

    // Verify WriteWatchRegion 4K -> 64K deduplication and reset.
    WriteWatchRegion ww(4 * kConsolePageSize);
    if (!ww.valid()) {
        report += "mem_guard: WriteWatchRegion init failed\n";
        return false;
    }
    auto* ww_bytes = reinterpret_cast<volatile std::uint8_t*>(ww.base());
    ww_bytes[100] = 1;
    ww_bytes[kOsPageSize + 200] = 2;             // still block 0
    ww_bytes[3 * kConsolePageSize + 77] = 3;     // block 3

    std::vector<std::size_t> dirty_blocks;
    if (!ww.poll_dirty_64k(dirty_blocks, true) || dirty_blocks.size() != 2 ||
        dirty_blocks[0] != 0 || dirty_blocks[1] != 3) {
        oss << "mem_guard: WriteWatchRegion dirty blocks mismatch (n=" << dirty_blocks.size() << ")\n";
        report += oss.str();
        return false;
    }
    if (!ww.poll_dirty_64k(dirty_blocks, true) || !dirty_blocks.empty()) {
        report += "mem_guard: WriteWatchRegion reset failed\n";
        return false;
    }

    oss << "mem_guard: PASS (armed=" << armed << ", MT faults=" << region.fault_count()
        << ", WriteWatch 64K + SEH ok)\n";
    report += oss.str();
    return true;
}

} // namespace victory::mem
