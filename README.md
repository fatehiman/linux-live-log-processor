# Linux Live Log Processor (`livelog`)

A tiny Linux service in C. It runs a command (for example `asterisk -rvvvvv`), reads its output **live**, and saves only the interesting parts into an **encrypted** daily log file.

## How it works

1. It reads the output line by line. A line ends with a newline (EOL).
2. A line that **starts with** `event_prefix` (for example `EVENT`, case sensitive) begins a new **chunk**.
3. If that EVENT line contains the `match` texts, logging is **ON**:
   - first a timestamp line is written (`yyyymmddhhmmss`),
   - then the EVENT line,
   - then all next lines, until the next EVENT line.
4. If the EVENT line does not contain the texts, logging is **OFF** until the next EVENT line.
5. Lines before the first EVENT line are not logged.
6. If the command stops (for example asterisk stopped), livelog waits `restart_delay` seconds and runs it again. It does this again and again until the command works.
7. It stops only with a signal (`systemctl stop`, SIGTERM, SIGINT, power off).

### Example

Config: `event_prefix = EVENT`, `match = SIP/100`

| Line from command | Result |
|---|---|
| `Asterisk starting...` | skip (no EVENT seen yet) |
| `EVENT new call SIP/100` | timestamp line + **LOG** (ON) |
| `  dialing...` | **LOG** |
| `EVENT new call SIP/300` | skip (OFF) |
| `  dialing SIP/100` | skip (only EVENT lines are checked) |
| `event lowercase SIP/100` | skip (lowercase: not an EVENT line) |
| `EVENT hangup SIP/100` | timestamp line + **LOG** (ON again) |

Decrypted log file:

```
20261021143005
EVENT new call SIP/100
  dialing...
20261021143112
EVENT hangup SIP/100
```

## Log files

- Name: `<file_prefix>yyyymmdd.log`, for example `ast-20261021.log`. At midnight a new file starts.
- Only the newest `keep_files` files are kept (default 20). Older ones are deleted.
- The files are encrypted. Simple encryption (RC4 with a random nonce for each line). It stops people from reading the file as plain text. It is not strong encryption.
- Each line is saved at once. After a crash or power loss, all lines written before are still readable.

## FTP upload (optional)

If you set `ftp_server`, `ftp_user` and `ftp_password` in the config, livelog uploads the log files to an FTP server. The files stay encrypted. It uses plain FTP in passive mode (no FTPS, no SFTP), and needs no extra library.

```
ftp_server = ftp.example.com
ftp_user = myuser
ftp_password = mypassword
ftp_path = /livelog/pbx1
ftp_interval = 60
```

A second process does the upload. It never slows down or blocks the logging. Every `ftp_interval` seconds (default 60) it does one **round**:

1. **File of today.** It is still written, so livelog first makes a copy in `/tmp` (only complete records, never a half written line) and uploads the copy. If the size on the server is already the same, nothing is sent. If it fails or times out, livelog tries again after `ftp_retry_delay` seconds (default 10), up to `ftp_retries` times (default 10).
2. **File before it.** The same: size check, and the same retries.
3. **Older files**, from newest to oldest. A file is uploaded only if its size on the server is different. One try each. If a try fails, there is no retry. The file is tried again in the next round.

Notes:

- Today's file has the first priority. When it fails 10 times, the round goes on with the other files. In the next round it starts again with today's file.
- If the server cannot be reached at all, the older files wait for the next round (this saves time).
- Files on the server are never deleted, even if livelog already deleted them (`keep_files`).
- The target folder (`ftp_path`) is created if it is missing.
- The next round starts `ftp_interval` seconds after the end of the last one.
- The FTP password is in the config file (`chmod 600`). Plain FTP sends the password without encryption.
- Messages are in `journalctl -u livelog -f` (lines that start with `livelog: ftp:`).
- To test the settings once, without the service: `/opt/livelog/livelog -u` (one round, then exit).

## Compile only

```sh
apt install gcc git
git clone https://github.com/fatehiman/linux-live-log-processor.git
cd linux-live-log-processor
sh compile.sh          # build only
sh compile.sh test     # build and run the tests
```

## Install as a service (Debian / Ubuntu)

```sh
sudo sh install.sh
sudo nano /opt/livelog/livelog.conf      # set key, command, match
sudo systemctl start livelog
```

Files after install:

| Path | What |
|---|---|
| `/opt/livelog/livelog` | the service program |
| `/opt/livelog/livelog-decrypt` | decrypt tool |
| `/opt/livelog/livelog.conf` | config (next to the program, `chmod 600`) |
| `/etc/systemd/system/livelog.service` | systemd unit (written by `install.sh`) |
| `/var/log/livelog/` | encrypted log files (default `log_dir`) |

The service starts after boot. It waits 30 seconds first, so asterisk has time to start. To change this, edit `START_DELAY` in `install.sh` and run it again.

`install.sh` needs only `compile.sh`, `src/`, and a config file in the folder. It uses `livelog.conf` if it is there, else `livelog.conf.example`. It **overwrites** `/opt/livelog/livelog.conf` every time (the old file is saved as `livelog.conf.bak`). If the key is still `change-me-please`, it installs but does not start the service.

Useful commands:

```sh
systemctl status livelog
journalctl -u livelog -f          # messages of the service
systemctl restart livelog         # after you change the config
```

To update: `git pull && sudo sh install.sh` (it builds, installs the new program and restarts the service; the config in the project folder replaces `/opt/livelog/livelog.conf`, so keep your real `livelog.conf` in the project folder).
To remove: `sudo sh uninstall.sh` (config and logs are kept).

## Config

See [livelog.conf.example](livelog.conf.example). Main keys:

| Key | Default | Meaning |
|---|---|---|
| `command` | – | command to run |
| `event_prefix` | – | a line that starts with this begins a chunk (case sensitive) |
| `match` | – | text to find in the EVENT line. One `match =` line per text. No match lines = log every chunk |
| `match_mode` | `any` | `any` = one text is enough, `all` = every text is needed |
| `key` | – | encryption key (max 248 characters) |
| `log_dir` | `/var/log/livelog` | folder for log files |
| `file_prefix` | `ast-` | start of the file name |
| `keep_files` | `20` | number of log files to keep (0 = keep all) |
| `restart_delay` | `5` | seconds to wait before running the command again |
| `use_pty` | `yes` | run the command in a pseudo-terminal |
| `strip_ansi` | `yes` | remove color codes and control characters |
| `ftp_server` | – | FTP server name or IP. Empty = no upload |
| `ftp_user` | – | FTP user (needed if `ftp_server` is set) |
| `ftp_password` | – | FTP password |
| `ftp_path` | – (login folder) | target folder on the server |
| `ftp_interval` | `60` | seconds between two upload rounds |
| `ftp_port` | `21` | FTP port |
| `ftp_retries` | `10` | retries for today's file and the file before it |
| `ftp_retry_delay` | `10` | seconds between two tries |
| `ftp_timeout` | `30` | seconds to wait for the server |

Match texts are case sensitive.

### About `asterisk -r`

`asterisk -r` wants a terminal. Without one it may exit at once or buffer its output. So `use_pty = yes` is the default: livelog gives the command a pseudo-terminal (PTY), like a real terminal. Color codes are removed by `strip_ansi = yes`.

Another way is to read the asterisk log file: `command = tail -n 0 -F /var/log/asterisk/full` (then `use_pty = no` is fine).

## Decrypt the log files

The key is the 2nd argument. The output file is optional; without it the text is printed.

**Linux:**

```sh
/opt/livelog/livelog-decrypt /var/log/livelog/ast-20261021.log MY_KEY
/opt/livelog/livelog-decrypt /var/log/livelog/ast-20261021.log MY_KEY out.txt
```

**Windows** (copy the `windows` folder, no install needed):

```bat
livelog-decrypt.cmd ast-20261021.log MY_KEY
livelog-decrypt.cmd ast-20261021.log MY_KEY out.txt
```

A wrong key gives the error `wrong key`.

## Build and test by hand

```sh
make          # builds livelog and livelog-decrypt
make test     # runs tests/run.sh
./livelog -c my.conf -s < sample.txt   # -s: read stdin once, for tests
./livelog -c my.conf -u                # -u: one FTP upload round, then exit
```

More details: [reference-docs/](reference-docs/).
