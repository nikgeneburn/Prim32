@echo off
rem Prim32 demo build - run from an x64 Native Tools Command Prompt for VS.
setlocal

where cl >nul 2>nul
if errorlevel 1 (
    echo ERROR: cl.exe was not found. Open an x64 Native Tools Command Prompt.
    exit /b 1
)

for %%I in ("%~dp0..") do set "ROOT=%%~fI"
set "OUT=%ROOT%\build\msvc-release"
set "OBJ=%OUT%\obj"

if not exist "%OBJ%" mkdir "%OBJ%"

cl /nologo /std:c++17 /O2 /Oi /Ot /GL /fp:fast /EHs-c- /GR- /DNDEBUG /DUNICODE /D_UNICODE ^
   /I"%ROOT%\include" ^
   /I"%ROOT%\src" ^
   "%ROOT%\src\prim32.cpp" ^
   "%ROOT%\src\prim32_resources.cpp" ^
   "%ROOT%\src\prim32_freetype.cpp" ^
   "%ROOT%\src\backends\prim32_dx12.cpp" ^
   "%ROOT%\src\profiler\p32prof.cpp" ^
   "%ROOT%\src\profiler\p32prof_ui.cpp" ^
   "%ROOT%\examples\d3d12_demo\main.cpp" ^
   /Fo"%OBJ%\\" /Fe"%OUT%\Prim32Demo.exe" ^
   /link /LTCG /SUBSYSTEM:WINDOWS d3d12.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib psapi.lib ole32.lib windowscodecs.lib

set "RESULT=%ERRORLEVEL%"
if "%RESULT%"=="0" (
    echo.
    echo   OK -^> %OUT%\Prim32Demo.exe
)

endlocal & exit /b %RESULT%
