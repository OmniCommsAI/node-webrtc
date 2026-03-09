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
REM %GN_GEN_ARGS% inside another quoted string. Python reads the raw
REM env var value and writes it to the file correctly.
ECHO Writing args.gn via Python to preserve inner quotes
python3 -c "import os; open(os.path.join(os.environ['BINARY_DIR'],'args.gn'),'w').write(os.environ['GN_GEN_ARGS'].replace(' ','\n'))"
IF %ERRORLEVEL% NEQ 0 (
  ECHO python3 not found, trying python
  python -c "import os; open(os.path.join(os.environ['BINARY_DIR'],'args.gn'),'w').write(os.environ['GN_GEN_ARGS'].replace(' ','\n'))"
  IF %ERRORLEVEL% NEQ 0 GOTO ERROR
)

ECHO gn gen BINARY_DIR (reading args from args.gn)
CALL gn gen %BINARY_DIR%
IF %ERRORLEVEL% NEQ 0 GOTO ERROR

GOTO DONE

:ERROR
ECHO ERRORLEVEL^: %ERRORLEVEL%
SET EL=%ERRORLEVEL%

:DONE

EXIT /b %EL%
