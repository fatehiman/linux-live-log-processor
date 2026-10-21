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
| `/etc/systemd/system/livelog.service` | systemd unit |
| `/var/log/livelog/` | encrypted log files (default `log_dir`) |

The service starts after boot. It waits 30 seconds first (`ExecStartPre=/bin/sleep 30` in the unit file), so asterisk has time to start.

Useful commands:

```sh
systemctl status livelog
journalctl -u livelog -f          # messages of the service
systemctl restart livelog         # after you change the config
```

To update: `git pull && sudo sh install.sh` (your config is kept).
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
```

More details: [reference-docs/](reference-docs/).
