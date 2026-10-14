@echo off
REM 便携工具链环境（免管理员）。用法：call tools\env.cmd  &&  cmake --build build
REM 若工具链装在别处，先设 WPM_TC 覆盖默认路径。
if "%WPM_TC%"=="" set "WPM_TC=D:\GitHub\winperfmon-toolchain"
set "PATH=%WPM_TC%\mingw\mingw64\bin;%WPM_TC%\cmake\bin;%WPM_TC%\ninja;%PATH%"
set "CC=%WPM_TC%\mingw\mingw64\bin\gcc.exe"
set "CXX=%WPM_TC%\mingw\mingw64\bin\g++.exe"
