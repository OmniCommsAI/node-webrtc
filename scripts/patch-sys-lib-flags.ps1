# Patch build/toolchain/win/BUILD.gn to fix "sys_lib_flags" unused variable error.
# The M98-era code sets sys_lib_flags in the win_toolchains template but only uses
# it in certain scopes. GN treats unused assignments as hard errors.
# Fix: insert not_needed(["sys_lib_flags"]) after the assignment.
param([string]$SourceDir)

$f = Join-Path $SourceDir 'build\toolchain\win\BUILD.gn'
if (-not (Test-Path $f)) {
    Write-Host "WARNING: BUILD.gn not found at $f"
    exit 0
}

$c = [IO.File]::ReadAllText($f)
if ($c -notmatch 'sys_lib_flags') {
    Write-Host "BUILD.gn: no sys_lib_flags found, skipping"
    exit 0
}
if ($c -match 'not_needed.*sys_lib_flags') {
    Write-Host "BUILD.gn: sys_lib_flags patch already applied"
    exit 0
}

# Insert not_needed after each sys_lib_flags assignment line
$lines = [IO.File]::ReadAllLines($f)
$output = [System.Collections.Generic.List[string]]::new()
foreach ($line in $lines) {
    $output.Add($line)
    if ($line -match '^\s*sys_lib_flags\s*=') {
        $indent = $line -replace '^(\s*).*', '$1'
        $output.Add("${indent}not_needed([`"sys_lib_flags`"])")
    }
}
[IO.File]::WriteAllLines($f, $output)
Write-Host "Patched BUILD.gn: added not_needed for sys_lib_flags"
