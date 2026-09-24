# Start this first, then put the board in download mode (unplug, hold BURN, plug in,
# hold ~3 s, release). As soon as the bootloader appears, this flashes BikeNav, so the
# few-second download-mode window is never missed.
#   .\flash_when_ready.ps1            flash BikeNav
#   .\flash_when_ready.ps1 -Backup    save the whole flash instead
#
# The board is found by what it is, not by being a port that was not there before:
# an earlier version waited for a *new* COM port, so starting it after the board
# was already in download mode meant waiting for something that had already
# happened. In download mode the BL618 enumerates as 349B:6180.
param(
    [switch]$Backup,
    [int]$TimeoutSec = 300
)

$ErrorActionPreference = "Continue"
$bootloaderId = "USB\VID_349B&PID_6180"

function Find-BoardPort {
    $device = Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.DeviceID -like "$bootloaderId*" -and $_.Name -match "\(COM\d+\)" } |
        Select-Object -First 1
    if ($null -eq $device) { return $null }
    if ($device.Name -match "\((COM\d+)\)") { return $Matches[1] }
    return $null
}

$port = Find-BoardPort
if ($port) {
    Write-Output "Board already in download mode on $port."
} else {
    Write-Output "Waiting for the board. Unplug it, hold BURN, plug it in, hold ~3 s."
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while (-not $port -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 100
        $port = Find-BoardPort
    }
}
if (-not $port) { Write-Output "timed out"; exit 2 }
Write-Output "$(Get-Date -Format HH:mm:ss) found $port"

if ($Backup) {
    & (Join-Path $PSScriptRoot "flash.ps1") -Port $port -Backup
} else {
    & (Join-Path $PSScriptRoot "flash.ps1") -Port $port
}
