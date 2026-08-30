param(
    [ValidateSet('Debug','Release')]
    [string]$Configuration = 'Release',
    [ValidateSet('x64','arm64')]
    [string]$Arch = 'x64'
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
Push-Location $root
try {
    if ($Arch -eq 'arm64') {
        if ($Configuration -ne 'Release') { throw 'The provided ARM64 preset is Release-only.' }
        cmake --preset windows-arm64-release
        cmake --build --preset arm64-release
    } elseif ($Configuration -eq 'Debug') {
        cmake --preset windows-x64-debug
        cmake --build --preset debug
    } else {
        cmake --preset windows-x64-release
        cmake --build --preset release
    }
} finally {
    Pop-Location
}
