[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Version = '0.5.2'
$Expected = '5f1d619b6b04a2b54b4ad21826a25bdcba2acf75a941c8f72fc95672b6b064ca'
$Tools = Join-Path $Root '.tools'
$Archive = Join-Path $Tools 'v_windows.zip'
New-Item -ItemType Directory -Force $Tools | Out-Null
if (!(Test-Path $Archive)) {
    Invoke-WebRequest -Uri "https://github.com/vlang/v/releases/download/$Version/v_windows.zip" -OutFile "$Archive.part"
    Move-Item "$Archive.part" $Archive
}
if ((Get-FileHash -Algorithm SHA256 $Archive).Hash.ToLowerInvariant() -ne $Expected) {
    throw "V archive SHA-256 mismatch: $Archive"
}
$Compiler = Join-Path $Tools 'v/v.exe'
if (!(Test-Path $Compiler)) { Expand-Archive -Path $Archive -DestinationPath $Tools -Force }
$ActualVersion = & $Compiler version
if ($LASTEXITCODE -ne 0) { throw 'Downloaded V compiler did not start.' }
if ($ActualVersion -notmatch ('^V ' + [regex]::Escape($Version) + '(?:\s|$)')) { throw "Unexpected compiler in .tools/v: $ActualVersion" }
Write-Output $ActualVersion
Write-Output "Compiler: $Compiler"
