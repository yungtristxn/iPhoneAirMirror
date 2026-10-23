@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "prepare_only="
if not "%~2"=="" goto usage
if "%~1"=="" goto prepare
if /i "%~1"=="--help" goto help
if /i "%~1"=="--prepare-only" set "prepare_only=1"
if not defined prepare_only goto usage

:prepare
if not exist "%~dp0iPhoneAirMirror.exe" goto missing
echo Preparing this copy of iPhone Air Mirror for its restricted codec worker...
rem Explicit owner preparation: existing correct permission is left unchanged.
rem CMD waits for the GUI-subsystem executable when invoked from a batch file.
"%~dp0iPhoneAirMirror.exe" --provision-worker
set "prepare_exit=%errorlevel%"
if not "%prepare_exit%"=="0" goto prepare_failed
if defined prepare_only exit /b 0
start "" /D "%~dp0" "%~dp0iPhoneAirMirror.exe"
if errorlevel 1 goto launch_failed
exit /b 0

:missing
echo iPhoneAirMirror.exe is missing beside this launcher.
echo Extract the whole ZIP to a folder you own, then run this launcher there.
if not defined prepare_only pause
exit /b 2

:prepare_failed
echo Worker preparation failed with exit code %prepare_exit%.
echo Read README.txt for the Microsoft runtime prerequisites.
echo Use a local extracted folder you own. Administrator mode is not required.
if not defined prepare_only pause
exit /b 1

:launch_failed
echo Windows could not launch iPhone Air Mirror. Read README.txt for requirements.
pause
exit /b 1

:usage
echo Unsupported arguments. Run Start-iPhoneAirMirror.cmd --help for usage.
exit /b 2

:help
echo Start-iPhoneAirMirror.cmd                 Prepare this copy and open the app.
echo Start-iPhoneAirMirror.cmd --prepare-only  Prepare the worker, then exit.
echo This launcher does not install prerequisites or change firewall settings.
exit /b 0
