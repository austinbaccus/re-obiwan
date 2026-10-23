# Run the same script under both renderers and compare the frames (pass
# -Env @{RECOMP_FIXEDFPS='60'} so a frame number is the same moment in both).
#   tools\cmp.ps1 -Tag menu -Keys "600:START" -Frames 700,900 [-Env @{..}] [-Only d3d11]
# Writes build\cmp\<tag>_<renderer>_<frame>.bmp and build\cmp_<tag>.png (d3d9 | d3d11 | difference x4).
param([string]$Tag = 'cmp', [string]$Keys = '', [int[]]$Frames = @(600), [int]$Timeout = 90, [hashtable]$Env = @{}, [string]$Only = '')
$repo = Split-Path -Parent $PSScriptRoot
New-Item -ItemType Directory -Force "$repo\build\cmp" | Out-Null
$renderers = if ($Only) { @($Only) } else { @('d3d9', 'd3d11') }
foreach ($r in $renderers) {
    $e = @{} + $Env
    $e['RECOMP_RENDERER'] = $r
    & "$PSScriptRoot\shots.ps1" -Tag "${Tag}_$r" -Keys $Keys -Frames $Frames -Timeout $Timeout -Env $e | Where-Object { $_ -notmatch '^sheet' }
    foreach ($f in $Frames) {
        if (Test-Path "$repo\build\shot_$f.bmp") { Move-Item -Force "$repo\build\shot_$f.bmp" "$repo\build\cmp\${Tag}_${r}_$f.bmp" }
    }
}
Add-Type -AssemblyName System.Drawing
function Bytes($path) {
    $b = [System.Drawing.Bitmap]::FromFile($path)
    $rect = New-Object System.Drawing.Rectangle 0, 0, $b.Width, $b.Height
    $d = $b.LockBits($rect, 'ReadOnly', [System.Drawing.Imaging.PixelFormat]::Format32bppRgb)
    $a = New-Object byte[] ($d.Stride * $b.Height)
    [System.Runtime.InteropServices.Marshal]::Copy($d.Scan0, $a, 0, $a.Length)
    $b.UnlockBits($d); $b.Dispose()
    return ,$a
}
$rows = @($Frames | Where-Object { (Test-Path "$repo\build\cmp\${Tag}_d3d9_$_.bmp") -and (Test-Path "$repo\build\cmp\${Tag}_d3d11_$_.bmp") })
if (-not $rows.Count) { "nothing to compare"; return }
$sheet = New-Object System.Drawing.Bitmap (3 * 640), ($rows.Count * 480)
$g = [System.Drawing.Graphics]::FromImage($sheet)
$font = New-Object System.Drawing.Font 'Consolas', 14
for ($i = 0; $i -lt $rows.Count; $i++) {
    $f = $rows[$i]
    $p9 = "$repo\build\cmp\${Tag}_d3d9_$f.bmp"; $p11 = "$repo\build\cmp\${Tag}_d3d11_$f.bmp"
    $a = Bytes $p9; $b = Bytes $p11
    $diff = New-Object byte[] $a.Length
    $n = 0; $big = 0; $sum = 0
    for ($k = 0; $k -lt $a.Length; $k += 4) {
        $m = 0
        for ($c = 0; $c -lt 3; $c++) { $dd = [Math]::Abs([int]$a[$k + $c] - [int]$b[$k + $c]); if ($dd -gt $m) { $m = $dd } }
        if ($m -gt 0) { $n++ }
        if ($m -gt 24) { $big++ }
        $sum += $m
        $v = [Math]::Min(255, $m * 4)
        $diff[$k] = $v; $diff[$k + 1] = $v; $diff[$k + 2] = $v; $diff[$k + 3] = 255
    }
    $px = $a.Length / 4
    "frame ${f}: {0:N2}% pixels differ, {1:N2}% by more than 24, mean {2:N2}" -f (100 * $n / $px), (100 * $big / $px), ($sum / $px)
    $db = New-Object System.Drawing.Bitmap 640, 480, ([System.Drawing.Imaging.PixelFormat]::Format32bppRgb)
    $rect = New-Object System.Drawing.Rectangle 0, 0, 640, 480
    $dl = $db.LockBits($rect, 'WriteOnly', [System.Drawing.Imaging.PixelFormat]::Format32bppRgb)
    [System.Runtime.InteropServices.Marshal]::Copy($diff, 0, $dl.Scan0, $diff.Length)
    $db.UnlockBits($dl)
    $i9 = [System.Drawing.Bitmap]::FromFile($p9); $i11 = [System.Drawing.Bitmap]::FromFile($p11)
    $g.DrawImage($i9, 0, $i * 480, 640, 480); $g.DrawImage($i11, 640, $i * 480, 640, 480); $g.DrawImage($db, 1280, $i * 480, 640, 480)
    $i9.Dispose(); $i11.Dispose(); $db.Dispose()
    foreach ($l in @(@(0, "d3d9 $f"), @(640, "d3d11 $f"), @(1280, "diff x4"))) {
        $g.FillRectangle([System.Drawing.Brushes]::Black, $l[0], $i * 480, 120, 22)
        $g.DrawString($l[1], $font, [System.Drawing.Brushes]::Yellow, $l[0] + 2, $i * 480 + 1)
    }
}
$g.Dispose()
$sheet.Save("$repo\build\cmp_$Tag.png", [System.Drawing.Imaging.ImageFormat]::Png); $sheet.Dispose()
"sheet: build\cmp_$Tag.png"
