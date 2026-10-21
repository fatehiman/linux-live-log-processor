# FTP upload

How it works and why. For the settings see the main [README](../README.md).

## Process

`livelog` starts a second process with `fork()` when `ftp_server` is set. The main process only reads and logs, so a slow or dead FTP server can never block the logging.

- The upload process stops with the main process (`SIGTERM` from the main process, and `PR_SET_PDEATHSIG` as a safety net).
- If it dies, the main process starts it again (after 10 seconds at the earliest).
- `livelog -u` runs one round in the foreground and exits. Used by the tests and for a manual check.

## One round

Files are listed by name (`<file_prefix>yyyymmdd.log` sorts by date), newest first.

| Rank | File | Size check | Tries |
|---|---|---|---|
| 1 | newest (today, if it exists) | yes | 1 + `ftp_retries` |
| 2 | second newest | yes | 1 + `ftp_retries` |
| 3... | older | yes | 1 |

- Between two tries: `ftp_retry_delay` seconds.
- Failed older files are not retried in the same round. They come again in the next round, after today's file and the file before it.
- Connection failure on an older file ends the round (all next files would fail too).
- After the round: wait `ftp_interval` seconds.

## Snapshot of today's file

The main process appends to today's file all the time. So the uploader first copies it to an unnamed temp file in `/tmp` (`mkstemp` + `unlink`, so nothing stays on disk, even after a crash). The copy follows the record format ([file-format.md](file-format.md)): header (12 bytes), then data. It stops at the first incomplete record. So a half written last record is never uploaded; it is uploaded in the next round when it is complete.

The size of the copy is compared with the size on the server (`SIZE`). Same size = nothing to upload. The log only grows, so the same size means the same content.

## FTP details

Own small client in [src/ftp.h](../src/ftp.h), no library:

- Plain FTP (no TLS). Passive mode: `EPSV`, and `PASV` if `EPSV` is refused. The IP address in a `PASV` reply is ignored; the address of the control connection is used (servers behind NAT often send a wrong one).
- Binary mode (`TYPE I`). `STOR` writes the final name directly. A cut upload leaves a smaller file on the server; the size check finds it and uploads again.
- Every socket has a timeout (`ftp_timeout`).
- One login per round. The connection is closed after the round.
- The target folder is entered with `CWD`, and created with `MKD` (one level at a time) if it is missing.

## Tests

`tests/run.sh` (test 8) starts [tests/fake_ftpd.py](../tests/fake_ftpd.py), a tiny FTP server, and checks: full upload, folder creation, snapshot cut, size check, retries, PASV fallback, server down. It needs `python3`; without it the FTP tests are skipped.
