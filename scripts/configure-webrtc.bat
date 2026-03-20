@ECHO OFF
SET EL=0

ECHO Add depot_tools to PATH
set PATH=%DEPOT_TOOLS%;%PATH%
IF %ERRORLEVEL% NEQ 0 GOTO ERROR

ECHO SET DEPOT_TOOLS_WIN_TOOLCHAIN=0
SET DEPOT_TOOLS_WIN_TOOLCHAIN=0
IF %ERRORLEVEL% NEQ 0 GOTO ERROR

ECHO cd SOURCE_DIR
cd %SOURCE_DIR%
IF %ERRORLEVEL% NEQ 0 GOTO ERROR

REM Write GN args to args.gn file instead of passing via --args="..."
REM on the command line. CMake passes GN_GEN_ARGS with embedded double
REM quotes (e.g. target_cpu="x64") which cmd.exe strips when expanding
REM %GN_GEN_ARGS% inside another quoted string. PowerShell reads the raw
REM env var value and writes it to the file correctly.
REM PowerShell is always available on Windows runners (unlike python3).
ECHO Writing args.gn via PowerShell to preserve inner quotes
powershell -NoProfile -Command "[System.IO.File]::WriteAllText('%BINARY_DIR%\args.gn', $env:GN_GEN_ARGS.Replace(' ', \"`n\"))"
IF %ERRORLEVEL% NEQ 0 GOTO ERROR

REM Detect VS 2022 install path via vswhere and export vs2022_install
REM so vs_toolchain.py can find it. VS 2022 is 64-bit only (Program Files,
REM not Program Files (x86)) which the M98-era detection code doesn't check.
ECHO Detecting VS 2022 install path
FOR /F "tokens=*" %%i IN ('"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2^>nul') DO SET vs2022_install=%%i
IF DEFINED vs2022_install (
  ECHO Found VS 2022 at: %vs2022_install%
) ELSE (
  ECHO WARNING: vswhere did not find Visual Studio
)

REM Patch vs_toolchain.py for VS 2022 support. Three changes:
REM 1. Add ('2022','17.0') to MSVS_VERSIONS OrderedDict
REM 2. Make GetVisualStudioVersion() honor GYP_MSVS_VERSION env var
REM 3. Make DetectVisualStudioPath() check vs2022_install env var
ECHO Patching vs_toolchain.py to support VS 2022
powershell -NoProfile -Command ^
  "$f = Join-Path $env:SOURCE_DIR 'build\vs_toolchain.py'; " ^
  "$c = [IO.File]::ReadAllText($f); " ^
  "if ($c -notmatch [regex]::Escape(\"('2022', '17.0')\")) { " ^
  "  $c = $c -replace [regex]::Escape(\"('2019', '16.0'),\"), \"('2022', '17.0'),`n    ('2019', '16.0'),\"; " ^
  "  Write-Host 'Patched MSVS_VERSIONS'; " ^
  "} else { Write-Host 'MSVS_VERSIONS already patched' }; " ^
  "if ($c -notmatch 'GYP_MSVS_VERSION') { " ^
  "  $c = $c -replace 'def GetVisualStudioVersion\(\):', (\"def GetVisualStudioVersion():`n  env_ver = os.environ.get('GYP_MSVS_VERSION')`n  if env_ver and env_ver in MSVS_VERSIONS:`n    return env_ver\"); " ^
  "  Write-Host 'Patched GetVisualStudioVersion'; " ^
  "} else { Write-Host 'GetVisualStudioVersion already patched' }; " ^
  "if ($c -notmatch 'vs2022_install') { " ^
  "  $c = $c -replace \"'vs2019_install'\)\", \"'vs2022_install') or os.environ.get('vs2019_install')\"; " ^
  "  Write-Host 'Patched DetectVisualStudioPath for vs2022_install'; " ^
  "} else { Write-Host 'vs2022_install already patched' }; " ^
  "[IO.File]::WriteAllText($f, $c); " ^
  "$lines = [IO.File]::ReadAllLines($f) | Where-Object { $_ -match 'MSVS_VERSIONS|2022|GetVisualStudioVersion|GYP_MSVS_VERSION|vs2022' }; " ^
  "$lines | ForEach-Object { Write-Host $_.Trim() }"
IF %ERRORLEVEL% NEQ 0 GOTO ERROR

REM Detect the correct Windows SDK version (the one with user32.lib).
REM Then find and replace the hardcoded M98-era SDK version (10.0.19041.0)
REM across ALL build config files (.py, .gn, .gni). The M98 source hardcodes
REM this version in multiple places and the "update toolchain" CMake step
REM already ran vs_toolchain.py (before our patches) writing stale env files.
REM We also delete cached environment files to force regeneration.
ECHO Detecting Windows SDK and replacing hardcoded version in build files
powershell -NoProfile -Command ^
  "$sdkDir = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\lib'; " ^
  "$sdkVer = Get-ChildItem $sdkDir -Directory | Sort-Object Name -Descending | Where-Object { Test-Path (Join-Path $_.FullName 'um\x86\user32.lib') } | Select-Object -First 1 -ExpandProperty Name; " ^
  "if (-not $sdkVer) { Write-Host 'ERROR: No Windows SDK with user32.lib'; exit 1 }; " ^
  "Write-Host \"SDK with user32.lib: $sdkVer\"; " ^
  "$buildDir = Join-Path $env:SOURCE_DIR 'build'; " ^
  "Write-Host '--- Files containing 10.0.19041 ---'; " ^
  "Get-ChildItem $buildDir -Recurse -Include '*.py','*.gn','*.gni' -ErrorAction SilentlyContinue | Select-String '10\.0\.19041' | ForEach-Object { Write-Host \"$($_.RelativePath):$($_.LineNumber): $($_.Line.Trim())\" }; " ^
  "Write-Host '--- Replacing 10.0.19041.0 with $sdkVer ---'; " ^
  "$count = 0; " ^
  "Get-ChildItem $buildDir -Recurse -Include '*.py','*.gn','*.gni' -ErrorAction SilentlyContinue | ForEach-Object { " ^
  "  $content = [IO.File]::ReadAllText($_.FullName); " ^
  "  if ($content -match '10\.0\.19041\.0') { " ^
  "    $content = $content -replace '10\.0\.19041\.0', $sdkVer; " ^
  "    [IO.File]::WriteAllText($_.FullName, $content); " ^
  "    Write-Host \"Patched: $($_.FullName)\"; " ^
  "    $count++; " ^
  "  } " ^
  "}; " ^
  "Write-Host \"Patched $count files\"; " ^
  "Write-Host '--- Deleting cached environment files ---'; " ^
  "Get-ChildItem $buildDir -Recurse -Filter 'environment.*' -ErrorAction SilentlyContinue | ForEach-Object { " ^
  "  Write-Host \"Deleting: $($_.FullName)\"; " ^
  "  Remove-Item $_.FullName -Force; " ^
  "}"
IF %ERRORLEVEL% NEQ 0 GOTO ERROR

REM Replace bundled M98-era clang (v14) with system clang (v19+).
REM MSVC 14.44+ STL headers require Clang 19+. We can't use clang_base_path
REM in nix.gni because GN's rebase_path() can't handle Windows drive letters
REM (produces broken paths like ..\..\..\C:\PROGRA~1\LLVM\bin\clang-cl.exe).
REM Instead, copy system clang binaries over the downloaded bundled ones.
ECHO Replacing bundled clang with system clang
ECHO Script path: %~dp0replace-bundled-clang.ps1
IF NOT EXIST "%~dp0replace-bundled-clang.ps1" (
  ECHO ERROR: replace-bundled-clang.ps1 not found at %~dp0
  GOTO ERROR
)
powershell -NoProfile -ExecutionPolicy Bypass -Command "& { try { & '%~dp0replace-bundled-clang.ps1' '%SOURCE_DIR%' } catch { Write-Host \"ERROR: $_\"; exit 1 } }"
IF %ERRORLEVEL% NEQ 0 GOTO ERROR

REM Patch build/toolchain/win/BUILD.gn to fix "sys_lib_flags" unused invoker error.
ECHO Patching BUILD.gn to suppress unused sys_lib_flags error
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0patch-sys-lib-flags.ps1" "%SOURCE_DIR%"
IF %ERRORLEVEL% NEQ 0 GOTO ERROR

REM Patch abseil-cpp type_traits.h for Clang 20 compatibility.
REM Deprecated builtins return different values than std:: equivalents,
REM causing compliance static_asserts to fail.
ECHO Patching abseil-cpp for Clang 20 compatibility
powershell -NoProfile -ExecutionPolicy Bypass -Command "& { try { & '%~dp0patch-abseil-builtins.ps1' '%SOURCE_DIR%' } catch { Write-Host \"ERROR: $_\"; exit 1 } }"
IF %ERRORLEVEL% NEQ 0 GOTO ERROR

ECHO gn gen BINARY_DIR (reading args from args.gn)
CALL gn gen %BINARY_DIR%
IF %ERRORLEVEL% NEQ 0 GOTO ERROR

GOTO DONE

:ERROR
ECHO ERRORLEVEL^: %ERRORLEVEL%
SET EL=%ERRORLEVEL%

:DONE

EXIT /b %EL%
