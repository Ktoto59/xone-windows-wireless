# SPDX-License-Identifier: GPL-2.0-or-later
# Encode the approved PNG artwork as a multi-resolution Windows ICO.
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
Add-Type -AssemblyName System.Drawing
$taskSource=Join-Path $taskRoot 'assets/branding/logo.png'
$taskTarget=Join-Path $taskRoot 'assets/branding/app.ico'
$taskSizes=@(16,24,32,48,64,128,256)
$taskImage=[Drawing.Image]::FromFile($taskSource)
try{
    $taskFrames=@()
    foreach($taskSize in $taskSizes){
        $taskBitmap=[Drawing.Bitmap]::new($taskSize,$taskSize,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $taskGraphics=[Drawing.Graphics]::FromImage($taskBitmap)
        $taskStream=[IO.MemoryStream]::new()
        try{
            $taskGraphics.CompositingMode=[Drawing.Drawing2D.CompositingMode]::SourceCopy
            $taskGraphics.CompositingQuality=[Drawing.Drawing2D.CompositingQuality]::HighQuality
            $taskGraphics.InterpolationMode=[Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $taskGraphics.PixelOffsetMode=[Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $taskGraphics.DrawImage($taskImage,[Drawing.Rectangle]::new(0,0,$taskSize,$taskSize))
            $taskBitmap.Save($taskStream,[Drawing.Imaging.ImageFormat]::Png)
            $taskFrames+=,[byte[]]$taskStream.ToArray()
        }finally{$taskStream.Dispose();$taskGraphics.Dispose();$taskBitmap.Dispose()}
    }
    $taskStream=[IO.MemoryStream]::new()
    $taskWriter=[IO.BinaryWriter]::new($taskStream)
    try{
        $taskWriter.Write([uint16]0);$taskWriter.Write([uint16]1);$taskWriter.Write([uint16]$taskSizes.Count)
        $taskOffset=6+16*$taskSizes.Count
        for($taskIndex=0;$taskIndex -lt $taskSizes.Count;$taskIndex++){
            $taskDimension=if($taskSizes[$taskIndex] -eq 256){0}else{$taskSizes[$taskIndex]}
            $taskWriter.Write([byte]$taskDimension);$taskWriter.Write([byte]$taskDimension)
            $taskWriter.Write([byte]0);$taskWriter.Write([byte]0)
            $taskWriter.Write([uint16]1);$taskWriter.Write([uint16]32)
            $taskWriter.Write([uint32]$taskFrames[$taskIndex].Length);$taskWriter.Write([uint32]$taskOffset)
            $taskOffset+=$taskFrames[$taskIndex].Length
        }
        foreach($taskFrame in $taskFrames){$taskWriter.Write([byte[]]$taskFrame)}
        $taskWriter.Flush()
        [IO.File]::WriteAllBytes($taskTarget,$taskStream.ToArray())
    }finally{$taskWriter.Dispose();$taskStream.Dispose()}
}finally{$taskImage.Dispose()}
Write-Output 'Encoded assets/branding/app.ico: 16, 24, 32, 48, 64, 128, 256 pixels'
