<#
.SYNOPSIS
    Copies a local UE4SS build (plus the fork's C++ mods) into a game's Win64 folder for testing.

.EXAMPLE
    tools\deploy-to-game.ps1 -GameBinaries "a0.10.0_0009\WindowsNoEditor\VotV\Binaries\Win64"

.NOTES
    Only adds/overwrites UE4SS files. Never touches the game's own files.
    An existing UE4SS-settings.ini in the game folder is preserved unless -ResetSettings is given.
#>
param(
    [Parameter(Mandatory = $true)] [string] $GameBinaries,
    [string] $BuildDir = "build_cmake_Game__Shipping__Win64\Game__Shipping__Win64\bin",
    [string[]] $CppMods = @("EventViewerMod", "KismetDebuggerMod", "ToolkitMod"),
    [switch] $ResetSettings
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo $BuildDir
$game = if ([System.IO.Path]::IsPathRooted($GameBinaries)) { $GameBinaries } else { Join-Path $repo $GameBinaries }
if (-not (Test-Path $game)) { throw "Game binaries folder not found: $game" }
if (-not (Test-Path (Join-Path $build "UE4SS.dll"))) { throw "UE4SS.dll not found in $build - build first." }

Copy-Item (Join-Path $build "UE4SS.dll") $game -Force
Copy-Item (Join-Path $build "dwmapi.dll") $game -Force

$settings = Join-Path $game "UE4SS-settings.ini"
if ($ResetSettings -or -not (Test-Path $settings)) {
    Copy-Item (Join-Path $repo "assets\UE4SS-settings.ini") $settings -Force
}

$modsRoot = Join-Path $game "Mods"
New-Item -ItemType Directory -Force $modsRoot | Out-Null
Get-ChildItem (Join-Path $repo "assets\Mods") | ForEach-Object {
    $dest = Join-Path $modsRoot $_.Name
    if ($_.PSIsContainer) { Copy-Item $_.FullName $dest -Recurse -Force } elseif (-not (Test-Path $dest)) { Copy-Item $_.FullName $dest }
}

$modsTxt = Join-Path $modsRoot "mods.txt"
$existing = if (Test-Path $modsTxt) { Get-Content $modsTxt } else { @() }
foreach ($mod in $CppMods) {
    $dll = Join-Path $build "$mod.dll"
    if (-not (Test-Path $dll)) { Write-Warning "Skipping $mod (not built)"; continue }
    $dlls = Join-Path $modsRoot "$mod\dlls"
    New-Item -ItemType Directory -Force $dlls | Out-Null
    Copy-Item $dll (Join-Path $dlls "main.dll") -Force
    # Mod-specific shipped data (default trainers, README, ...).
    $modAssets = Join-Path $repo "cppmods\$mod\assets"
    if (Test-Path $modAssets) { Copy-Item "$modAssets\*" (Join-Path $modsRoot $mod) -Recurse -Force }
    if (-not ($existing | Where-Object { $_ -match "^\s*$mod\s*:" })) {
        # Insert above the built-in keybinds line, which must stay last.
        $idx = ($existing | Select-String -Pattern "^\s*Keybinds\s*:" | Select-Object -First 1).LineNumber
        if ($idx) { $existing = @($existing[0..($idx - 2)]) + "$mod : 1" + @($existing[($idx - 1)..($existing.Count - 1)]) } else { $existing += "$mod : 1" }
    }
}
Set-Content $modsTxt $existing
Write-Host "Deployed to $game"
