# RTX 4090 Optimized Launch Script
# Target: >= 30 FPS (33.3 ms/frame) on pc-4090-local
#
# Based on:
#   - Live GPU memory benchmark (GPU_UPLOAD ReBAR reads at 4521 GB/s vs OpenAddr1 12.2 GB/s)
#   - Ecosystem research (ZJOU-VICTORY, fxceless, DexNom, cool_fox, stellarr-studio)
#   - Upstream play.ps1 defaults + pareto-residency tuning from Play-GtaVI.env
#
# Usage:
#   .\scripts\play-rtx4090.ps1 -Exe "P:\torr\GTA VI v1.0.2.1\game_scarlett_master.exe"
#   .\scripts\play-rtx4090.ps1 -Exe "P:\torr\GTA VI v1.0.2.1\game_scarlett_master.exe" -Measure
#   .\scripts\play-rtx4090.ps1 -Exe "P:\torr\GTA VI v1.0.2.1\game_scarlett_master.exe" -Build release

param(
    [Parameter(Mandatory)][string]$Exe,
    [switch]$Measure,
    [switch]$Hud,
    [switch]$Safe,
    [ValidateSet('diag', 'release')][string]$Build = 'release',
    [hashtable]$More = @{}
)

$settings = @{
    # === Core pareto-residency (from Play-GtaVI.env / play.ps1) ===
    VICTORY_TEXTURE_NO_WAIT     = '1'
    # Relay stage 2 (valid values are 1 or 2; enables late_stage=3 & ASYNC_SUBMIT)
    VICTORY_RELAY               = '2'
    VICTORY_WIDE_BACK           = '1'
    VICTORY_CARD_REHASH_RUNS    = '1'
    VICTORY_JOIN_COPIES         = '1'
    VICTORY_INDIRECT_WINDOW     = '1'
    VICTORY_RELAY_COPY          = '1'
    VICTORY_CARD_STEPS          = '2'
    VICTORY_INDIRECT_BATCH      = '2'
    VICTORY_MAP_THREADS         = '1'
    VICTORY_TITLE_PRIORITIES    = '0'
    VICTORY_CARD_AWAKE          = '1'
    VICTORY_SHOW_AFTER          = '1'
    VICTORY_AMBIENT             = '0.8'

    # === RTX 4090 Specific: GPU_UPLOAD ReBAR & 24 GB VRAM Budgets ===
    # 24 GB VRAM (23,370 MB DXGI budget) + 96 GB DDR5 (48,211 MB shared)
    VICTORY_WINDOWS_BAR_MB      = '8192'
    VICTORY_CARD_BUFFERS_MB     = '8192'
    VICTORY_MEMORY_CARD_MB      = '8192'
    VICTORY_CARD_ROOM_FOR_TARGETS_MB = '4096'
    VICTORY_TEXTURE_ROOM_PERCENT = '90'
    VICTORY_KEEP_FREED          = '1'

    # === Expanded Console Memory Budget (vs Series X 10 + 3.5 GB) ===
    VICTORY_TITLE_VRAM_MB       = '20480'
    VICTORY_TITLE_RAM_MB        = '16384'

    # === Wave32 (NVIDIA native) ===
    VICTORY_HOST_WAVE           = '32'
    VICTORY_HOST_RDNA           = '0'
    VICTORY_HOST_VENDOR         = 'nvidia'

    # === CPU Submit Optimizations (Intel Raptor Lake 32T) ===
    VICTORY_THIN                = '1'
    VICTORY_JOIN_PARTS          = '8'
    VICTORY_UPLOAD_AHEAD        = '1'
    VICTORY_FAST_CORES          = '2'
    VICTORY_LANE_CORE           = '1'
    VICTORY_LEVELS_ON_DEMAND    = '1'
    VICTORY_CARD_BUFFERS_MIN_KB = '64'
    VICTORY_LDS_FLAGS           = '0'
    VICTORY_D3D12_BACKGROUND    = '0'
    # Send buffers in GPU_UPLOAD: copies stay on the card
    VICTORY_SEND_BAR            = '1'

    # === Residency ===
    VICTORY_RESIDENCY           = '1'
    VICTORY_FALLBACK_HEAPS      = '1'

    # === Frame Pacing ===
    # 0 = unlocked (max FPS in menu + uncapped in world/cutscenes)
    VICTORY_FRAME_INTERVAL_MS   = '0'

    # === Ray Tracing (RTX 4090 HW RT) ===
    VICTORY_BVH_BUILDER         = '1'

    # === Worker threads (Raptor Lake: 8P+16E = 32 threads) ===
    VICTORY_LAND_THREADS        = '4'
    VICTORY_THIN_THREADS        = '8'
}

if ($Build -eq 'diag') { $settings.VICTORY_D3D12_DRED = 'light' }

if ($Safe) {
    $settings.Remove('VICTORY_THIN')
    $settings.Remove('VICTORY_JOIN_PARTS')
    $settings.Remove('VICTORY_UPLOAD_AHEAD')
    $settings.Remove('VICTORY_WINDOWS_BAR_MB')
    $settings.Remove('VICTORY_LDS_FLAGS')
    $settings.Remove('VICTORY_SEND_BAR')
}

if ($Hud) {
    $settings.VICTORY_PANEL = '1'
}

if ($Measure) {
    $settings.VICTORY_LOG       = 'info'
    # NOTE: GPU_TIMING forces late_stage=0 which disables ASYNC_SUBMIT
    # Only enable it for dedicated profiling, not normal gameplay measurement
    $settings.VICTORY_TIMER_LOG = '1'
    $settings.VICTORY_PANEL     = '1'
} else {
    $settings.VICTORY_LOG       = 'warn'
}

foreach ($k in $More.Keys) { $settings[$k] = $More[$k] }

$victory = Join-Path $PSScriptRoot "..\target\$Build\victory.exe"
if (-not (Test-Path $victory)) { Write-Host "No $victory. Build it first."; exit 1 }
if (-not (Test-Path $Exe)) { Write-Host "No $Exe."; exit 1 }

# Clean inherited VICTORY_ env vars (keep path-related ones)
$paths = 'VICTORY_CONFIG', 'VICTORY_GAME_DIR', 'VICTORY_CACHE_DIR', 'VICTORY_DATA_DIR', 'VICTORY_FIDELITYFX_DIR'
$inherited = @(Get-ChildItem Env:VICTORY_* | Where-Object { $_.Name -notin $paths })
foreach ($entry in $inherited) { Remove-Item "Env:$($entry.Name)" }
foreach ($k in $settings.Keys) { Set-Item "Env:$k" $settings[$k] }

Write-Host "=== RTX 4090 Optimized Launch ===" -ForegroundColor Cyan
Write-Host "Build:    $Build"
Write-Host "Target:   30+ FPS (33.3 ms/frame)"
Write-Host "VRAM BAR: $($settings['VICTORY_WINDOWS_BAR_MB']) MB (GPU_UPLOAD ReBAR)"
Write-Host "Wave:     $($settings['VICTORY_HOST_WAVE']) (NVIDIA native)"
Write-Host "Measure:  $Measure"
Write-Host ""

$Exe = (Resolve-Path $Exe).Path
Push-Location (Join-Path $PSScriptRoot '..')
try { & $victory run $Exe --execute --console series-x } finally { Pop-Location }
