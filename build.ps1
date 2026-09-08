param(
    [switch]$Run,
    [switch]$Clean
)

# Builds the project with MSVC + Ninja. Handles the vcvars environment setup
# so you don't need a "Developer PowerShell" window.
#
#   .\build.ps1            build
#   .\build.ps1 -Run       build, then run
#   .\build.ps1 -Clean     delete the build dir first (full rebuild)

$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$buildDir = Join-Path $root 'build'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Install Visual Studio Build Tools with the C++ workload."
}

$vsPath = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -latest -format value -property installationPath
if (-not $vsPath) {
    throw "No MSVC C++ toolchain found. Install the 'Desktop development with C++' workload."
}

$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found at: $vcvars"
}

# Pull the MSVC environment (compiler, cmake, ninja) into this session.
Write-Host "Loading MSVC environment..." -ForegroundColor DarkGray
$envDump = cmd /c "call `"$vcvars`" > nul 2>&1 & set"
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw "MSVC environment did not load (cl.exe not on PATH after vcvars64)."
}

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Removing $buildDir" -ForegroundColor DarkGray
    Remove-Item -Recurse -Force $buildDir
}

# -Wno-deprecated hides a warning from raylib's own CMakeLists that we cannot fix
# upstream, and which PowerShell turns into a terminating error when piped.
cmake -S $root -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Debug -Wno-deprecated
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

$exe = Join-Path $buildDir 'normalish.exe'
Write-Host "Built: $exe" -ForegroundColor Green

if ($Run) {
    # Run from the project root so relative asset/shader paths resolve.
    Push-Location $root
    try { & $exe } finally { Pop-Location }
}
