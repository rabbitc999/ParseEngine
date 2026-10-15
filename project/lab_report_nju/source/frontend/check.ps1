param(
    [Parameter(Mandatory = $true)][string]$Compiler,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = "Stop"
$samples = Join-Path $PSScriptRoot "samples"
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

function Invoke-ExpectedSuccess([string]$name) {
    & $Compiler (Join-Path $samples $name) --out (Join-Path $OutputDirectory ([IO.Path]::GetFileNameWithoutExtension($name)))
    if ($LASTEXITCODE -ne 0) { throw "$name should be accepted" }
}

function Invoke-ExpectedFailure([string]$name, [string]$pattern) {
    $text = (& $Compiler (Join-Path $samples $name) --out (Join-Path $OutputDirectory "rejected") 2>&1 | Out-String)
    if ($LASTEXITCODE -eq 0) { throw "$name should be rejected" }
    if ($text -notmatch $pattern) { throw "$name did not report $pattern`n$text" }
}

Invoke-ExpectedSuccess "valid_loop.cmm"
Invoke-ExpectedSuccess "valid_complete.cmm"
Invoke-ExpectedSuccess "valid_optimize.cmm"
Invoke-ExpectedSuccess "valid_array_parameter.cmm"
Invoke-ExpectedFailure "semantic_errors.cmm" "Error type (1|5)"
Invoke-ExpectedFailure "not_a_function.cmm" "Error type 11"
Invoke-ExpectedFailure "reject_for.cmm" "Error type B"
Invoke-ExpectedFailure "reject_pointer.cmm" "Error type B"

if (-not (Test-Path (Join-Path $OutputDirectory "valid_loop.ast"))) { throw "AST output missing" }
if (-not (Test-Path (Join-Path $OutputDirectory "valid_loop.quad"))) { throw "quadruple output missing" }
Write-Host "C-- frontend checks passed: 4 accepted, 4 rejected as expected."
