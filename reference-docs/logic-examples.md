# Chunk logic - examples

These examples were agreed before the code was written.

Config: `event_prefix = EVENT`, `match = SIP/100`

## 1. Basic flow

| # | Line | Starts with `EVENT`? | Has `SIP/100`? | Result |
|---|---|---|---|---|
| 1 | `Asterisk starting...` | no | - | skip (no EVENT yet) |
| 2 | `EVENT new call SIP/100` | yes | yes | timestamp + LOG (ON) |
| 3 | `  dialing...` | no | - | LOG |
| 4 | `  answered` | no | - | LOG |
| 5 | `EVENT new call SIP/300` | yes | no | skip (OFF) |
| 6 | `  dialing SIP/100` | no | - | skip (only EVENT lines are checked) |
| 7 | `EVENT hangup SIP/100` | yes | yes | timestamp + LOG (ON) |
| 8 | `  cause=16` | no | - | LOG |

## 2. Case sensitive, "starts with"

| Line | New chunk? |
|---|---|
| `EVENT call SIP/100` | yes |
| `event call SIP/100` | no (lowercase) |
| `  EVENT call SIP/100` | no (starts with spaces) |
| `EVENTS foo` | yes (it starts with `EVENT`) |

A line that is not a new chunk follows the current state (ON or OFF).

## 3. Midnight

A chunk that starts at 23:59:58 continues after midnight. Lines before midnight go to `ast-20261021.log`. Lines after midnight go to `ast-20261022.log`. The state stays ON.

## 4. Command stops

If the command stops, wait `restart_delay` seconds, run it again, and repeat until it works. The state goes back to OFF, and a half line from the old run is dropped.

## 5. Decisions

- Several match texts: `match_mode = any` (default) or `all`.
- Match texts are case sensitive.
- A timestamp line `yyyymmddhhmmss` is written before every matching EVENT line.
- Keep the newest 20 log files.
- Simple encryption (RC4 + nonce), decrypt tools for Linux and Windows, key as the 2nd argument.
- Language: C.
