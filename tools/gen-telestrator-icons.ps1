# Telestrator tool icons — flat white line-art glyphs on a TRANSPARENT background.
# Dependency-free: PowerShell + System.Drawing (GDI+) only. No ImageMagick/Inkscape/Node.
# Mirrors the approach in ../../hardware/ulanzi-window-control/.iconbuild/gen-icons.ps1
# (GDI+ shape drawing, high-quality antialiasing, multi-size PNG output).
#
# Each glyph is drawn on a 144x144 canvas with ~10% padding, then downscaled to 72.
# Output: <repo>/icons/<name>-144.png and <name>-72.png
Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot          # repo root (tools/ -> ..)
$out  = Join-Path $root 'icons'
if (-not (Test-Path $out)) { New-Item -ItemType Directory -Path $out | Out-Null }

# ---- canvas geometry -------------------------------------------------------
$SZ   = 144                                        # master canvas
$PAD  = [int]($SZ * 0.10)                           # ~10% padding => ~14px
$LO   = $PAD                                        # inner box left/top  (~14)
$HI   = $SZ - $PAD                                  # inner box right/bot  (~130)
$MID  = $SZ / 2.0                                   # 72
$STROKE = 11.0                                      # glyph stroke weight

# ---- helpers ---------------------------------------------------------------
$White = [System.Drawing.Color]::FromArgb(255, 248, 250, 252)   # near-white, faint cool tint

function New-Canvas {
  $bmp = New-Object System.Drawing.Bitmap($SZ, $SZ)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode     = 'AntiAlias'
  $g.InterpolationMode = 'HighQualityBicubic'
  $g.PixelOffsetMode   = 'HighQuality'
  $g.Clear([System.Drawing.Color]::Transparent)
  return @($bmp, $g)
}

function New-Pen([single]$w = $STROKE, $color = $White) {
  $p = New-Object System.Drawing.Pen($color, $w)
  $p.StartCap = 'Round'; $p.EndCap = 'Round'; $p.LineJoin = 'Round'
  return $p
}

function P([single]$x, [single]$y) { New-Object System.Drawing.PointF($x, $y) }

function Save-Sizes($bmp, $name) {
  $bmp.Save((Join-Path $out "$name-144.png"), [System.Drawing.Imaging.ImageFormat]::Png)
  $small = New-Object System.Drawing.Bitmap(72, 72)
  $gs = [System.Drawing.Graphics]::FromImage($small)
  $gs.InterpolationMode = 'HighQualityBicubic'
  $gs.SmoothingMode     = 'AntiAlias'
  $gs.PixelOffsetMode   = 'HighQuality'
  $gs.Clear([System.Drawing.Color]::Transparent)
  $gs.DrawImage($bmp, (New-Object System.Drawing.Rectangle(0, 0, 72, 72)))
  $small.Save((Join-Path $out "$name-72.png"), [System.Drawing.Imaging.ImageFormat]::Png)
  $gs.Dispose(); $small.Dispose()
}

# Draw an arrowhead at point (tx,ty) pointing along the direction (dx,dy).
function Add-Arrowhead($g, $pen, [single]$tx, [single]$ty, [single]$dx, [single]$dy, [single]$len = 30) {
  $a = [Math]::Atan2($dy, $dx)
  $spread = 0.55                                   # radians off the shaft axis
  $x1 = $tx - $len * [Math]::Cos($a - $spread); $y1 = $ty - $len * [Math]::Sin($a - $spread)
  $x2 = $tx - $len * [Math]::Cos($a + $spread); $y2 = $ty - $len * [Math]::Sin($a + $spread)
  $g.DrawLine($pen, $tx, $ty, [single]$x1, [single]$y1)
  $g.DrawLine($pen, $tx, $ty, [single]$x2, [single]$y2)
}

# ===========================================================================
#  GLYPHS
# ===========================================================================

# --- pen (pencil / nib) ----------------------------------------------------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$pen = New-Pen
# pencil body: a long diagonal bar from top-right to lower-left
$bx1 = 112; $by1 = 30; $bx2 = 50; $by2 = 92
$body = New-Object System.Drawing.Drawing2D.GraphicsPath
$bw = 16                                            # half-width of barrel
# barrel as a rotated rectangle (two parallel edges)
$ux = ($bx2 - $bx1); $uy = ($by2 - $by1); $ul = [Math]::Sqrt($ux*$ux + $uy*$uy); $ux /= $ul; $uy /= $ul
$nx = -$uy; $ny = $ux
$g.DrawLine($pen, [single]($bx1 + $nx*$bw), [single]($by1 + $ny*$bw), [single]($bx2 + $nx*$bw), [single]($by2 + $ny*$bw))
$g.DrawLine($pen, [single]($bx1 - $nx*$bw), [single]($by1 - $ny*$bw), [single]($bx2 - $nx*$bw), [single]($by2 - $ny*$bw))
# cap end (top-right)
$g.DrawLine($pen, [single]($bx1 + $nx*$bw), [single]($by1 + $ny*$bw), [single]($bx1 - $nx*$bw), [single]($by1 - $ny*$bw))
# nib: converge the two body edges to a point at lower-left
$tipx = $bx2 + $ux*26; $tipy = $by2 + $uy*26
$g.DrawLine($pen, [single]($bx2 + $nx*$bw), [single]($by2 + $ny*$bw), [single]$tipx, [single]$tipy)
$g.DrawLine($pen, [single]($bx2 - $nx*$bw), [single]($by2 - $ny*$bw), [single]$tipx, [single]$tipy)
$pen.Dispose()
Save-Sizes $bmp 'pen'; $g.Dispose(); $bmp.Dispose()

# --- line (diagonal line) --------------------------------------------------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$pen = New-Pen
$g.DrawLine($pen, [single]$LO, [single]$HI, [single]$HI, [single]$LO)
$pen.Dispose()
Save-Sizes $bmp 'line'; $g.Dispose(); $bmp.Dispose()

# --- arrow (single arrow) --------------------------------------------------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$pen = New-Pen
$g.DrawLine($pen, [single]$LO, [single]$HI, [single]$HI, [single]$LO)
Add-Arrowhead $g $pen $HI $LO ([single]($HI-$LO)) ([single]($LO-$HI))
$pen.Dispose()
Save-Sizes $bmp 'arrow'; $g.Dispose(); $bmp.Dispose()

# --- dblarrow (double-headed arrow) ----------------------------------------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$pen = New-Pen
$g.DrawLine($pen, [single]$LO, [single]$HI, [single]$HI, [single]$LO)
Add-Arrowhead $g $pen $HI $LO ([single]($HI-$LO)) ([single]($LO-$HI))
Add-Arrowhead $g $pen $LO $HI ([single]($LO-$HI)) ([single]($HI-$LO))
$pen.Dispose()
Save-Sizes $bmp 'dblarrow'; $g.Dispose(); $bmp.Dispose()

# --- curvedarrow (curved arrow) --------------------------------------------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$pen = New-Pen
# bezier sweeping from lower-left up to upper-right
$p0 = P 30 112; $p1 = P 34 40; $p2 = P 104 36; $p3 = P 116 60
$g.DrawBezier($pen, $p0, $p1, $p2, $p3)
# arrowhead at end, tangent ~ (p3 - p2)
Add-Arrowhead $g $pen ([single]$p3.X) ([single]$p3.Y) ([single]($p3.X-$p2.X)) ([single]($p3.Y-$p2.Y))
$pen.Dispose()
Save-Sizes $bmp 'curvedarrow'; $g.Dispose(); $bmp.Dispose()

# --- rect (square outline) -------------------------------------------------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$pen = New-Pen
$g.DrawRectangle($pen, [single]$LO, [single]$LO, [single]($HI-$LO), [single]($HI-$LO))
$pen.Dispose()
Save-Sizes $bmp 'rect'; $g.Dispose(); $bmp.Dispose()

# --- ellipse (circle outline) ----------------------------------------------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$pen = New-Pen
$g.DrawEllipse($pen, [single]$LO, [single]$LO, [single]($HI-$LO), [single]($HI-$LO))
$pen.Dispose()
Save-Sizes $bmp 'ellipse'; $g.Dispose(); $bmp.Dispose()

# --- spotlight (circle with radiating rays) --------------------------------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$pen = New-Pen 9
$cr = 26                                            # core circle radius
$g.DrawEllipse($pen, [single]($MID-$cr), [single]($MID-$cr), [single]($cr*2), [single]($cr*2))
$rIn = $cr + 10; $rOut = $cr + 30
for ($i = 0; $i -lt 8; $i++) {
  $a = $i * [Math]::PI / 4
  $ca = [Math]::Cos($a); $sa = [Math]::Sin($a)
  $g.DrawLine($pen, [single]($MID + $ca*$rIn), [single]($MID + $sa*$rIn), [single]($MID + $ca*$rOut), [single]($MID + $sa*$rOut))
}
$pen.Dispose()
Save-Sizes $bmp 'spotlight'; $g.Dispose(); $bmp.Dispose()

# --- firstdown (bold horizontal line — football first-down marker) ---------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$pen = New-Pen 20                                   # extra-bold bar
$g.DrawLine($pen, [single]$LO, [single]$MID, [single]$HI, [single]$MID)
# small end caps (vertical ticks) to read as a yard line
$tick = New-Pen 11
$g.DrawLine($tick, [single]$LO, [single]($MID-22), [single]$LO, [single]($MID+22))
$g.DrawLine($tick, [single]$HI, [single]($MID-22), [single]$HI, [single]($MID+22))
$pen.Dispose(); $tick.Dispose()
Save-Sizes $bmp 'firstdown'; $g.Dispose(); $bmp.Dispose()

# --- eraser (eraser block) -------------------------------------------------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$pen = New-Pen
# a tilted parallelogram block (rubber eraser seen at an angle)
$pts = @((P 34 96), (P 78 52), (P 116 52), (P 116 96))
$path = New-Object System.Drawing.Drawing2D.GraphicsPath
$path.AddPolygon([System.Drawing.PointF[]]$pts)
$g.DrawPath($pen, $path); $path.Dispose()
# divider line showing the rubber/sleeve seam
$g.DrawLine($pen, [single]60, [single]74, [single]116, [single]74)
$pen.Dispose()
Save-Sizes $bmp 'eraser'; $g.Dispose(); $bmp.Dispose()

# --- laser (dot with concentric rings) -------------------------------------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$dot = New-Object System.Drawing.SolidBrush($White)
$g.FillEllipse($dot, [single]($MID-11), [single]($MID-11), 22, 22)   # solid core
$dot.Dispose()
$ring1 = New-Pen 8; $ring2 = New-Pen 6
$r1 = 28; $r2 = 44
$g.DrawEllipse($ring1, [single]($MID-$r1), [single]($MID-$r1), [single]($r1*2), [single]($r1*2))
$g.DrawEllipse($ring2, [single]($MID-$r2), [single]($MID-$r2), [single]($r2*2), [single]($r2*2))
$ring1.Dispose(); $ring2.Dispose()
Save-Sizes $bmp 'laser'; $g.Dispose(); $bmp.Dispose()

# --- undo (counter-clockwise arrow) — mirror of redo -----------------------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$pen = New-Pen
$rad = 44
# 290deg arc opening at the top, head entering from the upper-left (CCW)
$g.DrawArc($pen, [single]($MID-$rad), [single]($MID-$rad), [single]($rad*2), [single]($rad*2), 20, 290)
$ax = $MID + $rad * [Math]::Cos(20 * [Math]::PI/180)
$ay = $MID + $rad * [Math]::Sin(20 * [Math]::PI/180)
Add-Arrowhead $g $pen ([single]$ax) ([single]$ay) ([single]-1) ([single]-0.9) 26
$pen.Dispose()
Save-Sizes $bmp 'undo'; $g.Dispose(); $bmp.Dispose()

# --- redo (clockwise arrow) — mirror of undo -------------------------------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$pen = New-Pen
$rad = 44
$g.DrawArc($pen, [single]($MID-$rad), [single]($MID-$rad), [single]($rad*2), [single]($rad*2), 160, -290)
$ax = $MID + $rad * [Math]::Cos(160 * [Math]::PI/180)
$ay = $MID + $rad * [Math]::Sin(160 * [Math]::PI/180)
Add-Arrowhead $g $pen ([single]$ax) ([single]$ay) ([single]1) ([single]-0.9) 26
$pen.Dispose()
Save-Sizes $bmp 'redo'; $g.Dispose(); $bmp.Dispose()

# --- clear (trash can with X-style lid + body) -----------------------------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$pen = New-Pen 10
# lid
$g.DrawLine($pen, [single]34, [single]44, [single]110, [single]44)
# handle
$g.DrawLine($pen, [single]58, [single]44, [single]62, [single]32)
$g.DrawLine($pen, [single]86, [single]44, [single]82, [single]32)
$g.DrawLine($pen, [single]62, [single]32, [single]82, [single]32)
# can body (tapered)
$g.DrawLine($pen, [single]44, [single]52, [single]50, [single]114)
$g.DrawLine($pen, [single]100, [single]52, [single]94, [single]114)
$g.DrawLine($pen, [single]50, [single]114, [single]94, [single]114)
# slats
$g.DrawLine($pen, [single]62, [single]60, [single]65, [single]106)
$g.DrawLine($pen, [single]72, [single]60, [single]72, [single]106)
$g.DrawLine($pen, [single]82, [single]60, [single]79, [single]106)
$pen.Dispose()
Save-Sizes $bmp 'clear'; $g.Dispose(); $bmp.Dispose()

# --- dash (dashed line) ----------------------------------------------------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$pen = New-Pen
$pen.DashStyle = 'Custom'
$pen.DashPattern = @(2.0, 1.6)                       # in units of stroke width
$pen.DashCap = 'Round'
$g.DrawLine($pen, [single]$LO, [single]$HI, [single]$HI, [single]$LO)
$pen.Dispose()
Save-Sizes $bmp 'dash'; $g.Dispose(); $bmp.Dispose()

# --- fill (filled square) --------------------------------------------------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$brush = New-Object System.Drawing.SolidBrush($White)
$gp = New-Object System.Drawing.Drawing2D.GraphicsPath
# slightly rounded filled square
$r = 14; $d = $r * 2
$x = $LO; $y = $LO; $w = $HI - $LO; $h = $HI - $LO
$gp.AddArc([single]$x, [single]$y, $d, $d, 180, 90)
$gp.AddArc([single]($x+$w-$d), [single]$y, $d, $d, 270, 90)
$gp.AddArc([single]($x+$w-$d), [single]($y+$h-$d), $d, $d, 0, 90)
$gp.AddArc([single]$x, [single]($y+$h-$d), $d, $d, 90, 90)
$gp.CloseFigure()
$g.FillPath($brush, $gp)
$brush.Dispose(); $gp.Dispose()
Save-Sizes $bmp 'fill'; $g.Dispose(); $bmp.Dispose()

# --- highlighter (marker) --------------------------------------------------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$pen = New-Pen
# marker body (diagonal, fat) from upper-right to mid
$mx1 = 110; $my1 = 34; $mx2 = 70; $my2 = 74
$mw = 20
$ux = ($mx2-$mx1); $uy = ($my2-$my1); $ul = [Math]::Sqrt($ux*$ux+$uy*$uy); $ux/=$ul; $uy/=$ul
$nx = -$uy; $ny = $ux
$g.DrawLine($pen, [single]($mx1+$nx*$mw), [single]($my1+$ny*$mw), [single]($mx2+$nx*$mw), [single]($my2+$ny*$mw))
$g.DrawLine($pen, [single]($mx1-$nx*$mw), [single]($my1-$ny*$mw), [single]($mx2-$nx*$mw), [single]($my2-$ny*$mw))
$g.DrawLine($pen, [single]($mx1+$nx*$mw), [single]($my1+$ny*$mw), [single]($mx1-$nx*$mw), [single]($my1-$ny*$mw))   # butt end
# chisel tip (wide nib block)
$tip = New-Object System.Drawing.Drawing2D.GraphicsPath
$tipPts = @(
  (P ($mx2+$nx*$mw) ($my2+$ny*$mw)),
  (P ($mx2-$nx*$mw) ($my2-$ny*$mw)),
  (P ($mx2-$nx*$mw+$ux*22) ($my2-$ny*$mw+$uy*22)),
  (P ($mx2+$nx*$mw+$ux*22) ($my2+$ny*$mw+$uy*22))
)
$tip.AddPolygon([System.Drawing.PointF[]]$tipPts)
$brush = New-Object System.Drawing.SolidBrush($White)
$g.FillPath($brush, $tip); $g.DrawPath($pen, $tip)
$brush.Dispose(); $tip.Dispose()
# underline swipe (the highlight stroke)
$swipe = New-Pen 14
$g.DrawLine($swipe, [single]34, [single]112, [single]96, [single]112)
$swipe.Dispose(); $pen.Dispose()
Save-Sizes $bmp 'highlighter'; $g.Dispose(); $bmp.Dispose()

# --- replay (circular replay arrow) ----------------------------------------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$pen = New-Pen
$rad = 42
$g.DrawArc($pen, [single]($MID-$rad), [single]($MID-$rad), [single]($rad*2), [single]($rad*2), 300, 320)
# arrowhead at the top
$ax = $MID + $rad * [Math]::Cos(300 * [Math]::PI/180)
$ay = $MID + $rad * [Math]::Sin(300 * [Math]::PI/180)
Add-Arrowhead $g $pen ([single]$ax) ([single]$ay) ([single]1) ([single]-0.8) 26
# small play triangle in the center
$tri = New-Object System.Drawing.Drawing2D.GraphicsPath
$tri.AddPolygon([System.Drawing.PointF[]]@((P 62 56), (P 62 88), (P 90 72)))
$brush = New-Object System.Drawing.SolidBrush($White)
$g.FillPath($brush, $tri); $brush.Dispose(); $tri.Dispose()
$pen.Dispose()
Save-Sizes $bmp 'replay'; $g.Dispose(); $bmp.Dispose()

# --- replayhide (back / exit arrow) ----------------------------------------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$pen = New-Pen
# straight back-pointing arrow into a wall (exit-left)
$g.DrawLine($pen, [single]108, [single]$MID, [single]44, [single]$MID)
Add-Arrowhead $g $pen ([single]44) ([single]$MID) ([single]-1) ([single]0) 30
# door / wall bar on the right
$g.DrawLine($pen, [single]120, [single]$LO, [single]120, [single]$HI)
$pen.Dispose()
Save-Sizes $bmp 'replayhide'; $g.Dispose(); $bmp.Dispose()

# --- arm (power / play toggle) ---------------------------------------------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$pen = New-Pen
$rad = 44
# power-symbol ring with a gap at the top
$g.DrawArc($pen, [single]($MID-$rad), [single]($MID-$rad), [single]($rad*2), [single]($rad*2), -65, 310)
# vertical stem through the gap
$g.DrawLine($pen, [single]$MID, [single]22, [single]$MID, [single]$MID)
$pen.Dispose()
Save-Sizes $bmp 'arm'; $g.Dispose(); $bmp.Dispose()

# --- sizeup (big dot) ------------------------------------------------------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$brush = New-Object System.Drawing.SolidBrush($White)
$r = 38
$g.FillEllipse($brush, [single]($MID-$r), [single]($MID-$r), [single]($r*2), [single]($r*2))
$brush.Dispose()
Save-Sizes $bmp 'sizeup'; $g.Dispose(); $bmp.Dispose()

# --- sizedown (small dot) --------------------------------------------------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$brush = New-Object System.Drawing.SolidBrush($White)
$r = 18
$g.FillEllipse($brush, [single]($MID-$r), [single]($MID-$r), [single]($r*2), [single]($r*2))
$brush.Dispose()
Save-Sizes $bmp 'sizedown'; $g.Dispose(); $bmp.Dispose()

# --- vertical (bold vertical line — sideline marker; firstdown rotated 90) --
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$pen = New-Pen 20                                   # extra-bold bar
$g.DrawLine($pen, [single]$MID, [single]$LO, [single]$MID, [single]$HI)
# small end caps (horizontal ticks) to read as a boundary line
$tick = New-Pen 11
$g.DrawLine($tick, [single]($MID-22), [single]$LO, [single]($MID+22), [single]$LO)
$g.DrawLine($tick, [single]($MID-22), [single]$HI, [single]($MID+22), [single]$HI)
$pen.Dispose(); $tick.Dispose()
Save-Sizes $bmp 'vertical'; $g.Dispose(); $bmp.Dispose()

# --- cone (vision/passing wedge opening to the upper-right) -----------------
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$pen = New-Pen
$apx = 30; $apy = 114                               # apex lower-left
$p1x = 64; $p1y = 25; $p2x = 119; $p2y = 80         # wedge mouth corners
$g.DrawLine($pen, [single]$apx, [single]$apy, [single]$p1x, [single]$p1y)
$g.DrawLine($pen, [single]$apx, [single]$apy, [single]$p2x, [single]$p2y)
$g.DrawLine($pen, [single]$p1x, [single]$p1y, [single]$p2x, [single]$p2y)
$pen.Dispose()
Save-Sizes $bmp 'cone'; $g.Dispose(); $bmp.Dispose()

# --- settings (solid cog with a punched center hole — matches OBS's config gear) --
$c = New-Canvas; $bmp = $c[0]; $g = $c[1]
$teeth = 8
$step  = [Math]::PI * 2 / $teeth
$rOut  = 54.0                                       # tooth-tip radius
$rIn   = 40.0                                       # valley / gear-body radius
$th    = $step * 0.26                               # tooth-top half-angle
$pts = New-Object System.Collections.Generic.List[System.Drawing.PointF]
for ($i = 0; $i -lt $teeth; $i++) {
  $cA = $i * $step
  $pts.Add((P ($MID + $rOut*[Math]::Cos($cA - $th)) ($MID + $rOut*[Math]::Sin($cA - $th))))
  $pts.Add((P ($MID + $rOut*[Math]::Cos($cA + $th)) ($MID + $rOut*[Math]::Sin($cA + $th))))
  $pts.Add((P ($MID + $rIn*[Math]::Cos($cA + $step*0.5)) ($MID + $rIn*[Math]::Sin($cA + $step*0.5))))
}
$cog = New-Object System.Drawing.Drawing2D.GraphicsPath
$cog.FillMode = 'Alternate'                         # even-odd: inner circle => hole
$cog.AddPolygon([System.Drawing.PointF[]]$pts.ToArray())
$hr = 16.0
$cog.AddEllipse([single]($MID-$hr), [single]($MID-$hr), [single]($hr*2), [single]($hr*2))
$brush = New-Object System.Drawing.SolidBrush($White)
$g.FillPath($brush, $cog)
$brush.Dispose(); $cog.Dispose()
Save-Sizes $bmp 'settings'; $g.Dispose(); $bmp.Dispose()

# ===========================================================================
Write-Host "Telestrator icons written to: $out"
Get-ChildItem $out -Filter *.png | Sort-Object Name |
  ForEach-Object { "{0,-22} {1,6} bytes" -f $_.Name, $_.Length } |
  ForEach-Object { Write-Host $_ }
$pngCount = (Get-ChildItem $out -Filter *.png).Count
Write-Host "Total PNGs: $pngCount"
