# Flash the board over USB. Put it in download mode first: unplug it, hold BURN,
# plug it in, keep holding ~3 s, release. It stays in download mode only a few
# seconds unless the tool connects, so run the command right away (or use
# flash_when_ready.ps1, which waits for the port and starts instantly).
#
#   .\flash.ps1 -Port COM6 -Backup            save the whole 8 MB flash
#   .\flash.ps1 -Port COM6                    write BikeNav
#   .\flash.ps1 -Port COM6 -Restore backups\factory_music_demo_8MB.bin
#
# Each run needs a fresh BURN + plug-in. BLFlashCommand exits 0 even on failure,
# so success is judged from its output.
param(
    [Parameter(Mandatory = $true)][string]$Port,
    [switch]$Backup,
    [string]$Restore,
    [string]$Map
)

# The street map goes above everything the partition table claims. The SDK ships
# a 4 MB layout and this part is 8 MB, so the whole top half is free. Must match
# MAP_FLASH_ADDR in main/mapdata.h.
$MapAddress = "0x400000"

# Native tools print progress on stderr; failures are detected from their output.
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
$tool = Join-Path $root "third_party\aithinker_Ai-M6X_SDK\tools\bflb_tools\bouffalo_flash_cube\BLFlashCommand.exe"
$common = @("--interface=uart", "--port=$Port", "--baudrate=2000000", "--chipname=bl616")

function Invoke-Flasher([string[]]$extra) {
    $out = & $tool @common @extra 2>&1 | ForEach-Object { "$_" }
    $out | Out-File -Encoding utf8 (Join-Path $PSScriptRoot "flash.log")
    $out | Select-String -Pattern "shake hand|Verify|All Success|ErrorMsg|Finished" | Select-Object -Last 6 |
        ForEach-Object { $_.Line }
    if ($out -match "ErrorMsg|shake hand fail|Burn return with retry fail") { throw "flash tool failed, see flash.log" }
}

Push-Location $PSScriptRoot
try {
    if ($Backup) {
        $dir = Join-Path $PSScriptRoot "backups"
        New-Item -ItemType Directory -Force $dir | Out-Null
        $file = Join-Path $dir "flash_backup_$(Get-Date -Format yyyyMMdd_HHmmss).bin"
        Invoke-Flasher @("--flash", "--read", "--start=0x0", "--len=0x800000", "--file=$file")
        if (-not (Test-Path $file) -or (Get-Item $file).Length -ne 0x800000) { throw "backup file missing or short" }
        Get-Item $file | Select-Object FullName, Length
    } elseif ($Restore) {
        Invoke-Flasher @("--flash", "--write", "--start=0x0", "--file=$((Resolve-Path $Restore).Path)")
    } elseif ($Map) {
        $path = (Resolve-Path $Map).Path
        $size = (Get-Item $path).Length
        # 8 MB part, and the map starts at 4 MB: anything larger would run off
        # the end of the chip, which the flasher will not tell you.
        if ($size -gt (0x800000 - 0x400000)) { throw "map is $size bytes; only $((0x800000 - 0x400000)) fit above $MapAddress" }
        Write-Output "writing $([math]::Round($size / 1MB, 2)) MB of map at $MapAddress"
        Invoke-Flasher @("--flash", "--write", "--start=$MapAddress", "--file=$path")
    } else {
        if (-not (Test-Path build\build_out\bikenav_bl616.bin)) { throw "Build first: .\build.ps1" }
        Invoke-Flasher @("--config=flash_prog_cfg.ini")
    }
} finally {
    Pop-Location
}
