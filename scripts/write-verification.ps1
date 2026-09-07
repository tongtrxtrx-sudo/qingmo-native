# Called only after all native tests pass in build-native.ps1.
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
$taskSelfTest = Get-Content -LiteralPath (Join-Path $taskRoot 'build/native/self-test.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$taskReport = [ordered]@{
    generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    commit = $env:GITHUB_SHA
    workflowRunId = $env:GITHUB_RUN_ID
    mainExecutableSHA256 = (Get-FileHash -LiteralPath (Join-Path $taskRoot 'dist/native/QingmoNative.exe') -Algorithm SHA256).Hash
    unitTests = 'passed: document, preview, scroll_motion, native_rich'
    windowIntegration = $taskSelfTest
    signed = $false
    memoryBenchmark = 'not measured'
}
$taskReport | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $taskRoot 'build/native/verification.json') -Encoding UTF8
