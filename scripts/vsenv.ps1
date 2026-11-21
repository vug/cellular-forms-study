# Dot-source this to get the build environment in the current PowerShell session:
#   . scripts\vsenv.ps1
# It imports the MSVC x64 environment (vcvars64) from the newest Visual Studio, puts the
# Ninja that ships with Visual Studio on PATH, and points CUDA_PATH at CUDA 13.x and
# VULKAN_SDK at the newest installed SDK. Nothing outside this process is changed.

$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "No Visual Studio with the C++ x64 tools was found." }

$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}

$ninjaDir = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
if (Test-Path (Join-Path $ninjaDir 'ninja.exe')) { $env:PATH = "$ninjaDir;$env:PATH" }

# CUDA: newest installed 13.x toolkit. A shell opened before the install may still carry the
# old CUDA_PATH, so do not trust the inherited value.
$cudaRoot = 'C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA'
$cuda = Get-ChildItem $cudaRoot -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^v(\d+)\.(\d+)$' } |
    Sort-Object { [version]($_.Name.Substring(1)) } -Descending | Select-Object -First 1
if ($cuda) {
    $env:CUDA_PATH = $cuda.FullName
    $env:PATH = "$($cuda.FullName)\bin;$env:PATH"
}

# Vulkan SDK: newest installed.
$vk = Get-ChildItem 'C:\VulkanSDK' -Directory -ErrorAction SilentlyContinue |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if ($vk) {
    $env:VULKAN_SDK = $vk.FullName
    $env:PATH = "$($vk.FullName)\Bin;$env:PATH"
}

$ErrorActionPreference = 'Continue'
