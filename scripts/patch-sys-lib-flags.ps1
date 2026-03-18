# Diagnostic: dump BUILD.gn structure around sys_lib_flags so we can understand
# the scope issue and craft the correct fix.
param([string]$SourceDir)

$f = Join-Path $SourceDir 'build\toolchain\win\BUILD.gn'
if (-not (Test-Path $f)) {
    Write-Host "WARNING: BUILD.gn not found at $f"
    exit 0
}

$lines = [IO.File]::ReadAllLines($f)
Write-Host "=== BUILD.gn has $($lines.Count) lines ==="

# Find all lines referencing sys_lib_flags and print context
for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match 'sys_lib_flags') {
        $start = [Math]::Max(0, $i - 5)
        $end = [Math]::Min($lines.Count - 1, $i + 5)
        Write-Host ""
        Write-Host "--- sys_lib_flags at line $($i + 1) ---"
        for ($j = $start; $j -le $end; $j++) {
            $marker = if ($j -eq $i) { ">>>" } else { "   " }
            Write-Host "$marker $($j + 1): $($lines[$j])"
        }
    }
}

# Also dump lines 520-545 (the area GN complains about)
Write-Host ""
Write-Host "=== Lines 510-550 (error region) ==="
$start = [Math]::Min(509, $lines.Count - 1)
$end = [Math]::Min(549, $lines.Count - 1)
for ($j = $start; $j -le $end; $j++) {
    Write-Host "   $($j + 1): $($lines[$j])"
}

Write-Host ""
Write-Host "=== Lines 340-365 (usage region) ==="
$start = [Math]::Min(339, $lines.Count - 1)
$end = [Math]::Min(364, $lines.Count - 1)
for ($j = $start; $j -le $end; $j++) {
    Write-Host "   $($j + 1): $($lines[$j])"
}

# Don't modify the file — just dump info. Exit 0 so configure continues
# and shows the original GN error for comparison.
Write-Host ""
Write-Host "=== No patch applied (diagnostic mode) ==="
exit 0
