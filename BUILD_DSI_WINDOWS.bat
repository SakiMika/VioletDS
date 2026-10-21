@echo off
rem Compatibility entrypoint: this project builds one normal NDS ROM.
rem DSi consoles can run that same NDS-mode ROM; there is no DSi variant.
rem Delegate to the canonical script to keep save profile, game code,
rem artifact name, errors and build logs consistent.
call "%~dp0build.bat" %*
exit /b %ERRORLEVEL%
