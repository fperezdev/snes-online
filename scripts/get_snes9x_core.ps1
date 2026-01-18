param(
  [string]$OutDir = "$(Join-Path $PSScriptRoot "..\cores")",
  [string]$Url = "https://buildbot.libretro.com/nightly/windows/x86_64/latest/snes9x_libretro.dll.zip"
)

$ErrorActionPreference = 'Stop'

$OutDir = (Resolve-Path $OutDir).Path
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$tmp = Join-Path $env:TEMP "snes9x_libretro.dll.zip"
$extract = Join-Path $env:TEMP "snes9x_libretro_extract"

Write-Host "Downloading: $Url"
Invoke-WebRequest -Uri $Url -OutFile $tmp

if (Test-Path $extract) { Remove-Item -Recurse -Force $extract }
New-Item -ItemType Directory -Force -Path $extract | Out-Null

Expand-Archive -Path $tmp -DestinationPath $extract -Force

$dll = Get-ChildItem -Path $extract -Filter "snes9x_libretro.dll" -Recurse | Select-Object -First 1
if (-not $dll) {
  throw "Download did not contain snes9x_libretro.dll"
}

$dest = Join-Path $OutDir "snes9x_libretro.dll"
Copy-Item -Force -Path $dll.FullName -Destination $dest

Write-Host "Saved: $dest"
