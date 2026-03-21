# Create a clang-cl wrapper that injects warning suppression flags.
#
# Instead of patching ninja files (which use complex variable expansion),
# we rename the real clang-cl.exe and create a wrapper batch file that
# calls the real one with extra flags prepended.
#
# Clang 20 warnings that break M98 WebRTC code:
# - -Wc++11-narrowing-const-reference (narrowing double->float, uint32_t->int)
# - -Wdeprecated-builtins (__has_trivial_* builtins)
param([string]$SourceDir)

$clangDir = Join-Path $SourceDir 'third_party\llvm-build\Release+Asserts\bin'
$clangCl = Join-Path $clangDir 'clang-cl.exe'
$clangClReal = Join-Path $clangDir 'clang-cl-real.exe'

if (-not (Test-Path $clangCl)) {
    Write-Host "WARNING: clang-cl.exe not found at $clangCl"
    exit 0
}

if (Test-Path $clangClReal) {
    Write-Host "clang-cl-real.exe already exists, wrapper already set up"
    exit 0
}

# Rename real clang-cl.exe
Rename-Item $clangCl 'clang-cl-real.exe'
Write-Host "Renamed clang-cl.exe to clang-cl-real.exe"

# Create wrapper batch file that injects suppression flags
# Using .cmd extension so Windows finds it via PATH/direct invocation
$wrapperContent = @"
@echo off
"%~dp0clang-cl-real.exe" -Wno-c++11-narrowing-const-reference -Wno-deprecated-builtins -Wno-unknown-warning-option %*
"@

# Write as .exe won't work - ninja calls clang-cl.exe directly
# Instead, create a small exe-like wrapper via cmd
# Actually, ninja on Windows calls the full path, so we need the wrapper
# to have the same name. Use a batch file renamed to .exe? No.
# Better: create a .cmd file and also create a copy of clang.exe as clang-cl.exe
# that acts as a wrapper.

# Simplest approach: write a .cmd wrapper AND copy clang-cl-real.exe back
# as clang-cl.exe but with a response file that adds flags
#
# Actually the simplest: create an rsp (response file) that clang-cl will
# automatically read. clang-cl reads clang-cl.cfg if it exists in the same dir.

# clang config file: clang-cl.exe reads <exename>.cfg from its directory
$cfgContent = "-Wno-c++11-narrowing-const-reference`n-Wno-deprecated-builtins`n-Wno-unknown-warning-option"
$cfgPath = Join-Path $clangDir 'clang-cl.cfg'

# Restore original name
Rename-Item $clangClReal 'clang-cl.exe'
Write-Host "Restored clang-cl.exe"

# Write config file
[IO.File]::WriteAllText($cfgPath, $cfgContent)
Write-Host "Created clang-cl.cfg with warning suppressions"
Write-Host "Contents:"
Get-Content $cfgPath | ForEach-Object { Write-Host "  $_" }
