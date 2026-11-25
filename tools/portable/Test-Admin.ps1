param([Parameter(Mandatory=$true)][string]$TestRoot)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Admin-Http.ps1')
if (Test-Path -LiteralPath $TestRoot) { throw 'Backend self-test needs a new isolated directory.' }
$awbInfo = [System.Diagnostics.ProcessStartInfo]::new()
$awbInfo.FileName = Join-Path $PSScriptRoot 'workbench-admin.exe'
$awbInfo.Arguments = '--root "' + $TestRoot + '" --port 0'
$awbInfo.UseShellExecute = $false
$awbInfo.CreateNoWindow = $true
$awbInfo.RedirectStandardOutput = $true
$awbInfo.RedirectStandardError = $true
$awbInfo.StandardOutputEncoding = [System.Text.UTF8Encoding]::new($false)
$awbInfo.StandardErrorEncoding = [System.Text.UTF8Encoding]::new($false)
$awbProcess = [System.Diagnostics.Process]::Start($awbInfo)
$awbReady = $null
try {
    $awbTask = $awbProcess.StandardOutput.ReadLineAsync()
    if (-not $awbTask.Wait(15000)) { throw 'Backend ready timed out.' }
    $awbReady = $awbTask.Result | ConvertFrom-Json
    if (-not $awbReady.ready) { throw ('Backend startup failed: ' + $awbProcess.StandardError.ReadToEnd()) }
    $awbKey = [System.IO.File]::ReadAllText((Join-Path $TestRoot 'bootstrap.key'))
    $awbPassword = [guid]::NewGuid().ToString('N') + '!Test'
    $awbBody = @{ key = $awbKey; email = 'portable@example.test'; password = $awbPassword; name = 'Portable self-test' } | ConvertTo-Json
    $awbSession = Invoke-AwbAdminHttp -Uri ($awbReady.url + '/api/bootstrap') -Method Post -ContentType 'application/json' -Body $awbBody
    if (-not $awbSession.ok) { throw 'Backend bootstrap failed.' }
    $awbHeaders = @{ Authorization = ('Bearer ' + $awbSession.result.token) }
    $awbSnapshot = Invoke-AwbAdminHttp -Uri ($awbReady.url + '/api/snapshot') -Method Post -ContentType 'application/json' -Body '{}' -Headers $awbHeaders
    if (-not $awbSnapshot.ok -or $awbSnapshot.result.entitlement.environmentLimit -ne 10) { throw 'Backend free entitlement failed.' }
    $awbPlan = @{ productId='portable'; name='Portable'; amountMinor=100; days=30; environmentLimit=20; concurrentLimit=11; seatLimit=2 } | ConvertTo-Json
    $awbPublished = Invoke-AwbAdminHttp -Uri ($awbReady.url + '/api/plan.save') -Method Post -ContentType 'application/json' -Body $awbPlan -Headers $awbHeaders
    $awbOrder = @{ planId=$awbPublished.result.id; quantity=1; idempotencyKey='portable-order' } | ConvertTo-Json
    $awbCreated = Invoke-AwbAdminHttp -Uri ($awbReady.url + '/api/order.create') -Method Post -ContentType 'application/json' -Body $awbOrder -Headers $awbHeaders
    $awbPayment = @{ orderId=$awbCreated.result.id; amountMinor=100; receipt='portable-fixture-receipt'; reason='isolated self-test only' } | ConvertTo-Json
    $awbPaid = Invoke-AwbAdminHttp -Uri ($awbReady.url + '/api/payment.confirm') -Method Post -ContentType 'application/json' -Body $awbPayment -Headers $awbHeaders
    if (-not $awbPaid.ok -or $awbPaid.result.state -ne 'paid') { throw 'Backend manual payment failed.' }
    $awbSnapshot = Invoke-AwbAdminHttp -Uri ($awbReady.url + '/api/snapshot') -Method Post -ContentType 'application/json' -Body '{}' -Headers $awbHeaders
    if ($awbSnapshot.result.entitlement.seatLimit -ne 2 -or $awbSnapshot.result.ledger.Count -ne 1) { throw 'Backend paid grant/ledger failed.' }
    Write-Output 'PASS: admin initialization, account, free plan, product, order, manual payment, entitlement, seats and ledger.'
} finally {
    try {
    if (-not $awbProcess.HasExited -and $awbReady) {
        $awbEndpoint = Get-Content -LiteralPath (Join-Path $TestRoot 'admin-endpoint.json') -Raw -Encoding UTF8 | ConvertFrom-Json
        Invoke-AwbAdminHttp -Uri ($awbReady.url + '/api/shutdown') -Method Post -ContentType 'application/json' -Body '{}' `
            -Headers @{ Authorization = ('Bearer ' + $awbEndpoint.shutdownToken) } | Out-Null
        if (-not $awbProcess.WaitForExit(15000)) { $awbProcess.Kill(); throw 'Backend shutdown timed out.' }
    }
    } finally {
        if (-not $awbProcess.HasExited) { $awbProcess.Kill(); $awbProcess.WaitForExit(5000) | Out-Null }
        $awbProcess.Dispose()
    }
}
