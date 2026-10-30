@echo off
rem Builds HB_ExpertMouse.exe with the C# compiler that ships with Windows (.NET Framework 4.x).
rem No SDK, Visual Studio or downloads needed.
setlocal
set CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe
if not exist "%CSC%" set CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe
if not exist "%CSC%" (echo csc.exe not found - .NET Framework 4.x is required & exit /b 1)
"%CSC%" /nologo /target:winexe /optimize+ /platform:anycpu /out:"%~dp0HB_ExpertMouse.exe" ^
  /r:System.Windows.Forms.dll /r:System.Drawing.dll "%~dp0src\*.cs"
if errorlevel 1 exit /b 1
echo Built %~dp0HB_ExpertMouse.exe
