@echo off
rem Visual Studio Build Tools Developer Command Prompt; pio run -e p4 dependencies required.
if not exist .pio\preview mkdir .pio\preview
cl /nologo /EHsc /std:c++17 /utf-8 /Itools\preview_support /I".pio/libdeps/p4/Adafruit GFX Library" /Itest\support /Isrc\common /Isrc\boards\p4 /Fo.pio\preview\ /Fe.pio\preview\preview.exe tools\preview_p4.cpp
if errorlevel 1 exit /b 1
.pio\preview\preview.exe
if errorlevel 1 exit /b 1
python tools\render_p4_preview.py
exit /b %errorlevel%
