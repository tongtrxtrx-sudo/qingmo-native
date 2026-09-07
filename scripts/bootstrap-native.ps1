$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $taskRoot
New-Item -ItemType Directory -Force -Path '.cache','.tools','third_party','dist\native','build\native' | Out-Null

function Get-VerifiedArchive([string]$Name, [string[]]$Uris, [string]$Hash) {
    $archive = Join-Path $taskRoot ('.cache\' + $Name)
    if (!(Test-Path -LiteralPath $archive)) {
        Write-Host "Downloading $Name"
        $downloaded = $false
        foreach ($uri in $Uris) {
            & curl.exe --fail --location --silent --show-error --connect-timeout 15 --max-time 180 --output $archive $uri
            if ($LASTEXITCODE -eq 0) { $downloaded = $true; break }
        }
        if (!$downloaded) { throw "Download failed: $Name" }
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $Hash) {
        throw "Checksum mismatch: $archive. Remove this archive and retry."
    }
    return $archive
}

$taskCompiler = Join-Path $taskRoot '.tools\zig-x86_64-windows-0.16.0\zig.exe'
if (!(Test-Path -LiteralPath $taskCompiler)) {
    $archive = Get-VerifiedArchive 'zig-0.16.0.zip' 'https://ziglang.org/download/0.16.0/zig-x86_64-windows-0.16.0.zip' '68659EB5F1E4EB1437A722F1DD889C5A322C9954607F5EDCF337BC3684A75A7E'
    & tar.exe -xf $archive -C '.tools'
    if ($LASTEXITCODE -ne 0) { throw 'Compiler extraction failed' }
}
if (!(Test-Path -LiteralPath 'third_party\scintilla\include\Scintilla.h') -or !(Test-Path -LiteralPath 'third_party\lexilla\include\Lexilla.h') -or !(Test-Path -LiteralPath 'third_party\scintilla\License.txt') -or !(Test-Path -LiteralPath 'third_party\lexilla\License.txt')) {
    $archive = Get-VerifiedArchive 'scite566.zip' @('https://sourceforge.net/projects/scintilla/files/SciTE/5.6.6/scite566.zip/download','https://www.scintilla.org/scite566.zip') '84D7DBE9D9CEB34961AD6FBDCF91A24F7CC1A2FD59D1851C0F5F4A0CDBFDE2F9'
    & tar.exe -xf $archive -C 'third_party' 'scintilla/include' 'scintilla/License.txt' 'lexilla/include' 'lexilla/License.txt'
    if ($LASTEXITCODE -ne 0) { throw 'Header extraction failed' }
}
if (!(Test-Path -LiteralPath 'dist\native\Scintilla.dll') -or !(Test-Path -LiteralPath 'dist\native\Lexilla.dll')) {
    $archive = Get-VerifiedArchive 'wscite566.zip' @('https://sourceforge.net/projects/scintilla/files/SciTE/5.6.6/wscite566.zip/download','https://www.scintilla.org/wscite566.zip') '2E8F2952E45F18B56ED94B3738F3129C61EA4EE833A1CB86A6A4005295D19B3F'
    New-Item -ItemType Directory -Force -Path 'build\native\scite-bin' | Out-Null
    & tar.exe -xf $archive -C 'build\native\scite-bin'
    if ($LASTEXITCODE -ne 0) { throw 'Runtime extraction failed' }
    Copy-Item -LiteralPath 'build\native\scite-bin\wscite\Scintilla.dll','build\native\scite-bin\wscite\Lexilla.dll' -Destination 'dist\native'
}
if (!(Test-Path -LiteralPath 'third_party\md4c\src\md4c.c') -or !(Test-Path -LiteralPath 'third_party\md4c\src\md4c.h') -or !(Test-Path -LiteralPath 'third_party\md4c\src\entity.c') -or !(Test-Path -LiteralPath 'third_party\md4c\src\entity.h') -or !(Test-Path -LiteralPath 'third_party\md4c\LICENSE.md')) {
    $archive = Get-VerifiedArchive 'md4c-0.5.3.zip' 'https://codeload.github.com/mity/md4c/zip/refs/tags/release-0.5.3' 'C5B2966CAF3CE9F3B97FB7672DB460C95DABD387CA57873F93979E7A9AE93026'
    & tar.exe -xf $archive -C 'build\native'
    if ($LASTEXITCODE -ne 0) { throw 'Parser extraction failed' }
    New-Item -ItemType Directory -Force -Path 'third_party\md4c\src' | Out-Null
    foreach ($name in @('md4c.c','md4c.h','entity.c','entity.h')) {
        Copy-Item -LiteralPath ('build\native\md4c-release-0.5.3\src\' + $name) -Destination 'third_party\md4c\src'
    }
    Copy-Item -LiteralPath 'build\native\md4c-release-0.5.3\LICENSE.md' -Destination 'third_party\md4c'
}
Write-Host 'Native build dependencies ready (dist/native).'
