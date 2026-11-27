#include "d3d12_host.hpp"
#include "d3d12_probe.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace victory::gpu {

namespace {

std::string fmt_hr(const char* what, HRESULT hr) {
    std::ostringstream oss;
    oss << what << " (0x" << std::hex << static_cast<std::uint32_t>(hr) << ")";
    return oss.str();
}

constexpr const char* kReadBenchHlsl = R"HLSL(
ByteAddressBuffer Src : register(t0);
RWByteAddressBuffer Dst : register(u0);
cbuffer Params : register(b0) { uint NumVec4; };
groupshared uint gSum[256];
[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID) {
    uint acc = 0;
    const uint totalThreads = 256u * 256u;
    for (uint i = tid.x; i < NumVec4; i += totalThreads) {
        uint4 v = Src.Load4(i * 16u);
        acc += (v.x ^ v.y ^ v.z ^ v.w);
    }
    gSum[gtid.x] = acc;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 128u; s > 0u; s >>= 1u) {
        if (gtid.x < s) {
            gSum[gtid.x] += gSum[gtid.x + s];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (gtid.x == 0u) {
        Dst.InterlockedAdd(0, gSum[0]);
    }
}
)HLSL";

D3D12_RESOURCE_DESC make_buffer_desc(std::uint64_t bytes, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Alignment = 0;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
    d.SampleDesc.Quality = 0;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    return d;
}

D3D12_RESOURCE_BARRIER transition_barrier(
    ID3D12Resource* res,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

void fill_pattern(std::uint32_t* words, std::size_t word_count, std::uint32_t& out_checksum) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < word_count; i += 4) {
        const std::uint32_t w0 = static_cast<std::uint32_t>(i * 0x9E37'79B1u + 1u);
        const std::uint32_t w1 = static_cast<std::uint32_t>(i * 0x85EB'CA77u + 3u);
        const std::uint32_t w2 = static_cast<std::uint32_t>(i * 0xC2B2'AE3Du + 7u);
        const std::uint32_t w3 = static_cast<std::uint32_t>(i ^ 0xA5A5'5A5Au);
        words[i + 0] = w0;
        words[i + 1] = w1;
        words[i + 2] = w2;
        words[i + 3] = w3;
        sum += (w0 ^ w1 ^ w2 ^ w3);
    }
    out_checksum = sum;
}

} // namespace

GpuUploadRing::~GpuUploadRing() {
    shutdown();
}

bool GpuUploadRing::init(
    ID3D12Device* device,
    std::uint64_t capacity_bytes,
    bool prefer_gpu_upload,
    std::string& err) {
    shutdown();
    if (!device || capacity_bytes == 0) {
        err = "invalid device or capacity";
        return false;
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = prefer_gpu_upload ? D3D12_HEAP_TYPE_GPU_UPLOAD : D3D12_HEAP_TYPE_UPLOAD;
    const auto desc = make_buffer_desc(capacity_bytes, D3D12_RESOURCE_FLAG_NONE);
    const D3D12_RESOURCE_STATES init_state =
        prefer_gpu_upload ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_GENERIC_READ;

    HRESULT hr = device->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE, &desc, init_state, nullptr, IID_PPV_ARGS(&buffer_));
    if (FAILED(hr) && prefer_gpu_upload) {
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        hr = device->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&buffer_));
        is_gpu_upload_ = false;
    } else {
        is_gpu_upload_ = prefer_gpu_upload && SUCCEEDED(hr);
    }

    if (FAILED(hr)) {
        err = fmt_hr("GpuUploadRing CreateCommittedResource failed", hr);
        return false;
    }

    D3D12_RANGE read_none{0, 0};
    void* ptr = nullptr;
    hr = buffer_->Map(0, &read_none, &ptr);
    if (FAILED(hr) || !ptr) {
        buffer_.Reset();
        err = fmt_hr("GpuUploadRing Map failed", hr);
        return false;
    }

    mapped_ = static_cast<std::uint8_t*>(ptr);
    gpu_base_ = buffer_->GetGPUVirtualAddress();
    capacity_ = capacity_bytes;
    cursor_ = 0;
    return true;
}

void GpuUploadRing::shutdown() {
    std::lock_guard lock(mutex_);
    if (buffer_ && mapped_) {
        D3D12_RANGE written{0, 0};
        buffer_->Unmap(0, &written);
    }
    mapped_ = nullptr;
    gpu_base_ = 0;
    capacity_ = 0;
    cursor_ = 0;
    is_gpu_upload_ = false;
    buffer_.Reset();
}

bool GpuUploadRing::allocate(std::uint64_t bytes, std::uint64_t alignment, Allocation& out_alloc) {
    if (!mapped_ || bytes == 0 || bytes > capacity_ || alignment == 0) {
        return false;
    }
    std::lock_guard lock(mutex_);
    const std::uint64_t mask = alignment - 1;
    std::uint64_t offset = (cursor_ + mask) & ~mask;
    if (offset + bytes > capacity_) {
        offset = 0;
    }
    cursor_ = offset + bytes;
    out_alloc.cpu = mapped_ + offset;
    out_alloc.gpu = gpu_base_ + offset;
    out_alloc.offset = offset;
    out_alloc.size = bytes;
    return true;
}

HostDevice::~HostDevice() {
    shutdown();
}

bool HostDevice::init(const char* adapter_filter, std::string& err) {
    shutdown();

    const int32_t cap_rc = probe_gpu_caps(adapter_filter, caps_, err);
    if (cap_rc != VICTORY_OK) {
        return false;
    }

    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_));
    if (FAILED(hr)) {
        err = fmt_hr("CreateDXGIFactory2 failed", hr);
        return false;
    }

    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> cand;
        hr = factory_->EnumAdapterByGpuPreference(
            i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&cand));
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr)) {
            continue;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(cand->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            continue;
        }
        if (desc.VendorId == caps_.vendor_id && desc.DeviceId == caps_.device_id) {
            cand.As(&adapter_);
            hr = D3D12CreateDevice(cand.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_));
            break;
        }
    }

    if (!device_) {
        err = "failed to create ID3D12Device for probed adapter";
        return false;
    }
    device_.As(&device13_);

    auto make_queue = [&](D3D12_COMMAND_LIST_TYPE type, ComPtr<ID3D12CommandQueue>& out) -> bool {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = type;
        qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        const HRESULT qhr = device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&out));
        if (FAILED(qhr)) {
            err = fmt_hr("CreateCommandQueue failed", qhr);
            return false;
        }
        return true;
    };

    if (!make_queue(D3D12_COMMAND_LIST_TYPE_DIRECT, direct_queue_) ||
        !make_queue(D3D12_COMMAND_LIST_TYPE_COMPUTE, compute_queue_) ||
        !make_queue(D3D12_COMMAND_LIST_TYPE_COPY, copy_queue_)) {
        shutdown();
        return false;
    }

    hr = direct_queue_->GetTimestampFrequency(&direct_ts_freq_);
    if (FAILED(hr) || direct_ts_freq_ == 0) {
        err = fmt_hr("GetTimestampFrequency failed", hr);
        shutdown();
        return false;
    }

    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (FAILED(hr)) {
        err = fmt_hr("CreateFence failed", hr);
        shutdown();
        return false;
    }
    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fence_event_) {
        err = "CreateEventW failed";
        shutdown();
        return false;
    }

    if (!upload_ring_.init(device_.Get(), 16ULL << 20, caps_.gpu_upload_heap_supported != 0, err)) {
        shutdown();
        return false;
    }

    return true;
}

void HostDevice::shutdown() {
    upload_ring_.shutdown();
    if (fence_event_) {
        CloseHandle(fence_event_);
        fence_event_ = nullptr;
    }
    fence_.Reset();
    copy_queue_.Reset();
    compute_queue_.Reset();
    direct_queue_.Reset();
    device13_.Reset();
    device_.Reset();
    adapter_.Reset();
    factory_.Reset();
    next_fence_ = 1;
    direct_ts_freq_ = 0;
}

bool HostDevice::execute_and_wait(ID3D12GraphicsCommandList* list, std::string& err) {
    if (!direct_queue_ || !fence_ || !fence_event_ || !list) {
        err = "HostDevice not initialized";
        return false;
    }
    ID3D12CommandList* lists[] = {list};
    direct_queue_->ExecuteCommandLists(1, lists);
    const std::uint64_t val = next_fence_++;
    HRESULT hr = direct_queue_->Signal(fence_.Get(), val);
    if (FAILED(hr)) {
        err = fmt_hr("CommandQueue Signal failed", hr);
        return false;
    }
    if (fence_->GetCompletedValue() < val) {
        hr = fence_->SetEventOnCompletion(val, fence_event_);
        if (FAILED(hr)) {
            err = fmt_hr("Fence SetEventOnCompletion failed", hr);
            return false;
        }
        WaitForSingleObject(fence_event_, INFINITE);
    }
    return true;
}

HRESULT HostDevice::open_heap_from_address1(
    void* ptr,
    std::uint64_t bytes,
    ComPtr<ID3D12Heap>& out_heap) const {
    if (!device13_) {
        return E_NOINTERFACE;
    }
    return device13_->OpenExistingHeapFromAddress1(ptr, static_cast<SIZE_T>(bytes), IID_PPV_ARGS(&out_heap));
}

HRESULT HostDevice::open_heap_from_section(
    HANDLE section,
    ComPtr<ID3D12Heap>& out_heap) const {
    ComPtr<ID3D12Device3> dev3;
    HRESULT hr = device_.As(&dev3);
    if (FAILED(hr)) {
        return hr;
    }
    return dev3->OpenExistingHeapFromFileMapping(section, IID_PPV_ARGS(&out_heap));
}

HRESULT HostDevice::create_placed_buffer(
    ID3D12Heap* heap,
    std::uint64_t offset,
    std::uint64_t bytes,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_RESOURCE_STATES initial_state,
    ComPtr<ID3D12Resource>& out_buffer) const {
    if (!device_ || !heap) {
        return E_INVALIDARG;
    }
    const auto desc = make_buffer_desc(bytes, flags);
    return device_->CreatePlacedResource(
        heap, offset, &desc, initial_state, nullptr, IID_PPV_ARGS(&out_buffer));
}

int32_t benchmark_memory_paths(
    const char* adapter_filter,
    uint32_t buffer_bytes,
    uint32_t iterations,
    VictoryMemBenchReport& out_report,
    std::string& err) {
    std::memset(&out_report, 0, sizeof(out_report));
    out_report.abi_version = VICTORY_ABI_VERSION;
    if (buffer_bytes == 0) {
        buffer_bytes = 16u << 20;
    }
    buffer_bytes = (buffer_bytes + 65535u) & ~65535u;
    if (iterations == 0) {
        iterations = 16;
    }
    out_report.buffer_bytes = buffer_bytes;
    out_report.iterations = iterations;

    HostDevice host;
    if (!host.init(adapter_filter, err)) {
        return VICTORY_ERR_D3D12;
    }

    // Compile the read-checksum compute shader via D3DCompile (pure text HLSL).
    ComPtr<ID3DBlob> cs_blob;
    ComPtr<ID3DBlob> err_blob;
    HRESULT hr = D3DCompile(
        kReadBenchHlsl, std::strlen(kReadBenchHlsl), "mem_bench",
        nullptr, nullptr, "main", "cs_5_1", 0, 0, &cs_blob, &err_blob);
    if (FAILED(hr)) {
        err = err_blob ? std::string(static_cast<const char*>(err_blob->GetBufferPointer()),
                                     err_blob->GetBufferSize())
                       : fmt_hr("D3DCompile failed", hr);
        return VICTORY_ERR_D3D12;
    }

    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 0;
    params[2].Constants.Num32BitValues = 1;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = 3;
    rs_desc.pParameters = params;
    ComPtr<ID3DBlob> sig_blob;
    hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1_0, &sig_blob, &err_blob);
    if (FAILED(hr)) {
        err = fmt_hr("D3D12SerializeRootSignature failed", hr);
        return VICTORY_ERR_D3D12;
    }
    ComPtr<ID3D12RootSignature> rs;
    hr = host.device()->CreateRootSignature(
        0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(), IID_PPV_ARGS(&rs));
    if (FAILED(hr)) {
        err = fmt_hr("CreateRootSignature failed", hr);
        return VICTORY_ERR_D3D12;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
    pso_desc.pRootSignature = rs.Get();
    pso_desc.CS = {cs_blob->GetBufferPointer(), cs_blob->GetBufferSize()};
    ComPtr<ID3D12PipelineState> pso;
    hr = host.device()->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso));
    if (FAILED(hr)) {
        err = fmt_hr("CreateComputePipelineState failed", hr);
        return VICTORY_ERR_D3D12;
    }

    // Destination copy buffer (DEFAULT), UAV checksum buffer (DEFAULT), zero src (UPLOAD),
    // checksum readback (READBACK), and timestamp query heap + readback.
    auto create_committed = [&](D3D12_HEAP_TYPE heap_type, std::uint64_t bytes,
                                D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                                ComPtr<ID3D12Resource>& out) -> HRESULT {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = heap_type;
        const auto d = make_buffer_desc(bytes, flags);
        return host.device()->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&out));
    };

    ComPtr<ID3D12Resource> copy_dst;
    ComPtr<ID3D12Resource> uav_out;
    ComPtr<ID3D12Resource> zero_src;
    ComPtr<ID3D12Resource> sum_readback;
    ComPtr<ID3D12Resource> ts_readback;
    if (FAILED(hr = create_committed(D3D12_HEAP_TYPE_DEFAULT, buffer_bytes,
                                     D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, copy_dst)) ||
        FAILED(hr = create_committed(D3D12_HEAP_TYPE_DEFAULT, 256,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_STATE_COMMON, uav_out)) ||
        FAILED(hr = create_committed(D3D12_HEAP_TYPE_UPLOAD, 256,
                                     D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, zero_src)) ||
        FAILED(hr = create_committed(D3D12_HEAP_TYPE_READBACK, 256,
                                     D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, sum_readback)) ||
        FAILED(hr = create_committed(D3D12_HEAP_TYPE_READBACK, 4 * sizeof(std::uint64_t),
                                     D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, ts_readback))) {
        err = fmt_hr("CreateCommittedResource helper buffers failed", hr);
        return VICTORY_ERR_D3D12;
    }

    {
        void* p = nullptr;
        D3D12_RANGE r{0, 0};
        if (SUCCEEDED(zero_src->Map(0, &r, &p)) && p) {
            std::memset(p, 0, 256);
            zero_src->Unmap(0, nullptr);
        }
    }

    D3D12_QUERY_HEAP_DESC qhd{};
    qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qhd.Count = 4;
    ComPtr<ID3D12QueryHeap> qh;
    hr = host.device()->CreateQueryHeap(&qhd, IID_PPV_ARGS(&qh));
    if (FAILED(hr)) {
        err = fmt_hr("CreateQueryHeap failed", hr);
        return VICTORY_ERR_D3D12;
    }

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    if (FAILED(hr = host.device()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(hr = host.device()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(),
                                                     nullptr, IID_PPV_ARGS(&list)))) {
        err = fmt_hr("CommandList creation failed", hr);
        return VICTORY_ERR_D3D12;
    }
    list->Close();

    std::uint64_t ts_freq = 0;
    host.direct_queue()->GetTimestampFrequency(&ts_freq);
    const std::uint32_t num_vec4 = buffer_bytes / 16u;

    auto run_case = [&](ID3D12Resource* src,
                        D3D12_RESOURCE_STATES copy_state,
                        D3D12_RESOURCE_STATES srv_state,
                        bool need_copy_barrier,
                        std::uint32_t expected_sum,
                        VictoryMemBenchEntry& entry) {
        entry.supported = 1;
        entry.hr = S_OK;

        auto exec_once = [&](std::uint32_t loops, bool do_copy, bool do_compute,
                             double& out_ms, std::uint32_t& out_sum) -> bool {
            alloc->Reset();
            list->Reset(alloc.Get(), nullptr);

            // Reset UAV checksum buffer to 0.
            auto b_uav_dst = transition_barrier(uav_out.Get(), D3D12_RESOURCE_STATE_COMMON,
                                                D3D12_RESOURCE_STATE_COPY_DEST);
            list->ResourceBarrier(1, &b_uav_dst);
            list->CopyBufferRegion(uav_out.Get(), 0, zero_src.Get(), 0, 256);
            auto b_uav_rw = transition_barrier(uav_out.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &b_uav_rw);

            if (do_copy) {
                if (need_copy_barrier) {
                    D3D12_RESOURCE_BARRIER bs[2] = {
                        transition_barrier(src, D3D12_RESOURCE_STATE_COMMON, copy_state),
                        transition_barrier(copy_dst.Get(), D3D12_RESOURCE_STATE_COMMON,
                                           D3D12_RESOURCE_STATE_COPY_DEST),
                    };
                    list->ResourceBarrier(2, bs);
                } else {
                    auto b = transition_barrier(copy_dst.Get(), D3D12_RESOURCE_STATE_COMMON,
                                                D3D12_RESOURCE_STATE_COPY_DEST);
                    list->ResourceBarrier(1, &b);
                }

                list->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
                for (std::uint32_t it = 0; it < loops; ++it) {
                    list->CopyBufferRegion(copy_dst.Get(), 0, src, 0, buffer_bytes);
                }
                list->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);

                if (need_copy_barrier) {
                    D3D12_RESOURCE_BARRIER bs[2] = {
                        transition_barrier(src, copy_state, D3D12_RESOURCE_STATE_COMMON),
                        transition_barrier(copy_dst.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                           D3D12_RESOURCE_STATE_COMMON),
                    };
                    list->ResourceBarrier(2, bs);
                } else {
                    auto b = transition_barrier(copy_dst.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                D3D12_RESOURCE_STATE_COMMON);
                    list->ResourceBarrier(1, &b);
                }
            }

            if (do_compute) {
                if (need_copy_barrier) {
                    auto b = transition_barrier(src, D3D12_RESOURCE_STATE_COMMON, srv_state);
                    list->ResourceBarrier(1, &b);
                }
                list->SetComputeRootSignature(rs.Get());
                list->SetPipelineState(pso.Get());
                list->SetComputeRootShaderResourceView(0, src->GetGPUVirtualAddress());
                list->SetComputeRootUnorderedAccessView(1, uav_out->GetGPUVirtualAddress());
                list->SetComputeRoot32BitConstant(2, num_vec4, 0);

                list->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
                for (std::uint32_t it = 0; it < loops; ++it) {
                    list->Dispatch(256, 1, 1);
                }
                list->EndQuery(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);

                if (need_copy_barrier) {
                    auto b = transition_barrier(src, srv_state, D3D12_RESOURCE_STATE_COMMON);
                    list->ResourceBarrier(1, &b);
                }
            }

            auto b_uav_src = transition_barrier(uav_out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &b_uav_src);
            list->CopyBufferRegion(sum_readback.Get(), 0, uav_out.Get(), 0, 256);
            auto b_uav_common = transition_barrier(uav_out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                   D3D12_RESOURCE_STATE_COMMON);
            list->ResourceBarrier(1, &b_uav_common);

            list->ResolveQueryData(qh.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 4, ts_readback.Get(), 0);
            list->Close();

            if (!host.execute_and_wait(list.Get(), err)) {
                return false;
            }

            std::uint64_t ts[4]{};
            void* tp = nullptr;
            D3D12_RANGE rr{0, sizeof(ts)};
            if (SUCCEEDED(ts_readback->Map(0, &rr, &tp)) && tp) {
                std::memcpy(ts, tp, sizeof(ts));
                D3D12_RANGE rw{0, 0};
                ts_readback->Unmap(0, &rw);
            }
            const std::uint64_t delta = do_copy ? (ts[1] - ts[0]) : (ts[3] - ts[2]);
            out_ms = (static_cast<double>(delta) * 1000.0) / static_cast<double>(ts_freq) /
                     static_cast<double>(loops);

            void* sp = nullptr;
            D3D12_RANGE sr{0, sizeof(std::uint32_t)};
            if (SUCCEEDED(sum_readback->Map(0, &sr, &sp)) && sp) {
                std::memcpy(&out_sum, sp, sizeof(std::uint32_t));
                D3D12_RANGE rw{0, 0};
                sum_readback->Unmap(0, &rw);
            }
            return true;
        };

        // Warmup 1 iteration (copy + compute checksum).
        double dummy_ms = 0.0;
        std::uint32_t got_sum = 0;
        if (!exec_once(1, true, true, dummy_ms, got_sum)) {
            entry.hr = E_FAIL;
            return;
        }
        entry.verified = (got_sum == expected_sum) ? 1 : 0;

        double copy_ms = 0.0;
        double compute_ms = 0.0;
        std::uint32_t unused_sum = 0;
        if (!exec_once(iterations, true, false, copy_ms, unused_sum) ||
            !exec_once(iterations, false, true, compute_ms, unused_sum)) {
            entry.hr = E_FAIL;
            return;
        }

        const double gb = static_cast<double>(buffer_bytes) / (1024.0 * 1024.0 * 1024.0);
        entry.copy_ms = copy_ms;
        entry.compute_read_ms = compute_ms;
        entry.copy_gbps = (copy_ms > 0.0) ? (gb / (copy_ms / 1000.0)) : 0.0;
        entry.compute_read_gbps = (compute_ms > 0.0) ? (gb / (compute_ms / 1000.0)) : 0.0;
    };

    std::uint32_t expected_sum = 0;

    // Prepare UPLOAD buffer first (used both as source for UPLOAD test and staging for DEFAULT).
    ComPtr<ID3D12Resource> upload_buf;
    hr = create_committed(D3D12_HEAP_TYPE_UPLOAD, buffer_bytes,
                          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, upload_buf);
    if (SUCCEEDED(hr)) {
        void* p = nullptr;
        D3D12_RANGE r{0, 0};
        if (SUCCEEDED(upload_buf->Map(0, &r, &p)) && p) {
            fill_pattern(static_cast<std::uint32_t*>(p), buffer_bytes / sizeof(std::uint32_t), expected_sum);
            upload_buf->Unmap(0, nullptr);
        }
    }

    // 1. DEFAULT VRAM baseline.
    ComPtr<ID3D12Resource> default_buf;
    hr = create_committed(D3D12_HEAP_TYPE_DEFAULT, buffer_bytes,
                          D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, default_buf);
    if (SUCCEEDED(hr) && upload_buf) {
        alloc->Reset();
        list->Reset(alloc.Get(), nullptr);
        auto b1 = transition_barrier(default_buf.Get(), D3D12_RESOURCE_STATE_COMMON,
                                     D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &b1);
        list->CopyBufferRegion(default_buf.Get(), 0, upload_buf.Get(), 0, buffer_bytes);
        auto b2 = transition_barrier(default_buf.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                     D3D12_RESOURCE_STATE_COMMON);
        list->ResourceBarrier(1, &b2);
        list->Close();
        if (host.execute_and_wait(list.Get(), err)) {
            run_case(default_buf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true, expected_sum,
                     out_report.default_vram);
        }
    } else {
        out_report.default_vram.hr = hr;
    }

    // 2. GPU_UPLOAD (Resizable BAR CPU-writable VRAM).
    if (host.caps().gpu_upload_heap_supported) {
        ComPtr<ID3D12Resource> gpu_upload_buf;
        hr = create_committed(D3D12_HEAP_TYPE_GPU_UPLOAD, buffer_bytes,
                              D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, gpu_upload_buf);
        if (SUCCEEDED(hr)) {
            void* p = nullptr;
            D3D12_RANGE r{0, 0};
            if (SUCCEEDED(gpu_upload_buf->Map(0, &r, &p)) && p) {
                std::uint32_t unused = 0;
                fill_pattern(static_cast<std::uint32_t*>(p), buffer_bytes / sizeof(std::uint32_t), unused);
                gpu_upload_buf->Unmap(0, nullptr);
            }
            run_case(gpu_upload_buf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true, expected_sum,
                     out_report.gpu_upload_rebar);
        } else {
            out_report.gpu_upload_rebar.hr = hr;
        }
    }

    // 3. UPLOAD (PCIe write-combined system RAM).
    if (upload_buf) {
        run_case(upload_buf.Get(), D3D12_RESOURCE_STATE_GENERIC_READ,
                 D3D12_RESOURCE_STATE_GENERIC_READ, false, expected_sum,
                 out_report.upload_wc);
    }

    // 4. OpenExistingHeapFromAddress1 (write-back VirtualAlloc memory directly read by GPU).
    if (host.caps().open_existing_heap_addr1) {
        void* wb_mem = VirtualAlloc(nullptr, buffer_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (wb_mem) {
            std::uint32_t unused = 0;
            fill_pattern(static_cast<std::uint32_t*>(wb_mem), buffer_bytes / sizeof(std::uint32_t), unused);
            ComPtr<ID3D12Heap> wb_heap;
            hr = host.open_heap_from_address1(wb_mem, buffer_bytes, wb_heap);
            if (SUCCEEDED(hr)) {
                ComPtr<ID3D12Resource> wb_buf;
                hr = host.create_placed_buffer(
                    wb_heap.Get(), 0, buffer_bytes,
                    D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COMMON, wb_buf);
                if (SUCCEEDED(hr)) {
                    run_case(wb_buf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true, expected_sum,
                             out_report.open_addr1_sysram);
                } else {
                    out_report.open_addr1_sysram.hr = hr;
                }
            } else {
                out_report.open_addr1_sysram.hr = hr;
            }
            VirtualFree(wb_mem, 0, MEM_RELEASE);
        }
    }

    // 5. OpenExistingHeapFromFileMapping(SEC_COMMIT) (section-backed memory directly read by GPU).
    if (host.caps().open_existing_heap_section_commit) {
        HANDLE sec = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0, buffer_bytes, nullptr);
        if (sec) {
            void* view = MapViewOfFile(sec, FILE_MAP_WRITE, 0, 0, buffer_bytes);
            if (view) {
                std::uint32_t unused = 0;
                fill_pattern(static_cast<std::uint32_t*>(view), buffer_bytes / sizeof(std::uint32_t), unused);
                UnmapViewOfFile(view);
            }
            ComPtr<ID3D12Heap> sec_heap;
            hr = host.open_heap_from_section(sec, sec_heap);
            if (SUCCEEDED(hr)) {
                ComPtr<ID3D12Resource> sec_buf;
                hr = host.create_placed_buffer(
                    sec_heap.Get(), 0, buffer_bytes,
                    D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_COMMON, sec_buf);
                if (SUCCEEDED(hr)) {
                    run_case(sec_buf.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, true, expected_sum,
                             out_report.open_section_sysram);
                } else {
                    out_report.open_section_sysram.hr = hr;
                }
            } else {
                out_report.open_section_sysram.hr = hr;
            }
            CloseHandle(sec);
        }
    }

    return VICTORY_OK;
}

bool run_host_self_tests(std::string& report) {
    HostDevice host;
    std::string err;
    if (!host.init(nullptr, err)) {
        report += "d3d12_host: init failed: " + err + "\n";
        return false;
    }

    GpuUploadRing::Allocation a1{};
    GpuUploadRing::Allocation a2{};
    if (!host.upload_ring().allocate(1024, 256, a1) ||
        !host.upload_ring().allocate(4096, 256, a2) ||
        (a1.gpu & 255) != 0 || (a2.gpu & 255) != 0) {
        report += "d3d12_host: GpuUploadRing alignment/allocation failed\n";
        return false;
    }
    std::memset(a1.cpu, 0x5A, 1024);

    std::ostringstream oss;
    oss << "d3d12_host: PASS (ReBAR gpu_upload=" << (host.upload_ring().is_gpu_upload() ? 1 : 0)
        << ", Direct/Compute/Copy queues + fence ok)\n";
    report += oss.str();
    return true;
}

} // namespace victory::gpu
