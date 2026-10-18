# ci-local.ps1 (WO-020): Windows mirror of scripts/ci-local.sh. UNVERIFIED on this machine: there is no
# Windows host here (ADR-0006: the windows-latest configure job after the first push is the evidence).
# Usage: pwsh scripts/ci-local.ps1 [-Quick]
#   doctor is skipped on Windows (scripts/doctor is bash); then fmt-check (git ls-files + clang-format),
#   then cmake --workflow --preset win-msvc-debug and win-clangcl-debug (-Quick: win-msvc-debug only).
#   Each step is timed; the first failure prints the summary, names the step and exits 1.
param([switch]$Quick)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $PSScriptRoot)

$results = New-Object System.Collections.Generic.List[object]
$total = [System.Diagnostics.Stopwatch]::StartNew()

function Write-Summary {
    Write-Host ''
    Write-Host 'ci-local summary'
    $results | ForEach-Object { Write-Host ('  {0,-28} {1,-8} {2}' -f $_.Step, $_.Result, $_.Seconds) }
    Write-Host ('  total wall: {0:N0} s' -f $total.Elapsed.TotalSeconds)
}

function Invoke-Step([string]$Name, [scriptblock]$Body) {
    Write-Host "ci-local: [$Name] ..."
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $Body
    $code = $LASTEXITCODE
    $secs = [int]$sw.Elapsed.TotalSeconds
    if ($code -ne 0) {
        $results.Add([pscustomobject]@{ Step = $Name; Result = 'FAIL'; Seconds = $secs })
        Write-Summary
        Write-Host "ci-local: FAILED at step '$Name' (exit $code)"
        exit 1
    }
    $results.Add([pscustomobject]@{ Step = $Name; Result = 'ok'; Seconds = $secs })
    Write-Host "ci-local: [$Name] ok ($secs s)"
}

$results.Add([pscustomobject]@{ Step = 'doctor'; Result = 'skipped'; Seconds = '-' })
Write-Host 'ci-local: [doctor] skipped (scripts/doctor is bash; run it from WSL or Git Bash)'

Invoke-Step 'fmt-check' {
    $files = @(git ls-files --cached --others --exclude-standard -- '*.cpp' '*.hpp' '*.h' '*.cc' '*.cxx' '*.inl')
    $global:LASTEXITCODE = 0
    if ($files.Count -gt 0) { clang-format --dry-run -Werror @files }
}

$presets = @('win-msvc-debug')
if (-not $Quick) { $presets += 'win-clangcl-debug' }
foreach ($p in $presets) {
    Invoke-Step "workflow $p" { cmake --workflow --preset $p }
}

Write-Summary
Write-Host 'ci-local: ok'
