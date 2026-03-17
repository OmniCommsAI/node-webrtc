# Patch build/toolchain/win/BUILD.gn to fix "sys_lib_flags" unused variable error.
# The M98-era code sets sys_lib_flags in the win_toolchains template but only uses
# it in certain scopes (x64 but not x86). GN treats unused assignments as errors.
# Fix: prefix with underscore — GN allows _prefixed variables to go unused.
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
if ($c -match '_sys_lib_flags') {
    Write-Host "BUILD.gn: sys_lib_flags already renamed, skipping"
    exit 0
}

# Rename sys_lib_flags to _sys_lib_flags everywhere in the file
$c = $c -replace '\bsys_lib_flags\b', '_sys_lib_flags'
[IO.File]::WriteAllText($f, $c)
Write-Host "Patched BUILD.gn: renamed sys_lib_flags to _sys_lib_flags"
