# Patch generated ninja files to add warning suppression flags.
#
# We patch ninja files AFTER gn gen, because patching GN BUILD.gn source
# files is fragile (wrong config scope, caching issues). Ninja files have
# the actual compiler commands, so we can reliably inject flags.
#
# Clang 20 warnings that break M98 WebRTC code:
# - -Wc++11-narrowing-const-reference (narrowing double->float, uint32_t->int)
# - -Wdeprecated-builtins (__has_trivial_* builtins)
param([string]$BinaryDir)

$suppressFlags = ' -Wno-c++11-narrowing-const-reference -Wno-deprecated-builtins -Wno-unknown-warning-option'

# Find all .ninja files in the build directory
$ninjaFiles = Get-ChildItem $BinaryDir -Filter '*.ninja' -Recurse -ErrorAction SilentlyContinue
$patchedCount = 0

foreach ($nf in $ninjaFiles) {
    $c = [IO.File]::ReadAllText($nf.FullName)
    if ($c -match 'clang-cl\.exe') {
        # Append suppression flags to every clang-cl command line.
        # clang-cl commands in ninja end with the source file path.
        # We inject flags before the /TP or /TC (C++ or C mode) flag.
        $original = $c
        $c = $c.Replace('/Zc:twoPhase', "$suppressFlags /Zc:twoPhase")
        if ($c -ne $original) {
            [IO.File]::WriteAllText($nf.FullName, $c)
            $patchedCount++
        }
    }
}

Write-Host "Patched $patchedCount ninja files with warning suppressions"
