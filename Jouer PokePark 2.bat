@echo off
cd /d "%~dp0"
if exist pokepark2.log move /y pokepark2.log pokepark2.previous.log >nul
rem The game writes its own log: no console window stays open behind it.
set WP_LOG_FILE=pokepark2.log
start "" "build\out\pokepark2.exe"
