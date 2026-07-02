# gen-readme-legend.ps1 — dark-tile versions of the tool glyphs for the README.
# The embedded dock icons are near-white on transparent, so they vanish on a
# light GitHub background. This composites each glyph onto a small dark rounded
# tile (the OBS dock-button look) so the legend reads on any theme.
# Output: <repo>/icons/legend/<name>.png
Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$src  = Join-Path $root 'icons'
$out  = Join-Path $src 'legend'
if (-not (Test-Path $out)) { New-Item -ItemType Directory -Path $out | Out-Null }

$TILE = 44; $GLY = 30; $RAD = 8
$bg = [System.Drawing.Color]::FromArgb(255, 43, 48, 56)   # OBS dock button grey

$names = @('pen','line','arrow','dblarrow','curvedarrow','rect','ellipse','cone',
           'spotlight','firstdown','vertical','eraser')

foreach ($n in $names) {
  $glyphPath = Join-Path $src "$n-72.png"
  if (-not (Test-Path $glyphPath)) { Write-Warning "missing $glyphPath"; continue }
  $bmp = New-Object System.Drawing.Bitmap($TILE, $TILE)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
  $g.Clear([System.Drawing.Color]::Transparent)
  # rounded-rect tile
  $gp = New-Object System.Drawing.Drawing2D.GraphicsPath
  $d = $RAD * 2
  $gp.AddArc(0, 0, $d, $d, 180, 90)
  $gp.AddArc($TILE-$d, 0, $d, $d, 270, 90)
  $gp.AddArc($TILE-$d, $TILE-$d, $d, $d, 0, 90)
  $gp.AddArc(0, $TILE-$d, $d, $d, 90, 90)
  $gp.CloseFigure()
  $brush = New-Object System.Drawing.SolidBrush($bg)
  $g.FillPath($brush, $gp); $brush.Dispose(); $gp.Dispose()
  # centered glyph
  $glyph = [System.Drawing.Bitmap]::FromFile($glyphPath)
  $off = ($TILE - $GLY) / 2
  $g.DrawImage($glyph, [int]$off, [int]$off, $GLY, $GLY)
  $glyph.Dispose()
  $bmp.Save((Join-Path $out "$n.png"), [System.Drawing.Imaging.ImageFormat]::Png)
  $g.Dispose(); $bmp.Dispose()
}
Write-Host "Legend chips written to: $out"
