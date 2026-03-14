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

ECHO gn gen BINARY_DIR (reading args from args.gn)
CALL gn gen %BINARY_DIR%
IF %ERRORLEVEL% NEQ 0 GOTO ERROR

GOTO DONE

:ERROR
ECHO ERRORLEVEL^: %ERRORLEVEL%
SET EL=%ERRORLEVEL%

:DONE

EXIT /b %EL%
