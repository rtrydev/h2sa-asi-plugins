# Build the H2SA plugins on Windows with MSYS2's 32-bit mingw-w64 gcc.
# Mirrors the mac flow: (cd runtime && make) then ./install.sh.
#
#   winget install MSYS2.MSYS2
#   C:\msys64\usr\bin\pacman.exe -S --noconfirm mingw-w64-i686-gcc make
#   .\build.ps1 -Install
param([switch]$Install)

$ErrorActionPreference = "Stop"
$mingw = "C:\msys64\mingw32\bin"
$msys  = "C:\msys64\usr\bin"
if (-not (Test-Path "$mingw\gcc.exe")) {
    throw "32-bit mingw gcc not found at $mingw. Install: pacman -S mingw-w64-i686-gcc make"
}
$env:PATH = "$mingw;$msys;$env:PATH"

Push-Location "$PSScriptRoot\runtime"
try {
    & "$msys\make.exe"
    if ($LASTEXITCODE -ne 0) { throw "make failed" }
} finally { Pop-Location }

if ($Install) {
    & "$msys\bash.exe" "$PSScriptRoot\install.sh"
}
