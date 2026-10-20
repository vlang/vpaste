[CmdletBinding()]
param([switch]$IsolatedDesktop)
$ErrorActionPreference = 'Stop'
if (!$IsolatedDesktop) {
    throw 'This integration test types fixture text and overwrites the clipboard. Run it only on a disposable interactive desktop, with -IsolatedDesktop.'
}
if (![Environment]::UserInteractive) {
    throw 'Windows input tests require an unlocked interactive desktop. A service/session-0 runner cannot validate global input.'
}
$Root = Split-Path -Parent $PSScriptRoot
Push-Location $Root
try {
    $CC = $env:CC
    if (!$CC) { $CC = 'gcc' }
    New-Item -ItemType Directory -Force build | Out-Null
    & $CC -std=c11 -O2 -Wall -Wextra -Werror tests/windows_backend_test.c -o build/windows_backend_test.exe -luser32
    if ($LASTEXITCODE -ne 0) { throw 'Native Windows integration harness failed to compile.' }
    & ./build/windows_backend_test.exe
    if ($LASTEXITCODE -ne 0) { throw 'Native Windows clipboard/input integration tests failed.' }
} finally { Pop-Location }
