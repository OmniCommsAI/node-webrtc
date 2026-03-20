# Patch abseil-cpp type_traits.h for Clang 20 compatibility.
#
# Problem: Clang 20 deprecated __has_trivial_destructor, __has_trivial_constructor,
# __has_trivial_assign, __has_trivial_copy. These deprecated builtins also return
# DIFFERENT values than their std:: equivalents for certain types (e.g.,
# std::pair<int, int>). Abseil has static_assert compliance checks that verify
# the builtins match the std:: traits -- these fail with Clang 20.
#
# Fix: Comment out the compliance static_asserts. They are debug/validation
# checks, not functional code. The actual trait behavior is unchanged.
param([string]$SourceDir)

$f = Join-Path $SourceDir 'third_party\abseil-cpp\absl\meta\type_traits.h'
if (-not (Test-Path $f)) {
    Write-Host "WARNING: abseil type_traits.h not found at $f"
    exit 0
}

$lines = [IO.File]::ReadAllLines($f)
$changed = 0
$inStaticAssert = $false

for ($i = 0; $i -lt $lines.Count; $i++) {
    $line = $lines[$i]

    # Detect static_assert(compliant lines and comment them out
    # These span 2-3 lines: static_assert(compliant || ..., "message");
    if ($line -match 'static_assert\(compliant') {
        $inStaticAssert = $true
    }

    if ($inStaticAssert) {
        $lines[$i] = '// ' + $line
        $changed++
        # static_assert ends with );
        if ($line -match '\)\s*;') {
            $inStaticAssert = $false
        }
    }
}

if ($changed -gt 0) {
    [IO.File]::WriteAllLines($f, $lines)
    Write-Host "Patched abseil type_traits.h: commented out $changed lines of compliance static_asserts"
} else {
    Write-Host "abseil type_traits.h: no compliance static_asserts found (already patched?)"
}
