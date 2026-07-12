@echo off
REM Tail the iOS app's live syslog, filtered to our "OnlyChess:" log tag — the
REM iPhone counterpart of logcat.bat. Every NSLog in the app (and the Vulkan
REM backend via PlatformLog) is prefixed "OnlyChess:", so this is the app-only
REM view, like `logcat -s OnlyChess`.
REM
REM Requires pymobiledevice3 (run setup-ios-logging.bat once) and the iPhone
REM connected via USB, unlocked + trusted, with Apple Mobile Device Service
REM running (ships with iTunes). No Mac / Xcode needed.
REM
REM Extra flags pass through, e.g.  scripts\ios-syslog.bat -m Renderer
REM For the full app-process firehose (incl. iOS framework noise) instead:
REM   pymobiledevice3 syslog live -pn OnlyChess
pymobiledevice3 syslog live -m "OnlyChess:" %*
