@echo off
echo ================================
echo    TCP/IP Server Application
echo ================================
echo.

REM Check if TCPServer.exe exists in project x64 Debug folder
if exist "%~dp0x64\Debug\TCPServer.exe" (
	echo Starting server from x64\Debug...
	"%~dp0x64\Debug\TCPServer.exe"
	goto :end
)

REM Check if TCPServer.exe exists in project x64 Release folder
if exist "%~dp0x64\Release\TCPServer.exe" (
	echo Starting server from x64\Release...
	"%~dp0x64\Release\TCPServer.exe"
	goto :end
)

REM Check if TCPServer.exe exists in project Debug folder
if exist "%~dp0Debug\TCPServer.exe" (
	echo Starting server from Debug...
	"%~dp0Debug\TCPServer.exe"
	goto :end
)

REM Check if TCPServer.exe exists in project Release folder
if exist "%~dp0Release\TCPServer.exe" (
	echo Starting server from Release...
	"%~dp0Release\TCPServer.exe"
	goto :end
)

REM Check current directory
if exist "TCPServer.exe" (
	echo Starting server from current directory...
	TCPServer.exe
	goto :end
)

echo ERROR: TCPServer.exe not found!
echo Please build the TCPServer project first.
echo.

:end
echo.
pause
