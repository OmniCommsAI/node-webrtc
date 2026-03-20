# Patch abseil-cpp type_traits.h for Clang 20 compatibility.
#
# Problem: Clang 20 deprecated __has_trivial_destructor, __has_trivial_constructor,
# __has_trivial_assign, __has_trivial_copy. These builtins return INCORRECT values
# in Clang 20 for certain types (e.g., __has_trivial_assign(std::pair<int,int>)
# returns true when it shouldn't). This causes abseil's type traits to produce
# wrong results, leading to deleted copy assignment operators on absl::optional.
#
# Fix: Replace deprecated builtins with standard library type traits which
# return correct values in all Clang versions. Also comment out compliance
# static_asserts (they compared builtins to std traits; now redundant).
param([string]$SourceDir)

$f = Join-Path $SourceDir 'third_party\abseil-cpp\absl\meta\type_traits.h'
if (-not (Test-Path $f)) {
    Write-Host "WARNING: abseil type_traits.h not found at $f"
    exit 0
}

$c = [IO.File]::ReadAllText($f)
$changes = 0

# Replace deprecated builtins with std:: trait equivalents.
# Each builtin takes one type arg and returns bool.
# The std:: traits return bool via ::value.
$replacements = @(
    @('__has_trivial_destructor\(([^)]+)\)',       'std::is_trivially_destructible<$1>::value'),
    @('__has_trivial_constructor\(([^)]+)\)',       'std::is_trivially_default_constructible<$1>::value'),
    @('__has_trivial_copy\(([^)]+)\)',              'std::is_trivially_copy_constructible<$1>::value'),
    @('__has_trivial_assign\(([^)]+)\)',            'std::is_trivially_copy_assignable<$1>::value')
)

foreach ($r in $replacements) {
    $pattern = $r[0]
    $replacement = $r[1]
    $matches = [regex]::Matches($c, $pattern)
    if ($matches.Count -gt 0) {
        $c = [regex]::Replace($c, $pattern, $replacement)
        Write-Host "Replaced $($matches.Count) occurrences: $pattern"
        $changes += $matches.Count
    }
}

# Comment out compliance static_asserts (they compared builtins to std traits,
# now redundant since we use std traits directly)
$lines = $c -split "`n"
$inStaticAssert = $false
$assertChanges = 0

for ($i = 0; $i -lt $lines.Count; $i++) {
    $line = $lines[$i]
    if ($line -match 'static_assert\(compliant') {
        $inStaticAssert = $true
    }
    if ($inStaticAssert -and $line -notmatch '^//') {
        $lines[$i] = '// ' + $line
        $assertChanges++
        if ($line -match '\)\s*;') {
            $inStaticAssert = $false
        }
    }
}

if ($assertChanges -gt 0) {
    $c = $lines -join "`n"
    Write-Host "Commented out $assertChanges lines of compliance static_asserts"
    $changes += $assertChanges
}

if ($changes -gt 0) {
    [IO.File]::WriteAllText($f, $c)
    Write-Host "Patched abseil type_traits.h: $changes total changes"
} else {
    Write-Host "abseil type_traits.h: no changes needed (already patched?)"
}
