---
name: victory-nvidia-launch
description: >-
  Launch and optimize GTA VI recompilation on NVIDIA RTX GPUs (especially RTX 4090).
  Use when running, profiling, or tuning VIctory with game_scarlett_master.exe on
  an NVIDIA + Intel system. Covers env var tuning, shader pre-translation, and
  frame timing measurement.
---

# VIctory NVIDIA RTX Launch Procedure

## Pre-flight
1. Verify config: `target\diag\victory.exe config` → game_dir, cache_dir, data_dir all valid
2. Pre-translate shaders (once per translator version):
   ```
   set VICTORY_HOST_WAVE=32
   set VICTORY_HOST_RDNA=0
   target\diag\victory.exe shaders --jobs 16
   ```
3. Build both profiles: `cargo build --profile diag -p victory --features diagnostics` and `cargo build --release -p victory`

## Launch (RTX 4090 Optimized)
Use `scripts\play-rtx4090.ps1`:
```powershell
.\scripts\play-rtx4090.ps1 -Exe "P:\torr\GTA VI v1.0.2.1\game_scarlett_master.exe"
.\scripts\play-rtx4090.ps1 -Exe "P:\torr\GTA VI v1.0.2.1\game_scarlett_master.exe" -Measure
```

## Key RTX 4090 Environment Variables
| Variable | Value | Why |
|---|---|---|
| VICTORY_HOST_WAVE | 32 | NVIDIA native wave size (not RDNA Wave64) |
| VICTORY_HOST_RDNA | 0 | Disable RDNA-specific shader paths |
| VICTORY_HOST_VENDOR | nvidia | Report NVIDIA vendor to title's DXGI adapter (auto-detects if unset) |
| VICTORY_WINDOWS_BAR_MB | 8192 | GPU_UPLOAD ReBAR windows |
| VICTORY_SEND_BAR | 1 | Send buffers in VRAM |
| VICTORY_CARD_BUFFERS_MB | 8192 | Avoid fragmentation (< 1024 -> 150 ms/frame) |
| VICTORY_MEMORY_CARD_MB | 8192 | Expand by-address card buffers on 24 GB VRAM |
| VICTORY_CARD_ROOM_FOR_TARGETS_MB | 4096 | Reserve VRAM for render targets & RT BVH |
| VICTORY_TEXTURE_ROOM_PERCENT | 90 | Use up to 90% of 23.37 GB DXGI VRAM budget |
| VICTORY_KEEP_FREED | 1 | Keep up to 1 GiB of GPU_UPLOAD scratch pooled |
| VICTORY_TITLE_VRAM_MB | 20480 | Expand title-visible GPU-optimal budget (20 GB) |
| VICTORY_TITLE_RAM_MB | 16384 | Expand title-visible standard budget (16 GB) |
| VICTORY_RELAY | 2 | Relay stage 2 (valid values: 1 or 2; enables ASYNC_SUBMIT) |
| VICTORY_THIN | 1 | Thin submit (enables ASYNC_SUBMIT automatically) |
| VICTORY_FAST_CORES | 2 | Keep submit + relay helpers on Raptor Lake P-cores |
| VICTORY_LANE_CORE | 1 | Give submitting thread its own dedicated P-core |
| VICTORY_JOIN_PARTS | 8 | Batch list joins |
| VICTORY_LDS_FLAGS | 0 | Disable LDS flags (no RDNA LDS banks) |
| VICTORY_D3D12_BACKGROUND | 0 | No background shader work needed on NVIDIA |
| VICTORY_BVH_BUILDER | 1 | Enable ray-traced indirect lighting |
| VICTORY_FRAME_INTERVAL_MS | 0 | Uncapped FPS in menu, world, and cutscenes |

## Measurement & Profiling
- `-Measure` flag enables `VICTORY_TIMER_LOG=1` and the HUD panel. Does NOT enable `VICTORY_GPU_TIMING` (that disables ASYNC_SUBMIT).
- Replay: `target\diag\victory.exe replay P:\project\victory-data\captures\rtx4090-baseline --timing detailed --compare`
- Key metrics: victory-submit ms (CPU), GPU list time (ms), total frame time (ms)

## Known Issues
- 198 shaders (of 25,985) don't translate — 195 are unsupported container types, 3 are lane mask edge cases
- OpenAddr1 (WB SysRAM) shader reads hit 12.2 GB/s wall on NVIDIA — always prefer GPU_UPLOAD
- If frame time > 33.3 ms, check CPU submit bottleneck first (stellarr-studio found this is the main RTX 4090 wall)
