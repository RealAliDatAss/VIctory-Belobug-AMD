# Multi-Session & Multi-User Collaboration Guide

This document defines how human contributors and AI coding sessions collaborate on this repository across different machines, GPUs, and sessions without losing context, duplicating failed experiments, or violating repository policy.

---

## 1. Non-Negotiable Rules

1. **Asset Policy ([`docs/asset-policy.md`](asset-policy.md))**:
   - **NEVER** commit, paste, or store game files, executable bytes, disassembly, extracted tables, shader dumps, bitcode, screenshots, or long hex/base64 strings (`> 512` chars).
   - Keep every committed file under **640 KiB** and within the allowlist in [`xtask/src/policy.rs`](../xtask/src/policy.rs):
     `rs, toml, md, txt, lock, yml, yaml, json, hlsl, hlsli, ps1, cmd, sh`.
   - **Important for Hybrid Language Work**: If new C/C++/Zig/CMake/header files (`.c`, `.cpp`, `.h`, `.hpp`, `.zig`, `.cmake`) are introduced into the repository, `ALLOWED_EXTENSIONS` in [`xtask/src/policy.rs`](../xtask/src/policy.rs) **must** be updated in the same PR/commit or `cargo xtask policy` and CI will reject them.
   - All game data, shader caches, captures, and saves must live **outside** the repository via `victory.toml` (`game_dir`, `cache_dir`, `data_dir`, `fidelityfx_dir`).
2. **Pre-Commit / CI Verification**:
   - Always run (when Rust toolchain is available):
     - `cargo xtask policy`
     - `cargo clippy --workspace --all-targets --locked -- -D warnings`
     - `cargo test --workspace --locked`
3. **Never Conflate Hardware Profiles**:
   - This fork (`VIctory-Belobug-AMD`) was tuned primarily on an **AMD RX 7900 XTX + i9-12900K (64 GB RAM)**.
   - Our primary target workstation in this collaboration has an **NVIDIA GeForce RTX 4090 (24 GB VRAM) + Intel Raptor Lake (32 logical threads) + CUDA 13.2**.
   - Every benchmark, bug report, or shader finding **must** tag the exact hardware profile, driver, git commit, build profile (`diag` vs `release`), and `VICTORY_*` env switches.

---

## 2. How Sessions Should Onboard & Hand Off

### Starting a Session (First 2 Minutes)
1. Read [`docs/SESSION_LOG.md`](SESSION_LOG.md) (latest 2–3 entries) to see what changed, what failed, and what the active next step is.
2. Read [`docs/HYBRID_ARCHITECTURE.md`](HYBRID_ARCHITECTURE.md) to stay aligned on the module boundaries, C ABI seam, and language split.
3. Check `git status -s` and `git log -n 10 --oneline`.
4. Verify local `victory.toml` (git-ignored) and installed toolchains before planning builds or runs.

### Ending a Session (Before Wrapping Up)
1. Append a dated entry to [`docs/SESSION_LOG.md`](SESSION_LOG.md) using the template at the top of that file:
   - Author / Machine Profile ID
   - Goal & Files Touched
   - Exact Commands / Env Switches Run & Measured Numbers (or exact blockers)
   - Decisions Made & Open Questions
   - Concrete Next Step (1–3 actionable items)
2. Update any status tables in [`docs/HYBRID_ARCHITECTURE.md`](HYBRID_ARCHITECTURE.md) if an architectural boundary or benchmark baseline changed.
3. Never leave unverified claims: distinguish **observed tool/runtime output** from **hypotheses/proposals**.

---

## 3. Registered Hardware & Toolchain Profiles

Add a row when collaborating from a new PC.

| Profile ID | CPU | GPU & VRAM | RAM | OS & Key SDKs / Toolchains | Notes |
|---|---|---|---|---|---|
| `ref-7900xtx` | Intel i9-12900K (24T) | AMD RX 7900 XTX (24 GB) | 64 GB | Windows 11, Rust 1.98.1, MSVC | Upstream fork baseline (~45 ms / ~22 fps city street, RT off) |
| `pc-4090-local` | Intel Family 6 Model 183 (Raptor Lake, 32T) | NVIDIA RTX 4090 (24138 MB VRAM, `0x10DE:0x2684`) | ~96 GB (`48979 MB` shared) | Windows, WinSDK `10.0.26100.0`, VS 2019 (`cl 19.29.30159`) & VS 2022, CMake 4.3.3, CUDA 13.2 / 12.3, Python 3.14 & 3.11, .NET | `rustc`/`cargo` NOT yet installed on PATH; `native/` builds via `vcvars64.bat` + `NMake Makefiles` |

---

## 4. Shared Experiment Naming & Environment Tagging

When logging runs or replay captures (stored *outside* the repo, e.g. `D:\victory-data\captures\...`), name runs consistently:

`<profile>-<yyyymmdd>-<short-topic>-<n>` (e.g., `4090-20261127-port1-ab-1`).

Record these core switch groups when reporting numbers:
- **Profile**: `--console series-x` vs `--console series-s`, `VICTORY_OUTPUT`, `VICTORY_FSR_4K`
- **Renderer Ports**: `VICTORY_PORT_CONSTANTS`, `VICTORY_PORT_DESCRIPTORS`, `VICTORY_PORT_CBV`, `VICTORY_PORT_GUARD`, `VICTORY_PORT_JOURNAL`
- **Threading / Async**: `VICTORY_THIN`, `VICTORY_ASYNC_SUBMIT`, `VICTORY_JOIN_PARTS`, `VICTORY_RELAY`, `VICTORY_CARD_STEPS`
- **Lighting**: `VICTORY_AMBIENT`, `VICTORY_BVH_BUILDER`
- **NVIDIA / Wave Tuning**: `VICTORY_HOST_WAVE`, `VICTORY_D3D12_BACKGROUND`, `VICTORY_INDIRECT_WIDE`

---

## 5. Local Machine Setup Checklist (`pc-4090-local`)

Observed gaps on `pc-4090-local` as of 2026-11-26:
1. **Rust toolchain (`1.98.1`, `x86_64-pc-windows-msvc`)**: Install `rustup` so `cargo build --locked --profile diag -p victory --features diagnostics` works for baseline measurements and Rust-side crate builds.
2. **`victory.toml`**: Copy [`victory.example.toml`](../victory.example.toml) to `victory.toml` (git-ignored) and point `game_dir`, `cache_dir`, `data_dir`, and `fidelityfx_dir` to paths **outside** `P:\project\VIctory-Belobug-AMD`.
3. **Git hooks**: Run `cargo xtask install-hooks` once Rust is installed so `cargo xtask policy` runs automatically on every commit.
4. **C++20 / Hybrid Native Toolchain**: VS 2022 (`Q:\microsoft Visual Studio\2022`), VS 2019, CMake, and Windows SDK `10.0.26100.0` are already present; verify `cl.exe` / `clang-cl` / Ninja availability via Developer Command Prompt when starting native runtime modules.
