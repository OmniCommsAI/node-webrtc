# Patch build/config/compiler/BUILD.gn to suppress Clang 20 warnings
# that are treated as errors in M98-era WebRTC code.
#
# Clang 18+ added -Wc++11-narrowing-const-reference which flags narrowing
# conversions (double->float, uint32_t->int) in initializer lists as errors.
# M98 code has many of these in stats_collector.cc and elsewhere.
param([string]$SourceDir)

$f = Join-Path $SourceDir 'build\config\compiler\BUILD.gn'
if (-not (Test-Path $f)) {
    Write-Host "WARNING: compiler BUILD.gn not found at $f"
    exit 0
}

$c = [IO.File]::ReadAllText($f)

# Find the section that sets clang-specific warning suppression flags.
# Look for existing -Wno- flags near is_clang checks and append ours.
# The pattern we're looking for is in the default_warnings config.
$flagsToAdd = @(
    '-Wno-c++11-narrowing-const-reference',
    '-Wno-deprecated-builtins',
    '-Wno-unknown-warning-option'
)

$flagString = ($flagsToAdd | ForEach-Object { "      `"$_`"," }) -join "`n"

# Strategy: Find a line with "-Wno-nonportable-include-path" (exists in M98
# default_warnings) and append our flags after it.
$marker = '"-Wno-nonportable-include-path"'
if ($c -match [regex]::Escape($marker)) {
    $c = $c.Replace(
        $marker + ',',
        $marker + ",`n" + $flagString
    )
    [IO.File]::WriteAllText($f, $c)
    Write-Host "Patched compiler BUILD.gn: added warning suppressions after $marker"
    foreach ($flag in $flagsToAdd) {
        Write-Host "  Added: $flag"
    }
} else {
    Write-Host "WARNING: Could not find marker '$marker' in $f"
    Write-Host "Attempting alternative: patching cflags directly"

    # Alternative: find any cflags block within is_clang scope and append
    $altMarker = '"-Wno-null-pointer-subtraction"'
    if ($c -match [regex]::Escape($altMarker)) {
        $c = $c.Replace(
            $altMarker + ',',
            $altMarker + ",`n" + $flagString
        )
        [IO.File]::WriteAllText($f, $c)
        Write-Host "Patched compiler BUILD.gn via alternative marker"
    } else {
        Write-Host "ERROR: Could not find any suitable marker in compiler BUILD.gn"
        # Dump some context for debugging
        $lines = [IO.File]::ReadAllLines($f)
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match 'Wno-') {
                Write-Host "  $($i + 1): $($lines[$i].Trim())"
            }
        }
        exit 1
    }
}
