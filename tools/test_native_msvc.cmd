@echo off
rem Visual Studio Build Tools Developer Command Prompt
where cl >nul 2>nul
if errorlevel 1 exit /b 1
if not exist .pio\native-msvc mkdir .pio\native-msvc
cl /nologo /EHsc /std:c++17 /utf-8 /Isrc\boards\p4 /I.pio\libdeps\native\Unity\src /Fo.pio\native-msvc\ /Fe.pio\native-msvc\listening.exe test\test_listening\test_main.cpp .pio\libdeps\native\Unity\src\unity.c
if errorlevel 1 exit /b 1
.pio\native-msvc\listening.exe
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++17 /utf-8 /Itest\support /Isrc\boards\p4 /Isrc\common /I.pio\libdeps\native\Unity\src /Fo.pio\native-msvc\ /Fe.pio\native-msvc\p4-ui.exe test\test_p4_ui\test_main.cpp .pio\libdeps\native\Unity\src\unity.c
if errorlevel 1 exit /b 1
.pio\native-msvc\p4-ui.exe
exit /b %errorlevel%
