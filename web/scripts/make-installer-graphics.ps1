# Generates high-quality branded graphics for the NSIS installer and uninstaller:
# - build/installerSidebar.bmp (164x314)
# - build/uninstallerSidebar.bmp (164x314)
# - build/installerHeader.bmp (150x57)

Add-Type -AssemblyName System.Drawing

function Build-Sidebar {
    param(
        [string]$OutputPath,
        [bool]$IsUninstall = $false
    )
    
    $w = 164
    $h = 314
    $bmp = [System.Drawing.Bitmap]::new($w, $h, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::ClearTypeGridFit

    # Dark background
    $topColor = [System.Drawing.Color]::FromArgb(10, 8, 18)
    $botColor = if ($IsUninstall) { [System.Drawing.Color]::FromArgb(26, 12, 22) } else { [System.Drawing.Color]::FromArgb(22, 14, 44) }
    $rect = [System.Drawing.Rectangle]::new(0, 0, $w, $h)
    $brush = [System.Drawing.Drawing2D.LinearGradientBrush]::new($rect, $topColor, $botColor, [System.Drawing.Drawing2D.LinearGradientMode]::Vertical)
    $g.FillRectangle($brush, $rect)
    $brush.Dispose()

    # Aurora glowing ellipses
    $aurora1Color = [System.Drawing.Color]::FromArgb(35, 120, 70, 240)
    $aurora1 = [System.Drawing.SolidBrush]::new($aurora1Color)
    $g.FillEllipse($aurora1, -30, 20, 220, 150)
    $aurora1.Dispose()

    $aurora2Color = if ($IsUninstall) { [System.Drawing.Color]::FromArgb(35, 235, 60, 110) } else { [System.Drawing.Color]::FromArgb(28, 60, 160, 255) }
    $aurora2 = [System.Drawing.SolidBrush]::new($aurora2Color)
    $g.FillEllipse($aurora2, 0, 170, 180, 140)
    $aurora2.Dispose()

    # Draw App Icon (from build/icon.png)
    $iconPath = Join-Path $PSScriptRoot "..\build\icon.png"
    if (Test-Path $iconPath) {
        $icon = [System.Drawing.Image]::FromFile($iconPath)
        $iconSize = 68
        $iconX = [int](($w - $iconSize) / 2)
        $iconY = 28
        $g.DrawImage($icon, $iconX, $iconY, $iconSize, $iconSize)
        $icon.Dispose()
    }

    # Title: Selfvoy
    $sfCenter = [System.Drawing.StringFormat]::new()
    $sfCenter.Alignment = [System.Drawing.StringAlignment]::Center
    $fontTitle = [System.Drawing.Font]::new("Segoe UI", [float]16, [System.Drawing.FontStyle]::Bold)
    $whiteBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(250, 248, 255))
    $g.DrawString("Selfvoy", $fontTitle, $whiteBrush, [float]($w / 2), [float]106, $sfCenter)
    $fontTitle.Dispose()

    # Gradient accent line under logo
    $penColor = if ($IsUninstall) { [System.Drawing.Color]::FromArgb(244, 63, 94) } else { [System.Drawing.Color]::FromArgb(168, 85, 247) }
    $pen = [System.Drawing.Pen]::new($penColor, [float]2)
    $g.DrawLine($pen, 36, 134, 128, 134)
    $pen.Dispose()

    # Tagline
    $fontSub = [System.Drawing.Font]::new("Segoe UI", [float]8.5, [System.Drawing.FontStyle]::Regular)
    $subBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(185, 175, 215))
    $tagline = if ($IsUninstall) { "Удаление приложения" } else { "Персональный ИИ-агент" }
    $g.DrawString($tagline, $fontSub, $subBrush, [float]($w / 2), [float]144, $sfCenter)
    $fontSub.Dispose()
    $subBrush.Dispose()

    # Feature points
    $fontFeat = [System.Drawing.Font]::new("Segoe UI", [float]8, [System.Drawing.FontStyle]::Regular)
    $featBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(160, 150, 190))
    $dotColor = if ($IsUninstall) { [System.Drawing.Color]::FromArgb(244, 63, 94) } else { [System.Drawing.Color]::FromArgb(147, 85, 247) }
    $dotBrush = [System.Drawing.SolidBrush]::new($dotColor)
    
    $features = if ($IsUninstall) {
        @("Чистое удаление файлов", "Очистка кэша и сессий", "Отказ от подписок")
    } else {
        @("Умные переговоры ИИ", "Календарь и расписание", "Шифрование и 2FA")
    }

    $yPos = 188
    foreach ($feat in $features) {
        $g.FillEllipse($dotBrush, 18, ($yPos + 4), 6, 6)
        $sfLeft = [System.Drawing.StringFormat]::new()
        $sfLeft.Alignment = [System.Drawing.StringAlignment]::Near
        $g.DrawString($feat, $fontFeat, $featBrush, [float]28, [float]$yPos, $sfLeft)
        $yPos += 24
        $sfLeft.Dispose()
    }
    $fontFeat.Dispose()
    $featBrush.Dispose()
    $dotBrush.Dispose()

    # Footer note
    $fontFoot = [System.Drawing.Font]::new("Segoe UI", [float]7.5, [System.Drawing.FontStyle]::Regular)
    $footBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(115, 105, 145))
    $g.DrawString("app.selfvoy.network", $fontFoot, $footBrush, [float]($w / 2), [float]292, $sfCenter)
    $fontFoot.Dispose()
    $footBrush.Dispose()

    $whiteBrush.Dispose()
    $sfCenter.Dispose()
    $g.Dispose()

    $bmp.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Bmp)
    $bmp.Dispose()
    Write-Output "Generated: $OutputPath"
}

function Build-Header {
    param([string]$OutputPath)

    $w = 150
    $h = 57
    $bmp = [System.Drawing.Bitmap]::new($w, $h, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic

    # Dark gradient
    $cLeft = [System.Drawing.Color]::FromArgb(24, 16, 44)
    $cRight = [System.Drawing.Color]::FromArgb(12, 10, 22)
    $rect = [System.Drawing.Rectangle]::new(0, 0, $w, $h)
    $brush = [System.Drawing.Drawing2D.LinearGradientBrush]::new($rect, $cLeft, $cRight, [System.Drawing.Drawing2D.LinearGradientMode]::Horizontal)
    $g.FillRectangle($brush, $rect)
    $brush.Dispose()

    # Aurora glow on the right
    $glowBrush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::FromArgb(45, 139, 92, 246))
    $g.FillEllipse($glowBrush, 85, -5, 75, 65)
    $glowBrush.Dispose()

    # Draw icon on the right
    $iconPath = Join-Path $PSScriptRoot "..\build\icon.png"
    if (Test-Path $iconPath) {
        $icon = [System.Drawing.Image]::FromFile($iconPath)
        $iconSize = 42
        $g.DrawImage($icon, ($w - $iconSize - 10), 8, $iconSize, $iconSize)
        $icon.Dispose()
    }

    $g.Dispose()
    $bmp.Save($OutputPath, [System.Drawing.Imaging.ImageFormat]::Bmp)
    $bmp.Dispose()
    Write-Output "Generated: $OutputPath"
}

$buildDir = Join-Path $PSScriptRoot "..\build"
if (!(Test-Path $buildDir)) { New-Item -ItemType Directory -Path $buildDir -Force | Out-Null }

Build-Sidebar -OutputPath (Join-Path $buildDir "installerSidebar.bmp") -IsUninstall $false
Build-Sidebar -OutputPath (Join-Path $buildDir "uninstallerSidebar.bmp") -IsUninstall $true
Build-Header -OutputPath (Join-Path $buildDir "installerHeader.bmp")
