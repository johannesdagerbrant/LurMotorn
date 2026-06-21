@echo off
REM Remove host + Android build output for a from-scratch rebuild.
setlocal
set "ROOT=%~dp0.."
if exist "%ROOT%\build" rmdir /s /q "%ROOT%\build"
if exist "%ROOT%\Games\Chess\Android\app\build" rmdir /s /q "%ROOT%\Games\Chess\Android\app\build"
if exist "%ROOT%\Games\Chess\Android\.gradle" rmdir /s /q "%ROOT%\Games\Chess\Android\.gradle"
echo Clean complete.
