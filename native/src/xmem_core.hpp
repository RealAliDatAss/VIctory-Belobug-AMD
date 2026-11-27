#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace victory::xmem {

inline constexpr std::size_t kPage64K = 64u << 10;
inline constexpr std::size_t kMib2 = 2u << 20;
inline constexpr std::size_t kFramesPerPage = kPage64K >> 12; // 16 x 4 KiB

// Console XMemVirtualAlloc flags/kind extensions (from victory-hle::xmem).
inline constexpr std::uint32_t kKindPages2M = 0x2000'0000;
inline constexpr std::uint32_t kKindPages64KBit = 0x0040'0000;
inline constexpr std::uint32_t kKindPages64K = kKindPages2M | kKindPages64KBit;
inline constexpr std::uint32_t kHostProtectMask = 0x1ff;

inline constexpr std::uint64_t kFlagGraphics = 3;
inline constexpr std::uint64_t kFlagAddress32Bit = 4;

struct AddressRange {
    std::uintptr_t lowest = 0;
    std::uintptr_t highest = 0;
};

/// Computes console address bounds from XMem allocation flags:
/// - `kFlagAddress32Bit`: `[0, 4 GiB - 1]`
/// - Otherwise: `[4 GiB, 2^bits - 1]` where `bits` is `(flags >> 12) & 7` -> `47 - field`, else `40`.
AddressRange compute_address_range(std::uint64_t flags) noexcept;

/// Allocates virtual memory honoring console address bounds, 2 MiB alignment,
/// placeholder reservations (`MEM_RESERVE_PLACEHOLDER`), and `MEM_WRITE_WATCH`.
void* virtual_alloc(
    void* base,
    std::size_t size,
    std::uint32_t kind,
    std::uint64_t flags,
    std::uint32_t protect,
    bool enable_write_watch = true);

bool virtual_free(void* base, std::size_t size, std::uint32_t free_type) noexcept;

/// Splits a placeholder reservation at `split_addr` (`split_size` bytes) so it
/// can accept independent physical page mappings.
bool split_placeholder(void* split_addr, std::size_t split_size) noexcept;

/// Pagefile-backed physical section backing `XMemAllocatePhysicalPages` +
/// persistent whole-section window + GPU `OpenExistingHeapFromFileMapping` compat.
class PhysicalSection {
public:
    PhysicalSection(HANDLE handle, std::size_t page_count, bool precommitted);
    ~PhysicalSection();

    PhysicalSection(const PhysicalSection&) = delete;
    PhysicalSection& operator=(const PhysicalSection&) = delete;

    HANDLE handle() const noexcept { return handle_; }
    std::size_t page_count() const noexcept { return page_count_; }
    std::size_t byte_size() const noexcept { return page_count_ * kPage64K; }
    bool precommitted() const noexcept { return precommitted_; }

    /// Lazily maps one persistent host read/write view of the entire section and
    /// commits `[first_page, first_page + count)` on demand.
    void* ensure_window_slice(std::size_t first_page, std::size_t count);

    /// Maps `[first_page, first_page + count)` over a placeholder at `target_va`.
    void* map_into_placeholder(
        void* target_va,
        std::size_t first_page,
        std::size_t count,
        std::uint32_t protect);

private:
    bool ensure_committed_pages(std::uintptr_t view_base, std::size_t first_page, std::size_t count);

    HANDLE handle_ = nullptr;
    std::size_t page_count_ = 0;
    bool precommitted_ = false;
    std::once_flag window_once_;
    std::uintptr_t window_base_ = 0;
    std::unique_ptr<std::atomic<std::uint64_t>[]> committed_bits_;
    std::size_t bitmap_words_ = 0;
};

/// Manages physical page allocations and opaque console page numbers (`0x10_0000+`).
class PhysicalAllocator {
public:
    bool allocate_pages(
        std::uint64_t flags,
        std::size_t count,
        bool precommit_section,
        std::vector<std::uintptr_t>& out_page_numbers);

    bool free_pages(const std::uintptr_t* page_numbers, std::size_t count);

    /// Maps consecutive runs of `page_numbers[0..count)` onto a reserved placeholder at `base`.
    void* map_pages(
        void* base,
        std::size_t count,
        const std::uintptr_t* page_numbers,
        std::uint32_t protect);

    /// Unmaps a previously mapped physical view, preserving its placeholder.
    bool unmap_preserving_placeholder(void* base, std::size_t bytes);

    std::size_t tracked_page_count() const;

private:
    struct PageEntry {
        std::shared_ptr<PhysicalSection> section;
        std::size_t page_index = 0;
    };

    mutable std::mutex mutex_;
    std::uintptr_t next_page_num_ = 0x10'0000;
    std::unordered_map<std::uintptr_t, PageEntry> pages_;
};

/// Runs unit tests covering 32-bit/40-bit bounds, SEC_COMMIT sections,
/// placeholder splitting, multi-view aliasing, and placeholder preservation.
bool run_self_tests(std::string& report);

} // namespace victory::xmem
