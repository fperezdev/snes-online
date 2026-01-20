param(
    [string]$OutDir = "$PSScriptRoot\..\platform\android\app\src\main\res\drawable-nodpi",
    [int]$ImgSize = 256
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Some hosts may pass scalar parameters as 1-element arrays; normalize.
if ($ImgSize -is [System.Array]) { $ImgSize = $ImgSize[0] }
$ImgSize = [int]$ImgSize

Add-Type -AssemblyName System.Drawing

function New-Color([int]$a, [int]$r, [int]$g, [int]$b) {
    return [System.Drawing.Color]::FromArgb($a, $r, $g, $b)
}

function New-RoundRectPath([float]$x, [float]$y, [float]$w, [float]$h, [float]$radius) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    if ($radius -le 0) {
        $path.AddRectangle((New-Object System.Drawing.RectangleF($x, $y, $w, $h)))
        return $path
    }

    $d = $radius * 2
    $path.AddArc($x, $y, $d, $d, 180, 90)
    $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

function Save-RoundButton([string]$OutPath, [System.Drawing.Color]$Fill, [System.Drawing.Color]$Stroke) {
    $sz = [single]$ImgSize
    $bmp = New-Object System.Drawing.Bitmap($ImgSize, $ImgSize, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    $pad = [single]([Math]::Round($sz * 0.10))
    $wh = [single]($sz - ([single]2.0 * $pad))
    $rect = New-Object System.Drawing.RectangleF -ArgumentList @($pad, $pad, $wh, $wh)

    $brush = New-Object System.Drawing.SolidBrush($Fill)
    $pen = New-Object System.Drawing.Pen($Stroke, [float]($sz * 0.05))

    $g.FillEllipse($brush, $rect)
    $g.DrawEllipse($pen, $rect)

    $g.Dispose(); $bmp.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
}

function Save-RoundedRect([string]$OutPath, [System.Drawing.Color]$Fill, [System.Drawing.Color]$Stroke, [float]$RadiusRatio) {
    $sz = [single]$ImgSize
    $bmp = New-Object System.Drawing.Bitmap($ImgSize, $ImgSize, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    $pad = [single]($sz * 0.10)
    $x = $pad; $y = $pad
    $w = [single]($sz - ([single]2.0 * $pad)); $h = [single]($sz - ([single]2.0 * $pad))

    $r = [float]([Math]::Min($w, $h) * $RadiusRatio)
    $path = New-RoundRectPath $x $y $w $h $r

    $brush = New-Object System.Drawing.SolidBrush($Fill)
    $pen = New-Object System.Drawing.Pen($Stroke, [float]($sz * 0.05))

    $g.FillPath($brush, $path)
    $g.DrawPath($pen, $path)

    $path.Dispose(); $g.Dispose(); $bmp.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
}

function Save-Pill([string]$OutPath, [System.Drawing.Color]$Fill, [System.Drawing.Color]$Stroke) {
    $sz = [single]$ImgSize
    $bmp = New-Object System.Drawing.Bitmap($ImgSize, $ImgSize, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    $padX = [single]($sz * 0.08)
    $padY = [single]($sz * 0.28)
    $x = $padX; $y = $padY
    $w = [single]($sz - ([single]2.0 * $padX)); $h = [single]($sz - ([single]2.0 * $padY))

    $r = [float]($h * 0.5)
    $path = New-RoundRectPath $x $y $w $h $r

    $brush = New-Object System.Drawing.SolidBrush($Fill)
    $pen = New-Object System.Drawing.Pen($Stroke, [float]($sz * 0.05))

    $g.FillPath($brush, $path)
    $g.DrawPath($pen, $path)

    $path.Dispose(); $g.Dispose(); $bmp.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
}

function Save-Dpad([string]$OutPath, [System.Drawing.Color]$Fill, [System.Drawing.Color]$Stroke) {
    $sz = [single]$ImgSize
    $bmp = New-Object System.Drawing.Bitmap($ImgSize, $ImgSize, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    $pad = [single]($sz * 0.10)
    $wh = [single]($sz - ([single]2.0 * $pad))
    $outer = New-Object System.Drawing.RectangleF -ArgumentList @($pad, $pad, $wh, $wh)

    # Base circle
    $brush = New-Object System.Drawing.SolidBrush($Fill)
    $pen = New-Object System.Drawing.Pen($Stroke, [float]($sz * 0.05))
    $g.FillEllipse($brush, $outer)
    $g.DrawEllipse($pen, $outer)

    # Cross
    $cx = [single]($sz / 2.0)
    $cy = [single]($sz / 2.0)
    $arm = [float]($sz * 0.18)
    $th = [float]($sz * 0.13)

    $crossColor = [System.Drawing.Color]::FromArgb(170, $Stroke.R, $Stroke.G, $Stroke.B)
    $crossBrush = New-Object System.Drawing.SolidBrush($crossColor)

    # vertical bar
    $g.FillRectangle($crossBrush, $cx - $th/2, $cy - $arm, $th, $arm*2)
    # horizontal bar
    $g.FillRectangle($crossBrush, $cx - $arm, $cy - $th/2, $arm*2, $th)

    $g.Dispose(); $bmp.Save($OutPath, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$styles = @(
    @{ name = 'snes'; fill = (New-Color 210 20 20 20); stroke = (New-Color 255 167 142 255) },
    @{ name = 'neon'; fill = (New-Color 210 10 10 18); stroke = (New-Color 255 0 255 234) },
    @{ name = 'mono'; fill = (New-Color 210 8 8 8); stroke = (New-Color 255 230 230 230) }
)

foreach ($s in $styles) {
    $n = $s.name
    $fill = $s.fill
    $stroke = $s.stroke

    Save-Dpad (Join-Path $OutDir "bg_dpad_${n}.png") $fill $stroke
    Save-RoundButton (Join-Path $OutDir "bg_btn_round_${n}.png") $fill $stroke
    Save-RoundedRect (Join-Path $OutDir "bg_btn_rect_${n}.png") $fill $stroke 0.12
    Save-Pill (Join-Path $OutDir "bg_btn_pill_${n}.png") $fill $stroke
}

Write-Host "Generated controller style PNGs in: $OutDir"