# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskAssets = Join-Path $taskRoot 'assets'
New-Item -ItemType Directory -Path $taskAssets -Force | Out-Null
$taskFrames = @()
foreach ($taskSize in @(16,20,24,32,40,48,64,128,256)) {
    $taskBitmap = [Drawing.Bitmap]::new($taskSize,$taskSize,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $taskGraphics = [Drawing.Graphics]::FromImage($taskBitmap)
    $taskMint = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#58ddc0'))
    $taskDark = [Drawing.SolidBrush]::new([Drawing.ColorTranslator]::FromHtml('#0e1219'))
    try {
        $taskGraphics.Clear($taskDark.Color)
        $taskGraphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $taskGraphics.ScaleTransform(($taskSize/256.0),($taskSize/256.0))
        $taskGraphics.FillRectangle($taskMint,72,46,152,118)
        $taskGraphics.FillRectangle($taskDark,82,56,132,98)
        $taskGraphics.FillRectangle($taskMint,143,164,10,24)
        $taskGraphics.FillRectangle($taskMint,109,188,78,10)
        $taskGraphics.FillRectangle($taskDark,29,81,90,148)
        $taskGraphics.FillRectangle($taskMint,34,86,80,138)
        $taskGraphics.FillRectangle($taskDark,44,98,60,102)
        $taskGraphics.FillRectangle($taskDark,59,209,30,5)
        $taskStream = [IO.MemoryStream]::new()
        try {
            $taskBitmap.Save($taskStream,[Drawing.Imaging.ImageFormat]::Png)
            $taskFrames += [pscustomobject]@{Size=$taskSize;Data=$taskStream.ToArray()}
        } finally { $taskStream.Dispose() }
        if ($taskSize -eq 256) { $taskBitmap.Save((Join-Path $taskAssets 'USBMirror.png'),[Drawing.Imaging.ImageFormat]::Png) }
    } finally { $taskGraphics.Dispose(); $taskBitmap.Dispose(); $taskMint.Dispose(); $taskDark.Dispose() }
}
$taskFile = [IO.File]::Create((Join-Path $taskAssets 'USBMirror.ico'))
$taskWriter = [IO.BinaryWriter]::new($taskFile)
try {
    $taskWriter.Write([uint16]0); $taskWriter.Write([uint16]1); $taskWriter.Write([uint16]$taskFrames.Count)
    $taskOffset = 6 + 16 * $taskFrames.Count
    foreach ($taskFrame in $taskFrames) {
        $taskDimension = if ($taskFrame.Size -eq 256) {0} else {$taskFrame.Size}
        $taskWriter.Write([byte]$taskDimension); $taskWriter.Write([byte]$taskDimension)
        $taskWriter.Write([byte]0); $taskWriter.Write([byte]0)
        $taskWriter.Write([uint16]1); $taskWriter.Write([uint16]32)
        $taskWriter.Write([uint32]$taskFrame.Data.Length); $taskWriter.Write([uint32]$taskOffset)
        $taskOffset += $taskFrame.Data.Length
    }
    foreach ($taskFrame in $taskFrames) { $taskWriter.Write([byte[]]$taskFrame.Data) }
} finally { $taskWriter.Dispose() }
Write-Output 'Created assets/USBMirror.ico (9 sizes) and PNG preview.'
