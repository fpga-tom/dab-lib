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
    $StageDir = Join-Path $repoRoot "dist\install\msix-v0.1.5"
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
$ManifestTemplate = Join-Path $PSScriptRoot "AppxManifest.xml.in"
$IconPath = Join-Path $PSScriptRoot "..\src\gui\winui\dab_gui.ico"

$VersionParts = $AppVersion.Split(".")
$InvalidVersionParts = @($VersionParts | Where-Object {
    $_ -notmatch '^\d{1,5}$' -or [int]$_ -gt 65535
})
if ($VersionParts.Count -lt 3 -or $VersionParts.Count -gt 4 -or $InvalidVersionParts.Count -gt 0) {
    throw "AppVersion must contain three or four numeric components, each no greater than 65535."
}
while ($VersionParts.Count -lt 4) {
    $VersionParts += "0"
}
$PackageVersion = $VersionParts -join "."

$CMake = Get-Command cmake.exe -ErrorAction SilentlyContinue
if ($null -eq $CMake) {
    $DefaultCMake = "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if (Test-Path $DefaultCMake) {
        $CMake = [pscustomobject]@{ Source = $DefaultCMake }
    } else {
        throw "cmake.exe was not found. Add it to PATH or install the Visual Studio CMake tools."
    }
}

$WindowsKitBin = "C:\Program Files (x86)\Windows Kits\10\bin"
$MakeAppx = Get-ChildItem -Path $WindowsKitBin -Filter MakeAppx.exe -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.DirectoryName -match '\\x64$' } |
    Sort-Object FullName -Descending |
    Select-Object -First 1
$SignTool = Get-ChildItem -Path $WindowsKitBin -Filter SignTool.exe -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.DirectoryName -match '\\x64$' } |
    Sort-Object FullName -Descending |
    Select-Object -First 1
if ($null -eq $MakeAppx -or $null -eq $SignTool) {
    throw "MakeAppx.exe and SignTool.exe from the Windows SDK are required."
}
if (-not (Test-Path $BuildDirPath)) {
    throw "CMake build directory not found: $BuildDirPath"
}
if (-not (Test-Path $ManifestTemplate)) {
    throw "MSIX manifest template not found: $ManifestTemplate"
}
if (-not (Test-Path $IconPath)) {
    throw "Application icon not found: $IconPath"
}

New-Item -ItemType Directory -Force -Path $StageDir, $OutputDir | Out-Null
Get-ChildItem -LiteralPath $StageDir -Force |
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

$AssetsDir = Join-Path $StageDir "Assets"
New-Item -ItemType Directory -Force -Path $AssetsDir | Out-Null
$Manifest = (Get-Content -LiteralPath $ManifestTemplate -Raw).Replace("@PACKAGE_VERSION@", $PackageVersion)
Set-Content -LiteralPath (Join-Path $StageDir "AppxManifest.xml") -Value $Manifest -Encoding UTF8

Add-Type -AssemblyName System.Drawing
$AppIcon = [System.Drawing.Icon]::new($IconPath)
try {
    foreach ($Asset in @(
        @{ Name = "StoreLogo.png"; Size = 50 },
        @{ Name = "Square44x44Logo.png"; Size = 44 },
        @{ Name = "Square150x150Logo.png"; Size = 150 }
    )) {
        $Bitmap = [System.Drawing.Bitmap]::new(
            $Asset.Size,
            $Asset.Size,
            [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
        )
        $Graphics = [System.Drawing.Graphics]::FromImage($Bitmap)
        try {
            $Graphics.Clear([System.Drawing.Color]::Transparent)
            $Graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $Graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
            $Bounds = [System.Drawing.Rectangle]::new(0, 0, $Asset.Size, $Asset.Size)
            $Graphics.DrawIcon($AppIcon, $Bounds)
            $Bitmap.Save(
                (Join-Path $AssetsDir $Asset.Name),
                [System.Drawing.Imaging.ImageFormat]::Png
            )
        } finally {
            $Graphics.Dispose()
            $Bitmap.Dispose()
        }
    }
} finally {
    $AppIcon.Dispose()
}

$PackagePath = Join-Path $OutputDir "dab-sdr-gui-$AppVersion-x64.msix"
$CertificatePath = Join-Path $OutputDir "dab-sdr-gui-test-certificate.cer"
$VerifyDir = Join-Path $StageDir "verify-unpack"
Remove-Item -LiteralPath $PackagePath -Force -ErrorAction SilentlyContinue
& $MakeAppx.FullName pack /d $StageDir /p $PackagePath /o
if ($LASTEXITCODE -ne 0) {
    throw "MakeAppx failed with exit code $LASTEXITCODE."
}

$Publisher = "CN=dab-lib"
$Certificate = Get-ChildItem Cert:\CurrentUser\My |
    Where-Object { $_.Subject -eq $Publisher -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date) } |
    Sort-Object NotAfter -Descending |
    Select-Object -First 1
if ($null -eq $Certificate) {
    $Certificate = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $Publisher `
        -FriendlyName "DAB SDR GUI MSIX development signing" `
        -CertStoreLocation Cert:\CurrentUser\My `
        -NotAfter (Get-Date).AddYears(5)
}
Export-Certificate -Cert $Certificate -FilePath $CertificatePath -Force | Out-Null
& $SignTool.FullName sign /fd SHA256 /sha1 $Certificate.Thumbprint /s My /d "DAB SDR GUI" $PackagePath
if ($LASTEXITCODE -ne 0) {
    throw "SignTool failed with exit code $LASTEXITCODE."
}

New-Item -ItemType Directory -Force -Path $VerifyDir | Out-Null
& $MakeAppx.FullName unpack /p $PackagePath /d $VerifyDir /o
if ($LASTEXITCODE -ne 0) {
    throw "MakeAppx could not validate and unpack the signed MSIX."
}
foreach ($RequiredFile in @("AppxManifest.xml", "dab_gui.exe", "faad-2.dll", "portaudio.dll")) {
    if (-not (Test-Path (Join-Path $VerifyDir $RequiredFile))) {
        throw "The MSIX is missing required package content: $RequiredFile"
    }
}

Remove-Item -LiteralPath $VerifyDir -Recurse -Force
Write-Output "Created signed MSIX: $PackagePath"
Write-Output "Development certificate: $CertificatePath"
Write-Output "Import the .cer into Trusted People before installing the MSIX. The private key remains in the current-user certificate store."
