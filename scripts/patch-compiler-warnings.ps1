# Patch generated ninja files to add warning suppression flags.
#
# We patch ninja files AFTER gn gen, because patching GN BUILD.gn source
# files is fragile (wrong config scope). Ninja files have the actual
# compiler commands, so we can reliably inject flags.
#
# Clang 20 warnings that break M98 WebRTC code:
# - -Wc++11-narrowing-const-reference (narrowing double->float, uint32_t->int)
# - -Wdeprecated-builtins (__has_trivial_* builtins)
param([string]$BinaryDir)

$suppressFlags = '-Wno-c++11-narrowing-const-reference -Wno-deprecated-builtins -Wno-unknown-warning-option'

Write-Host "Searching for ninja files in: $BinaryDir"

# Find all .ninja files in the build directory
$ninjaFiles = Get-ChildItem $BinaryDir -Filter '*.ninja' -Recurse -ErrorAction SilentlyContinue
Write-Host "Found $($ninjaFiles.Count) ninja files"

$patchedCount = 0

foreach ($nf in $ninjaFiles) {
    $c = [IO.File]::ReadAllText($nf.FullName)
    # Match clang-cl with any path prefix (relative or absolute)
    if ($c -match 'clang-cl') {
        $original = $c
        # Insert suppression flags before /Zc:twoPhase which appears in every CXX command
        $c = $c.Replace('/Zc:twoPhase', "$suppressFlags /Zc:twoPhase")
        if ($c -ne $original) {
            [IO.File]::WriteAllText($nf.FullName, $c)
            $patchedCount++
        }
    }
}

Write-Host "Patched $patchedCount ninja files with warning suppressions"

if ($patchedCount -eq 0) {
    Write-Host "WARNING: No ninja files were patched. Listing ninja files for debugging:"
    foreach ($nf in $ninjaFiles) {
        $c = [IO.File]::ReadAllText($nf.FullName)
        $hasClang = $c -match 'clang'
        $hasZc = $c -match '/Zc:twoPhase'
        Write-Host "  $($nf.Name): clang=$hasClang, /Zc:twoPhase=$hasZc, size=$($nf.Length)"
    }
}
