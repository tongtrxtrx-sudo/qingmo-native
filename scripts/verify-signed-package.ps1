param([Parameter(Mandatory)][string]$UnsignedZip, [Parameter(Mandatory)][string]$SignedZip)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
function Read-EntryHash($Entry) {
    $taskStream=$Entry.Open()
    $taskSha=[Security.Cryptography.SHA256]::Create()
    try { [BitConverter]::ToString($taskSha.ComputeHash($taskStream)).Replace('-','') }
    finally { $taskStream.Dispose(); $taskSha.Dispose() }
}
$taskOriginal=[IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($UnsignedZip))
try {
    $taskSigned=[IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($SignedZip))
    try {
        $taskNames=@($taskOriginal.Entries | ForEach-Object FullName)
        $taskSignedNames=@($taskSigned.Entries | ForEach-Object FullName)
        if (@(Compare-Object $taskNames $taskSignedNames).Count -ne 0 -or
            @($taskSignedNames | Select-Object -Unique).Count -ne $taskSignedNames.Count) { throw 'Signed ZIP file list differs.' }
        foreach($taskEntry in $taskOriginal.Entries) {
            if ($taskEntry.FullName -eq 'QingmoNative.exe') { continue }
            $taskSignedEntry=$taskSigned.GetEntry($taskEntry.FullName)
            if ($taskEntry.Length -ne $taskSignedEntry.Length -or (Read-EntryHash $taskEntry) -ne (Read-EntryHash $taskSignedEntry)) {
                throw "Signing changed an unrelated payload: $($taskEntry.FullName)"
            }
        }
        $taskExe=$taskSigned.GetEntry('QingmoNative.exe')
        if (!$taskExe -or $taskExe.Length -gt 32MB) { throw 'Missing or unexpectedly large signed executable.' }
        # Extract only the explicitly named EXE to a generated local path; never use archive paths.
        $taskCheckDir=Join-Path (Split-Path -Parent $PSScriptRoot) ('build/signature-check/' + [Guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $taskCheckDir -Force | Out-Null
        $taskExePath=Join-Path $taskCheckDir 'QingmoNative.exe'
        [IO.Compression.ZipFileExtensions]::ExtractToFile($taskExe,$taskExePath)
        $taskSignature=Get-AuthenticodeSignature -LiteralPath $taskExePath
        if($taskSignature.Status -ne 'Valid') { throw 'Signed EXE does not have a valid trusted Authenticode signature.' }
        $taskReport=[ordered]@{
            packageSHA256=(Get-FileHash -LiteralPath $SignedZip -Algorithm SHA256).Hash
            executableSHA256=(Get-FileHash -LiteralPath $taskExePath -Algorithm SHA256).Hash
            authenticodeStatus=[string]$taskSignature.Status
            signerSubject=$taskSignature.SignerCertificate.Subject
            signerThumbprint=$taskSignature.SignerCertificate.Thumbprint
            unrelatedPayloads='unchanged'
        }
        $taskReport | ConvertTo-Json | Set-Content -LiteralPath (Join-Path (Split-Path ([IO.Path]::GetFullPath($SignedZip))) 'signed-verification.json') -Encoding UTF8
    } finally { $taskSigned.Dispose() }
} finally { $taskOriginal.Dispose() }
