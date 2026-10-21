@echo off
rem Usage: livelog-decrypt.cmd <file> <key> [output-file]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0livelog-decrypt.ps1" %*
