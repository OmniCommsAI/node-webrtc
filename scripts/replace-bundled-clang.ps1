# Replace bundled M98-era clang (v14) with system clang (v19+).
# MSVC 14.44+ STL headers require Clang 19+. The bundled clang at
# third_party/llvm-build/Release+Asserts/ is too old.
# We can't use clang_base_path in nix.gni because GN's rebase_path()
# can't handle Windows drive letter paths. Instead, copy system clang
# binaries over the bundled ones so GN's default path resolution works.
param([string]$SourceDir)

$systemLlvm = "C:\Program Files\LLVM"
$bundledDir = Join-Path $SourceDir "third_party\llvm-build\Release+Asserts\bin"

if (-not (Test-Path $bundledDir)) {
    Write-Host "WARNING: Bundled clang dir not found at $bundledDir"
    exit 0
}

if (-not (Test-Path "$systemLlvm\bin\clang-cl.exe")) {
    Write-Host "ERROR: System clang-cl.exe not found at $systemLlvm\bin"
    exit 1
}

# Show versions
$bundledVer = & "$bundledDir\clang-cl.exe" --version 2>&1 | Select-String 'clang version'
$systemVer = & "$systemLlvm\bin\clang-cl.exe" --version 2>&1 | Select-String 'clang version'
Write-Host "Bundled clang: $bundledVer"
Write-Host "System clang:  $systemVer"

# Copy key binaries from system LLVM over bundled ones
$binaries = @("clang-cl.exe", "clang.exe", "clang++.exe", "clang-cpp.exe", "lld-link.exe", "llvm-lib.exe")
foreach ($bin in $binaries) {
    $src = Join-Path "$systemLlvm\bin" $bin
    $dst = Join-Path $bundledDir $bin
    if (Test-Path $src) {
        Copy-Item $src $dst -Force
        Write-Host "Replaced: $bin"
    }
}

# Also copy any clang DLLs that might be needed
Get-ChildItem "$systemLlvm\bin\*.dll" -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item $_.FullName (Join-Path $bundledDir $_.Name) -Force
}

# Copy the clang resource directory (compiler builtins, sanitizer runtimes)
# GN expects it at third_party/llvm-build/Release+Asserts/lib/clang/<version>/
$systemClangVer = & "$systemLlvm\bin\clang-cl.exe" -dumpversion 2>&1
$systemClangVer = $systemClangVer.Trim()
$systemResDir = Join-Path $systemLlvm "lib\clang\$systemClangVer"
$bundledLibDir = Join-Path $SourceDir "third_party\llvm-build\Release+Asserts\lib\clang"

if (Test-Path $systemResDir) {
    # Find the old bundled version directory name (e.g., "14.0.0")
    # GN's generated ninja files reference this path for -libpath
    $oldVersionDirs = Get-ChildItem $bundledLibDir -Directory -ErrorAction SilentlyContinue
    $oldVersion = if ($oldVersionDirs) { $oldVersionDirs[0].Name } else { $null }

    # Remove old bundled clang resource dirs
    foreach ($d in $oldVersionDirs) {
        Remove-Item $d.FullName -Recurse -Force
        Write-Host "Removed old resource dir: $($d.Name)"
    }

    # Copy system clang resource dir under the NEW version name
    Copy-Item $systemResDir $bundledLibDir -Recurse -Force
    Write-Host "Copied clang resource dir: $systemClangVer"

    # Also create a copy under the OLD version name so GN's generated
    # -libpath:lib/clang/14.0.0/lib/windows still resolves
    if ($oldVersion -and $oldVersion -ne $systemClangVer) {
        $oldPath = Join-Path $bundledLibDir $oldVersion
        $newPath = Join-Path $bundledLibDir $systemClangVer
        # Use directory junction (symlink) to avoid duplicating files
        cmd /c "mklink /J `"$oldPath`" `"$newPath`"" 2>&1 | Out-Null
        if (Test-Path $oldPath) {
            Write-Host "Created junction: $oldVersion -> $systemClangVer"
        } else {
            # Fallback: copy if junction fails
            Copy-Item $newPath $oldPath -Recurse -Force
            Write-Host "Copied resource dir as: $oldVersion (junction failed)"
        }
    }
}

# Verify
$newVer = & "$bundledDir\clang-cl.exe" --version 2>&1 | Select-String 'clang version'
Write-Host "Bundled clang after replacement: $newVer"
