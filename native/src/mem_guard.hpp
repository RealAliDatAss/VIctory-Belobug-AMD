#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace victory::mem {

/// Console page size (64 KiB) used for guard granularity and GPU copy windows.
inline constexpr std::size_t kConsolePageSize = 64u << 10;
inline constexpr std::size_t kOsPageSize = 4u << 10;
inline constexpr std::size_t kMaxGuardedRegions = 256;

/// Lock-free atomic bitset covering 64 KiB console blocks (64 blocks per uint64_t).
class DirtyBitset {
public:
    explicit DirtyBitset(std::size_t bit_count = 0);

    void reset(std::size_t bit_count);
    std::size_t bit_count() const noexcept { return bit_count_; }

    void set(std::size_t idx) noexcept;
    bool test_and_clear(std::size_t idx) noexcept;
    void drain_set_bits(std::vector<std::size_t>& out_indices) noexcept;

private:
    std::size_t bit_count_ = 0;
    std::size_t word_count_ = 0;
    std::unique_ptr<std::atomic<std::uint64_t>[]> words_;
};

/// Tracks a contiguous range of title memory using lock-free per-block atomic
/// state and a mutex-free Vectored Exception Handler (VEH) slot table.
class GuardedRegion {
public:
    GuardedRegion(std::uintptr_t base, std::size_t size);
    ~GuardedRegion();

    GuardedRegion(const GuardedRegion&) = delete;
    GuardedRegion& operator=(const GuardedRegion&) = delete;

    std::uintptr_t base() const noexcept { return base_; }
    std::size_t size() const noexcept { return size_; }
    std::size_t block_count() const noexcept { return block_count_; }

    /// Arms `PAGE_READONLY` guards on blocks that are currently clean/unclaimed.
    /// Returns the number of blocks newly guarded.
    std::size_t arm_all();

    /// Disarms all guarded blocks back to `PAGE_READWRITE`.
    void disarm_all();

    /// Returns true and resets dirty bit if block `index` was written since last query.
    bool consume_dirty(std::size_t index) noexcept;

    /// Total faults handled across this region.
    std::uint64_t fault_count() const noexcept {
        return fault_count_.load(std::memory_order_relaxed);
    }

    /// Last faulting instruction pointer observed (0 if none).
    std::uintptr_t last_fault_ip() const noexcept {
        return last_fault_ip_.load(std::memory_order_relaxed);
    }

    /// Internal VEH entry point called when an access violation falls inside this region.
    bool handle_write_fault(std::uintptr_t fault_addr, std::uintptr_t rip) noexcept;

private:
    enum BlockState : std::uint8_t {
        kWritableDirty = 0,
        kGuardedClean = 1,
        kTransitioning = 2,
    };

    std::uintptr_t base_ = 0;
    std::size_t size_ = 0;
    std::size_t block_count_ = 0;
    std::size_t veh_slot_ = kMaxGuardedRegions;
    std::unique_ptr<std::atomic<std::uint8_t>[]> states_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> dirty_epochs_;
    std::atomic<std::uint64_t> fault_count_{0};
    std::atomic<std::uintptr_t> last_fault_ip_{0};
};

/// Wraps a `MEM_WRITE_WATCH` memory range with reusable scratch arrays and
/// deduplication to 64 KiB console block granularity.
class WriteWatchRegion {
public:
    /// Allocates and owns `size` bytes (rounded up to 64 KiB) with `MEM_WRITE_WATCH`,
    /// or attaches non-owning to an existing `MEM_WRITE_WATCH` range if `existing_base != 0`.
    WriteWatchRegion(std::size_t size, std::uintptr_t existing_base = 0);
    ~WriteWatchRegion();

    WriteWatchRegion(const WriteWatchRegion&) = delete;
    WriteWatchRegion& operator=(const WriteWatchRegion&) = delete;

    bool valid() const noexcept { return base_ != 0; }
    std::uintptr_t base() const noexcept { return base_; }
    std::size_t size() const noexcept { return size_; }
    std::size_t block_count() const noexcept { return block_count_; }

    /// Polls `GetWriteWatch` into reusable buffers, updates `dirty_blocks()`, and
    /// appends unique 64 KiB block indices modified since the prior reset.
    bool poll_dirty_64k(std::vector<std::size_t>& out_blocks, bool reset_watch = true);

    /// Resets OS write-watch tracking without scanning addresses.
    bool reset_watch() noexcept;

    DirtyBitset& dirty_blocks() noexcept { return dirty_blocks_; }
    const DirtyBitset& dirty_blocks() const noexcept { return dirty_blocks_; }

private:
    std::uintptr_t base_ = 0;
    std::size_t size_ = 0;
    std::size_t block_count_ = 0;
    bool owns_memory_ = false;
    std::vector<void*> scratch_addrs_;
    DirtyBitset dirty_blocks_;
};

/// Installs the global VEH handler if not already installed.
void ensure_veh_installed();

/// SEH-guarded memory copy: returns true if `bytes` could be read from `src`
/// into `dst` without an access violation, false if any page was unmapped/guarded.
bool safe_read_bytes(void* dst, const void* src, std::size_t bytes) noexcept;

/// Runs unit and concurrency tests for GuardedRegion, WriteWatchRegion, and SEH safe reads.
bool run_self_tests(std::string& report);

} // namespace victory::mem
