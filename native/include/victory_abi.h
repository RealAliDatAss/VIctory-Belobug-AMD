#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  if defined(VICTORY_NATIVE_EXPORTS)
#    define VICTORY_API __declspec(dllexport)
#  else
#    define VICTORY_API
#  endif
#else
#  define VICTORY_API
#endif

enum { VICTORY_ABI_VERSION = 1 };

typedef enum VictoryStatus {
    VICTORY_OK = 0,
    VICTORY_ERR_INVALID_ARG = -1,
    VICTORY_ERR_ABI_MISMATCH = -2,
    VICTORY_ERR_TRANSLATE = -3,
    VICTORY_ERR_UNSUPPORTED = -4,
    VICTORY_ERR_D3D12 = -5,
    VICTORY_ERR_OS = -6,
} VictoryStatus;

typedef enum VictoryHwStage {
    VICTORY_STAGE_LS = 0,
    VICTORY_STAGE_HS = 1,
    VICTORY_STAGE_ES = 2,
    VICTORY_STAGE_GS = 3,
    VICTORY_STAGE_VS = 4,
    VICTORY_STAGE_PS = 5,
    VICTORY_STAGE_MS = 6,
    VICTORY_STAGE_CS = 7,
} VictoryHwStage;

typedef enum VictoryRootType {
    VICTORY_ROOT_CONSTANTS = 0,
    VICTORY_ROOT_DESCRIPTOR = 1,
    VICTORY_ROOT_TABLE = 2,
    VICTORY_ROOT_IMMEDIATE_SAMPLER = 3,
    VICTORY_ROOT_RESOURCE_HEAP = 4,
    VICTORY_ROOT_SAMPLER_HEAP = 5,
    VICTORY_ROOT_OTHER = 6,
} VictoryRootType;

typedef struct VictoryUserData {
    uint32_t index;
    uint32_t dword;
    uint32_t other_code;
    uint8_t slot;
    uint8_t root_type;
    uint8_t reserved[2];
} VictoryUserData;

typedef struct VictoryStaticSampler {
    uint32_t filter;
    uint32_t address[3];
    float mip_lod_bias;
    uint32_t max_anisotropy;
    uint32_t comparison;
    uint32_t border;
    float min_lod;
    float max_lod;
} VictoryStaticSampler;

typedef struct VictoryRootIndexedView {
    uint32_t constant_dword;
    uint32_t byte_offset;
    uint8_t table_slot;
    uint8_t constant_slot;
    uint8_t shift;
    uint8_t reserved;
} VictoryRootIndexedView;

typedef struct VictoryTranslatedShader {
    uint32_t abi_version;
    uint32_t translator_version;
    uint8_t stage;
    uint8_t reserved[3];
    uint32_t user_data_dwords;
    const uint8_t* dxil_ptr;
    size_t dxil_len;
    const VictoryUserData* user_data_ptr;
    size_t user_data_len;
    const uint32_t* samplers_words_ptr; /* 4 uint32_t per sampler */
    const VictoryStaticSampler* static_samplers_ptr;
    size_t samplers_len;
    const uint8_t* root_cbvs_ptr;
    size_t root_cbvs_len;
    const uint8_t* root_srvs_ptr;
    size_t root_srvs_len;
    const uint8_t* root_uavs_ptr;
    size_t root_uavs_len;
    const VictoryRootIndexedView* root_indexed_views_ptr;
    size_t root_indexed_views_len;
    void* opaque_owner;
} VictoryTranslatedShader;

typedef struct VictoryGpuCaps {
    uint32_t abi_version;
    uint32_t vendor_id;
    uint32_t device_id;
    uint64_t dedicated_vram_bytes;
    uint64_t shared_sys_bytes;
    uint32_t wave_lane_min;
    uint32_t wave_lane_max;
    uint32_t wave_lane_total;
    uint32_t resource_heap_tier;
    uint32_t raytracing_tier;
    uint32_t mesh_shader_tier;
    uint32_t shader_model;
    uint8_t gpu_upload_heap_supported;
    uint8_t sm66_directly_indexed;
    uint8_t open_existing_heap_addr;
    uint8_t open_existing_heap_addr1;
    uint8_t open_existing_heap_section_commit;
    uint8_t open_existing_heap_section_reserve;
    uint8_t reserved[2];
    int32_t hr_open_addr;
    int32_t hr_open_addr1;
    int32_t hr_open_section_commit;
    int32_t hr_open_section_reserve;
    char adapter_name[128];
} VictoryGpuCaps;

/* Matches victory_gpu::port_descriptors::Marks (32 bytes) for inline descriptor hooks. */
typedef struct VictoryDescriptorMarks {
    uint64_t start;
    uint64_t len;
    uint8_t* slots;
    uint8_t* chunks;
} VictoryDescriptorMarks;

typedef struct VictoryMemBenchEntry {
    uint8_t supported;
    uint8_t verified;
    uint8_t reserved[2];
    int32_t hr;
    double copy_ms;
    double compute_read_ms;
    double copy_gbps;
    double compute_read_gbps;
} VictoryMemBenchEntry;

typedef struct VictoryMemBenchReport {
    uint32_t abi_version;
    uint32_t buffer_bytes;
    uint32_t iterations;
    uint32_t reserved;
    VictoryMemBenchEntry default_vram;
    VictoryMemBenchEntry gpu_upload_rebar;
    VictoryMemBenchEntry upload_wc;
    VictoryMemBenchEntry open_addr1_sysram;
    VictoryMemBenchEntry open_section_sysram;
} VictoryMemBenchReport;

/* Rust -> C ABI shader translation exports (implemented in crates/victory-ffi) */
VICTORY_API uint32_t victory_shader_abi_version(void);
VICTORY_API uint32_t victory_shader_translator_version(void);
VICTORY_API int32_t victory_shader_translate(
    const uint8_t* container_ptr,
    size_t container_len,
    uint32_t host_max_wave,
    uint8_t host_rdna,
    VictoryTranslatedShader* out_shader,
    char* err_buf,
    size_t err_buf_len);
VICTORY_API void victory_shader_free(VictoryTranslatedShader* shader);

/* C++20 native runtime exports (implemented in native/src) */
VICTORY_API uint32_t victory_native_abi_version(void);
VICTORY_API int32_t victory_native_probe_gpu(
    const char* adapter_filter,
    VictoryGpuCaps* out_caps,
    char* err_buf,
    size_t err_buf_len);
VICTORY_API int32_t victory_native_bench_memory(
    const char* adapter_filter,
    uint32_t buffer_bytes,
    uint32_t iterations,
    VictoryMemBenchReport* out_report,
    char* err_buf,
    size_t err_buf_len);
VICTORY_API int32_t victory_native_apply_descriptor_sites(
    uintptr_t image_base,
    size_t image_size,
    const VictoryDescriptorMarks* marks,
    size_t* out_patched);
VICTORY_API int32_t victory_native_run_self_tests(
    char* report_buf,
    size_t report_buf_len);

#ifdef __cplusplus
}
#endif
