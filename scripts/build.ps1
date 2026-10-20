[CmdletBinding()]
param([switch]$Package)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
Push-Location $Root
try {
    if (![Environment]::Is64BitOperatingSystem) { throw 'Windows x64 is required.' }
    $V = $env:KEYPOUR_V
    if (!$V) {
        $Bundled = Join-Path $Root '.tools/v/v.exe'
        if (Test-Path $Bundled) { $V = $Bundled } else { $V = (Get-Command v -ErrorAction Stop).Source }
    }
    $CC = $env:CC
    if (!$CC) { $CC = 'gcc' }
    if ((& $CC -dumpmachine) -notmatch 'x86_64.*mingw') { throw 'Use a 64-bit MinGW-w64 GCC compiler.' }
    New-Item -ItemType Directory -Force build, dist | Out-Null
    & $CC -std=c11 -O2 -Wall -Wextra -D_WIN32_WINNT=0x0A00 -I native -c native/keypour_windows.c -o build/native.o
    if ($LASTEXITCODE -ne 0) { throw 'Windows native backend compilation failed.' }
    & $V -cc $CC -prod -cflags '-D_WIN32_WINNT=0x0A00' -ldflags '-static-libgcc' -o dist/keypour.exe src
    if ($LASTEXITCODE -ne 0) { throw 'V compilation failed.' }
    & $V -cc $CC test core
    if ($LASTEXITCODE -ne 0) { throw 'Core tests failed.' }
    & ./dist/keypour.exe --version
    if ($LASTEXITCODE -ne 0) { throw 'Binary smoke test failed.' }
    & ./dist/keypour.exe --help | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Help smoke test failed.' }
    if ($Package) {
        $BinaryVersion = ((& ./dist/keypour.exe --version) -split '\s+')[-1]
        $SourceVersion = python scripts/project-version.py
        if ($LASTEXITCODE -ne 0) { throw 'Source version validation failed.' }
        $Version = $env:KEYPOUR_VERSION
        if (!$Version) { $Version = $BinaryVersion }
        if (($Version -ne $BinaryVersion) -or ($Version -ne $SourceVersion)) { throw 'Binary, source, and package versions must match.' }
        if ($Version -notmatch '^\d+\.\d+\.\d+([-.][A-Za-z0-9.]+)?$') { throw "Invalid release version: $Version" }
        $Stage = Join-Path $Root "build/keypour-$Version-windows-amd64"
        if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
        New-Item -ItemType Directory $Stage | Out-Null
        Copy-Item dist/keypour.exe $Stage
        Copy-Item README.md, LICENSE, config.example.json, BUILDING.md, VALIDATION.md $Stage
        python scripts/runtime-notices.py $V (Join-Path $Stage 'licenses')
        if ($LASTEXITCODE -ne 0) { throw 'Runtime notice collection failed.' }
        # Verify portable distribution: only OS DLLs may be imported.
        $SystemDlls = @('kernel32.dll','user32.dll','advapi32.dll','shell32.dll','ole32.dll','oleaut32.dll','ws2_32.dll','dbghelp.dll','msvcrt.dll','ucrtbase.dll','winmm.dll','gdi32.dll','version.dll','psapi.dll','ntdll.dll','secur32.dll','bcrypt.dll')
        $Imports = & objdump -p dist/keypour.exe
        if ($LASTEXITCODE -ne 0) { throw 'objdump is required to verify runtime imports.' }
        foreach ($Line in $Imports) {
            if ($Line -match 'DLL Name:\s*(\S+)') {
                $Dll = $Matches[1].ToLowerInvariant()
                if (($Dll -notin $SystemDlls) -and ($Dll -notlike 'api-ms-win-*.dll')) {
                    throw "Unexpected runtime dependency $Dll; bundle it and its license or choose a static compiler runtime."
                }
            }
        }
        $Zip = Join-Path $Root "dist/keypour-$Version-windows-amd64.zip"
        Compress-Archive -Path $Stage -DestinationPath $Zip -Force
        Write-Output "Packaged $Zip"
    }
} finally { Pop-Location }
