#requires -Version 7.0
[CmdletBinding()]
param([string]$OutputRoot = (Join-Path $env:LOCALAPPDATA 'OkuFlow\play-listing'))
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$checkout = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..')) + [IO.Path]::DirectorySeparatorChar
if ($OutputRoot.StartsWith($checkout, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Export listing images outside the synced checkout.'
}
[IO.Directory]::CreateDirectory($OutputRoot) | Out-Null

function Draw-OkuFlowMark {
    param([Drawing.Graphics]$Graphics, [single]$X, [single]$Y, [single]$Size)
    # Original vector geometry from res/drawable/ic_launcher_foreground.xml.
    $state = $Graphics.Save()
    $lens = [Drawing.Pen]::new([Drawing.Color]::White, 6)
    $handle = [Drawing.Pen]::new([Drawing.Color]::White, 8)
    $sparkle = [Drawing.Drawing2D.GraphicsPath]::new()
    $magenta = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#C424C8'))
    try {
        $Graphics.TranslateTransform($X, $Y)
        $Graphics.ScaleTransform($Size / 108, $Size / 108)
        $Graphics.DrawEllipse($lens, [single]30, [single]30, [single]40, [single]40)
        $handle.StartCap = $handle.EndCap = [Drawing.Drawing2D.LineCap]::Round
        $Graphics.DrawLine($handle, [single]64.5, [single]64.5, [single]74, [single]74)
        $sparkle.AddBezier(50, 38, 51.3, 46.2, 53.8, 48.7, 62, 50)
        $sparkle.AddBezier(62, 50, 53.8, 51.3, 51.3, 53.8, 50, 62)
        $sparkle.AddBezier(50, 62, 48.7, 53.8, 46.2, 51.3, 38, 50)
        $sparkle.AddBezier(38, 50, 46.2, 48.7, 48.7, 46.2, 50, 38)
        $sparkle.CloseFigure()
        $Graphics.FillPath($magenta, $sparkle)
    } finally {
        $Graphics.Restore($state)
        $lens.Dispose(); $handle.Dispose(); $sparkle.Dispose(); $magenta.Dispose()
    }
}

foreach ($kind in @('icon', 'feature')) {
    $width = if ($kind -eq 'icon') {512} else {1024}
    $height = if ($kind -eq 'icon') {512} else {500}
    $bitmap = [Drawing.Bitmap]::new($width, $height, [Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $fonts = @()
    try {
        $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $graphics.TextRenderingHint = [Drawing.Text.TextRenderingHint]::AntiAliasGridFit
        $graphics.Clear([Drawing.Color]::Black)
        if ($kind -eq 'icon') {
            Draw-OkuFlowMark $graphics 0 0 512
        } else {
            Draw-OkuFlowMark $graphics 28 85 340
            $titleFont = [Drawing.Font]::new('Segoe UI', 58, [Drawing.FontStyle]::Bold, [Drawing.GraphicsUnit]::Pixel)
            $bodyFont = [Drawing.Font]::new('Segoe UI', 30, [Drawing.FontStyle]::Regular, [Drawing.GraphicsUnit]::Pixel)
            $hintFont = [Drawing.Font]::new('Segoe UI', 21, [Drawing.FontStyle]::Regular, [Drawing.GraphicsUnit]::Pixel)
            $fonts = @($titleFont, $bodyFont, $hintFont)
            $graphics.DrawString('OkuFlow', $titleFont, [Drawing.Brushes]::White, [single]386, [single]112)
            $graphics.DrawString("Your phone camera.`nConnected to Windows.", $bodyFont, [Drawing.Brushes]::White, [single]390, [single]196)
            $graphics.DrawString('USB data cable · Windows companion required', $hintFont, [Drawing.Brushes]::White, [single]390, [single]314)
        }
        $destination = Join-Path $OutputRoot ($kind + '.png')
        $bitmap.Save($destination, [Drawing.Imaging.ImageFormat]::Png)
        Write-Output $destination
    } finally {
        foreach ($font in $fonts) {$font.Dispose()}
        $graphics.Dispose(); $bitmap.Dispose()
    }
}
