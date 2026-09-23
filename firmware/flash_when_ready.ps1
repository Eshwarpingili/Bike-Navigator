# Start this first, then put the board in download mode (unplug, hold BURN, plug in,
# hold ~3 s, release). As soon as its USB port appears, this flashes BikeNav, so the
# few-second download-mode window is never missed.
#   .\flash_when_ready.ps1            flash BikeNav
#   .\flash_when_ready.ps1 -Backup    save the whole flash instead
param(
    [switch]$Backup,
    [int]$TimeoutSec = 300
)

$ErrorActionPreference = "Continue"
$existing = [System.IO.Ports.SerialPort]::GetPortNames()
Write-Output "Waiting for the board's port (already present: $($existing -join ', '))."
Write-Output "If the board is plugged in, unplug it first, then hold BURN and plug it in."

$deadline = (Get-Date).AddSeconds($TimeoutSec)
$port = $null
while (-not $port -and (Get-Date) -lt $deadline) {
    $current = [System.IO.Ports.SerialPort]::GetPortNames()
    # A port that vanished (board unplugged) counts as new when it comes back.
    $existing = $existing | Where-Object { $current -contains $_ }
    $port = $current | Where-Object { $existing -notcontains $_ } | Select-Object -First 1
    if (-not $port) { Start-Sleep -Milliseconds 50 }
}
if (-not $port) { Write-Output "timed out"; exit 2 }
Write-Output "$(Get-Date -Format HH:mm:ss) found $port"

if ($Backup) {
    & (Join-Path $PSScriptRoot "flash.ps1") -Port $port -Backup
} else {
    & (Join-Path $PSScriptRoot "flash.ps1") -Port $port
}
