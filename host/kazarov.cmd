@echo off
rem Kazarov entry point for Windows hosts.
rem Usage: kazarov.cmd ROOT build|run [config] [args]
rem Windows configs run through KZ_WINDOWS_HOST_SCRIPT; every other config runs in the container.
setlocal

if "%~1"=="" goto :usage
if "%~2"=="" goto :usage
if /i not "%~2"=="build" if /i not "%~2"=="run" goto :usage

rem KZ is the Kazarov checkout this script lives in.
for %%I in ("%~dp0..") do set "KZ=%%~fI"

rem ROOT is the project directory, without a trailing backslash.
for %%I in ("%~1") do set "ROOT=%%~fI"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "CMD=%~2"
set "ARG1=%~3"

rem The first config slot is ARG1. Forward the rest by hand, since shift does not move %*.
shift
shift
shift
set "REST="
:collect
if "%~1"=="" goto :collected
set "REST=%REST% %1"
shift
goto :collect
:collected

set "CONF=%ROOT%\kazarov.conf"
if not exist "%CONF%" (
  echo kazarov: kazarov.conf not found in the project root 1>&2
  exit /b 1
)

call :readvar KZ_PROJECT
call :readvar KZ_DEFAULT_BUILD
call :readvar KZ_DEFAULT_BUILD_WINDOWS
call :readvar KZ_DEFAULT_RUN
call :readvar KZ_DEFAULT_RUN_WINDOWS
call :readvar KZ_WINDOWS_HOST_SCRIPT
call :readvar KZ_WINDOWS_CONFIGS

rem The Windows default is the per-OS default, then the generic one.
set "DEFCFG=%KZ_DEFAULT_BUILD_WINDOWS%"
if /i "%CMD%"=="run" set "DEFCFG=%KZ_DEFAULT_RUN_WINDOWS%"
if not defined DEFCFG if /i "%CMD%"=="build" set "DEFCFG=%KZ_DEFAULT_BUILD%"
if not defined DEFCFG if /i "%CMD%"=="run" set "DEFCFG=%KZ_DEFAULT_RUN%"

rem The config is the first argument, or the Windows default when none is given.
set "CHK=%ARG1%"
if not defined CHK set "CHK=%DEFCFG%"
set "ISWIN="
if defined KZ_WINDOWS_CONFIGS if defined CHK for %%c in (%KZ_WINDOWS_CONFIGS%) do if /i "%%c"=="%CHK%" set "ISWIN=1"
if defined ISWIN goto :windows
goto :container

:windows
if not defined KZ_WINDOWS_HOST_SCRIPT (
  echo kazarov: KZ_WINDOWS_HOST_SCRIPT is not set in kazarov.conf 1>&2
  exit /b 1
)
set "PS="
where pwsh >nul 2>&1 && set "PS=pwsh"
if not defined PS (where powershell >nul 2>&1 && set "PS=powershell")
if not defined PS (
  echo kazarov: neither pwsh nor powershell is on PATH 1>&2
  exit /b 1
)
pushd "%ROOT%"
"%PS%" -NoProfile -File "%ROOT%\%KZ_WINDOWS_HOST_SCRIPT%" %CMD% %CHK% %REST%
set "RC=%errorlevel%"
popd
exit /b %RC%

:container
if not defined KZ_PROJECT (
  echo kazarov: KZ_PROJECT is not set in kazarov.conf 1>&2
  exit /b 1
)

rem Builder: BUILDER, then podman, then docker.
set "BLD="
if defined BUILDER set "BLD=%BUILDER%"
if not defined BLD (where podman >nul 2>&1 && set "BLD=podman")
if not defined BLD (where docker >nul 2>&1 && set "BLD=docker")
if not defined BLD (
  echo kazarov: no container builder found: set BUILDER, or install podman or docker 1>&2
  exit /b 1
)

rem Image tag: the first 12 hex digits of the image tree hash. Uncommitted edits or no commit give "dirty".
set "FULL="
for /f "delims=" %%t in ('git -C "%KZ%" rev-parse HEAD:image 2^>nul') do set "FULL=%%t"
set "DIRTY="
for /f "delims=" %%s in ('git -C "%KZ%" status --porcelain -- image 2^>nul') do set "DIRTY=1"
set "TAG=dirty"
if defined FULL if not defined DIRTY set "TAG=%FULL:~0,12%"

set "IMG=kazarov-dev:%TAG%"
set "NEED="
if "%TAG%"=="dirty" set "NEED=1"
if "%REBUILD_IMAGE%"=="1" set "NEED=1"
if not defined NEED (
  "%BLD%" image inspect "%IMG%" >nul 2>&1
  if errorlevel 1 set "NEED=1"
)
if defined NEED (
  "%BLD%" build --platform linux/amd64 --build-arg KZ_IMAGE_ID=%TAG% --label org.themoonpeople.kazarov.image=%TAG% -t "%IMG%" "%KZ%\image"
  if errorlevel 1 (
    echo kazarov: image build failed 1>&2
    exit /b 1
  )
)

rem The run descriptor is written inside the project so the host can read it back.
if not exist "%ROOT%\.kazarov" mkdir "%ROOT%\.kazarov"
set "DESC=run-%RANDOM%%RANDOM%.txt"
"%BLD%" run --rm --init -v "%ROOT%:/work/%KZ_PROJECT%" -w "/work/%KZ_PROJECT%" -e HOME=/tmp -e KZ_HOST_OS=windows -e CLEAN -e JOBS -e KZ_RUN_OUT=.kazarov/%DESC% "%IMG%" tools/kazarov/bin/kazarov --inside %CMD% %ARG1% %REST%
set "RC=%errorlevel%"
if not "%RC%"=="0" exit /b %RC%

if /i not "%CMD%"=="run" exit /b 0
set "DESCP=%ROOT%\.kazarov\%DESC%"
if not exist "%DESCP%" exit /b 0
set "DLINE="
set /p DLINE=<"%DESCP%"
del "%DESCP%"

set "KIND="
if "%DLINE:~0,4%"=="iso:" set "KIND=iso"
if "%DLINE:~0,4%"=="exe:" set "KIND=exe"
if not defined KIND (
  echo kazarov: unreadable run descriptor 1>&2
  exit /b 1
)
set "REL=%DLINE:~4%"
set "REL=%REL:/=\%"
set "TARGET=%ROOT%\%REL%"

if "%KIND%"=="exe" goto :runexe

call :findpcsx2
if not defined PCSX2X (
  echo kazarov: pcsx2-qt.exe not found: set PCSX2 or install PCSX2 1>&2
  exit /b 1
)
start "" "%PCSX2X%" -fastboot -- "%TARGET%"
exit /b 0

:runexe
"%TARGET%"
exit /b %errorlevel%

:findpcsx2
set "PCSX2X="
if defined PCSX2 set "PCSX2X=%PCSX2%"
if not defined PCSX2X for /f "delims=" %%p in ('where pcsx2-qt.exe 2^>nul') do if not defined PCSX2X set "PCSX2X=%%p"
if not defined PCSX2X if exist "%ProgramFiles%\PCSX2\pcsx2-qt.exe" set "PCSX2X=%ProgramFiles%\PCSX2\pcsx2-qt.exe"
exit /b 0

rem Sets the variable named by %1 from the NAME= line in kazarov.conf, quotes and parentheses removed.
:readvar
set "RV="
for /f "tokens=1* delims==" %%a in ('findstr /b /c:"%~1=" "%CONF%"') do set "RV=%%b"
if defined RV set "RV=%RV:"=%"
if defined RV set "RV=%RV:(=%"
if defined RV set "RV=%RV:)=%"
set "%~1=%RV%"
exit /b 0

:usage
echo usage: kazarov.cmd ROOT build^|run [config] [args] 1>&2
exit /b 2
