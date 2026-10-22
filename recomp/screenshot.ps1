# Capture the game window's client area to a PNG (for unattended runs):
#   powershell -ExecutionPolicy Bypass -File recomp\screenshot.ps1 build\shot.png
param([string]$Out = "build\screenshot.png")
Add-Type -AssemblyName System.Drawing
$src = @"
using System;
using System.Runtime.InteropServices;
[StructLayout(LayoutKind.Sequential)] public struct ObiRect { public int Left, Top, Right, Bottom; }
[StructLayout(LayoutKind.Sequential)] public struct ObiPoint { public int X, Y; }
public static class ObiWin {
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out ObiRect r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref ObiPoint p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
}
"@
Add-Type -TypeDefinition $src -ErrorAction Stop
$proc = Get-Process obiwan -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $proc) { Write-Error "no game window"; exit 1 }
$h = $proc.MainWindowHandle
[ObiWin]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 300
$r = New-Object ObiRect
[ObiWin]::GetClientRect($h, [ref]$r) | Out-Null
$p = New-Object ObiPoint
[ObiWin]::ClientToScreen($h, [ref]$p) | Out-Null
$w = $r.Right - $r.Left; $hgt = $r.Bottom - $r.Top
if ($w -le 0 -or $hgt -le 0) { Write-Error "empty client area"; exit 1 }
$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($p.X, $p.Y, 0, 0, $bmp.Size)
$bmp.Save((Resolve-Path (Split-Path $Out -Parent)).Path + "\" + (Split-Path $Out -Leaf), [System.Drawing.Imaging.ImageFormat]::Png)
"saved $Out ($w x $hgt)"
