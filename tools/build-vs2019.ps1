param(
    [ValidateSet('Debug', 'Release', 'Both')]
    [string]$Configuration = 'Both',
    [string]$VsRoot,
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$awbRoot = Split-Path -Parent $PSScriptRoot
if (-not $VsRoot) {
    $awbVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $awbVswhere)) { throw 'VS2019 Installer/vswhere not found.' }
    $VsRoot = & $awbVswhere -version '[16.0,17.0)' -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath -latest
    if (-not $VsRoot) { throw 'Install VS2019 Desktop development with C++ (v142) and SDK 10.0.19041.0.' }
}
$awbMsbuild = Join-Path $VsRoot 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path -LiteralPath $awbMsbuild)) { throw 'VS2019 MSBuild not found.' }
$awbEvidence = Join-Path $awbRoot ('build\evidence\native-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $awbEvidence -Force | Out-Null
$awbConfigurations = if ($Configuration -eq 'Both') { @('Debug', 'Release') } else { @($Configuration) }
foreach ($awbConfig in $awbConfigurations) {
    & $awbMsbuild (Join-Path $awbRoot 'AIBrowserWorkbench.sln') /m:4 /t:Build `
        "/p:Configuration=$awbConfig" /p:Platform=x64 /nologo `
        "/flp:logfile=$awbEvidence\$awbConfig-build.log;verbosity=normal;encoding=UTF-8"
    if ($LASTEXITCODE -ne 0) { throw 'Native VS2019 solution build failed.' }
    if (-not $SkipTests) {
        $awbOutput = Join-Path $awbRoot "build\native-vs2019\x64\$awbConfig"
        & (Join-Path $awbOutput 'awb-core-tests.exe')
        if ($LASTEXITCODE -ne 0) { throw 'Core tests failed.' }
        & (Join-Path $awbOutput 'awb-storage-tests.exe') --report (Join-Path $awbEvidence "$awbConfig-storage-report.json")
        if ($LASTEXITCODE -ne 0) { throw 'Storage tests failed.' }
        $awbProcess = Start-Process -FilePath (Join-Path $awbOutput 'workbench.exe') -ArgumentList '--smoke-test' `
            -PassThru -WindowStyle Hidden
        if (-not $awbProcess.WaitForExit(30000)) { $awbProcess.Kill(); throw 'Win32 smoke timed out.' }
        if ($awbProcess.ExitCode -ne 0) { throw 'Win32 smoke failed.' }
    }
}
if ($SkipTests) {
    Write-Output "Native solution build passed; tests skipped. Output: $awbRoot\build\native-vs2019\x64"
} else {
    Write-Output "Native solution and tests passed. Output: $awbRoot\build\native-vs2019\x64"
}
Write-Output "Evidence: $awbEvidence"
