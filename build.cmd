@echo off
rem to build.ps1 (e.g.  build.cmd -Test  /  build.cmd -Clean -Run  /  build.cmd -Config Debug).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*