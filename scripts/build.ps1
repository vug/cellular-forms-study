# Configure (when needed) and build with CMake + Ninja + MSVC.
#   scripts\build.ps1                      # release, all targets
#   scripts\build.ps1 -Preset debug
#   scripts\build.ps1 -Target cf_tests
#   scripts\build.ps1 -Fresh               # reconfigure from scratch
param(
    [string]$Preset = 'release',
    [string[]]$Target = @(),
    [switch]$Fresh
)

$repo = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'vsenv.ps1')

Push-Location $repo
try {
    $buildDir = Join-Path $repo "build\$Preset"
    if ($Fresh -or -not (Test-Path (Join-Path $buildDir 'build.ninja'))) {
        $cfg = @('--preset', $Preset)
        if ($Fresh) { $cfg += '--fresh' }
        cmake @cfg
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
    }
    $bld = @('--build', '--preset', $Preset)
    if ($Target.Count -gt 0) { $bld += '--target'; $bld += $Target }
    cmake @bld
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }
}
finally {
    Pop-Location
}
