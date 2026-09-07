param([switch]$Tests, [switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $taskRoot
if (!$SkipBuild) { & (Join-Path $PSScriptRoot 'build-native.ps1') -Tests:$Tests }
elseif ($Tests) { throw '-Tests requires a build; omit -SkipBuild.' }

# An explicit file list prevents saved documents, settings and stale outputs from entering the ZIP.
$taskFiles = @(
    'QingmoNative.exe','Scintilla.dll','Lexilla.dll','NATIVE.md','原生版开始.md','LICENSE','THIRD_PARTY_NOTICES.md',
    'licenses/Scintilla.txt','licenses/Lexilla.txt','licenses/MD4C.txt',
    'licenses/libcxx.txt','licenses/libcxxabi.txt','licenses/libunwind.txt','licenses/mingw.txt'
)
$taskOutput = Join-Path $taskRoot 'dist/native'
foreach ($taskFile in $taskFiles) {
    if (!(Test-Path -LiteralPath (Join-Path $taskOutput $taskFile) -PathType Leaf)) {
        throw "Native release file missing: $taskFile. Run scripts/build-native.ps1 first."
    }
}
New-Item -ItemType Directory -Force -Path (Join-Path $taskRoot 'build/native') | Out-Null
$taskZip = Join-Path $taskRoot 'build/Qingmo-native-0.3-windows-x64.zip'
$taskTemporaryZip = Join-Path $taskRoot ('build/native/package-' + [Guid]::NewGuid().ToString('N') + '.zip')
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
try {
    $taskArchive = [IO.Compression.ZipFile]::Open($taskTemporaryZip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($taskFile in $taskFiles) {
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($taskArchive,
                (Join-Path $taskOutput $taskFile), $taskFile,
                [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally { $taskArchive.Dispose() }
    $taskArchive = [IO.Compression.ZipFile]::OpenRead($taskTemporaryZip)
    try {
        $taskActual = @($taskArchive.Entries | ForEach-Object { $_.FullName })
        if (@(Compare-Object $taskFiles $taskActual).Count -ne 0) { throw 'ZIP contents differ from the native release file list.' }
    } finally { $taskArchive.Dispose() }
    Move-Item -LiteralPath $taskTemporaryZip -Destination $taskZip -Force
} finally {
    if (Test-Path -LiteralPath $taskTemporaryZip) { Remove-Item -LiteralPath $taskTemporaryZip -Force }
}
Get-Item -LiteralPath $taskZip | Select-Object FullName,Length
Get-FileHash -LiteralPath $taskZip -Algorithm SHA256
