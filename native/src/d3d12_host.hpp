#pragma once

#include "victory_abi.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <mutex>
#include <string>

namespace victory::gpu {

/// Linear ring allocator backed by `D3D12_HEAP_TYPE_GPU_UPLOAD` (Resizable BAR)
/// when supported, falling back to `D3D12_HEAP_TYPE_UPLOAD`.
class GpuUploadRing {
public:
    GpuUploadRing() = default;
    ~GpuUploadRing();

    GpuUploadRing(const GpuUploadRing&) = delete;
    GpuUploadRing& operator=(const GpuUploadRing&) = delete;

    bool init(ID3D12Device* device, std::uint64_t capacity_bytes, bool prefer_gpu_upload, std::string& err);
    void shutdown();

    bool is_gpu_upload() const noexcept { return is_gpu_upload_; }
    std::uint64_t capacity() const noexcept { return capacity_; }
    ID3D12Resource* resource() const noexcept { return buffer_.Get(); }

    struct Allocation {
        std::uint8_t* cpu = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
        std::uint64_t offset = 0;
        std::uint64_t size = 0;
    };

    /// Allocates `bytes` aligned to `alignment` (power of two). Wraps around ring capacity.
    bool allocate(std::uint64_t bytes, std::uint64_t alignment, Allocation& out_alloc);

private:
    std::mutex mutex_;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer_;
    std::uint8_t* mapped_ = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpu_base_ = 0;
    std::uint64_t capacity_ = 0;
    std::uint64_t cursor_ = 0;
    bool is_gpu_upload_ = false;
};

/// Phase 3 C++20 D3D12 host runtime foundation: owns adapter, device, queues,
/// sync fence, ReBAR upload ring, and placed-memory bridge helpers.
class HostDevice {
public:
    HostDevice() = default;
    ~HostDevice();

    HostDevice(const HostDevice&) = delete;
    HostDevice& operator=(const HostDevice&) = delete;

    bool init(const char* adapter_filter, std::string& err);
    void shutdown();

    ID3D12Device* device() const noexcept { return device_.Get(); }
    ID3D12CommandQueue* direct_queue() const noexcept { return direct_queue_.Get(); }
    ID3D12CommandQueue* compute_queue() const noexcept { return compute_queue_.Get(); }
    ID3D12CommandQueue* copy_queue() const noexcept { return copy_queue_.Get(); }
    const VictoryGpuCaps& caps() const noexcept { return caps_; }
    GpuUploadRing& upload_ring() noexcept { return upload_ring_; }

    /// Synchronously executes a direct command list and waits on `fence_`.
    bool execute_and_wait(ID3D12GraphicsCommandList* list, std::string& err);

    /// Opens CPU memory as a D3D12 heap via `ID3D12Device13::OpenExistingHeapFromAddress1`.
    HRESULT open_heap_from_address1(void* ptr, std::uint64_t bytes, Microsoft::WRL::ComPtr<ID3D12Heap>& out_heap) const;

    /// Opens a `SEC_COMMIT` file mapping section as a D3D12 heap.
    HRESULT open_heap_from_section(HANDLE section, Microsoft::WRL::ComPtr<ID3D12Heap>& out_heap) const;

    /// Places a cross-adapter/UAV-capable buffer at `offset` inside `heap`.
    HRESULT create_placed_buffer(
        ID3D12Heap* heap,
        std::uint64_t offset,
        std::uint64_t bytes,
        D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES initial_state,
        Microsoft::WRL::ComPtr<ID3D12Resource>& out_buffer) const;

private:
    VictoryGpuCaps caps_{};
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory_;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12Device13> device13_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> direct_queue_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> compute_queue_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> copy_queue_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_ = nullptr;
    std::uint64_t next_fence_ = 1;
    std::uint64_t direct_ts_freq_ = 0;
    GpuUploadRing upload_ring_;
};

/// Benchmarks GPU copy and compute-read throughput across DEFAULT, GPU_UPLOAD (ReBAR),
/// UPLOAD (WC), OpenExistingHeapFromAddress1, and OpenExistingHeapFromFileMapping(SEC_COMMIT).
int32_t benchmark_memory_paths(
    const char* adapter_filter,
    uint32_t buffer_bytes,
    uint32_t iterations,
    VictoryMemBenchReport& out_report,
    std::string& err);

/// Runs HostDevice initialization + ReBAR upload ring + zero-copy heap self-tests.
bool run_host_self_tests(std::string& report);

} // namespace victory::gpu
