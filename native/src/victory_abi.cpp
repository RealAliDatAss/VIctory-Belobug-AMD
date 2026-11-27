#include "victory_abi.h"
#include "d3d12_host.hpp"
#include "d3d12_probe.hpp"
#include "hook_x64.hpp"
#include "mem_guard.hpp"
#include "xmem_core.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace {

void write_c_buf(char* buf, size_t len, const std::string& msg) {
    if (!buf || len == 0) {
        return;
    }
    const size_t n = std::min(msg.size(), len - 1);
    std::memcpy(buf, msg.data(), n);
    buf[n] = '\0';
}

} // namespace

extern "C" {

uint32_t victory_native_abi_version(void) {
    return VICTORY_ABI_VERSION;
}

int32_t victory_native_probe_gpu(
    const char* adapter_filter,
    VictoryGpuCaps* out_caps,
    char* err_buf,
    size_t err_buf_len) {
    if (!out_caps) {
        write_c_buf(err_buf, err_buf_len, "out_caps is null");
        return VICTORY_ERR_INVALID_ARG;
    }
    std::string err;
    const int32_t rc = victory::gpu::probe_gpu_caps(adapter_filter, *out_caps, err);
    if (rc != VICTORY_OK) {
        write_c_buf(err_buf, err_buf_len, err);
    }
    return rc;
}

int32_t victory_native_bench_memory(
    const char* adapter_filter,
    uint32_t buffer_bytes,
    uint32_t iterations,
    VictoryMemBenchReport* out_report,
    char* err_buf,
    size_t err_buf_len) {
    if (!out_report) {
        write_c_buf(err_buf, err_buf_len, "out_report is null");
        return VICTORY_ERR_INVALID_ARG;
    }
    std::string err;
    const int32_t rc = victory::gpu::benchmark_memory_paths(
        adapter_filter, buffer_bytes, iterations, *out_report, err);
    if (rc != VICTORY_OK) {
        write_c_buf(err_buf, err_buf_len, err);
    }
    return rc;
}

int32_t victory_native_apply_descriptor_sites(
    uintptr_t image_base,
    size_t image_size,
    const VictoryDescriptorMarks* marks,
    size_t* out_patched) {
    if (image_base == 0 || image_size == 0 || !marks) {
        if (out_patched) {
            *out_patched = 0;
        }
        return VICTORY_ERR_INVALID_ARG;
    }
    const size_t patched = victory::hook::apply_descriptor_sites(
        image_base, image_size, marks, victory::hook::known_descriptor_sites());
    if (out_patched) {
        *out_patched = patched;
    }
    return VICTORY_OK;
}

int32_t victory_native_run_self_tests(char* report_buf, size_t report_buf_len) {
    std::string report;
    const bool mem_ok = victory::mem::run_self_tests(report);
    const bool xmem_ok = victory::xmem::run_self_tests(report);
    const bool hook_ok = victory::hook::run_self_tests(report);
    const bool host_ok = victory::gpu::run_host_self_tests(report);
    write_c_buf(report_buf, report_buf_len, report);
    return (mem_ok && xmem_ok && hook_ok && host_ok) ? VICTORY_OK : VICTORY_ERR_OS;
}

} // extern "C"
