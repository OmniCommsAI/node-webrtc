# Replace bundled M98-era clang (v14) with system clang (v19+).
# MSVC 14.44+ STL headers require Clang 19+. The bundled clang at
# third_party/llvm-build/Release+Asserts/ is too old.
# We can't use clang_base_path in nix.gni because GN's rebase_path()
# can't handle Windows drive letter paths. Instead, copy system clang
# binaries over the bundled ones so GN's default path resolution works.
param([string]$SourceDir)

$systemLlvm = "C:\Program Files\LLVM"
$bundledBase = Join-Path $SourceDir "third_party\llvm-build\Release+Asserts"
$bundledBin = Join-Path $bundledBase "bin"

if (-not (Test-Path $bundledBin)) {
    Write-Host "WARNING: Bundled clang dir not found at $bundledBin"
    exit 0
}

if (-not (Test-Path "$systemLlvm\bin\clang-cl.exe")) {
    Write-Host "ERROR: System clang-cl.exe not found at $systemLlvm\bin"
    exit 1
}

# Show versions
$bundledVer = & "$bundledBin\clang-cl.exe" --version 2>&1 | Select-String 'clang version'
$systemVer = & "$systemLlvm\bin\clang-cl.exe" --version 2>&1 | Select-String 'clang version'
Write-Host "Bundled clang: $bundledVer"
Write-Host "System clang:  $systemVer"

# Copy ALL exe and dll files from system LLVM bin to bundled bin
Write-Host "Copying system LLVM binaries..."
Get-ChildItem "$systemLlvm\bin\*.exe" -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $bundledBin $_.Name) -Force
}
Get-ChildItem "$systemLlvm\bin\*.dll" -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $bundledBin $_.Name) -Force
}
Write-Host "Copied binaries from $systemLlvm\bin"

# Copy the clang resource directory (compiler builtins, sanitizer runtimes)
# GN generates -libpath:lib/clang/<version>/lib/windows in ninja files
# LLVM 16+ uses major-version-only dirs: lib/clang/20/ (not lib/clang/20.1.8/)
$bundledLibClang = Join-Path $bundledBase "lib\clang"

# Find the OLD bundled version dir name (e.g., "14.0.0")
$oldVersionDir = Get-ChildItem $bundledLibClang -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
$oldVersion = if ($oldVersionDir) { $oldVersionDir.Name } else { $null }
Write-Host "Old bundled resource dir: $oldVersion"

# Find the SYSTEM resource dir (could be major-only like "20" or full like "20.1.8")
$systemLibClang = Join-Path $systemLlvm "lib\clang"
$systemResDir = $null
if (Test-Path $systemLibClang) {
    $systemResDir = Get-ChildItem $systemLibClang -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
}

if ($systemResDir) {
    Write-Host "System resource dir: $($systemResDir.Name) at $($systemResDir.FullName)"

    # Remove old bundled dirs
    Get-ChildItem $bundledLibClang -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        Remove-Item $_.FullName -Recurse -Force
        Write-Host "Removed: $($_.Name)"
    }

    # Copy system resource dir
    Copy-Item $systemResDir.FullName (Join-Path $bundledLibClang $systemResDir.Name) -Recurse -Force
    Write-Host "Copied resource dir: $($systemResDir.Name)"

    # Create junction from old version name to new so GN's generated
    # -libpath:lib/clang/14.0.0/lib/windows still resolves
    if ($oldVersion -and $oldVersion -ne $systemResDir.Name) {
        $junctionTarget = Join-Path $bundledLibClang $systemResDir.Name
        $junctionPath = Join-Path $bundledLibClang $oldVersion
        cmd /c "mklink /J `"$junctionPath`" `"$junctionTarget`"" 2>&1 | Out-Null
        if (Test-Path $junctionPath) {
            Write-Host "Created junction: $oldVersion -> $($systemResDir.Name)"
        } else {
            Copy-Item $junctionTarget $junctionPath -Recurse -Force
            Write-Host "Copied as fallback: $oldVersion"
        }
    }
} else {
    Write-Host "WARNING: System clang resource dir not found at $systemLibClang"
    # List what's actually there
    if (Test-Path $systemLibClang) {
        Get-ChildItem $systemLibClang | ForEach-Object { Write-Host "  Found: $($_.Name)" }
    }
}

# Verify
$newVer = & "$bundledBin\clang-cl.exe" --version 2>&1 | Select-String 'clang version'
Write-Host "Bundled clang after replacement: $newVer"

# Verify resource dir exists for old version path
if ($oldVersion) {
    $checkPath = Join-Path $bundledLibClang "$oldVersion\lib\windows"
    if (Test-Path $checkPath) {
        Write-Host "Verified: $oldVersion\lib\windows exists"
    } else {
        Write-Host "WARNING: $oldVersion\lib\windows NOT found — linker may fail"
        # List what we have
        Get-ChildItem $bundledLibClang -Recurse -Depth 3 | ForEach-Object {
            Write-Host "  $($_.FullName.Replace($bundledLibClang, ''))"
        }
    }
}
