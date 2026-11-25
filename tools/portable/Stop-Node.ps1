param([Parameter(Mandatory=$true)][string]$Config)
$ErrorActionPreference = 'Stop'
$awbConfigPath = (Resolve-Path -LiteralPath $Config).Path
& (Join-Path $PSScriptRoot 'workbench-node.exe') --stop --config $awbConfigPath
if ($LASTEXITCODE -ne 0) { throw 'Node stop request failed; inspect the node process before retrying.' }
Write-Output 'Stop requested. The node fences new actions, stops its browsers, and preserves completion receipts.'
