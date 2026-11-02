[CmdletBinding()]
param([switch]$SkipTests)
$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
$taskSource = Join-Path $taskRoot 'third_party\capture-engine'
$taskBuild = Join-Path $taskRoot 'build\capture-engine'
$taskVsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $taskVsWhere)) { throw 'Visual Studio C++ Build Tools are required.' }
$taskVs = & $taskVsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $taskVs) { throw 'No x64 MSVC toolchain is installed.' }
$taskVcVars = Join-Path $taskVs 'VC\Auxiliary\Build\vcvars64.bat'
$taskCmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
$taskCmake = if ($taskCmakeCommand) { $taskCmakeCommand.Source } else {
    Join-Path $taskVs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
if (-not (Test-Path -LiteralPath $taskCmake)) { throw 'CMake 3.20 or newer is required. Install the C++ CMake tools in Visual Studio Build Tools.' }
$taskCtest = Join-Path (Split-Path $taskCmake -Parent) 'ctest.exe'
New-Item -ItemType Directory -Path $taskBuild -Force | Out-Null
$taskTargets = if ($SkipTests) { 'iPhoneMirror.Core' } else {
    'iPhoneMirror.Core iPhoneMirror.Core.Tests iPhoneMirror.OutputModeState.Tests iPhoneMirror.UsbConfigurationRestorePolicy.Tests USBMirror.PreviewRotation.Tests'
}
$taskBatch = @"
@echo off
call "$taskVcVars" >nul
if errorlevel 1 exit /b 1
"$taskCmake" -S "$taskSource" -B "$taskBuild" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DIPHONEMIRROR_BUILD_TESTS=ON -DIPHONEMIRROR_BUILD_DANGEROUS_USB_TOOLS=OFF -DCMAKE_PROJECT_INCLUDE="$taskRoot/tools/rotation-tests.cmake"
if errorlevel 1 exit /b 1
"$taskCmake" --build "$taskBuild" --target $taskTargets
exit /b %errorlevel%
"@
$taskBatchPath = Join-Path $taskBuild 'compile-core.cmd'
Set-Content -LiteralPath $taskBatchPath -Value $taskBatch -Encoding ascii
& $env:ComSpec /d /c $taskBatchPath
if ($LASTEXITCODE -ne 0) { throw 'The native capture engine did not compile.' }
$taskBinaryDirectory = Join-Path $taskBuild 'src\Core'
Copy-Item -LiteralPath (Join-Path $taskSource 'src\DriverInstaller\Assets\libusb-win32-1.2.6.0\amd64\libusb0.dll') -Destination $taskBinaryDirectory -Force
if (-not $SkipTests) {
    & $taskCtest --test-dir $taskBuild --output-on-failure -R '^(CoreProtocolTests|OutputModeStateTests|UsbConfigurationRestorePolicyTests|PreviewRotationRenderTests)$'
    if ($LASTEXITCODE -ne 0) { throw 'The native capture-engine regression checks failed.' }
}
Write-Output (Join-Path $taskBinaryDirectory 'iPhoneMirror.Core.dll')
