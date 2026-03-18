# Patch build/toolchain/win/BUILD.gn to fix "sys_lib_flags" unused invoker error.
#
# Problem: The msvc_toolchain template has a condition at line 145:
#   if (host_os != "win" || (use_lld && defined(invoker.sys_lib_flags)))
# When use_lld=false (our config), the condition short-circuits and never reads
# invoker.sys_lib_flags. GN treats unread invoker variables as hard errors.
#
# Fix: Remove the use_lld guard. The defined() check alone is correct —
# if the invoker provides sys_lib_flags, it should always be used.
# This also fixes a latent bug: clang lib paths were being silently dropped
# from the linker when use_lld=false.
param([string]$SourceDir)

$f = Join-Path $SourceDir 'build\toolchain\win\BUILD.gn'
if (-not (Test-Path $f)) {
    Write-Host "WARNING: BUILD.gn not found at $f"
    exit 0
}

$c = [IO.File]::ReadAllText($f)

# Original:  if (host_os != "win" || (use_lld && defined(invoker.sys_lib_flags))) {
# Patched:   if (host_os != "win" || defined(invoker.sys_lib_flags)) {
$old = 'host_os != "win" || (use_lld && defined(invoker.sys_lib_flags))'
$new = 'host_os != "win" || defined(invoker.sys_lib_flags)'

if ($c -match [regex]::Escape($old)) {
    $c = $c.Replace($old, $new)
    [IO.File]::WriteAllText($f, $c)
    Write-Host "Patched BUILD.gn: removed use_lld guard from sys_lib_flags condition"
} elseif ($c -match [regex]::Escape($new)) {
    Write-Host "BUILD.gn: sys_lib_flags condition already patched"
} else {
    Write-Host "WARNING: expected sys_lib_flags condition not found in BUILD.gn"
    # Dump lines with sys_lib_flags for debugging
    $lines = [IO.File]::ReadAllLines($f)
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match 'sys_lib_flags') {
            Write-Host "  $($i + 1): $($lines[$i].Trim())"
        }
    }
    exit 1
}
