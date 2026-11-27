# Hybrid Compatibility Layer Architecture & Language Analysis

**Goal**: Achieve high, stable frame rates and proper lighting/memory efficiency for GTA VI (`game_scarlett_master.exe` 1.0.2.1) on PC — specifically targeting `pc-4090-local` (NVIDIA RTX 4090 + Raptor Lake 32T) while remaining maintainable across contributors.

This document synthesizes a full audit of the current ~198K-line Rust workspace (`VIctory-Belobug-AMD`, branch `a-publier`), evaluates whether Rust should be replaced (and *where*), and lays out an incremental **Hybrid Compatibility Layer** architecture.

---

## 1. Codebase Inventory & Current Bottlenecks

### Workspace Footprint (`crates/` = 197,897 lines across 217 `.rs` files)

| Crate | Lines | Files | Primary Responsibility | Hot-Path / Friction Profile |
|---|---:|---:|---|---|
| `victory-gpu` | 106,653 | 103 | D3D12.X -> Host D3D12, memory guards, shadows, descriptors, submit lane, replay | **Primary CPU + RAM + complexity bottleneck** |
| `victory-shader` | 50,867 | 40 | GFX10 -> SSA IR -> Structurization -> DXIL SM 6.6 + Opt + BVH ref + Wave32 lowering | **Ideal in Rust**; mostly offline/cached |
| `victory-hle` | 25,362 | 37 | Console DLLs (`xmem`, `XCRDAPI`/DirectStorage, `gameinput`, `xgamesave`, `audio`, `files`) | Functional; `xmem`/`dstorage` have raw-pointer & memory-perf seams |
| `victory-loader` | 7,011 | 19 | PE32+ prepare/map, IAT binding, TLS fixups, x86-64 binary site patching, crash/unwind | Heavy raw x86-64 machine-code emitters & Win32 SEH/VEH |
| `victory-formats` | 5,457 | 6 | Console texture tiling/detiling, BCPack, XMA audio | Self-contained, portable; BCPack CPU speed (~15–60 MB/s) is a minor streaming limit |
| `victory-pe` | 1,635 | 4 | Read-only PE32+ and `.lib` parser | Clean, keep as-is |
| `victory-core` | 460 | 4 | Config, asset-policy guard, logging | Clean, keep as-is |
| `victory-pipeline` | 259 | 2 | Content-addressed cache | Clean, keep as-is |
| `victory-game` | 243 | 2 | Title build IDs (`KNOWN_BUILDS` currently empty), `sgd5` archive parser | Keep/extend with title hook registry |

### Observed Runtime Bottlenecks (from [`README.md`](../README.md), [`docs/findings.md`](findings.md), and source audit)

1. **Unified Memory Emulation Tax (CPU + RAM)**:
   - On `ref-7900xtx` (with Ports 1, 2, 5, 6, 7 on), city street sits at **~45 ms/frame (~22 fps)**:
     - Single submission lane (`victory-submit` in [`queue/behind.rs`](../crates/victory-gpu/src/queue/behind.rs)): **~40 ms/frame**.
     - GPU busy: **~36 ms/frame**, GPU idle waiting on CPU: **~7 ms/frame**.
     - Private RAM: **~30 GB** (up to 40–46 GB without ports), VRAM: **10.5 GB**.
   - Huge modules exist solely to bridge discrete PC memory vs. unified console memory:
     - [`watch.rs`](../crates/victory-gpu/src/watch.rs) (5,962 lines): `MEM_WRITE_WATCH` + VEH `PAGE_READONLY` guards across multi-mapped physical views + 64-shard dirty page journal.
     - [`shadow.rs`](../crates/victory-gpu/src/shadow.rs) (5,910 lines) + [`land.rs`](../crates/victory-gpu/src/land.rs) + [`host/card_steps.rs`](../crates/victory-gpu/src/host/card_steps.rs): Host shadow buffers, GPU hash-compare passes, AVX2 CPU read-back merges, and pre-submission GPU copy lists.
     - [`descriptor/host.rs`](../crates/victory-gpu/src/descriptor/host.rs) (6,184 lines): Shadowing ~400K–600K descriptor slots into SM 6.6 directly indexed host heaps.
2. **NVIDIA RTX 3090 / 4070 / 4090 Specific Regressions**:
   - Community reports in [`README.md`](../README.md) show RTX 3090 at **~532–545 ms/frame** (~1.8 fps) and RTX 4070 at **190–450 ms/frame** (2–5 fps) vs. ~22 fps on RX 7900 XTX.
   - Root causes visible in source:
     - **Wave32 vs. Wave64**: [`host.rs`](../crates/victory-gpu/src/host.rs#L1044-L1056) detects `WaveLaneCountMax == 32` on NVIDIA and disables `host_rdna`, forcing [`lower.rs`](../crates/victory-shader/src/emit/lower.rs#L1844-L1906) to split Wave64 compute shaders into dual Wave32 waves with LDS exchange dwords + extra `GroupMemoryBarrier`s.
     - **Indirect Dispatch Clamp Width**: [`indirect.rs`](../crates/victory-gpu/src/indirect.rs#L280-L294) restricts `!amd(device)` to `65_535` groups instead of `1 << 20`, and clamps/synchronizes via HLSL SM 5.1 compute scratch passes.
     - **Driver Behavior on In-Place Heaps & Raw CBV Twins**: [`port.rs`](../crates/victory-gpu/src/port.rs) (`OpenExistingHeapFromAddress1`) and [`port_cbv.rs`](../crates/victory-gpu/src/port_cbv.rs) (raw SRV reads of constant buffers) were authored and measured on AMD; NVIDIA WDDM/driver behavior for custom write-back L0 heaps, raw SRV aliasing, and `D3D12_BACKGROUND_PROCESSING_MODE` differs materially and needs isolated measurement on RTX 4090.
3. **Software Ray Tracing Cost**:
   - [`docs/fork-ray-tracing.md`](fork-ray-tracing.md) shows the title builds/traverses console BVHs via compute shaders + `image_bvh64_intersect_ray` rather than DXR. Emulating node tests in DXIL over a 40-bit software page directory ([`memory.rs`](../crates/victory-gpu/src/memory.rs)) adds **~40 ms/frame** on 7900 XTX and suffers dynamic-BVH size-readback overlap bugs.

---

## 2. Language Evaluation: Can Rust Be Replaced with a Better Language?

### Short Answer
**Yes for the hot runtime / GPU / loader / ABI layer (`victory-gpu` + `victory-loader` hooks + `xmem`); NO for a full blind rewrite of the compiler (`victory-shader`) and parsers (`victory-formats`, `victory-pe`).**

The optimal path is a **Language-Hybrid + Execution-Hybrid** split.

### Evidence from the Codebase: Where Rust Hurts vs. Helps

#### Where Rust Actively Fights This Problem
1. **COM / D3D12.X Vtable & ABI Boilerplate**:
   - D3D12.X, DXGI, DirectStorage, and GameInput are C++ COM vtables. Rust forces thousands of lines of `#[repr(C)]` vtables, raw `*const c_void` (`This`) casting, `Box::into_raw`/`Box::from_raw`, `#![allow(clippy::not_unsafe_ptr_arg_deref)]`, and manual GUID tables.
   - [`Cargo.toml`](../Cargo.toml#L51-L65) disables 9 Clippy lints including `undocumented_unsafe_blocks` because the runtime's unsafe/FFI density outgrew idiomatic Rust.
2. **Concurrent Memory Tracking & Lock Contention**:
   - Rust's strict aliasing (`&mut` XOR `&`) pushes lock-heavy designs (`Mutex<BTreeMap>`, `RwLock`, `Box::leak` in [`port_descriptors.rs`](../crates/victory-gpu/src/port_descriptors.rs#L117)) across [`watch.rs`](../crates/victory-gpu/src/watch.rs) and [`shadow.rs`](../crates/victory-gpu/src/shadow.rs). Writing lock-free hazard-pointer page tables, custom slab allocators, or SEH-guarded memory probes in Rust requires fighting the borrow checker via `UnsafeCell` everywhere.
3. **Native Windows SDKs, GPU Tools, and Binary Patching**:
   - `write_sites.rs` and `descriptor_sites.rs` hand-assemble raw x86-64 machine code arrays in `Vec<u8>`.
   - Direct use of **D3D12 Agility SDK**, **NVAPI**, **Streamline / DLSS**, **CUDA-D3D12 interop**, **DXC**, and **FidelityFX** requires hand-maintained Rust FFI bindings, whereas in C++ they are native headers.
4. **Incremental Build Latency**:
   - Modifying one line in `victory-gpu` (106K lines) triggers heavy monomorphization and slow link cycles.

#### Where Rust Excels (Keep It!)
1. **`victory-shader` (50.9K lines)**: Algebraic data types (`enum`s) and exhaustive pattern matching over GFX10 opcodes, SSA IR, CFG structurization, and symbolic wave analysis are Rust's sweet spot. 25,790 / 25,985 shaders already translate and pass GPU diff tests. Rewriting this into C++ would waste 3–6 months for zero runtime gain (translations are cached on disk).
2. **`victory-formats`, `victory-pe`, `victory-core`, `victory-pipeline`**: Already tested, memory-safe binary parsers and CLI/replay orchestration.

### Language Comparison for the Replacement Runtime

| Criterion | C++20 / C++23 (MSVC / Clang-CL) | Zig (0.14+) | Pure C17 | Keep 100% Rust |
|---|---|---|---|---|
| D3D12 / Agility SDK / COM vtables | **Native** (`d3d12.h`, `d3dx12.h`, zero FFI shim) | `@cImport` works, but COM vtables still manual | Verbose macros, no RAII | Heavy `windows` crate + manual `#[repr(C)]` |
| NVIDIA SDKs (NVAPI, CUDA, Streamline, Nsight) | **First-class native headers** | Requires C wrappers | C wrappers needed | Manual `bindgen` / hand FFI |
| Lock-free atomics, custom arenas, SIMD, SEH (`__try`) | **Mature** (`std::atomic`, `<immintrin.h>`, native SEH) | Great allocators & `@ptrCast`; SEH is awkward | Manual everything | `unsafe` + `Mutex` friction |
| Inline x86-64 hooking / VEH / JIT stubs | Native + `asmjit` / `detours` / `minhook` | `comptime` helpers, less Windows tooling | Works, verbose | Hand-coded byte vectors |
| Windows Debugging (VS, WinDbg, PIX, Nsight) | **Best-in-class** PDB & natvis | PDB improving, rough in Nsight/VS | Good PDB | Good PDB, noisy std types |
| Contributor Onboarding & Ecosystem Stability | **Very high** (universal in emulators: Xenia, RPCS3, DXVK, vkd3d) | Pre-1.0 breaking changes | High, low abstraction | High in this repo, steep FFI learning curve |

**Recommendation**: **C++20 (compiled with MSVC / Clang-CL via CMake)** for the new native GPU/Loader/Memory runtime (`victory_native.dll` / `victory_gpu.dll`), paired with **Rust** for `victory-shader`, `victory-formats`, `victory-pe`, `apps/victory` CLI, and gradual migration of `victory-hle`.

---

## 3. Proposed Hybrid Compatibility Layer Design

"Hybrid" operates on **two orthogonal axes**:

### Axis A: Language Hybrid (Clean C ABI Seam)

Instead of a risky "big bang" rewrite, split the process across a stable **C ABI (`include/victory_abi.h`)**:

```text
+-------------------------------------------------------------------+
|  apps/victory (Rust CLI / Config / Replay / Shader Warmup)        |
+-------------------------------+-----------------------------------+
                                | C ABI (victory_abi.h)
+-------------------------------v-----------------------------------+
|  victory_runtime.dll (C++20 / CMake)  <-- Hot Path Replacement    |
|  - PE mapping, x86-64 hook engine, VEH/SEH memory guard engine    |
|  - D3D12.X Device/Queue/List/Descriptor/Resource runtime          |
|  - Lock-free frame ring, zero-copy heaps, NVAPI/CUDA/DXR hooks    |
+---------------+-------------------------------+-------------------+
                | C ABI calls                   | C ABI calls
+---------------v---------------+   +-----------v-------------------+
| victory-shader (Rust staticlib|   | victory-hle / formats (Rust)  |
| or DLL: GFX10 -> DXIL cache)  |   | DirectStorage, BCPack, Saves  |
+-------------------------------+   +-------------------------------+
```

**Why this works**:
- The game continues to boot and run at every migration milestone.
- New C++ modules can live in a dedicated `native/` CMake tree (with `xtask/src/policy.rs` updated to allow `.c`, `.cpp`, `.h`, `.hpp`, `.cmake` or kept as a clear subproject).
- `victory-shader` becomes an isolated shader compiler library exposed via one C function: `victory_translate_shader(...) -> TranslatedShaderC`.

### Axis B: Execution Hybrid ("Porting + Selective Emulation + GPU Offload")

To break past the 45 ms/frame CPU wall and fix NVIDIA RTX 4090 performance:

1. **Aggressive Renderer Porting (Zero-Copy by Default)**:
   - Extend the [`port.rs`](../crates/victory-gpu/src/port.rs) / [`port_cbv.rs`](../crates/victory-gpu/src/port_cbv.rs) / [`port_descriptors.rs`](../crates/victory-gpu/src/port_descriptors.rs) philosophy:
     - Hook title frame allocators, upload rings, constant rings, and vertex/index rings **at the title's own sync/commit points** instead of polling `MEM_WRITE_WATCH` or taking 350+ `PAGE_READONLY` faults/frame.
     - On 24 GB RTX 4090 with Resizable BAR / `OpenExistingHeapFromAddress1`, test mapping persistent upload/constant regions in place and explicitly flushing dirty subranges by title-hook notifications rather than CPU shadow diffs.
2. **Multi-Threaded D3D12 Command Recording (Kill the Single `victory-submit` Thread)**:
   - Currently [`queue/behind.rs`](../crates/victory-gpu/src/queue/behind.rs) serializes submission preparation on one `victory-submit` thread (~40 ms/frame!).
   - Replace with a lock-free multi-worker command list translator + **parallel state/barrier resolver** per frame graph pass, only serializing the final `ExecuteCommandLists` call.
3. **NVIDIA RTX 4090 Shader & Compute Pipeline**:
   - Profile [`replay/`](../crates/victory-gpu/src/replay) captures directly on `pc-4090-local` (first using existing Rust `victory replay` once Rust is installed!) to separate **CPU submit time** from **RTX 4090 GPU pass time**.
   - Audit Wave64 -> Wave32 lowering bottlenecks (`lower.rs`): eliminate unnecessary `GroupMemoryBarrier`s and LDS exchanges where 32-lane ballots suffice or where shader pattern replacement (e.g., native PC compute kernels for specific hotspots like screen-space probes / fog / skinning) outperforms translated GFX10.
   - Explore **CUDA-D3D12 external memory / Compute offloads**:
     - Offload **BCPack decompression** and **texture tiling/detiling** (`texback` / `dstorage`) to GPU compute or CUDA if CPU streaming stutters.
     - Evaluate **Hardware DXR / OptiX hybrid RT** vs. software `memory.rs` BVH traversal: instead of building a 40-bit software page directory and sorting 64-byte nodes in DXIL ALU for 40 ms/frame, test either (a) translating dynamic/static BVH instances into native DXR TLAS/BLAS on GPU, or (b) optimizing the software BVH traversal specifically for RTX 4090 Wave32/shared memory.

---

## 4. Incremental Migration Roadmap

| Phase | Milestone | Deliverable | Status / Exit Criteria |
|---|---|---|---|
| **0. Hardware & ABI Foundation** | Probe RTX 4090 caps + lock policy + scaffold C ABI | `native/include/victory_abi.h`, `crates/victory-ffi`, `native/` (`mem_guard`, `hook_x64`, `d3d12_probe`) | **DONE (2026-11-27)**: `victory_probe.exe` built & passed; RTX 4090 caps measured (`gpu_upload_heap=1`, `OpenAddr1=1`, `Wave32`, `dxr_tier=12`, `sm66_direct=1`) |
| **0b. Rust Baseline on `pc-4090-local`** | Install Rust 1.98.1, configure `victory.toml`, build `diag` | Baseline FPS, frame-time breakdown, CPU vs GPU split, and `cargo xtask policy` | **Pending Rust install**: reproducible baseline entry in `docs/SESSION_LOG.md` |
| **1. C ABI Shader Link** | Build & test `victory-ffi` staticlib/cdylib against `victory_native` | `victory_shader_translate` callable from C++20 | **Code written (`crates/victory-ffi`)**: pending Rust 1.98.1 install to link & verify DXIL parity |
| **2. C++20 Loader + Memory Core** | Port `xmem` + `watch` + `descriptor_sites`/`write_sites` hooks to C++20 (`native/`) | Mutex-free VEH `GuardedRegion`, `WriteWatchRegion` (64K bitset), `xmem_core` (`VirtualAlloc2` + `SEC_COMMIT` aliasing), `hook_x64` (`descriptor_sites` + `write_sites` look-first stubs) | **IN PROGRESS (2026-11-27)**: Core C++20 modules implemented & verified via `victory_probe.exe` self-tests; Rust/HLE wiring next |
| **3. C++20 D3D12.X Backend** | Replace `victory-gpu` object/list/queue/descriptor layers behind C ABI | `HostDevice` (`Direct/Compute/Copy` queues, `GpuUploadRing` on `GPU_UPLOAD` ReBAR, `OpenAddr1`/`SecCommit` bridge) + multi-threaded list recording | **STARTED (2026-11-27)**: `d3d12_host` foundation & live RTX 4090 memory benchmark passing |
| **4. RTX 4090 Hybrid Acceleration** | Wave32 hotspot replacement (dual-bank / LDS opt), Pareto residency, GPU BCPack/detiling, Hybrid DXR RT | Custom PC compute / DXR passes + ReBAR constant/descriptor rings | Target: stable 30+ fps (goal 60 fps) with working indoor lighting and lower RAM |

---

## 5. Open Architectural Questions & Empirical Findings

1. **[ANSWERED 2026-11-27 on `pc-4090-local`]** On RTX 4090, does `OpenExistingHeapFromAddress1` work stably without TDR, and what is the PCIe read penalty vs. `GPU_UPLOAD` (ReBAR)?
   - **Measured (`victory_probe.exe --bench`, 16 MB x 16 iters, GPU timestamp queries, verified checksums)**:
     - `DEFAULT (VRAM)`: copy `0.011 ms` (`1427.7 GB/s`), compute read `0.003 ms` (`4521.1 GB/s`)
     - `GPU_UPLOAD (ReBAR)`: copy `0.011 ms` (`1427.7 GB/s`), compute read **`0.003 ms` (`4521.1 GB/s`)** — identical to VRAM!
     - `UPLOAD (WC SysRAM)`: copy `0.707 ms` (`22.1 GB/s`), compute read `0.092 ms` (`170.0 GB/s`)
     - `OpenAddr1 (WB SysRAM)`: copy `0.814 ms` (`19.2 GB/s`), compute read **`1.283 ms` (`12.2 GB/s`)** (no TDR, verified=1, **370x slower** shader read than `GPU_UPLOAD`)
     - `SecCommit (WB SysRAM)`: copy `0.916 ms` (`17.1 GB/s`), compute read `1.272 ms` (`12.3 GB/s`)
   - **Design Consequence**: Unlike AMD UMA-like behavior where in-place WB heaps (`VICTORY_PORT_CONSTANTS=1` / `VICTORY_PORT_CBV=1`) shine, RTX 4090 incurs heavy PCIe cache-snoop overhead (`12.2 GB/s`) when shaders read WB L0 memory directly. On RTX 4090 (`gpu_upload_heap=1`, 24 GB VRAM), dynamic constant buffers, upload rings, and hot shader-read buffers should target **`D3D12_HEAP_TYPE_GPU_UPLOAD` (ReBAR VRAM)** via dirty-subrange flushes rather than raw `OpenAddr1` shader reads.
2. On `pc-4090-local`, of the ~200–500 ms/frame observed on other NVIDIA cards in [`README.md`](../README.md), how much is **CPU `victory-submit`** vs **GPU Wave32 shader execution** vs **D3D12 background shader recompilation** (`VICTORY_D3D12_BACKGROUND=0`)? *(Requires Rust baseline capture/replay)*
3. Can `DirectStorage` + `BCPack` (`crates/victory-formats/src/bcpack.rs`) be offloaded to GPU compute/CUDA on RTX 4090, or is CPU multi-threading already fast enough once memory guards aren't stalling worker threads?
4. Which title passes benefit most from **native HLSL/DXR replacement** or **dual-bank Wave64->Warp32 shader lowering** vs. improving GFX10->DXIL translation?

---

## 6. Comparative Ecosystem Strategies (Research Summary, 2026-11-27)

| Strategy / Lineage | Notable Repos / Forks | Core Approach | Takeaways for `VIctory-Belobug-AMD` Hybrid |
|---|---|---|---|
| **Static Source / Binary Recomp** | *Project Leonida* | Lift executable to standalone C++ | Superseded by native x86-64 PE mapping + HLE due to DMCA & scale |
| **Clean-Room Rust HLE + HLSL->DXC** | `ivall/xcl` (`xcl-dev/xcl`) | Native PE + GDK HLE + GFX10->HLSL->DXC->DXIL | Clean HLE separation; direct DXIL emission (`victory-shader`) avoids DXC runtime latency |
| **Emulation-First -> Renderer Porting** | `Marvisak/VIctory`, `fxceless/victory-fork`, `Belobug/VIctory-Belobug-AMD`, `avatarmanz/VIctory-7900XTX` | Patch title's own descriptor/constant/write sites (`Ports 1/2/5/6/7`, `VICTORY_THIN=1`, `VICTORY_RELAY=2`) | Core strategy we are accelerating in C++20 (`hook_x64` + `mem_guard` + `d3d12_host`) |
| **Upscaling / RT / Render-Target Forks** | `DexNom/VIctory`, `FQY55kai/ZJOU-VICTORY` | FidelityFX FSR 4.1.2 / 3.1, BVH/cube-gradient fixes, RT dedup | Adopt FSR/DLSS hook cleanly behind C++20 swapchain/upscaler path |
| **NVIDIA VRAM/RAM & Wave32 Forks** | `codec-d/cool_fox` (`CGBJ` / `pareto-residency`), `soldsawn/victory-nvidia-8gb`, dual-bank Wave64 experiments | Adaptive residency budgets (`VICTORY_LEVELS_ON_DEMAND=1`, `VICTORY_JOIN_PARTS=8`, `VICTORY_CARD_BUFFERS_MIN_KB=64`), mip dropping, dual-bank Wave64->32 | Combine Pareto residency + dual-bank Wave32 lowering with `GPU_UPLOAD` (ReBAR) heaps on RTX 4090 |
