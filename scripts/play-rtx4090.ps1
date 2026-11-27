# RTX 4090 Optimized Launch Script
# Target: >= 30 FPS (33.3 ms/frame) on pc-4090-local with clean textures & stability
#
# Usage:
#   .\scripts\play-rtx4090.ps1 -Exe "P:\torr\GTA VI v1.0.2.1\game_scarlett_master.exe"
#   .\scripts\play-rtx4090.ps1 -Exe "..." -Output 1920x1080
#   .\scripts\play-rtx4090.ps1 -Exe "..." -RayTracing        # enables BVH builder (~40 ms GPU cost)
#   .\scripts\play-rtx4090.ps1 -Exe "..." -Hud -Measure

param(
    [Parameter(Mandatory)][string]$Exe,
    [ValidateRange(1, 4)][int]$Tier = 1,
    [ValidateSet('1280x720', '1664x936', '1920x1080', '2560x1440', '3840x2160')][string]$Output = '2560x1440',
    [ValidateSet('quality', 'balanced', 'performance')][string]$Fsr4K,
    [switch]$RayTracing,
    [switch]$AllPorts,
    [switch]$Uncapped,
    [switch]$Measure,
    [switch]$Hud,
    [switch]$Safe,
    [ValidateSet('diag', 'release')][string]$Build = 'release',
    [hashtable]$More = @{}
)

$settings = @{
    # === Core pareto-residency (from play.ps1) ===
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
    # Screen-space indirect light estimate (active when -RayTracing is off)
    VICTORY_AMBIENT             = '0.8'

    # === Renderer Ports (safe CPU wins that match existing shader cache) ===
    VICTORY_PORT_DESCRIPTORS    = '1'
    VICTORY_PORT_GUARD          = '1'
    VICTORY_PORT_JOURNAL        = '1'
    # Port 1: title's 32-bit graphics heap in-place (skinning buffer & constant ring)
    VICTORY_PORT_CONSTANTS      = '1'

    # === Skinning, Vertex Streams & Texture Streaming ===
    # Allow full cutscene mesh vertex streams & UAV buffers (prevents truncated meshes)
    VICTORY_ROOT_VIEW_MB        = '32'
    VICTORY_ROOT_UAV_MB         = '32'
    # Stream high-detail mips on demand
    VICTORY_LEVELS_ON_DEMAND    = '1'

    # === Wave32 (NVIDIA native; matches pre-translated shader cache) ===
    VICTORY_HOST_WAVE           = '32'
    VICTORY_HOST_RDNA           = '0'
    VICTORY_HOST_VENDOR         = 'nvidia'

    # === CPU Submit & Threading (Intel Raptor Lake 32T) ===
    VICTORY_THIN                = '1'
    VICTORY_JOIN_PARTS          = '8'
    VICTORY_UPLOAD_AHEAD        = '1'
    VICTORY_FAST_CORES          = '2'
    VICTORY_LANE_CORE           = '1'
    VICTORY_LAND_THREADS        = '4'
    VICTORY_THIN_THREADS        = '8'
    VICTORY_CARD_BUFFERS_MIN_KB = '64'
    VICTORY_KEEP_FREED          = '1'

    # === Residency & Frame Pacing ===
    VICTORY_RESIDENCY           = '1'
    # 16.667 ms (60 FPS target) avoids spin-polling contention caused by 0
    VICTORY_FRAME_INTERVAL_MS   = '16.667'
    VICTORY_OUTPUT              = $Output
}

# Tiers scale card buffer/VRAM room cleanly WITHOUT starving textures (>=90% room)
# or wasting GBs of VRAM in GPU_UPLOAD windows (which caused 3s texture sweeps)
$tiers = @{
    1 = @{
        # Balanced default: full room for world textures, Series X native title budget
        VICTORY_CARD_BUFFERS_MB          = '2048'
        VICTORY_CARD_ROOM_FOR_TARGETS_MB = '3072'
        VICTORY_TEXTURE_ROOM_PERCENT     = '92'
    }
    2 = @{
        VICTORY_CARD_BUFFERS_MB          = '4096'
        VICTORY_CARD_ROOM_FOR_TARGETS_MB = '3072'
        VICTORY_TEXTURE_ROOM_PERCENT     = '92'
    }
    3 = @{
        VICTORY_CARD_BUFFERS_MB          = '4096'
        VICTORY_CARD_ROOM_FOR_TARGETS_MB = '3072'
        VICTORY_TEXTURE_ROOM_PERCENT     = '94'
        VICTORY_TITLE_VRAM_MB            = '16384'
        VICTORY_TITLE_RAM_MB             = '8192'
    }
    4 = @{
        VICTORY_CARD_BUFFERS_MB          = '6144'
        VICTORY_CARD_ROOM_FOR_TARGETS_MB = '3072'
        VICTORY_TEXTURE_ROOM_PERCENT     = '95'
        VICTORY_TITLE_VRAM_MB            = '20480'
        VICTORY_TITLE_RAM_MB             = '12288'
    }
}
foreach ($k in $tiers[$Tier].Keys) { $settings[$k] = $tiers[$Tier][$k] }

if ($RayTracing) {
    $settings.VICTORY_BVH_BUILDER = '1'
    # BVH requires large card arenas; raise card arena cap while keeping texture room healthy
    if (-not $settings.ContainsKey('VICTORY_MEMORY_CARD_MB')) {
        $settings.VICTORY_MEMORY_CARD_MB = '4096'
    }
}

if ($AllPorts) {
    # Note: VICTORY_PORT_CBV=1 uses its own shader cache key; warm/translate shaders with it set
    $settings.VICTORY_PORT_CONSTANTS = '1'
    $settings.VICTORY_PORT_CBV       = '1'
}

if ($Fsr4K) {
    $settings.Remove('VICTORY_OUTPUT')
    $settings.VICTORY_FSR_4K = $Fsr4K.ToLowerInvariant()
}

if ($Uncapped) {
    $settings.VICTORY_FRAME_INTERVAL_MS = '0'
}

if ($Build -eq 'diag') { $settings.VICTORY_D3D12_DRED = 'light' }

if ($Safe) {
    $settings.Remove('VICTORY_THIN')
    $settings.Remove('VICTORY_JOIN_PARTS')
    $settings.Remove('VICTORY_UPLOAD_AHEAD')
    $settings.Remove('VICTORY_PORT_DESCRIPTORS')
    $settings.Remove('VICTORY_PORT_GUARD')
    $settings.Remove('VICTORY_PORT_JOURNAL')
    $settings.Remove('VICTORY_PORT_CONSTANTS')
    $settings.Remove('VICTORY_PORT_CBV')
    $settings.VICTORY_UNDER_WIDE = '0'
    $settings.VICTORY_LAND_ECHO  = '0'
}

if ($Hud) {
    $settings.VICTORY_PANEL = '1'
}

if ($Measure) {
    $settings.VICTORY_LOG       = 'info'
    # NOTE: GPU_TIMING forces late_stage=0 which disables ASYNC_SUBMIT
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

Write-Host "=== nVIctory-RTX (RTX 4090) Launch ===" -ForegroundColor Cyan
Write-Host "Build:    $Build  |  Tier: $Tier / 4  |  Output: $(if ($Fsr4K) { "FSR 4K ($Fsr4K)" } else { $Output })"
Write-Host "RT BVH:   $([bool]$RayTracing) (ambient=$($settings['VICTORY_AMBIENT']))  |  Ports: desc=$($settings['VICTORY_PORT_DESCRIPTORS']) guard=$($settings['VICTORY_PORT_GUARD']) journal=$($settings['VICTORY_PORT_JOURNAL'])"
Write-Host "Textures: $($settings['VICTORY_TEXTURE_ROOM_PERCENT'])% room  |  Card Buffers: $($settings['VICTORY_CARD_BUFFERS_MB']) MB"
Write-Host "Wave:     $($settings['VICTORY_HOST_WAVE']) (NVIDIA native)  |  Frame interval: $($settings['VICTORY_FRAME_INTERVAL_MS']) ms"
Write-Host ""

$Exe = (Resolve-Path $Exe).Path
Push-Location (Join-Path $PSScriptRoot '..')
try { & $victory run $Exe --execute --console series-x } finally { Pop-Location }
