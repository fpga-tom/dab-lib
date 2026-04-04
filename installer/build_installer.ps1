param(
    [string]$BuildDir = "build-win",
    [string]$Config = "Release",
    [string]$StageDir = "",
    [string]$OutputDir = "",
    [string]$AppVersion = "0.1.0"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($StageDir)) {
    $StageDir = Join-Path $repoRoot ("dist\\install\\{0}" -f $Config)
}
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $repoRoot "dist\\installer"
}

$buildDirPath = Join-Path $repoRoot $BuildDir
$scriptPath = Join-Path $PSScriptRoot "dab_gui.iss"

New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

Get-ChildItem -Path $StageDir -Force -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if ($null -eq $cmake) {
    $defaultCmake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $defaultCmake) {
        $cmake = [pscustomobject]@{ Source = $defaultCmake }
    } else {
        throw "cmake was not found. Add it to PATH or install the Visual Studio CMake tools."
    }
}
& $cmake.Source --install $buildDirPath --config $Config --prefix $StageDir

$iscc = Get-Command ISCC.exe -ErrorAction SilentlyContinue
if ($null -eq $iscc) {
    $defaultIscc = "C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
    if (Test-Path $defaultIscc) {
        $iscc = [pscustomobject]@{ Source = $defaultIscc }
    } else {
        throw "ISCC.exe was not found. Install Inno Setup 6 or add ISCC.exe to PATH."
    }
}

& $iscc.Source `
    "/DAppVersion=$AppVersion" `
    "/DSourceDir=$StageDir" `
    "/DOutputDir=$OutputDir" `
    $scriptPath
