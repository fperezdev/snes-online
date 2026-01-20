param(
    [string]$SourcePng = "$PSScriptRoot\..\platform\android\app\src\main\res\icons\app-icon.png",
    [string]$ResDir = "$PSScriptRoot\..\platform\android\app\src\main\res"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $SourcePng)) {
    throw "Source icon not found: $SourcePng`nPlace app-icon.png at repo root or pass -SourcePng." 
}

Add-Type -AssemblyName System.Drawing

function Write-ResizedPng([string]$outPath, [int]$px) {
    $src = [System.Drawing.Image]::FromFile($SourcePng)
    try {
        $bmp = New-Object System.Drawing.Bitmap($px, $px, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            try {
                $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
                $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                $g.CompositingQuality = [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
                $g.Clear([System.Drawing.Color]::Transparent)
                $g.DrawImage($src, 0, 0, $px, $px)
            } finally {
                $g.Dispose()
            }
            $dir = Split-Path -Parent $outPath
            New-Item -ItemType Directory -Force -Path $dir | Out-Null
            $bmp.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
        } finally {
            $bmp.Dispose()
        }
    } finally {
        $src.Dispose()
    }
}

# Launcher icon sizes (px)
$sizes = @{
    'mipmap-mdpi' = 48
    'mipmap-hdpi' = 72
    'mipmap-xhdpi' = 96
    'mipmap-xxhdpi' = 144
    'mipmap-xxxhdpi' = 192
}

foreach ($kv in $sizes.GetEnumerator()) {
    $dirName = $kv.Key
    $px = [int]$kv.Value

    $ic = Join-Path $ResDir "$dirName\ic_launcher.png"
    $icRound = Join-Path $ResDir "$dirName\ic_launcher_round.png"

    Write-ResizedPng $ic $px
    Write-ResizedPng $icRound $px
}

Write-Host "Applied app-icon.png to launcher icons under: $ResDir"