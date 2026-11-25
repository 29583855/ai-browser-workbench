param([Parameter(Mandatory=$true)][string]$Config)
$ErrorActionPreference = 'Stop'
$awbConfigPath = (Resolve-Path -LiteralPath $Config).Path
$awbNodeConfig = [System.IO.File]::ReadAllText($awbConfigPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
if (-not $awbNodeConfig.root -or -not [System.IO.Path]::IsPathRooted($awbNodeConfig.root)) { throw 'Node root must be an absolute private directory.' }
$awbNodeParent = Split-Path -Parent $awbNodeConfig.root
if (-not (Test-Path -LiteralPath $awbNodeParent -PathType Container)) { throw 'Create the configured root parent directory first.' }
$awbLogDirectory = Split-Path -Parent $awbConfigPath
$awbStamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$awbProcess = Start-Process -FilePath (Join-Path $PSScriptRoot 'workbench-node.exe') `
    -ArgumentList ('--config "' + $awbConfigPath + '"') -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $awbLogDirectory ('node-' + $awbStamp + '.log')) `
    -RedirectStandardError (Join-Path $awbLogDirectory ('node-' + $awbStamp + '-error.log'))
Write-Output ('Node process started: ' + $awbProcess.Id + '. Check its startup log and approve the node in the portal.')
