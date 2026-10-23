# Run the game with scripted input, capture frames, build a contact sheet
# (build\sheet_<tag>.png). Uses a scratch hard disk mirrored from build\hdd_template.
#   tools\shots.ps1 -Tag menu -Keys "600:START,800:A" -Frames 700,900 [-Keys2 ".." -Frames2 ..] [-Timeout 90] [-Cols 3]
# Keys2/Frames2 script the process the game relaunches into (XLaunchNewImageA).
param([string]$Tag = 'run', [string]$Keys = '', [int[]]$Frames = @(600), [string]$Keys2 = '', [int[]]$Frames2 = @(),
      [int]$Timeout = 90, [int]$Cols = 3, [hashtable]$Env = @{}, [switch]$KeepHdd)
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
Get-Process obiwan -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item build\game\launch.bin -ErrorAction SilentlyContinue
# a scratch disk mirrored from the save-free template (the real xbox_hdd holds the owner's profile)
if (-not $KeepHdd) { robocopy build\hdd_template build\hdd_test /MIR /NFL /NDL /NJH /NJS /NP | Out-Null }
$env:RECOMP_HDD = "$repo\build\hdd_test"
Remove-Item build\shot_*.bmp -ErrorAction SilentlyContinue
Remove-Item env:RECOMP_RUN -ErrorAction SilentlyContinue
$env:RECOMP_SHOT = ($Frames -join ',')
$env:RECOMP_INPUT = $Keys
if ($Frames2.Count -or $Keys2) { $env:RECOMP_SHOT += '|' + ($Frames2 -join ','); $env:RECOMP_INPUT += '|' + $Keys2 }
foreach ($k in $Env.Keys) { Set-Item "env:$k" $Env[$k] }
$log = "build\run_$Tag.log"
$p = Start-Process -FilePath "$repo\build\game\obiwan.exe" -ArgumentList 'extracted\default.xbe' -WorkingDirectory $repo `
    -RedirectStandardError $log -RedirectStandardOutput "build\run_$Tag.out" -PassThru
$all = @($Frames | ForEach-Object { "$_" }) + @($Frames2 | ForEach-Object { "1_$_" })
$last = "build\shot_$($all[-1]).bmp"
$t0 = Get-Date
while (-not (Test-Path $last) -and ((Get-Date) - $t0).TotalSeconds -lt $Timeout) { Start-Sleep -Milliseconds 500 }
Start-Sleep -Milliseconds 700
Get-Process obiwan -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 300
Remove-Item build\game\launch.bin -ErrorAction SilentlyContinue
Remove-Item env:RECOMP_SHOT, env:RECOMP_INPUT, env:RECOMP_HDD -ErrorAction SilentlyContinue
foreach ($k in $Env.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
"elapsed $([int]((Get-Date) - $t0).TotalSeconds)s"
Add-Type -AssemblyName System.Drawing
$have = @($all | Where-Object { Test-Path "build\shot_$_.bmp" })
"captured: $($have -join ',')"
if ($have.Count) {
    $cols = [Math]::Min($Cols, $have.Count); $rows = [Math]::Ceiling($have.Count / $cols)
    $sheet = New-Object System.Drawing.Bitmap ($cols * 640), ($rows * 480)
    $g = [System.Drawing.Graphics]::FromImage($sheet)
    $font = New-Object System.Drawing.Font 'Consolas', 14
    for ($i = 0; $i -lt $have.Count; $i++) {
        $b = [System.Drawing.Bitmap]::FromFile("$repo\build\shot_$($have[$i]).bmp")
        $x = ($i % $cols) * 640; $y = [Math]::Floor($i / $cols) * 480
        $g.DrawImage($b, $x, $y, 640, 480); $b.Dispose()
        $g.FillRectangle([System.Drawing.Brushes]::Black, $x, $y, 80, 22)
        $g.DrawString("$($have[$i])", $font, [System.Drawing.Brushes]::Yellow, $x + 2, $y + 1)
    }
    $g.Dispose()
    $sheet.Save("$repo\build\sheet_$Tag.png", [System.Drawing.Imaging.ImageFormat]::Png); $sheet.Dispose()
    "sheet: build\sheet_$Tag.png"
}
Select-String -Path $log -Pattern 'exception|trap|XLaunch|launch data|BootToDash|has scale|stall|unknown|not drawn|not translated' | Select-Object -First 10 | ForEach-Object Line
