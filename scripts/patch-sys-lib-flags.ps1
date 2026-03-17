# Patch build/toolchain/win/BUILD.gn to fix "sys_lib_flags" unused variable error.
# The M98-era code sets sys_lib_flags in the win_toolchains template but only uses
# it in certain scopes. GN treats unused assignments as hard errors with no way to
# suppress. Fix: remove the assignment lines entirely — GN confirms they have no effect.
param([string]$SourceDir)

$f = Join-Path $SourceDir 'build\toolchain\win\BUILD.gn'
if (-not (Test-Path $f)) {
    Write-Host "WARNING: BUILD.gn not found at $f"
    exit 0
}

$lines = [IO.File]::ReadAllLines($f)
$output = [System.Collections.Generic.List[string]]::new()
$removed = 0
foreach ($line in $lines) {
    if ($line -match '^\s*sys_lib_flags\s*=') {
        Write-Host "Removing: $($line.Trim())"
        $removed++
    } else {
        $output.Add($line)
    }
}

if ($removed -gt 0) {
    [IO.File]::WriteAllLines($f, $output)
    Write-Host "Patched BUILD.gn: removed $removed unused sys_lib_flags assignment(s)"
} else {
    Write-Host "BUILD.gn: no sys_lib_flags assignments found, skipping"
}
