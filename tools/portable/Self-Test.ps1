param([switch]$SkipHashCheck)
$ErrorActionPreference = 'Stop'
$awbRoot = $PSScriptRoot
$awbOutput = Join-Path $awbRoot ('test-results\run-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $awbOutput -Force | Out-Null
$awbReport = [ordered]@{ result = 'failed'; date = (Get-Date -Format 'yyyy-MM-ddTHH:mm:ssK'); checks = @(); errors = @(); root = $awbRoot; cleanMachine = $false }
$awbClient = $null
$awbServer = $null
$awbSequence = 0

# 用途：使用系统.NET计算文件哈希，不依赖额外PowerShell模块；日期：2026-11-04.
function Get-AwbSha256([string]$Path) {
    $awbHash = [System.Security.Cryptography.SHA256]::Create()
    $awbStream = [System.IO.File]::OpenRead($Path)
    try {
        return [System.BitConverter]::ToString($awbHash.ComputeHash($awbStream)).Replace('-', '').ToLowerInvariant()
    } finally { $awbStream.Dispose(); $awbHash.Dispose() }
}

# 用途：构造不依赖shell的进程配置，统一UTF-8标准输入输出；日期：2026-11-04.
function New-AwbProcess([string]$Executable, [string]$Arguments) {
    $awbInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $awbInfo.FileName = if ([System.IO.Path]::IsPathRooted($Executable)) { $Executable } else { Join-Path $awbRoot $Executable }
    $awbInfo.Arguments = $Arguments
    $awbInfo.WorkingDirectory = $env:SystemRoot
    $awbInfo.UseShellExecute = $false
    $awbInfo.CreateNoWindow = $true
    $awbInfo.RedirectStandardInput = $true
    $awbInfo.RedirectStandardOutput = $true
    $awbInfo.RedirectStandardError = $true
    $awbInfo.StandardOutputEncoding = [System.Text.UTF8Encoding]::new($false)
    $awbInfo.StandardErrorEncoding = [System.Text.UTF8Encoding]::new($false)
    return [System.Diagnostics.Process]::Start($awbInfo)
}

# 用途：有界执行独立测试程序，保留输出和真实退出码；日期：2026-11-04.
function Invoke-AwbExecutable([string]$Name, [string]$Arguments) {
    $awbProcess = New-AwbProcess $Name $Arguments
    $awbOutTask = $awbProcess.StandardOutput.ReadToEndAsync()
    $awbErrorTask = $awbProcess.StandardError.ReadToEndAsync()
    try {
        if (-not $awbProcess.WaitForExit(45000)) { $awbProcess.Kill(); throw "$Name timed out." }
        $awbText = $awbOutTask.Result + $awbErrorTask.Result
        [System.IO.File]::WriteAllText((Join-Path $awbOutput "$Name.log"), $awbText, [System.Text.UTF8Encoding]::new($false))
        if ($awbProcess.ExitCode -ne 0) { throw "$Name exited $($awbProcess.ExitCode). See test-results." }
    } finally { $awbProcess.Dispose() }
    $awbReport.checks += $Name
}

# 用途：按JSONL协议驱动包内CLI，每条请求有超时且验证业务错误；日期：2026-11-04.
function Invoke-AwbCommand([string]$Command, $Arguments) {
    $script:awbSequence++
    $awbJson = @{ id = $script:awbSequence; command = $Command; arguments = $Arguments } | ConvertTo-Json -Compress -Depth 20
    $awbClient.StandardInput.WriteLine($awbJson)
    $awbClient.StandardInput.Flush()
    $awbReply = $awbClient.StandardOutput.ReadLineAsync()
    if (-not $awbReply.Wait(25000)) { throw "CLI timeout: $Command" }
    if (-not $awbReply.Result) { throw "CLI terminated: $Command" }
    $awbResponse = $awbReply.Result | ConvertFrom-Json
    if ($awbResponse.id -ne $script:awbSequence -or -not $awbResponse.ok) { throw "CLI rejected $Command : $($awbReply.Result)" }
    $awbReport.checks += $Command
    return $awbResponse.result
}

try {
    if (-not [Environment]::Is64BitOperatingSystem) { throw 'This package requires Windows x64.' }
    if (-not $SkipHashCheck) {
        $awbManifest = Get-Content -LiteralPath (Join-Path $awbRoot 'package-manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
        foreach ($awbEntry in $awbManifest.files) {
            $awbFile = Join-Path $awbRoot $awbEntry.path
            if ((Get-AwbSha256 $awbFile) -ne $awbEntry.sha256 -or ([System.IO.FileInfo]$awbFile).Length -ne $awbEntry.bytes) {
                throw "Package checksum mismatch: $($awbEntry.path)"
            }
        }
        $awbReport.checks += 'package-sha256'
    }
    Invoke-AwbExecutable 'awb-core-tests.exe' ''
    Invoke-AwbExecutable 'awb-storage-tests.exe' ('--report "' + (Join-Path $awbOutput 'storage-report.json') + '"')
    & (Join-Path $awbRoot 'Test-Admin.ps1') -TestRoot (Join-Path $awbOutput 'admin-isolated')
    $awbReport.checks += 'admin-account-orders-payments-entitlement-seats-ledger'
    Invoke-AwbExecutable 'workbench.exe' '--smoke-test'
    $awbCanvasReport = Join-Path $awbOutput 'canvas-report.json'
    Invoke-AwbExecutable 'workbench.exe' ('--flow-self-test "' + $awbCanvasReport + '"')
    $awbCanvas = Get-Content -LiteralPath $awbCanvasReport -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($awbCanvas.result -ne 'passed' -or $awbCanvas.checks.Count -ne 19) { throw 'Native flow canvas self-test failed.' }
    $awbReport.checks += 'native-flow-canvas-19-checks'
    $awbServerScript = Join-Path $awbRoot 'Smoke-Server.ps1'
    $awbPowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $awbServer = New-AwbProcess $awbPowerShell ('-NoLogo -NoProfile -ExecutionPolicy Bypass -File "' + $awbServerScript + '" -StateDirectory "' + $awbOutput + '"')
    $awbDeadline = [DateTime]::UtcNow.AddSeconds(15)
    $awbEndpointFile = Join-Path $awbOutput 'endpoint.txt'
    while (-not (Test-Path -LiteralPath $awbEndpointFile)) {
        if ($awbServer.HasExited -or [DateTime]::UtcNow -gt $awbDeadline) { throw 'Offline fixture did not start.' }
        Start-Sleep -Milliseconds 100
    }
    $awbOrigin = [System.IO.File]::ReadAllText($awbEndpointFile)
    $awbData = Join-Path $awbOutput 'cli-data'
    $awbClient = New-AwbProcess 'workbench-cli.exe' ('--root "' + $awbData + '"')
    $awbCapabilities = Invoke-AwbCommand 'capabilities' @{}
    if (-not $awbCapabilities.sqliteCatalog) { throw 'Unexpected binary capabilities.' }
    $awbClient.Refresh()
    $awbReport.runtimeModules = @()
    foreach ($awbDll in @('msvcp140.dll', 'vcruntime140.dll')) {
        $awbModule = @($awbClient.Modules | Where-Object { $_.ModuleName -eq $awbDll })
        if ($awbModule.Count -ne 1 -or $awbModule[0].FileName -ne (Join-Path $awbRoot $awbDll)) {
            throw "Runtime was not loaded from the package: $awbDll"
        }
        $awbReport.runtimeModules += $awbModule[0].FileName
    }
    $awbReport.checks += 'app-local-release-crt'
    $awbProfile = Invoke-AwbCommand 'profile_create' @{ name = 'portable-offline-smoke'; allowedOrigins = @($awbOrigin) }
    $awbPolicy = Invoke-AwbCommand 'admin_policy' @{}
    if ($awbPolicy.freeEnvironmentLimit -ne 10 -or $awbPolicy.freeConcurrentLimit -ne 10 -or -not $awbPolicy.freeProxy) { throw 'Free policy differs from the current plan.' }
    $awbExport = Invoke-AwbCommand 'profile_export' @{ profileIds = @($awbProfile.id) }
    $awbImported = Invoke-AwbCommand 'profile_import' @{ bundle = $awbExport }
    if ($awbImported.profiles[0].id -eq $awbProfile.id -or -not $awbImported.atomicCatalog) { throw 'Configuration import did not create an isolated identity.' }
    $awbGroup = Invoke-AwbCommand 'profile_group_create' @{ name = 'portable-group'; profileIds = @($awbProfile.id, $awbImported.profiles[0].id) }
    if ($awbGroup.profileIds.Count -ne 2) { throw 'Local group membership was not saved.' }
    $awbTemplate = Invoke-AwbCommand 'profile_template_create' @{ name = 'portable-template'; configuration = $awbExport.configurations[0] }
    $awbHistory = Invoke-AwbCommand 'profile_template_history' @{ id = $awbTemplate.id }
    if ($awbHistory.items.Count -ne 1 -or $awbHistory.items[0].revision -ne 1) { throw 'Template history was not retained.' }
    $awbTemplate = Invoke-AwbCommand 'profile_template_rollback' @{ id = $awbTemplate.id; expectedRevision = 1; sourceRevision = 1 }
    if ($awbTemplate.revision -ne 2) { throw 'Rollback did not create a new revision.' }
    $awbInstance = Invoke-AwbCommand 'profile_template_instantiate' @{ id = $awbTemplate.id; expectedRevision = $awbTemplate.revision; name = 'portable-instance' }
    if ($awbInstance.profiles[0].id -eq $awbProfile.id) { throw 'Template instance reused source identity.' }
    $awbReport.checks += 'free-policy-groups-templates-config-import-export'
    $awbBatchRequest = @{ requestId = [guid]::NewGuid().ToString('N'); operations = @(@{ command = 'browser_stop'; arguments = @{ profileId = $awbProfile.id } }) }
    $awbBatch = Invoke-AwbCommand 'profile_batch' $awbBatchRequest
    $awbReceipt = Invoke-AwbCommand 'profile_batch' $awbBatchRequest
    $awbStored = Invoke-AwbCommand 'profile_batch_get' @{ batchId = $awbBatch.batchId }
    if ($awbBatch.state -ne 'succeeded' -or -not $awbReceipt.receiptOnly -or $awbReceipt.replayed -or -not $awbStored.finished) { throw 'Durable batch receipt or request deduplication failed.' }
    $awbReport.checks += 'immutable-template-history-and-durable-batch-deduplication'
    $awbArchived = Invoke-AwbCommand 'profile_batch_archive' @{ batchId = $awbBatch.batchId; confirmBatchId = $awbBatch.batchId }
    $awbPage = Invoke-AwbCommand 'profile_batch_list' @{ scope = 'archived'; limit = 1 }
    $awbReceipt = Invoke-AwbCommand 'profile_batch' $awbBatchRequest
    if (-not $awbArchived.archived -or -not $awbArchived.detailsCompacted -or $awbArchived.itemCount -ne 1 -or
        $awbPage.items.Count -ne 1 -or $awbPage.hasMore -or -not $awbReceipt.receiptOnly -or
        -not $awbReceipt.archived -or $awbReceipt.replayed) { throw 'Batch archive, paging or retained deduplication failed.' }
    $awbReport.checks += 'batch-retention-paging-and-archived-request-deduplication'
    $awbStatus = Invoke-AwbCommand 'browser_start' @{ profileId = $awbProfile.id; headless = $true }
    $awbBrowser = [System.IO.Path]::GetFullPath((Join-Path $awbRoot 'browser\chrome-win64\chrome.exe'))
    if (-not $awbStatus.running -or $awbStatus.executable -ne $awbBrowser) { throw 'CLI did not start the bundled browser.' }
    $awbLease = Invoke-AwbCommand 'lease_acquire' @{ profileId = $awbProfile.id; owner = 'portable-self-test'; ttlMs = 300000 }
    $awbAction = @{ profileId = $awbProfile.id; lease = $awbLease }
    $awbNavigate = $awbAction.Clone(); $awbNavigate.url = $awbOrigin + '/'
    $null = Invoke-AwbCommand 'browser_navigate' $awbNavigate
    $awbLocate = $awbAction.Clone(); $awbLocate.selector = '#name'
    $awbRef = Invoke-AwbCommand 'page_locate' $awbLocate
    $awbFill = $awbAction.Clone(); $awbFill.ref = $awbRef.ref; $awbFill.value = 'portable-smoke'
    $null = Invoke-AwbCommand 'page_fill' $awbFill
    $awbLocate.selector = '#submit'
    $awbRef = Invoke-AwbCommand 'page_locate' $awbLocate
    $awbClick = $awbAction.Clone(); $awbClick.ref = $awbRef.ref
    $null = Invoke-AwbCommand 'page_click' $awbClick
    $awbWait = $awbAction.Clone(); $awbWait.selector = '#result'; $awbWait.expected = 'server-confirmed:portable-smoke'; $awbWait.timeoutMs = 10000
    $null = Invoke-AwbCommand 'page_wait_text' $awbWait
    if ([System.IO.File]::ReadAllText((Join-Path $awbOutput 'receipt.txt')) -ne '1') { throw 'Server did not acknowledge the page action.' }
    $awbReport.checks += 'offline-page-server-receipt'
    $null = Invoke-AwbCommand 'browser_stop' @{ profileId = $awbProfile.id }
    $awbClient.StandardInput.Close()
    if (-not $awbClient.WaitForExit(10000) -or $awbClient.ExitCode -ne 0) { throw 'CLI shutdown failed.' }
    $awbClient.Dispose(); $awbClient = $null
    $awbClient = New-AwbProcess 'workbench-mcp.exe' ('--root "' + (Join-Path $awbOutput 'mcp-data') + '"')
    $awbClient.StandardInput.WriteLine('{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"portable-smoke","version":"1"}}}')
    $awbReply = $awbClient.StandardOutput.ReadLineAsync()
    if (-not $awbReply.Wait(10000)) { throw 'MCP initialize timeout.' }
    $awbInitialize = $awbReply.Result | ConvertFrom-Json
    if (-not $awbInitialize.result.serverInfo -or $awbInitialize.error) { throw 'MCP handshake failed.' }
    $awbClient.StandardInput.WriteLine('{"jsonrpc":"2.0","method":"notifications/initialized"}')
    $awbClient.StandardInput.WriteLine('{"jsonrpc":"2.0","id":2,"method":"tools/list"}')
    $awbReply = $awbClient.StandardOutput.ReadLineAsync()
    if (-not $awbReply.Wait(10000)) { throw 'MCP tools timeout.' }
    $awbTools = $awbReply.Result | ConvertFrom-Json
    $awbExpectedContract = Get-Content -LiteralPath (Join-Path $awbRoot 'source/docs/contracts/tools-v1.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($awbTools.result.tools.Count -ne $awbExpectedContract.tools.Count) { throw 'MCP tool registry mismatch.' }
    foreach ($awbExpectedTool in $awbExpectedContract.tools) {
        $awbActualTool = @($awbTools.result.tools | Where-Object { $_.name -eq $awbExpectedTool.name })
        if ($awbActualTool.Count -ne 1 -or ($awbActualTool[0].inputSchema | ConvertTo-Json -Depth 32 -Compress) -ne ($awbExpectedTool.inputSchema | ConvertTo-Json -Depth 32 -Compress)) {
            throw "MCP tool schema mismatch: $($awbExpectedTool.name)"
        }
    }
    foreach ($awbRequiredTool in @('profile_update', 'profile_archive', 'profile_trash', 'profile_restore', 'profile_purge', 'page_list', 'page_open', 'page_switch', 'page_close', 'browser_events', 'browser_reconnect', 'page_inspect', 'page_preview', 'page_clear_highlight', 'page_select')) {
        if ($awbRequiredTool -notin $awbTools.result.tools.name) { throw "Missing lifecycle tool: $awbRequiredTool" }
    }
    foreach ($awbRequiredTool in @('recorder_start', 'recorder_read', 'recorder_control', 'page_pick_start', 'page_pick_read', 'page_pick_stop')) {
        if ($awbRequiredTool -notin $awbTools.result.tools.name) { throw "Missing visual RPA tool: $awbRequiredTool" }
    }
    $awbClient.StandardInput.Close()
    if (-not $awbClient.WaitForExit(10000) -or $awbClient.ExitCode -ne 0) { throw 'MCP shutdown failed.' }
    $awbReport.checks += 'mcp-initialize-tools-list'
    $awbClient.Dispose(); $awbClient = $null
    $awbProbe = Join-Path $awbOutput 'default-root-probe'
    New-Item -ItemType Directory -Path $awbProbe | Out-Null
    foreach ($awbProbeFile in @('workbench-cli.exe', 'portable.mode')) {
        [System.IO.File]::Copy((Join-Path $awbRoot $awbProbeFile), (Join-Path $awbProbe $awbProbeFile))
    }
    foreach ($awbDllFile in [System.IO.Directory]::GetFiles($awbRoot, '*.dll')) {
        [System.IO.File]::Copy($awbDllFile, (Join-Path $awbProbe ([System.IO.Path]::GetFileName($awbDllFile))))
    }
    $awbClient = New-AwbProcess (Join-Path $awbProbe 'workbench-cli.exe') ('--browser "' + $awbBrowser + '"')
    $null = Invoke-AwbCommand 'capabilities' @{}
    if (-not (Test-Path -LiteralPath (Join-Path $awbProbe 'data\desktop\workbench.sqlite3'))) {
        throw 'Default portable data directory was not created beside the executable.'
    }
    $awbClient.StandardInput.Close()
    if (-not $awbClient.WaitForExit(10000) -or $awbClient.ExitCode -ne 0) { throw 'Default-root shutdown failed.' }
    $awbReport.checks += 'default-portable-root-from-unrelated-cwd'
    $awbReport.checks += 'self-test-keeps-live-user-data-isolated'
    $awbReport.result = 'passed'
} catch {
    $awbReport.errors += $_.Exception.Message
    Write-Host $_.Exception.Message -ForegroundColor Red
} finally {
    if ($awbClient) {
        if (-not $awbClient.HasExited) { $awbClient.Kill(); $awbClient.WaitForExit() }
        $awbClient.Dispose()
    }
    if ($awbServer) {
        if (-not $awbServer.HasExited) { $awbServer.Kill(); $null = $awbServer.WaitForExit(5000) }
        $awbServer.Dispose()
    }
    $awbReportPath = Join-Path $awbOutput 'report.json'
    [System.IO.File]::WriteAllText($awbReportPath, ($awbReport | ConvertTo-Json -Depth 12), [System.Text.UTF8Encoding]::new($false))
    Write-Host "Self-test: $($awbReport.result). Report: $awbReportPath"
}
if ($awbReport.result -ne 'passed') { exit 1 }
exit 0
