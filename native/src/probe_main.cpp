#include "victory_abi.h"

#include <cstdio>
#include <cstring>

namespace {

void print_bench_row(const char* label, const VictoryMemBenchEntry& e) {
    if (!e.supported) {
        std::printf("  %-26s unsupported (hr=0x%08X)\n", label, static_cast<unsigned>(e.hr));
        return;
    }
    std::printf("  %-26s verified=%u  copy=%6.3f ms (%6.1f GB/s)  cs_read=%6.3f ms (%6.1f GB/s)\n",
                label,
                static_cast<unsigned>(e.verified),
                e.copy_ms,
                e.copy_gbps,
                e.compute_read_ms,
                e.compute_read_gbps);
}

} // namespace

int main(int argc, char** argv) {
    bool run_bench = false;
    const char* filter = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--bench") == 0) {
            run_bench = true;
        } else if (!filter) {
            filter = argv[i];
        }
    }

    std::printf("victory_native ABI v%u\n", victory_native_abi_version());

    char report[4096]{};
    const int32_t test_rc = victory_native_run_self_tests(report, sizeof(report));
    std::printf("%s", report);
    if (test_rc != VICTORY_OK) {
        std::fprintf(stderr, "self-tests failed (%d)\n", test_rc);
        return 1;
    }

    VictoryGpuCaps caps{};
    char err[512]{};
    const int32_t gpu_rc = victory_native_probe_gpu(filter, &caps, err, sizeof(err));
    if (gpu_rc != VICTORY_OK) {
        std::fprintf(stderr, "GPU probe failed (%d): %s\n", gpu_rc, err);
        return 2;
    }

    std::printf("Adapter: %s (vendor=0x%04X device=0x%04X)\n",
                caps.adapter_name, caps.vendor_id, caps.device_id);
    std::printf("Memory:  dedicated=%llu MB shared=%llu MB heap_tier=%u gpu_upload_heap=%u\n",
                static_cast<unsigned long long>(caps.dedicated_vram_bytes >> 20),
                static_cast<unsigned long long>(caps.shared_sys_bytes >> 20),
                caps.resource_heap_tier,
                static_cast<unsigned>(caps.gpu_upload_heap_supported));
    std::printf("Waves:   min=%u max=%u total=%u\n",
                caps.wave_lane_min, caps.wave_lane_max, caps.wave_lane_total);
    std::printf("Tiers:   shader_model=0x%X sm66_direct=%u dxr_tier=%u mesh_tier=%u\n",
                caps.shader_model,
                static_cast<unsigned>(caps.sm66_directly_indexed),
                caps.raytracing_tier,
                caps.mesh_shader_tier);
    std::printf("Heaps:   OpenAddr=%u (0x%08X) OpenAddr1=%u (0x%08X) SecCommit=%u (0x%08X) SecReserve=%u (0x%08X)\n",
                static_cast<unsigned>(caps.open_existing_heap_addr),
                static_cast<unsigned>(caps.hr_open_addr),
                static_cast<unsigned>(caps.open_existing_heap_addr1),
                static_cast<unsigned>(caps.hr_open_addr1),
                static_cast<unsigned>(caps.open_existing_heap_section_commit),
                static_cast<unsigned>(caps.hr_open_section_commit),
                static_cast<unsigned>(caps.open_existing_heap_section_reserve),
                static_cast<unsigned>(caps.hr_open_section_reserve));

    if (run_bench) {
        VictoryMemBenchReport bench{};
        char berr[512]{};
        const int32_t brc = victory_native_bench_memory(filter, 16u << 20, 16, &bench, berr, sizeof(berr));
        if (brc != VICTORY_OK) {
            std::fprintf(stderr, "Memory benchmark failed (%d): %s\n", brc, berr);
            return 3;
        }
        std::printf("\nGPU Memory Path Benchmark (%u MB x %u iters):\n",
                    bench.buffer_bytes >> 20, bench.iterations);
        print_bench_row("DEFAULT (VRAM)", bench.default_vram);
        print_bench_row("GPU_UPLOAD (ReBAR)", bench.gpu_upload_rebar);
        print_bench_row("UPLOAD (WC SysRAM)", bench.upload_wc);
        print_bench_row("OpenAddr1 (WB SysRAM)", bench.open_addr1_sysram);
        print_bench_row("SecCommit (WB SysRAM)", bench.open_section_sysram);
    }
    return 0;
}
