# Patch build/toolchain/win/BUILD.gn to fix "sys_lib_flags" unused variable error.
# The M98-era code sets sys_lib_flags in the win_toolchains template but only uses
# it in certain scopes. GN treats unused assignments as hard errors with no way to
# suppress. Fix: remove the assignment lines entirely — GN confirms they have no effect.
#
# Handles both single-line and multi-line assignments:
#   sys_lib_flags = "value"           <- single line
#   sys_lib_flags =                   <- multi-line: also remove next line (the value)
#       "-libpath:..."
param([string]$SourceDir)

$f = Join-Path $SourceDir 'build\toolchain\win\BUILD.gn'
if (-not (Test-Path $f)) {
    Write-Host "WARNING: BUILD.gn not found at $f"
    exit 0
}

$lines = [IO.File]::ReadAllLines($f)
$output = [System.Collections.Generic.List[string]]::new()
$removed = 0
$skipNext = $false

for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($skipNext) {
        Write-Host "Removing continuation: $($lines[$i].Trim())"
        $skipNext = $false
        $removed++
        continue
    }

    if ($lines[$i] -match '^\s*sys_lib_flags\s*=') {
        Write-Host "Removing: $($lines[$i].Trim())"
        $removed++

        # If the line ends with just = (no value), the value is on the next line
        if ($lines[$i].Trim() -match '=\s*$') {
            $skipNext = $true
        }
    } else {
        $output.Add($lines[$i])
    }
}

if ($removed -gt 0) {
    [IO.File]::WriteAllLines($f, $output)
    Write-Host "Patched BUILD.gn: removed $removed line(s) for unused sys_lib_flags"
} else {
    Write-Host "BUILD.gn: no sys_lib_flags assignments found, skipping"
}
