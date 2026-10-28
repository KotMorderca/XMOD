@echo off
rem SPDX-License-Identifier: GPL-3.0-only
rem Install a firmware package on the FM-1 over USB-MIDI (Felucca's installer, Python 3.12 via uv).
rem   tools\install.cmd                      build\fw\xmod.fwsc
rem   tools\install.cmd FM-1.fwsc            the official V15 file: back to the stock firmware
rem   tools\install.cmd --info               what the FM-1 runs now
setlocal
set ROOT=%~dp0..
set PKG=%1
if "%PKG%"=="" set PKG=%ROOT%\build\fw\xmod.fwsc
if "%PKG%"=="--info" (
  uv run --python 3.12 --with mido --with python-rtmidi python "%ROOT%\third_party\Felucca\tools\fm1_install.py" --info
) else (
  uv run --python 3.12 --with mido --with python-rtmidi python "%ROOT%\third_party\Felucca\tools\fm1_install.py" "%PKG%" %2 %3
)
