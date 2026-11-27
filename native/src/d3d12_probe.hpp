#pragma once

#include "victory_abi.h"

#include <string>

namespace victory::gpu {

/// Selects a high-performance D3D12 adapter (optionally matching `adapter_filter`),
/// queries SM 6.6, wave lanes, DXR, Mesh Shaders, GPU Upload Heap (Resizable BAR),
/// and tests `OpenExistingHeapFromAddress`, `OpenExistingHeapFromAddress1`, and
/// `OpenExistingHeapFromFileMapping` against real host driver behavior.
int32_t probe_gpu_caps(const char* adapter_filter, VictoryGpuCaps& out_caps, std::string& err);

} // namespace victory::gpu
