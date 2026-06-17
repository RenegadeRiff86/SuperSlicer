@echo off
rem Standalone build + run for the AdaptivePAModel test harness (issue #39).
rem Compiles only AdaptivePressureAdvance.cpp + run_tests.cpp -- no libslic3r needed.
rem (Named run_tests.bat rather than build.bat because the repo .gitignore ignores build*.)
setlocal
set "DIR=%~dp0"
set "REPO=%DIR%..\.."
set "GCODE=%REPO%\src\libslic3r\GCode"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

call "%VCVARS%" >nul
if errorlevel 1 (echo vcvars64 failed& exit /b 1)

if not exist "%DIR%obj" mkdir "%DIR%obj"

cl /nologo /std:c++17 /EHsc /O2 /I"%GCODE%" "%DIR%run_tests.cpp" "%GCODE%\AdaptivePressureAdvance.cpp" /Fe:"%DIR%run_tests.exe" /Fo:"%DIR%obj\\"
if errorlevel 1 (echo build failed& exit /b 1)

"%DIR%run_tests.exe"
exit /b %errorlevel%
