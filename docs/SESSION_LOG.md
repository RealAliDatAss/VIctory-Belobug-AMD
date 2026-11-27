# Collaboration & Experiment Session Log

Append new sessions at the **bottom** (or newest-first if preferred — keep consistent; currently chronological). Follow [`docs/COLLABORATION.md`](COLLABORATION.md) and [`docs/asset-policy.md`](asset-policy.md). Never paste game binary bytes, disassembly, or long hex/base64 dumps.

---

## Entry Template

```markdown
## Session <N> — <YYYY-MM-DD> (<Author/Agent>, Profile: `<profile-id>`)
- **Commit / Branch**: `<hash>` on `<branch>`
- **Goal**: <1-2 lines>
- **Environment / Switches**: <build profile, console mode, VICTORY_* env vars>
- **Findings / Observations**:
  - <concrete file references or observed tool/benchmark numbers>
- **Changes Made**:
  - <files created/edited>
- **Verification**:
  - <commands run and exact pass/fail outcome>
- **Next Steps**:
  1. <actionable next item>
```

---

## Session 1 — 2026-11-26 (Antigravity / Gemini 4 Hydrogen, Profile: `pc-4090-local`)
- **Commit / Branch**: `4eb6cfd` on `a-publier`
- **Goal**: Audit the entire `VIctory-Belobug-AMD` codebase, evaluate replacing Rust with a better language for a hybrid compatibility layer on this PC, and establish durable multi-session/multi-user collaboration docs.
- **Environment / Machine Audit (`pc-4090-local`)**:
  - **GPU**: `NVIDIA GeForce RTX 4090` (observed via `nvidia-smi -L`).
  - **CPU**: `Intel64 Family 6 Model 183 Stepping 1, GenuineIntel`, `32` logical processors (Raptor Lake hybrid P+E).
  - **Installed Toolchains**: CMake (`C:\Program Files\CMake\bin\cmake.exe`), Python 3.14 & 3.11, .NET, Visual Studio 2019 (Community & BuildTools), Visual Studio 2022 (`Q:\microsoft Visual Studio\2022`), Windows SDK `10.0.26100.0`, NVIDIA CUDA Toolkit `v13.2` and `v12.3`.
  - **Missing Local Setup**: `rustc`/`cargo`/`rustup` not installed in `%USERPROFILE%\.cargo` or `%USERPROFILE%\.rustup` or on default PATH; no `victory.toml` present yet in repo root.
- **Findings / Observations**:
  - **Codebase Scale**: ~197,897 lines across 217 `.rs` files in `crates/`. `victory-gpu` alone is **106,653 lines** (103 files); `victory-shader` is **50,867 lines** (40 files); `victory-hle` is **25,362 lines** (37 files); `victory-loader` is **7,011 lines**.
  - **Why AMD vs NVIDIA differs so sharply**:
    - `README.md` baseline on `ref-7900xtx` reaches ~45 ms/frame (~22 fps) with Ports 1/2/5/6/7, while RTX 3090 reports ~532–545 ms/frame and RTX 4070 ~190–450 ms/frame.
    - [`host.rs#L1044-L1056`](../crates/victory-gpu/src/host.rs#L1044-L1056) sets `host_max_wave=32` and `host_rdna=false` for NVIDIA, causing [`lower.rs#L1844-L1906`](../crates/victory-shader/src/emit/lower.rs#L1844-L1906) to split Wave64 compute shaders into dual Wave32 waves with LDS exchange dwords and `GroupMemoryBarrier`s.
    - [`indirect.rs#L280-L294`](../crates/victory-gpu/src/indirect.rs#L280-L294) caps indirect dispatch width to `65_535` on non-AMD GPUs (`!amd(device)`).
    - [`Cargo.toml#L51-L65`](../Cargo.toml#L51-L65) suppresses 9 Clippy lints (including `undocumented_unsafe_blocks`), reflecting high FFI/COM/unsafe-pointer friction in `victory-gpu` and `victory-hle`.
  - **Asset Policy Gate**: [`xtask/src/policy.rs`](../xtask/src/policy.rs) enforces an extension allowlist (`rs, toml, md, txt, lock, yml, yaml, json, hlsl, hlsli, ps1, cmd, sh`), 640 KiB max file size, and max 512-char encoded runs. Adding C++/Zig files later requires updating `ALLOWED_EXTENSIONS`.
- **Changes Made**:
  - Created [`docs/COLLABORATION.md`](COLLABORATION.md)
  - Created [`docs/HYBRID_ARCHITECTURE.md`](HYBRID_ARCHITECTURE.md)
  - Created [`docs/SESSION_LOG.md`](SESSION_LOG.md)
- **Verification**:
  - Verified all three new `.md` files comply with [`xtask/src/policy.rs`](../xtask/src/policy.rs) rules (allowed extension `.md`, under 640 KiB, UTF-8 text, no binary magic bytes, no hex/base64 runs > 512 chars, zero game assets).
- **Next Steps**:
  1. Install Rust `1.98.1` (`x86_64-pc-windows-msvc`), run `cargo xtask policy`, configure `victory.toml` pointing outside the repo, and build `diag` profile (`cargo build --locked --profile diag -p victory --features diagnostics`).
  2. Run a baseline capture + `victory replay --timing detailed` on `pc-4090-local` (RTX 4090) testing `VICTORY_D3D12_BACKGROUND=0` and Ports 1/2/5/6/7 to isolate CPU submit vs GPU Wave32 bottlenecks.
  3. Stand up the Phase 1 C ABI seam (`victory_abi.h`) + C++20 CMake native runtime target (`native/`) once baseline numbers are recorded.

---

## Session 2 — 2026-11-27 (Antigravity / Gemini 4 Hydrogen, Profile: `pc-4090-local`)
- **Commit / Branch**: `4eb6cfd` + local uncommitted hybrid scaffold on `a-publier`
- **Goal**: Implement Phase 1/2 C ABI seam (`victory_abi.h`, `crates/victory-ffi`) and build/run the C++20 native core (`mem_guard`, `hook_x64`, `d3d12_probe`) directly against the local RTX 4090.
- **Environment / Switches**: MSVC `19.29.30159.0` (VS 2019 BuildTools), WinSDK `10.0.26100.0`, CMake `4.3.3` (`NMake Makefiles`, `/std:c++20`, `Embedded` `/Z7` debug info to avoid `mspdbsrv` `C1902` in sandbox).
- **Changes Made**:
  - Updated [`xtask/src/policy.rs`](../xtask/src/policy.rs) (`ALLOWED_EXTENSIONS` + `CMakeLists.txt`) and [`.gitignore`](../.gitignore) (`/native/build/`, `/build/`).
  - Created [`native/include/victory_abi.h`](../native/include/victory_abi.h) (`VICTORY_ABI_VERSION = 1`).
  - Created [`crates/victory-ffi/Cargo.toml`](../crates/victory-ffi/Cargo.toml) and [`crates/victory-ffi/src/lib.rs`](../crates/victory-ffi/src/lib.rs) (C ABI wrapper around `victory_shader::emit::translate`).
  - Created [`native/CMakeLists.txt`](../native/CMakeLists.txt), [`native/src/mem_guard.hpp`](../native/src/mem_guard.hpp), [`native/src/mem_guard.cpp`](../native/src/mem_guard.cpp), [`native/src/hook_x64.hpp`](../native/src/hook_x64.hpp), [`native/src/hook_x64.cpp`](../native/src/hook_x64.cpp), [`native/src/d3d12_probe.hpp`](../native/src/d3d12_probe.hpp), [`native/src/d3d12_probe.cpp`](../native/src/d3d12_probe.cpp), [`native/src/victory_abi.cpp`](../native/src/victory_abi.cpp), [`native/src/probe_main.cpp`](../native/src/probe_main.cpp).
- **Measured Results (`victory_probe.exe`)**:
  - `mem_guard`: PASS (4 x 64 KiB blocks armed, 2 write faults handled via lock-free VEH, SEH `safe_read_bytes` verified).
  - `hook_x64`: PASS (`±2 GiB` near trampoline + `E9 rel32` patch + restore verified).
  - **RTX 4090 Live D3D12 Probe**:
    - Adapter: `NVIDIA GeForce RTX 4090 (vendor=0x10DE device=0x2684)`
    - Memory: `dedicated=24138 MB`, `shared=48979 MB` (~96 GB system RAM), `heap_tier=2`, **`gpu_upload_heap=1`** (`D3D12_HEAP_TYPE_GPU_UPLOAD` / Resizable BAR supported!)
    - Waves: `min=32`, `max=32`, `total=16384`
    - Tiers: `shader_model=0x67` (SM 6.7), `sm66_direct=1`, `dxr_tier=12`, `mesh_tier=10`
    - Heaps: `OpenAddr=1 (0x00000000)`, `OpenAddr1=1 (0x00000000)`, `SecCommit=1 (0x00000000)`, `SecReserve=0 (0x80070057)`
- **Key Architectural Takeaway**:
  - Because `gpu_upload_heap=1` and `OpenAddr1=1` both succeed on RTX 4090, the C++20 D3D12.X backend can place dynamic constant/upload/descriptor rings directly in CPU-writable VRAM (`D3D12_HEAP_TYPE_GPU_UPLOAD`) instead of copying across staging buffers or taking PCIe read penalties on L0 write-back heaps.
- **Next Steps**:
  1. Install Rust `1.98.1` (`x86_64-pc-windows-msvc`) to compile & test `crates/victory-ffi`, run `cargo xtask policy`, and capture a baseline `victory replay --timing detailed` frame on RTX 4090.
  2. Begin Phase 2/3 in `native/`: implement the C++20 `D3D12.X` `Device` / `CommandQueue` / `CommandList` / `DescriptorHeap` objects using `GPU_UPLOAD` heaps and lock-free `mem_guard` tracking.

---

## Session 3 — 2026-11-27 (Antigravity / Gemini 4 Hydrogen, Profile: `pc-4090-local`)
- **Commit / Branch**: `4eb6cfd` + local hybrid workspace on `a-publier`
- **Goal**: Advance Phase 2/3 C++20 runtime modules (`mem_guard`, `xmem_core`, `hook_x64`, `d3d12_host`), run live RTX 4090 memory-path benchmarks to answer Open Question #1, clean workspace hygiene, and survey competing GTA VI Xbox-to-Windows recompilation strategies.
- **Environment / Switches**: MSVC `19.29.30159.0` (VS 2019 BuildTools `14.29.30133`), WinSDK `10.0.26100.0`, CMake `4.3.3` (`NMake Makefiles`, `/std:c++20`, `/W4`, `/permissive-`, linked with `d3d12`, `dxgi`, `dxguid`, `d3dcompiler`, `onecore`).
- **Changes Made**:
  - Removed duplicate/stale root `SESSION_LOG.md` (canonical log is [`docs/SESSION_LOG.md`](SESSION_LOG.md)) and fixed [`.vscode/settings.json`](../.vscode/settings.json) to point `cmake.sourceDirectory` / `cmake.buildDirectory` to `native` and `native/build`.
  - Updated [`native/include/victory_abi.h`](../native/include/victory_abi.h) (`VictoryDescriptorMarks`, `VictoryMemBenchReport`, `victory_native_bench_memory`, `victory_native_apply_descriptor_sites`) and [`native/CMakeLists.txt`](../native/CMakeLists.txt).
  - Upgraded [`native/src/mem_guard.hpp`](../native/src/mem_guard.hpp) & [`native/src/mem_guard.cpp`](../native/src/mem_guard.cpp): mutex-free atomic VEH slot table (`kMaxGuardedRegions = 256` with per-slot active-fault counter), lock-free `DirtyBitset`, and `WriteWatchRegion` (`MEM_WRITE_WATCH` + 4K->64K deduplication).
  - Created [`native/src/xmem_core.hpp`](../native/src/xmem_core.hpp) & [`native/src/xmem_core.cpp`](../native/src/xmem_core.cpp): console 32-bit/40-bit/46-bit `VirtualAlloc2` constraints, `SEC_COMMIT` physical page allocator, persistent whole-section window, and placeholder splitting + multi-view alias mapping (`MapViewOfFile3` / `UnmapViewOfFile2`).
  - Expanded [`native/src/hook_x64.hpp`](../native/src/hook_x64.hpp) & [`native/src/hook_x64.cpp`](../native/src/hook_x64.cpp): ported `descriptor_sites` (`vmovups` -> `VictoryDescriptorMarks` bitmap emitter + 6 GTA VI 1.0.2.1 sites) and `write_sites` look-first `or byte [mem], imm8` guard stub emitter.
  - Created [`native/src/d3d12_host.hpp`](../native/src/d3d12_host.hpp) & [`native/src/d3d12_host.cpp`](../native/src/d3d12_host.cpp): `HostDevice` (Direct/Compute/Copy queues, fence/event, `GpuUploadRing` on `D3D12_HEAP_TYPE_GPU_UPLOAD`, `OpenExistingHeapFromAddress1` / `OpenExistingHeapFromFileMapping` helpers) + GPU timestamp memory benchmark; updated [`native/src/victory_abi.cpp`](../native/src/victory_abi.cpp) and [`native/src/probe_main.cpp`](../native/src/probe_main.cpp).
  - Updated [`docs/HYBRID_ARCHITECTURE.md`](HYBRID_ARCHITECTURE.md) with empirical RTX 4090 benchmark data and ecosystem strategy comparison.
- **Verification (`victory_probe.exe --bench`)**:
  - `mem_guard`: PASS (`armed=8, MT faults=4, WriteWatch 64K + SEH ok`)
  - `xmem_core`: PASS (`32b=0x2d100000, 40b=0x100000000, SEC_COMMIT alias verified`)
  - `hook_x64`: PASS (`trampoline + descriptor-site stub + write-site look-first guard verified`)
  - `d3d12_host`: PASS (`ReBAR gpu_upload=1, Direct/Compute/Copy queues + fence ok`)
  - **Live RTX 4090 GPU Memory Path Benchmark (16 MB x 16 iters, verified checksums)**:
    - `DEFAULT (VRAM)`: copy `0.011 ms` (`1427.7 GB/s`), `cs_read` `0.003 ms` (`4521.1 GB/s`)
    - `GPU_UPLOAD (ReBAR)`: copy `0.011 ms` (`1427.7 GB/s`), `cs_read` **`0.003 ms` (`4521.1 GB/s`)**
    - `UPLOAD (WC SysRAM)`: copy `0.707 ms` (`22.1 GB/s`), `cs_read` `0.092 ms` (`170.0 GB/s`)
    - `OpenAddr1 (WB SysRAM)`: copy `0.814 ms` (`19.2 GB/s`), `cs_read` **`1.283 ms` (`12.2 GB/s`)**
    - `SecCommit (WB SysRAM)`: copy `0.916 ms` (`17.1 GB/s`), `cs_read` `1.272 ms` (`12.3 GB/s`)
- **Findings / Recomp Strategy Synthesis**:
  - `OpenExistingHeapFromAddress1` is stable on RTX 4090 (no TDR), **but** GPU shader reads from WB system memory suffer a ~370x penalty vs `GPU_UPLOAD` (`12.2 GB/s` vs `4521.1 GB/s`) due to PCIe cache snooping. On RTX 4090 (`gpu_upload_heap=1`), Phase 3 should stream constants/descriptor mirrors into `D3D12_HEAP_TYPE_GPU_UPLOAD` (ReBAR VRAM) rather than letting shaders read WB system RAM directly.
  - Web survey across `Marvisak/VIctory`, `ivall/xcl` (`xcl-dev/xcl`), `fxceless/victory-fork`, `DexNom/VIctory`, `Belobug/VIctory-Belobug-AMD`, `ZJOU-VICTORY`, `codec-d/cool_fox` (`CGBJ` / `pareto-residency`), and `soldsawn/victory-nvidia-8gb` confirms native x86-64 PE + HLE + GFX10->DXIL + title renderer porting dominates static recompilation, with key NVIDIA-specific gaps around Wave64->Warp32 (`dual-bank` lowering), residency budgets, and CPU-GPU memory copies.
  - Note: `browser` subagent could not attach to Chrome (`DevToolsActivePort` missing; Chrome remote debugging disabled); web research was gathered via `search_web`.
- **Next Steps**:
  1. Install Rust `1.98.1` (`x86_64-pc-windows-msvc`) and create git-ignored `victory.toml` pointing `game_dir = 'P:\\torr\\GTA VI v1.0.2.1'`, then run `cargo xtask policy`, build `crates/victory-ffi`, and capture an RTX 4090 `diag` baseline.
  2. Implement Phase 3 C++20 `D3D12.X` COM object wrappers (`Device`, `Resource`, `DescriptorHeap`, `CommandList`, `CommandQueue`, `Fence`) backed by `GpuUploadRing` (`D3D12_HEAP_TYPE_GPU_UPLOAD`) and `mem_guard` / `xmem_core`.
  3. Enable Chrome remote debugging (`chrome://inspect/#remote-debugging`) if deep interactive inspection of specific private/web GitHub PR diffs is needed via `/browser`.
