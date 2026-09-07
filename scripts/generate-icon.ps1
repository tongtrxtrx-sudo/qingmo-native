param([string]$OutputDirectory = (Join-Path $PSScriptRoot '..\assets'), [switch]$StoreAssetsOnly)

$ErrorActionPreference = 'Stop'
# Use the Windows GDI+ assemblies consistently when invoked from PowerShell 7.
if ($PSVersionTable.PSEdition -eq 'Core') {
    $windowsPowerShell = Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $taskIconArguments = @('-NoProfile','-ExecutionPolicy','Bypass','-File',$PSCommandPath,'-OutputDirectory',$OutputDirectory)
    if ($StoreAssetsOnly) { $taskIconArguments += '-StoreAssetsOnly' }
    & $windowsPowerShell @taskIconArguments
    if ($LASTEXITCODE -ne 0) { throw "Icon generation failed with exit code $LASTEXITCODE" }
    return
}
Add-Type -AssemblyName System.Drawing
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null

# The geometry mirrors assets/qingmo.svg; render at 4x for clean small icons.
# PNG entries preserve full alpha at every ICO resolution on supported Windows.
if (-not ('QingmoIconRenderer' -as [type])) {
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;

public static class QingmoIconRenderer {
    public static byte[] Render(int size) {
        int scale = 4;
        using (var large = new Bitmap(size * scale, size * scale, PixelFormat.Format32bppArgb)) {
            using (var g = Graphics.FromImage(large)) {
                g.Clear(Color.Transparent);
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.ScaleTransform(size * scale / 256f, size * scale / 256f);
                using (var sage = new SolidBrush(Color.FromArgb(61, 122, 96)))
                using (var ivory = new SolidBrush(Color.FromArgb(255, 253, 244)))
                using (var fold = new SolidBrush(Color.FromArgb(196, 216, 201)))
                using (var background = new GraphicsPath())
                using (var paper = new GraphicsPath())
                using (var corner = new GraphicsPath())
                using (var stroke = new Pen(sage, 11)) {
                    background.AddArc(8, 8, 112, 112, 180, 90);
                    background.AddArc(136, 8, 112, 112, 270, 90);
                    background.AddArc(136, 136, 112, 112, 0, 90);
                    background.AddArc(8, 136, 112, 112, 90, 90);
                    background.CloseFigure();
                    g.FillPath(sage, background);
                    paper.AddLines(new PointF[] { new PointF(80,45), new PointF(151,45), new PointF(190,84), new PointF(190,199) });
                    paper.AddBezier(190,199,190,207,186,211,178,211);
                    paper.AddLine(178,211,80,211);
                    paper.AddBezier(80,211,72,211,68,207,68,199);
                    paper.AddLine(68,199,68,57);
                    paper.AddBezier(68,57,68,49,72,45,80,45);
                    paper.CloseFigure();
                    g.FillPath(ivory, paper);
                    corner.AddLine(151,45,151,74);
                    corner.AddBezier(151,74,151,80.667f,154.333f,84,161,84);
                    corner.AddLine(161,84,190,84);
                    corner.CloseFigure();
                    g.FillPath(fold, corner);
                    stroke.StartCap = LineCap.Round;
                    stroke.EndCap = LineCap.Round;
                    g.DrawLine(stroke,94,115,162,115);
                    g.DrawLine(stroke,94,145,162,145);
                    g.DrawLine(stroke,94,175,138,175);
                }
            }
            using (var result = new Bitmap(size, size, PixelFormat.Format32bppArgb)) {
                using (var g = Graphics.FromImage(result)) {
                    g.CompositingMode = CompositingMode.SourceCopy;
                    g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                    g.PixelOffsetMode = PixelOffsetMode.HighQuality;
                    g.DrawImage(large, new Rectangle(0, 0, size, size), 0, 0, large.Width, large.Height, GraphicsUnit.Pixel);
                }
                using (var stream = new MemoryStream()) {
                    result.Save(stream, ImageFormat.Png);
                    return stream.ToArray();
                }
            }
        }
    }
}
'@
}

if ($StoreAssetsOnly) {
    foreach ($asset in @(
        @{ Name = 'Square44x44Logo.png'; Size = 44 },
        @{ Name = 'Square150x150Logo.png'; Size = 150 },
        @{ Name = 'StoreLogo.png'; Size = 50 },
        @{ Name = 'ListingLogo.png'; Size = 300 }
    )) {
        [IO.File]::WriteAllBytes((Join-Path $OutputDirectory $asset.Name), [QingmoIconRenderer]::Render($asset.Size))
    }
    Write-Output "Created Microsoft Store PNG assets in $OutputDirectory"
    return
}

$sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
$images = @($sizes | ForEach-Object { ,([QingmoIconRenderer]::Render($_)) })
$icoPath = Join-Path $OutputDirectory 'qingmo.ico'
$stream = [IO.File]::Create($icoPath)
$writer = [IO.BinaryWriter]::new($stream)
try {
    $writer.Write([uint16]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]$sizes.Count)
    $offset = 6 + 16 * $sizes.Count
    for ($i = 0; $i -lt $sizes.Count; $i++) {
        $dimension = if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
        $writer.Write([byte]$dimension)
        $writer.Write([byte]$dimension)
        $writer.Write([byte]0)
        $writer.Write([byte]0)
        $writer.Write([uint16]1)
        $writer.Write([uint16]32)
        $writer.Write([uint32]$images[$i].Length)
        $writer.Write([uint32]$offset)
        $offset += $images[$i].Length
    }
    foreach ($png in $images) { $writer.Write([byte[]]$png) }
} finally {
    $writer.Dispose()
}
[IO.File]::WriteAllBytes((Join-Path $OutputDirectory 'icon-preview.png'), [QingmoIconRenderer]::Render(512))
Write-Output "Created $icoPath ($($sizes -join ', ') px) and icon-preview.png"
