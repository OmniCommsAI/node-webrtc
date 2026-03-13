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

REM Patch vs_toolchain.py to support VS 2022 (17.0).
REM The M98-era WebRTC source only knows VS 2017/2019 but GitHub runners
REM now ship VS 2022 exclusively. We inject ('17.0','2022') into the
REM supported versions OrderedDict before gn gen calls vs_toolchain.py.
ECHO Patching vs_toolchain.py to support VS 2022
powershell -NoProfile -Command ^
  "$f = Join-Path $env:SOURCE_DIR 'build\vs_toolchain.py'; " ^
  "$c = [IO.File]::ReadAllText($f); " ^
  "if ($c -notmatch '17\.0') { " ^
  "  $c = $c -replace \"collections\.OrderedDict\(\[\('16\.0',\s*'2019'\)\", \"collections.OrderedDict([('17.0', '2022'), ('16.0', '2019')\"; " ^
  "  [IO.File]::WriteAllText($f, $c); " ^
  "  Write-Host 'Patched: added VS 2022 support'; " ^
  "} else { Write-Host 'Already patched' }"
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
