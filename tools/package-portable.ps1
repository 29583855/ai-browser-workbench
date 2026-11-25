param(
    [string]$VsRoot,
    [string]$OutputDirectory,
    [string]$Python = 'python',
    [string]$BrowserArchive
)
$ErrorActionPreference = 'Stop'
$awbRoot = Split-Path -Parent $PSScriptRoot
if (-not $VsRoot) {
    $awbVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $VsRoot = & $awbVswhere -version '[16.0,17.0)' -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath -latest
    if (-not $VsRoot) { throw 'VS2019 C++ tools not found.' }
}
$awbVersion = [regex]::Match([System.IO.File]::ReadAllText((Join-Path $awbRoot 'include\awb\version.hpp')), 'Version\[\] = "([0-9.]+)-poc"')
if (-not $awbVersion.Success) { throw 'Source version is missing.' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $awbRoot ('dist\AI-Browser-Workbench-' + $awbVersion.Groups[1].Value + '-win64-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
& powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-vs2019.ps1') `
    -Configuration Release -VsRoot $VsRoot
if ($LASTEXITCODE -ne 0) { throw 'Package build/tests failed.' }
$awbArguments = @((Join-Path $PSScriptRoot 'package_portable.py'), '--vs-root', $VsRoot, '--output', $OutputDirectory)
if ($BrowserArchive) { $awbArguments += @('--browser-archive', $BrowserArchive) }
& $Python @awbArguments
if ($LASTEXITCODE -ne 0) { throw 'Portable package creation failed.' }
