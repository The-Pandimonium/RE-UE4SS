<#
.SYNOPSIS
    Saves a PNG of one window of a process (only that window's rectangle), for checking what the game renders.

.DESCRIPTION
    Games started with UE4SS own two top-level windows: the game viewport and the UE4SS debugging UI.
    -TitleLike selects the window by title substring; -ExcludeTitleLike skips windows (default: the UE4SS UI).

.EXAMPLE
    tools\capture-window.ps1 -ProcessName VotV-Win64-Shipping -Out game.png
    tools\capture-window.ps1 -ProcessName VotV-Win64-Shipping -TitleLike "UE4SS" -ExcludeTitleLike "" -Out ui.png
#>
param(
    [Parameter(Mandatory = $true)] [string] $ProcessName,
    [Parameter(Mandatory = $true)] [string] $Out,
    [string] $TitleLike = "",
    [string] $ExcludeTitleLike = "UE4SS"
)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class WinCap {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    public static List<IntPtr> WindowsOf(uint pid) {
        var list = new List<IntPtr>();
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h)) list.Add(h); return true; }, IntPtr.Zero);
        return list;
    }
    public static string Title(IntPtr h) { var sb = new StringBuilder(256); GetWindowText(h, sb, 256); return sb.ToString(); }
}
"@
[WinCap]::SetProcessDPIAware() | Out-Null
$proc = Get-Process -Name $ProcessName -ErrorAction Stop | Select-Object -First 1
$best = $null; $bestArea = 0
foreach ($h in [WinCap]::WindowsOf([uint32]$proc.Id)) {
    $title = [WinCap]::Title($h)
    if ($TitleLike -and $title -notlike "*$TitleLike*") { continue }
    if ($ExcludeTitleLike -and $title -like "*$ExcludeTitleLike*") { continue }
    $r = New-Object WinCap+RECT
    [WinCap]::GetWindowRect($h, [ref]$r) | Out-Null
    $area = ($r.Right - $r.Left) * ($r.Bottom - $r.Top)
    if ($area -gt $bestArea) { $best = $h; $bestArea = $area }
}
if (-not $best) { throw "No matching window for $ProcessName" }
$rect = New-Object WinCap+RECT
[WinCap]::GetWindowRect($best, [ref]$rect) | Out-Null
$w = $rect.Right - $rect.Left; $h = $rect.Bottom - $rect.Top
$bmp = New-Object System.Drawing.Bitmap $w, $h
$gfx = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $gfx.GetHdc()
# PW_RENDERFULLCONTENT (2) captures DirectX/composited windows even when partially covered.
$ok = [WinCap]::PrintWindow($best, $hdc, 2)
$gfx.ReleaseHdc($hdc)
if (-not $ok) { throw "PrintWindow failed; refusing to fall back to a screen copy, which would capture whatever overlaps the window." }
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$gfx.Dispose(); $bmp.Dispose()
Write-Host "Saved $Out ($w x $h) from '$([WinCap]::Title($best))'"
