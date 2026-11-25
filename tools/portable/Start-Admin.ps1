param([int]$Port = 17861, [switch]$NoBrowser)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Admin-Http.ps1')
if ($Port -lt 0 -or $Port -gt 65535) { throw 'Port must be 0..65535.' }
$awbAdminRoot = Join-Path $PSScriptRoot 'data\admin'
New-Item -ItemType Directory -Path $awbAdminRoot -Force | Out-Null
$awbProcess = Start-Process -FilePath (Join-Path $PSScriptRoot 'workbench-admin.exe') `
    -ArgumentList ('--root "' + $awbAdminRoot + '" --port ' + $Port) -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput (Join-Path $awbAdminRoot 'startup.log') `
    -RedirectStandardError (Join-Path $awbAdminRoot 'startup-error.log')
$awbDeadline = [DateTime]::UtcNow.AddSeconds(15)
$awbEndpoint = $null
while ([DateTime]::UtcNow -lt $awbDeadline) {
    if ($awbProcess.HasExited) { throw ('Admin exited. Read ' + (Join-Path $awbAdminRoot 'startup-error.log')) }
    $awbFile = Join-Path $awbAdminRoot 'admin-endpoint.json'
    if (Test-Path -LiteralPath $awbFile) {
        try {
            $awbCandidate = Read-AwbAdminEndpoint -Path $awbFile
            if ($awbCandidate.pid -eq $awbProcess.Id) { $awbEndpoint = $awbCandidate; break }
        } catch [System.IO.IOException] { }
    }
    Start-Sleep -Milliseconds 100
}
if (-not $awbEndpoint) { throw 'Admin startup did not finish. Inspect startup logs before retrying.' }
Write-Output ('Admin ready: ' + $awbEndpoint.url)
Write-Output ('First initialization key: ' + (Join-Path $awbAdminRoot 'bootstrap.key'))
if (-not $NoBrowser) { Start-Process -FilePath $awbEndpoint.url }
