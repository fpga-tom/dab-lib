param(
    [string]$BuildDir = "out\build\winui-v015",
    [string]$Config = "Release",
    [string]$StageDir = "",
    [string]$OutputDir = "",
    [string]$AppVersion = "0.1.5"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($StageDir)) {
    $StageDir = Join-Path $repoRoot "dist\install\msi-v0.1.5"
}
if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $repoRoot "dist\installer"
}
if (-not [System.IO.Path]::IsPathRooted($StageDir)) {
    $StageDir = Join-Path $repoRoot $StageDir
}
if (-not [System.IO.Path]::IsPathRooted($OutputDir)) {
    $OutputDir = Join-Path $repoRoot $OutputDir
}
$StageDir = [System.IO.Path]::GetFullPath($StageDir)
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)
$BuildDirPath = Join-Path $repoRoot $BuildDir
$WixSource = Join-Path $PSScriptRoot "dab_gui.wxs"
$PackageVersion = "$AppVersion.0"

$VersionParts = $AppVersion.Split(".")
$InvalidVersion = $VersionParts.Count -ne 3
if (-not $InvalidVersion) {
    $InvalidVersion = $VersionParts[0] -notmatch '^\d{1,3}$' -or [int]$VersionParts[0] -gt 255 -or
        $VersionParts[1] -notmatch '^\d{1,3}$' -or [int]$VersionParts[1] -gt 255 -or
        $VersionParts[2] -notmatch '^\d{1,5}$' -or [int]$VersionParts[2] -gt 65535
}
if ($InvalidVersion) {
    throw "AppVersion must be a valid MSI version: major.minor.build, with major and minor at most 255 and build at most 65535."
}

if (-not (Test-Path $BuildDirPath)) {
    throw "CMake build directory not found: $BuildDirPath"
}
if (-not (Test-Path $WixSource)) {
    throw "WiX source file not found: $WixSource"
}

$Wix = Get-Command wix.exe -ErrorAction SilentlyContinue
if ($null -eq $Wix) {
    $LocalWix = Join-Path $repoRoot "out\tools\wix5\wix.exe"
    if (Test-Path $LocalWix) {
        $Wix = [pscustomobject]@{ Source = $LocalWix }
    } else {
        throw "WiX 5 is required. Install with: dotnet tool install wix --tool-path out\tools\wix5 --version 5.0.2"
    }
}

$CMake = Get-Command cmake.exe -ErrorAction SilentlyContinue
if ($null -eq $CMake) {
    $DefaultCMake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $DefaultCMake) {
        $CMake = [pscustomobject]@{ Source = $DefaultCMake }
    } else {
        throw "cmake.exe was not found. Add it to PATH or install the Visual Studio CMake tools."
    }
}

New-Item -ItemType Directory -Force -Path $StageDir, $OutputDir | Out-Null
Get-ChildItem -LiteralPath $StageDir -Force -ErrorAction SilentlyContinue |
    Remove-Item -Recurse -Force
& $CMake.Source --install $BuildDirPath --config $Config --prefix $StageDir
if ($LASTEXITCODE -ne 0) {
    throw "CMake install failed with exit code $LASTEXITCODE."
}

foreach ($RequiredFile in @("dab_gui.exe", "faad-2.dll", "portaudio.dll", "LICENSE")) {
    if (-not (Test-Path (Join-Path $StageDir $RequiredFile))) {
        throw "The staged application is missing required file: $RequiredFile"
    }
}

# MUI satellite files are optional translations and have language tags that are
# not valid Windows Installer Language identifiers. The GUI's built-in text is English.
Get-ChildItem -LiteralPath $StageDir -Filter "*.mui" -File -Recurse |
    Remove-Item -Force
Get-ChildItem -LiteralPath $StageDir -Filter "*.pdb" -File -Recurse |
    Remove-Item -Force

$PackagePath = Join-Path $OutputDir "dab-sdr-gui-$AppVersion-x64.msi"
$PackagePdbPath = [System.IO.Path]::ChangeExtension($PackagePath, ".wixpdb")
$IntermediateFolder = Join-Path $BuildDirPath "msi-intermediate"
New-Item -ItemType Directory -Force -Path $IntermediateFolder | Out-Null
Remove-Item -LiteralPath $PackagePath, $PackagePdbPath -Force -ErrorAction SilentlyContinue
& $Wix.Source build $WixSource `
    -arch x64 `
    -d "StageDir=$StageDir" `
    -d "PackageVersion=$PackageVersion" `
    -intermediatefolder $IntermediateFolder `
    -pdbtype none `
    -o $PackagePath
if ($LASTEXITCODE -ne 0) {
    throw "WiX failed to build the MSI with exit code $LASTEXITCODE."
}

Write-Output "Created MSI: $PackagePath"
