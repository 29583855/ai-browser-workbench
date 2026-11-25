$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Admin-Http.ps1')
$awbFile = Join-Path $PSScriptRoot 'data\admin\admin-endpoint.json'
$awbEndpoint = Read-AwbAdminEndpoint -Path $awbFile
$awbResponse = Invoke-AwbAdminHttp -Uri ($awbEndpoint.url + '/api/shutdown') -Method Post `
    -ContentType 'application/json' -Body '{}' -Headers @{ Authorization = ('Bearer ' + $awbEndpoint.shutdownToken) }
if (-not $awbResponse.ok) { throw 'Admin shutdown was rejected.' }
Write-Output 'Admin shutdown requested; wait for the process to exit before copying its database.'
