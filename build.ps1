# Build and run the seeded-universe test suite.
# Requires any g++ with C++20 support on PATH (MSYS2 / WinLibs / MinGW-w64).
$ErrorActionPreference = "Stop"

$gxx = Get-Command g++ -ErrorAction SilentlyContinue
if (-not $gxx) {
    # Fall back to a WinGet-installed WinLibs toolchain if present.
    $candidate = Join-Path $env:LOCALAPPDATA `
        "Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\g++.exe"
    if (Test-Path $candidate) { $gxx = @{ Source = $candidate } }
    else { throw "g++ not found. Install one, e.g.: winget install BrechtSanders.WinLibs.POSIX.UCRT" }
}

& $gxx.Source -std=c++20 -O2 -Wall -Wextra -static tests/test_raft.cpp -o test_raft.exe
if ($LASTEXITCODE -ne 0) { throw "compile failed" }

./test_raft.exe
exit $LASTEXITCODE
