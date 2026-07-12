@echo off
REM One-time: install pymobiledevice3 — the Windows-friendly, pure-Python reader
REM for a connected iPhone's live syslog. Used by ios-syslog.bat (the logcat
REM analog for iOS), so an agent/human can read live device logs on Windows with
REM no Mac and no Xcode.
REM
REM Prereq: Apple's "Apple Mobile Device Service" must be running. It ships with
REM iTunes (the Apple-website build already installed for Sideloadly) and provides
REM the USB (usbmux) channel pymobiledevice3 talks over. Syslog streams over plain
REM lockdown/usbmux, so no developer tunnel or disk image is required (works on
REM iOS 26).
python -m pip install --upgrade pymobiledevice3
echo.
echo Done. Plug in the iPhone (USB), unlock it, tap "Trust this computer", then:
echo   pymobiledevice3 usbmux list      (confirm the phone is seen)
echo   scripts\ios-syslog.bat           (stream the app's logs)
