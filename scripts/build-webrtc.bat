@ECHO OFF
SET EL=0

ECHO Add depot_tools to PATH
set PATH=%DEPOT_TOOLS%;%PATH%
IF %ERRORLEVEL% NEQ 0 GOTO ERROR

REM Set up MSVC developer environment so lib.exe and link.exe are on PATH.
REM GN generates ninja files that call lib.exe/link.exe by bare name,
REM expecting the MSVC tools to be in PATH via vcvarsall.
ECHO Setting up MSVC developer environment
FOR /F "tokens=*" %%i IN ('"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath 2^>nul') DO SET VS_INSTALL=%%i
IF DEFINED VS_INSTALL (
  ECHO Found VS at: %VS_INSTALL%
  CALL "%VS_INSTALL%\VC\Auxiliary\Build\vcvarsall.bat" amd64
  IF %ERRORLEVEL% NEQ 0 GOTO ERROR
) ELSE (
  ECHO WARNING: Visual Studio not found via vswhere
)

ECHO ninja
call autoninja webrtc libjingle_peerconnection
IF %ERRORLEVEL% NEQ 0 GOTO ERROR

GOTO DONE

:ERROR
ECHO ERRORLEVEL^: %ERRORLEVEL%
SET EL=%ERRORLEVEL%

:DONE

EXIT /b %EL%
