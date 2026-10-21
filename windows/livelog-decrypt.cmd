@echo off
rem Usage: livelog-decrypt.cmd <file> <key> [output-file]
rem    or: livelog-decrypt.cmd   (no arguments: key from livelog.conf next to this file, decrypts all *.log here)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0livelog-decrypt.ps1" %*
