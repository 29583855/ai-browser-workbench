param(
    [ValidateSet('Debug', 'Release', 'Both')]
    [string]$Configuration = 'Both',
    [string]$VsRoot = 'D:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise',
    [switch]$WithBrowserTests,
    [switch]$WithMcpTests,
    [string]$McpPython,
    [string]$Browser
)
$ErrorActionPreference = 'Stop'
$awbRoot = Split-Path -Parent $PSScriptRoot
$awbCmake = Join-Path $VsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$awbCtest = Join-Path $VsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
$awbFormatter = Join-Path $VsRoot 'VC\Tools\Llvm\bin\clang-format.exe'
$awbEvidence = Join-Path $awbRoot ('build\evidence\run-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $awbEvidence -Force | Out-Null
Push-Location $awbRoot
try {
    & $awbCmake -S . -B build/vs2019-x64 -G 'Visual Studio 16 2019' -A x64 `
        -T 'v142,version=14.29.30133' "-DCMAKE_GENERATOR_INSTANCE=$VsRoot" '-DCMAKE_SYSTEM_VERSION=10.0.19041.0' `
        2>&1 | Tee-Object -FilePath (Join-Path $awbEvidence 'configure.log')
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    if ($WithBrowserTests) {
        $awbTypescript = Join-Path $awbRoot 'build/sdk-tools/node_modules/typescript/bin/tsc'
        if (-not (Test-Path -LiteralPath $awbTypescript)) { throw 'Install pinned TypeScript 5.9.3 into build/sdk-tools; see sdk/AGENT_API_V1.md.' }
        & node $awbTypescript sdk/typescript/client.ts sdk/typescript/contract_v1.ts --target ES2022 --module commonjs --strict --outDir build/sdk-compiled
        if ($LASTEXITCODE -ne 0) { throw 'TypeScript SDK compilation failed' }
    }
    $awbConfigurations = if ($Configuration -eq 'Both') { @('Debug', 'Release') } else { @($Configuration) }
    foreach ($awbCurrentConfiguration in $awbConfigurations) {
        & $awbCmake --build build/vs2019-x64 --config $awbCurrentConfiguration --parallel 4 `
            2>&1 | Tee-Object -FilePath (Join-Path $awbEvidence ($awbCurrentConfiguration + '-build.log'))
        if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
        & $awbCtest --test-dir build/vs2019-x64 -C $awbCurrentConfiguration --output-on-failure `
            2>&1 | Tee-Object -FilePath (Join-Path $awbEvidence ($awbCurrentConfiguration + '-ctest.log'))
        if ($LASTEXITCODE -ne 0) { throw 'CTest failed' }
        & python tools/generate_contract.py --executable (Join-Path $awbRoot ('build/vs2019-x64/' + $awbCurrentConfiguration + '/workbench-cli.exe')) --check
        if ($LASTEXITCODE -ne 0) { throw 'Generated tool contract drift' }
        $awbStorageReport = Join-Path $awbRoot ('build\vs2019-x64\' + $awbCurrentConfiguration + '\storage-report.json')
        Copy-Item -LiteralPath $awbStorageReport -Destination (Join-Path $awbEvidence ($awbCurrentConfiguration + '-storage-report.json'))
        if ($WithBrowserTests) {
            if (-not $Browser) { throw 'Browser integration requires an explicit -Browser path' }
            $awbExecutable = Join-Path $awbRoot ('build\vs2019-x64\' + $awbCurrentConfiguration + '\workbench-cli.exe')
            & python tests/integration_browser.py --executable $awbExecutable --browser $Browser `
                --output (Join-Path $awbEvidence ($awbCurrentConfiguration + '-browser'))
            if ($LASTEXITCODE -ne 0) { throw 'Real browser integration failed' }
            & python tests/integration_proxy.py --executable $awbExecutable --browser $Browser `
                --output (Join-Path $awbEvidence ($awbCurrentConfiguration + '-proxy'))
            if ($LASTEXITCODE -ne 0) { throw 'Real proxy integration failed' }
            foreach ($awbSuite in @('lifecycle', 'recovery', 'socks', 'runtime', 'api', 'pages', 'inspector', 'workflow', 'diagnostics', 'extraction', 'storage_paths', 'credentials_debug', 'model_packages', 'discovery_repair', 'sdk')) {
                & python ("tests/integration_$awbSuite.py") --executable $awbExecutable --browser $Browser `
                    --output (Join-Path $awbEvidence ($awbCurrentConfiguration + '-' + $awbSuite))
                if ($LASTEXITCODE -ne 0) { throw "Integration failed: $awbSuite" }
            }
            & python tests/integration_protocol.py --cli $awbExecutable `
                --mcp (Join-Path $awbRoot ('build\vs2019-x64\' + $awbCurrentConfiguration + '\workbench-mcp.exe')) `
                --browser $Browser --output (Join-Path $awbEvidence ($awbCurrentConfiguration + '-protocol'))
            if ($LASTEXITCODE -ne 0) { throw 'Protocol integration failed' }
        }
        if ($WithMcpTests) {
            if (-not $Browser -or -not $McpPython) { throw 'MCP tests require -Browser and -McpPython' }
            $awbMcpExecutable = Join-Path $awbRoot ('build\vs2019-x64\' + $awbCurrentConfiguration + '\workbench-mcp.exe')
            & $McpPython tests/integration_mcp.py --executable $awbMcpExecutable --browser $Browser `
                --output (Join-Path $awbEvidence ($awbCurrentConfiguration + '-mcp'))
            if ($LASTEXITCODE -ne 0) { throw 'Official MCP SDK integration failed' }
        }
    }
    & python tools/audit_source.py --formatter $awbFormatter --output (Join-Path $awbEvidence 'source-audit.json')
    if ($LASTEXITCODE -ne 0) { throw 'Source quality audit failed' }
    Write-Output "Evidence: $awbEvidence"
}
finally {
    Pop-Location
}
