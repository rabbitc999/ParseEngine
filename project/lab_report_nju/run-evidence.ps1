$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$tmp = Join-Path $root "tmp/nju_lab"
$engine = Join-Path $root "build/bin/ParseEngine.exe"
$source = Join-Path $PSScriptRoot "source"
$frontend = Join-Path $source "frontend"
$backend = Join-Path $source "backend"
$generated = Join-Path $tmp "cminus-generated"
$checks = Join-Path $tmp "cminus-checks"
$compiler = Join-Path $generated "cminus.exe"
$transferBuild = Join-Path $tmp "bundled-transfer-build"

New-Item -ItemType Directory -Force -Path $tmp, $generated, $checks | Out-Null

function Assert-NativeSuccess([string]$step) {
    if ($LASTEXITCODE -ne 0) { throw "$step failed with exit code $LASTEXITCODE" }
}

Write-Host "[1/6] Generate the strict C-- LALR frontend with ParseEngine"
& $engine (Join-Path $frontend "cminus.gram") --out-dir $generated --lang cpp17
Assert-NativeSuccess "ParseEngine generation"

Write-Host "[2/6] Compile generated parser and run C-- boundary checks"
& g++ -std=c++17 -O2 `
    (Join-Path $generated "cminus_parser.cpp") `
    (Join-Path $generated "cminus_main.cpp") `
    -o $compiler
Assert-NativeSuccess "C-- frontend compilation"
& (Join-Path $frontend "check.ps1") -Compiler $compiler -OutputDirectory $checks

Write-Host "[3/6] Show generated C-- quadruples"
Get-Content -LiteralPath (Join-Path $checks "valid_loop.quad")

Write-Host "[4/6] Build and test the bundled transfer backend"
cmake -S $backend -B $transferBuild -G "MinGW Makefiles"
Assert-NativeSuccess "Backend configuration"
cmake --build $transferBuild -j 4
Assert-NativeSuccess "Backend build"
ctest --test-dir $transferBuild --output-on-failure
Assert-NativeSuccess "Backend tests"

$t1t = Join-Path $transferBuild "t1t.exe"
Write-Host "[5/6] Run the generated loop IR through the complete pipeline"
& $t1t pipeline (Join-Path $checks "valid_loop.quad")
Assert-NativeSuccess "C-- backend pipeline"

Write-Host "[6/6] Generate and simulate optimized MIPS from C-- IR"
& $t1t transfer (Join-Path $checks "valid_optimize.quad") --asm --no-opt
Assert-NativeSuccess "Unoptimized C-- MIPS generation"
& $t1t transfer (Join-Path $checks "valid_optimize.quad") --asm
Assert-NativeSuccess "Optimized C-- MIPS generation"
