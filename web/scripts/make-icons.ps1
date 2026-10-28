Add-Type -AssemblyName System.Drawing

function Generate-Icon([string]$path, [int]$size, [bool]$transparent = $false) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic

    if (-not $transparent) {
        $bgRect = New-Object System.Drawing.Rectangle 0, 0, $size, $size
        $bgBrush = New-Object System.Drawing.Drawing2D.LinearGradientBrush $bgRect, `
            ([System.Drawing.Color]::FromArgb(255, 30, 16, 50)), `
            ([System.Drawing.Color]::FromArgb(255, 9, 8, 14)), `
            45.0
        $g.FillRectangle($bgBrush, $bgRect)
        $bgBrush.Dispose()
    } else {
        $g.Clear([System.Drawing.Color]::Transparent)
    }

    # Center swirl / circle
    $cx = $size / 2.0
    $cy = $size / 2.0
    $r = $size * 0.32
    $stroke = [Math]::Max(2.0, $size * 0.035)

    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(230, 190, 140, 255)), $stroke
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round

    # Draw 12 arcs
    for ($i = 0; $i -lt 12; $i++) {
        $angle = $i * (360.0 / 12.0)
        $g.TranslateTransform($cx, $cy)
        $g.RotateTransform($angle)
        $arcRect = New-Object System.Drawing.RectangleF (-$r), (-$r), ($r * 2), ($r * 2)
        $g.DrawArc($pen, $arcRect, 0.0, 18.0)
        $g.ResetTransform()
    }

    # Inner bright ring
    $innerPen = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(200, 240, 220, 255)), ($stroke * 0.6)
    $innerR = $r * 0.65
    $innerRect = New-Object System.Drawing.RectangleF ($cx - $innerR), ($cy - $innerR), ($innerR * 2), ($innerR * 2)
    $g.DrawEllipse($innerPen, $innerRect)

    # Core dot
    $coreBrush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 255, 255, 255))
    $dotR = $size * 0.04
    $g.FillEllipse($coreBrush, [float]($cx - $dotR), [float]($cy - $dotR), [float]($dotR * 2), [float]($dotR * 2))

    $dir = [System.IO.Path]::GetDirectoryName($path)
    if (-not (Test-Path $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)

    $coreBrush.Dispose()
    $innerPen.Dispose()
    $pen.Dispose()
    $g.Dispose()
    $bmp.Dispose()
    Write-Host "Created: $path ($size x $size)"
}

$base = "c:\Users\Konstantin\Desktop\selfvoy\web"
Generate-Icon "$base\build\icon.png" 512 $false
Generate-Icon "$base\assets\icon-only.png" 1024 $false
Generate-Icon "$base\assets\icon-foreground.png" 1024 $true
Generate-Icon "$base\assets\icon-background.png" 1024 $false
Generate-Icon "$base\assets\splash.png" 2048 $false
Generate-Icon "$base\assets\splash-dark.png" 2048 $false
