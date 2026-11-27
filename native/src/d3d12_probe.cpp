#include "d3d12_probe.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>
#include <string>

using Microsoft::WRL::ComPtr;

namespace victory::gpu {

namespace {

std::string wide_to_utf8(const wchar_t* w) {
    if (!w) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

bool contains_case_insensitive(const std::string& haystack, const char* needle) {
    if (!needle || !*needle) {
        return true;
    }
    std::string n(needle);
    auto lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::string h = haystack;
    std::transform(h.begin(), h.end(), h.begin(), lower);
    std::transform(n.begin(), n.end(), n.begin(), lower);
    return h.find(n) != std::string::npos;
}

HRESULT test_placed_buffer(ID3D12Device* dev, ID3D12Heap* heap, UINT64 size) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ComPtr<ID3D12Resource> res;
    return dev->CreatePlacedResource(heap, 0, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                     IID_PPV_ARGS(&res));
}

} // namespace

int32_t probe_gpu_caps(const char* adapter_filter, VictoryGpuCaps& out_caps, std::string& err) {
    std::memset(&out_caps, 0, sizeof(out_caps));
    out_caps.abi_version = VICTORY_ABI_VERSION;

    ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        err = "CreateDXGIFactory2 failed";
        return VICTORY_ERR_D3D12;
    }

    ComPtr<IDXGIAdapter1> chosen;
    DXGI_ADAPTER_DESC1 chosen_desc{};
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> cand;
        hr = factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                 IID_PPV_ARGS(&cand));
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr)) {
            continue;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(cand->GetDesc1(&desc))) {
            continue;
        }
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;
        }
        const std::string name = wide_to_utf8(desc.Description);
        if (!contains_case_insensitive(name, adapter_filter)) {
            continue;
        }
        chosen = cand;
        chosen_desc = desc;
        break;
    }

    if (!chosen) {
        err = "no matching hardware DXGI adapter found";
        return VICTORY_ERR_D3D12;
    }

    const std::string name_utf8 = wide_to_utf8(chosen_desc.Description);
    std::snprintf(out_caps.adapter_name, sizeof(out_caps.adapter_name), "%s", name_utf8.c_str());
    out_caps.vendor_id = chosen_desc.VendorId;
    out_caps.device_id = chosen_desc.DeviceId;
    out_caps.dedicated_vram_bytes = chosen_desc.DedicatedVideoMemory;
    out_caps.shared_sys_bytes = chosen_desc.SharedSystemMemory;

    ComPtr<ID3D12Device> device;
    hr = D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        std::ostringstream oss;
        oss << "D3D12CreateDevice failed (0x" << std::hex << static_cast<uint32_t>(hr) << ")";
        err = oss.str();
        return VICTORY_ERR_D3D12;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS opts{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opts, sizeof(opts)))) {
        out_caps.resource_heap_tier = static_cast<uint32_t>(opts.ResourceHeapTier);
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS1 opts1{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &opts1, sizeof(opts1)))) {
        out_caps.wave_lane_min = opts1.WaveLaneCountMin;
        out_caps.wave_lane_max = opts1.WaveLaneCountMax;
        out_caps.wave_lane_total = opts1.TotalLaneCount;
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5)))) {
        out_caps.raytracing_tier = static_cast<uint32_t>(opts5.RaytracingTier);
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS7 opts7{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &opts7, sizeof(opts7)))) {
        out_caps.mesh_shader_tier = static_cast<uint32_t>(opts7.MeshShaderTier);
    }

    D3D12_FEATURE_DATA_D3D12_OPTIONS16 opts16{};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &opts16, sizeof(opts16)))) {
        out_caps.gpu_upload_heap_supported = opts16.GPUUploadHeapSupported ? 1 : 0;
    }

    D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_7};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
        out_caps.shader_model = static_cast<uint32_t>(sm.HighestShaderModel);
    }

    // Test SM 6.6 directly-indexed root signature acceptance on host runtime.
    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                    D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
    ComPtr<ID3DBlob> sig_blob;
    ComPtr<ID3DBlob> err_blob;
    if (SUCCEEDED(D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1_0,
                                              &sig_blob, &err_blob))) {
        ComPtr<ID3D12RootSignature> rs;
        if (SUCCEEDED(device->CreateRootSignature(0, sig_blob->GetBufferPointer(),
                                                  sig_blob->GetBufferSize(), IID_PPV_ARGS(&rs)))) {
            out_caps.sm66_directly_indexed = 1;
        }
    }

    // Probe OpenExistingHeapFromAddress / OpenExistingHeapFromAddress1 / OpenExistingHeapFromFileMapping
    constexpr SIZE_T kTestSize = 64u << 10;
    void* mem_whole = VirtualAlloc(nullptr, kTestSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (mem_whole) {
        ComPtr<ID3D12Device3> dev3;
        if (SUCCEEDED(device.As(&dev3))) {
            ComPtr<ID3D12Heap> heap;
            HRESULT hr_open = dev3->OpenExistingHeapFromAddress(mem_whole, IID_PPV_ARGS(&heap));
            if (SUCCEEDED(hr_open)) {
                hr_open = test_placed_buffer(device.Get(), heap.Get(), kTestSize);
            }
            out_caps.hr_open_addr = static_cast<int32_t>(hr_open);
            out_caps.open_existing_heap_addr = SUCCEEDED(hr_open) ? 1 : 0;
        } else {
            out_caps.hr_open_addr = E_NOINTERFACE;
        }
        VirtualFree(mem_whole, 0, MEM_RELEASE);
    }

    void* mem_part = VirtualAlloc(nullptr, kTestSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (mem_part) {
        ComPtr<ID3D12Device13> dev13;
        if (SUCCEEDED(device.As(&dev13))) {
            ComPtr<ID3D12Heap> heap;
            void* slice = static_cast<uint8_t*>(mem_part) + kTestSize;
            HRESULT hr_open = dev13->OpenExistingHeapFromAddress1(slice, kTestSize, IID_PPV_ARGS(&heap));
            if (SUCCEEDED(hr_open)) {
                hr_open = test_placed_buffer(device.Get(), heap.Get(), kTestSize);
            }
            out_caps.hr_open_addr1 = static_cast<int32_t>(hr_open);
            out_caps.open_existing_heap_addr1 = SUCCEEDED(hr_open) ? 1 : 0;
        } else {
            out_caps.hr_open_addr1 = E_NOINTERFACE;
        }
        VirtualFree(mem_part, 0, MEM_RELEASE);
    }

    ComPtr<ID3D12Device3> dev3;
    if (SUCCEEDED(device.As(&dev3))) {
        HANDLE sec_commit = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                               PAGE_READWRITE | SEC_COMMIT, 0,
                                               static_cast<DWORD>(kTestSize), nullptr);
        if (sec_commit) {
            ComPtr<ID3D12Heap> heap;
            HRESULT hr_open = dev3->OpenExistingHeapFromFileMapping(sec_commit, IID_PPV_ARGS(&heap));
            if (SUCCEEDED(hr_open)) {
                hr_open = test_placed_buffer(device.Get(), heap.Get(), kTestSize);
            }
            out_caps.hr_open_section_commit = static_cast<int32_t>(hr_open);
            out_caps.open_existing_heap_section_commit = SUCCEEDED(hr_open) ? 1 : 0;
            CloseHandle(sec_commit);
        }

        HANDLE sec_reserve = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                                                PAGE_READWRITE | SEC_RESERVE, 0,
                                                static_cast<DWORD>(kTestSize), nullptr);
        if (sec_reserve) {
            void* view = MapViewOfFile(sec_reserve, FILE_MAP_WRITE, 0, 0, kTestSize);
            if (view) {
                VirtualAlloc(view, kTestSize, MEM_COMMIT, PAGE_READWRITE);
                UnmapViewOfFile(view);
            }
            ComPtr<ID3D12Heap> heap;
            HRESULT hr_open = dev3->OpenExistingHeapFromFileMapping(sec_reserve, IID_PPV_ARGS(&heap));
            if (SUCCEEDED(hr_open)) {
                hr_open = test_placed_buffer(device.Get(), heap.Get(), kTestSize);
            }
            out_caps.hr_open_section_reserve = static_cast<int32_t>(hr_open);
            out_caps.open_existing_heap_section_reserve = SUCCEEDED(hr_open) ? 1 : 0;
            CloseHandle(sec_reserve);
        }
    }

    return VICTORY_OK;
}

} // namespace victory::gpu
