$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot '..\plugin-sdk\examples\usage-meters'
$target = Join-Path $env:LOCALAPPDATA 'Isle\plugins\usage-meters'
New-Item -ItemType Directory -Force -Path $target | Out-Null
Copy-Item -Force (Join-Path $source '*') $target
Write-Host "Installed example plugin to $target"
Write-Host 'Restart Isle to load it.'
