# Build the BikeNav firmware with the SDK's bundled make/cmake and the T-Head RISC-V compiler.
#   .\build.ps1          incremental build
#   .\build.ps1 -Clean   full rebuild
param([switch]$Clean)

# Native tools print progress on stderr; failures are detected via $LASTEXITCODE.
$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot
$sdk = Join-Path $root "third_party\aithinker_Ai-M6X_SDK"
$toolchain = Join-Path $root "third_party\toolchain_gcc_t-head_windows\bin"

if (-not (Test-Path (Join-Path $sdk ".aipi_patched"))) {
    throw "SDK not patched yet. Run: bash firmware/tools/patch_sdk.sh"
}

$env:PATH = "$toolchain;$sdk\tools\make;$sdk\tools\cmake\bin;$sdk\tools\ninja;$env:SystemRoot\System32;$env:SystemRoot"
Push-Location $PSScriptRoot
try {
    if ($Clean -and (Test-Path build)) { Remove-Item -Recurse -Force build }
    & "$sdk\tools\make\make.exe"
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
    Get-Item build\build_out\bikenav_bl616.bin | Select-Object Name, Length
} finally {
    Pop-Location
}
