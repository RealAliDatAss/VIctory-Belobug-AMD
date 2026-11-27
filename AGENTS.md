# VIctory-Belobug-AMD Workspace Rules

## Hardware Profile: pc-4090-local
- **GPU**: NVIDIA GeForce RTX 4090 (24 GB VRAM, CUDA, ReBAR, PCIe 4.0 x16)
- **CPU**: Intel Raptor Lake (32 threads)
- **RAM**: ~96 GB DDR5
- This is NOT an AMD RDNA card. Never use RDNA-specific paths, wave sizes, or flags.
- Always use `VICTORY_HOST_WAVE=32` and `VICTORY_HOST_RDNA=0` for shader translation.
- Prefer `D3D12_HEAP_TYPE_GPU_UPLOAD` (ReBAR) over `D3D12_HEAP_TYPE_UPLOAD` for scratch and send buffers — GPU reads at ~4500 GB/s vs ~170 GB/s.

## Storage Rules
- **Never use `D:\`** for any purpose. The user has forbidden it.
- Use `P:\project\victory-data\` (sibling to the repo) for cache and data directories.
- Game files live at `P:\torr\GTA VI v1.0.2.1` (workspace #2).
- All game-derived data must be outside the repo tree (enforced by `xtask policy` and `docs/asset-policy.md`).

## VIctory Configuration
- `victory.toml` uses `[paths]` section (see `victory.example.toml` for format).
- Run `victory.exe config` to verify paths after any change.
- Shader translation: `victory.exe shaders --jobs 16` with `VICTORY_HOST_WAVE=32 VICTORY_HOST_RDNA=0`.
- 25,787 shaders translate (99.2% rate); 198 unsupported containers are expected.

## Build & Policy
- `cargo xtask policy` must pass before any commit (292+ files ok).
- Never commit game files, executables, shader dumps, or hex blobs > 512 chars.
- Files must be <= 640 KiB and use allowed extensions only.
- `.victory-data/` is git-ignored in the workspace.
