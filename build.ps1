[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$RuntimeSource,
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
$taskOriginal = [IO.Path]::GetFullPath($RuntimeSource)
$taskUpstream = Join-Path $taskRoot 'third_party\capture-engine'
$taskOutput = Join-Path $taskRoot 'dist\USBMirror'
$taskSingle = Join-Path $taskRoot 'dist\single-file'
$taskBuild = Join-Path $taskRoot 'build'
$taskVendor = Join-Path $taskBuild 'vendor'

if (-not (Test-Path -LiteralPath (Join-Path $taskUpstream 'src\Core\include\iPhoneMirror\CoreApi.h'))) {
    throw 'The capture-engine source directory is missing. See README.md.'
}
$taskCore = Join-Path $taskOriginal 'iPhoneMirror.Core.dll'
if (-not (Test-Path -LiteralPath $taskCore)) { throw 'The supplied folder does not contain iPhoneMirror.Core.dll.' }
if ((Get-Item -LiteralPath $taskCore).VersionInfo.FileVersion -ne '1.8.3.0') {
    throw 'This frontend targets the working v1.8.3 capture DLL. Supply that exact baseline.'
}
foreach ($taskDirectory in @($taskOutput, $taskSingle, $taskBuild, $taskVendor, (Join-Path $taskOutput 'licenses'))) {
    New-Item -ItemType Directory -Path $taskDirectory -Force | Out-Null
}
Copy-Item -LiteralPath (Join-Path $taskUpstream 'src\Core\include\iPhoneMirror\CoreApi.h') -Destination $taskVendor
Copy-Item -LiteralPath (Join-Path $taskUpstream 'src\VirtualCamera\include\iPhoneMirror\VirtualCameraApi.h') -Destination $taskVendor

$taskRuntimeFiles = @(
    'iPhoneMirror.Core.dll', 'libusb-1.0.dll', 'libusb0.dll',
    'iPhoneMirror.UsbConfigurationSwitch.exe',
    'iPhoneMirror.VirtualCamera.dll', 'iPhoneMirror.VirtualCamera.Admin.exe'
)
$taskManifest = @()
foreach ($taskName in $taskRuntimeFiles) {
    $taskSource = Join-Path $taskOriginal $taskName
    if (-not (Test-Path -LiteralPath $taskSource)) { throw "Required baseline component is missing: $taskName" }
    $taskPackagedName = if ($taskName -eq 'iPhoneMirror.VirtualCamera.Admin.exe') { 'USBMirror.CameraSetup.exe' } else { $taskName }
    Copy-Item -LiteralPath $taskSource -Destination (Join-Path $taskOutput $taskPackagedName) -Force
    if ($taskPackagedName -ne $taskName -and (Test-Path -LiteralPath (Join-Path $taskOutput $taskName))) { Remove-Item -LiteralPath (Join-Path $taskOutput $taskName) -Force }
    $taskManifest += [pscustomobject]@{File=$taskPackagedName; SourceFile=$taskName; SHA256=(Get-FileHash -LiteralPath $taskSource -Algorithm SHA256).Hash; Bytes=(Get-Item -LiteralPath $taskSource).Length}
}
Copy-Item -LiteralPath (Join-Path $taskUpstream 'LICENSE') -Destination (Join-Path $taskOutput 'LICENSE') -Force
Copy-Item -LiteralPath (Join-Path $taskUpstream 'THIRD_PARTY_NOTICES.md') -Destination (Join-Path $taskOutput 'licenses\upstream-notices.md') -Force
$taskOldNotices = Join-Path $taskOutput 'licenses\iPhoneMirror-THIRD_PARTY_NOTICES.md'
if (Test-Path -LiteralPath $taskOldNotices) { Remove-Item -LiteralPath $taskOldNotices -Force }
Copy-Item -LiteralPath (Join-Path $taskUpstream 'third_party\libusb\COPYING') -Destination (Join-Path $taskOutput 'licenses\libusb-COPYING.txt') -Force
$taskLibUsb0License = Join-Path $taskUpstream 'third_party\libusb-win32\LICENSE.txt'
if (-not (Test-Path -LiteralPath $taskLibUsb0License)) {
    $taskLibUsb0License = @(Get-ChildItem -LiteralPath (Join-Path $taskUpstream 'third_party\libusb-win32') -File | Where-Object { $_.Name -match 'COPYING|LICENSE' } | Select-Object -First 1).FullName
}
if ($taskLibUsb0License) { Copy-Item -LiteralPath $taskLibUsb0License -Destination (Join-Path $taskOutput 'licenses\libusb-win32-LICENSE.txt') -Force }
$taskManifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskOutput 'runtime-provenance.json') -Encoding utf8
$taskOldBaseline = Join-Path $taskOutput 'baseline.ini'
if (Test-Path -LiteralPath $taskOldBaseline) { Remove-Item -LiteralPath $taskOldBaseline -Force }
if (-not (Test-Path -LiteralPath (Join-Path $taskRoot 'assets\USBMirror.ico'))) { & (Join-Path $taskRoot 'tools\generate-icon.ps1') }
Copy-Item -LiteralPath (Join-Path $taskRoot 'assets\USBMirror.ico') -Destination $taskOutput -Force
foreach ($taskLicense in @('COPYING_GPL.txt','COPYING_LGPL.txt')) {
    Copy-Item -LiteralPath (Join-Path $taskUpstream ('src\DriverInstaller\Assets\libusb-win32-1.2.6.0\' + $taskLicense)) -Destination (Join-Path $taskOutput ('licenses\driver-' + $taskLicense)) -Force
}
Copy-Item -LiteralPath (Join-Path $taskRoot 'README.txt') -Destination $taskOutput -Force
Copy-Item -LiteralPath (Join-Path $taskRoot 'README.md') -Destination $taskOutput -Force
Copy-Item -LiteralPath (Join-Path $taskRoot 'SOURCE.md') -Destination $taskOutput -Force
if (Test-Path -LiteralPath (Join-Path $taskRoot 'VALIDATION.md')) {
    Copy-Item -LiteralPath (Join-Path $taskRoot 'VALIDATION.md') -Destination $taskOutput -Force
}

$taskVsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $taskVsWhere)) { throw 'Visual Studio C++ Build Tools are required.' }
$taskVs = & $taskVsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $taskVs) { throw 'No x64 MSVC toolchain is installed.' }
$taskVcVars = Join-Path $taskVs 'VC\Auxiliary\Build\vcvars64.bat'
$taskBatch = @"
@echo off
call "$taskVcVars" >nul
if errorlevel 1 exit /b 1
rc /nologo /i"$taskRoot" /i"$taskRoot\src" /fo"$taskBuild\app.res" "$taskRoot\src\app.rc"
if errorlevel 1 exit /b 1
rc /nologo /dUSB_MIRROR_DRIVER /i"$taskRoot" /i"$taskRoot\src" /fo"$taskBuild\driver.res" "$taskRoot\src\app.rc"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I"$taskVendor" /Fo"$taskBuild\USBMirror.obj" /Fe"$taskOutput\USBMirror.exe" "$taskRoot\src\main.cpp" "$taskBuild\app.res" /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:"$taskRoot\src\USBMirror.manifest" user32.lib gdi32.lib comctl32.lib shell32.lib ole32.lib uuid.lib dwmapi.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Fo"$taskBuild\USBMirror.Driver.obj" /Fe"$taskOutput\USBMirror.Driver.exe" "$taskRoot\src\driver_main.cpp" "$taskBuild\driver.res" /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:"$taskRoot\src\USBMirror.manifest" user32.lib gdi32.lib comctl32.lib shell32.lib ole32.lib uuid.lib dwmapi.lib advapi32.lib setupapi.lib cfgmgr32.lib wintrust.lib crypt32.lib
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Fo"$taskBuild\BundlePack.obj" /Fe"$taskBuild\BundlePack.exe" "$taskRoot\src\bundle_pack.cpp" /link cabinet.lib advapi32.lib
exit /b %errorlevel%
"@
$taskBatchPath = Join-Path $taskBuild 'compile.cmd'
Set-Content -LiteralPath $taskBatchPath -Value $taskBatch -Encoding ascii
& $env:ComSpec /d /c $taskBatchPath
if ($LASTEXITCODE -ne 0) { throw 'The native frontend did not compile.' }

if (-not $SkipTests) {
    $taskReport = Join-Path $taskBuild 'self-test.json'
    $taskProcess = Start-Process -FilePath (Join-Path $taskOutput 'USBMirror.exe') -ArgumentList @('--self-test', ('"{0}"' -f $taskReport)) -WindowStyle Hidden -PassThru -Wait
    if ($taskProcess.ExitCode -ne 0) { if (Test-Path -LiteralPath $taskReport) { Get-Content -LiteralPath $taskReport }; throw 'Native/runtime checks failed.' }
    Get-Content -LiteralPath $taskReport
    $taskDriverReport = Join-Path $taskBuild 'driver-self-test.json'
    $taskDriverTest = Start-Process -FilePath (Join-Path $taskOutput 'USBMirror.Driver.exe') -ArgumentList @('--self-test', ('"{0}"' -f $taskDriverReport)) -WindowStyle Hidden -PassThru -Wait
    if ($taskDriverTest.ExitCode -ne 0) { if (Test-Path -LiteralPath $taskDriverReport) { Get-Content -LiteralPath $taskDriverReport }; throw 'Driver setup read-only checks failed.' }
    Get-Content -LiteralPath $taskDriverReport
}
if (-not $SkipTests) {
    & (Join-Path $taskBuild 'BundlePack.exe') --self-test
    if ($LASTEXITCODE -ne 0) { throw 'Bundle-format checks failed.' }
}
& (Join-Path $taskBuild 'BundlePack.exe') $taskOutput (Join-Path $taskBuild 'USBMirror.bundle')
if ($LASTEXITCODE -ne 0) { throw 'The application files could not be bundled.' }
$taskLauncherBatch = @"
@echo off
call "$taskVcVars" >nul
if errorlevel 1 exit /b 1
rc /nologo /dUSB_MIRROR_BUNDLE /i"$taskRoot" /i"$taskRoot\src" /fo"$taskBuild\launcher.res" "$taskRoot\src\app.rc"
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Fo"$taskBuild\USBMirror.Launcher.obj" /Fe"$taskSingle\USBMirror.exe" "$taskRoot\src\launcher.cpp" "$taskBuild\launcher.res" /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:"$taskRoot\src\USBMirror.manifest" user32.lib shell32.lib ole32.lib advapi32.lib cabinet.lib
exit /b %errorlevel%
"@
$taskLauncherBatchPath = Join-Path $taskBuild 'compile-launcher.cmd'
Set-Content -LiteralPath $taskLauncherBatchPath -Value $taskLauncherBatch -Encoding ascii
& $env:ComSpec /d /c $taskLauncherBatchPath
if ($LASTEXITCODE -ne 0) { throw 'The single-file launcher did not compile.' }
$taskBytes = (Get-ChildItem -LiteralPath $taskOutput -File -Recurse | Measure-Object -Property Length -Sum).Sum
[pscustomobject]@{Executable=(Join-Path $taskSingle 'USBMirror.exe'); SingleFileBytes=(Get-Item -LiteralPath (Join-Path $taskSingle 'USBMirror.exe')).Length; UnpackedExecutable=(Join-Path $taskOutput 'USBMirror.exe'); PackageBytes=$taskBytes; PackageMiB=[math]::Round($taskBytes/1MB, 2)} | ConvertTo-Json
