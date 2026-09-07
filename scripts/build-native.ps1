param([switch]$Tests)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $taskRoot
& (Join-Path $PSScriptRoot 'bootstrap-native.ps1')
New-Item -ItemType Directory -Force -Path 'build\native','dist\native\licenses' | Out-Null
$taskCompiler = Join-Path $taskRoot '.tools\zig-x86_64-windows-0.16.0\zig.exe'
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $taskRoot '.cache\zig-global'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $taskRoot '.cache\zig-native-local'
$taskFlags = @('-target','x86_64-windows-gnu','-O2','-DNDEBUG','-DUNICODE','-D_UNICODE','-DWINVER=0x0a00','-DNOMINMAX','-Ithird_party/md4c/src','-Isrc')
foreach ($name in @('md4c','entity')) {
    & $taskCompiler cc @taskFlags -c "third_party/md4c/src/$name.c" -o "build/native/$name.o" *> "build/native/$name-compile.log"
    if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath "build/native/$name-compile.log" -Tail 60; throw "C compile failed: $name" }
}
& $taskCompiler rc /c 65001 /fo build/native/native_app.res src/native_app.rc *> build/native/resources-compile.log
if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath 'build/native/resources-compile.log' -Tail 60; throw 'Native resource build failed' }
& $taskCompiler c++ @taskFlags -std=c++17 -Wall -Wextra -Wno-nullability-completeness -Ithird_party/scintilla/include -Ithird_party/lexilla/include src/native_main.cpp src/native_rich.cpp src/document.cpp src/preview.cpp build/native/md4c.o build/native/entity.o build/native/native_app.res -o dist/native/QingmoNative.exe -municode '-Wl,--subsystem,windows' -static -luser32 -lgdi32 -lcomdlg32 -lcomctl32 -lshell32 -lole32 -luuid -ldwmapi -lpsapi -limm32 *> build/native/application-compile.log
if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath 'build/native/application-compile.log' -Tail 80; throw 'Application build failed' }
if (Test-Path -LiteralPath 'dist\native\QingmoNative.pdb') { Move-Item -LiteralPath 'dist\native\QingmoNative.pdb' -Destination 'build\native\QingmoNative.pdb' -Force }
Copy-Item -LiteralPath 'third_party\scintilla\License.txt' -Destination 'dist\native\licenses\Scintilla.txt'
Copy-Item -LiteralPath 'third_party\lexilla\License.txt' -Destination 'dist\native\licenses\Lexilla.txt'
Copy-Item -LiteralPath 'third_party\md4c\LICENSE.md' -Destination 'dist\native\licenses\MD4C.txt'
foreach ($name in @('libcxx','libcxxabi','libunwind')) {
    Copy-Item -LiteralPath (Join-Path (Split-Path $taskCompiler) "lib\$name\LICENSE.TXT") -Destination "dist\native\licenses\$name.txt"
}
Copy-Item -LiteralPath (Join-Path (Split-Path $taskCompiler) 'lib\libc\mingw\COPYING') -Destination 'dist\native\licenses\mingw.txt'
Copy-Item -LiteralPath 'docs\NATIVE.md','docs\原生版开始.md' -Destination 'dist\native'
Copy-Item -LiteralPath 'LICENSE','THIRD_PARTY_NOTICES.md' -Destination 'dist\native'
if ($Tests) {
    & $taskCompiler c++ @taskFlags -std=c++17 -Wall -Wextra tests/document_tests.cpp src/document.cpp -o build/native/document_tests.exe -static *> build/native/document-tests-compile.log
    if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath 'build/native/document-tests-compile.log' -Tail 60; throw 'Document test build failed' }
    & $taskCompiler c++ @taskFlags -std=c++17 -Wall -Wextra -Wno-nullability-completeness tests/preview_tests.cpp src/preview.cpp build/native/md4c.o build/native/entity.o -o build/native/preview_tests.exe -static -luser32 *> build/native/preview-tests-compile.log
    if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath 'build/native/preview-tests-compile.log' -Tail 60; throw 'Preview test build failed' }
    & '.\build\native\document_tests.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Document tests failed' }
    & '.\build\native\preview_tests.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Preview tests failed' }
    & $taskCompiler c++ @taskFlags -std=c++17 -Wall -Wextra tests/scroll_motion_tests.cpp -o build/native/scroll_motion_tests.exe -static *> build/native/scroll-tests-compile.log
    if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath 'build/native/scroll-tests-compile.log' -Tail 60; throw 'Scroll test build failed' }
    & '.\build\native\scroll_motion_tests.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Scroll tests failed' }
    & $taskCompiler c++ @taskFlags -std=c++17 -Wall -Wextra -Wno-nullability-completeness tests/native_rich_tests.cpp src/native_rich.cpp build/native/md4c.o build/native/entity.o -o build/native/native_rich_tests.exe -static -luser32 -lgdi32 -lole32 -luuid *> build/native/native-rich-tests-compile.log
    if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath 'build/native/native-rich-tests-compile.log' -Tail 80; throw 'Native rich text test build failed' }
    & '.\build\native\native_rich_tests.exe'
    if ($LASTEXITCODE -ne 0) { throw 'Native rich text tests failed' }
    $taskSmoke = Start-Process -FilePath (Join-Path $taskRoot 'dist\native\QingmoNative.exe') -ArgumentList '--self-test' -PassThru -WindowStyle Hidden
    if (!$taskSmoke.WaitForExit(60000)) { $taskSmoke.Kill(); throw 'UI integration test timed out' }
    if ($taskSmoke.ExitCode -ne 0) { throw 'UI integration tests failed; see build/native/self-test.json' }
    Get-Content -LiteralPath 'build\native\self-test.json'
    & (Join-Path $PSScriptRoot 'write-verification.ps1')
}
Get-Item -LiteralPath 'dist\native\QingmoNative.exe','dist\native\Scintilla.dll','dist\native\Lexilla.dll' | Select-Object Name,Length
