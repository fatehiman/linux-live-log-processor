# Encrypted log file format

File name: `<file_prefix>yyyymmdd.log` (default `ast-yyyymmdd.log`).

## Layout

```
"LLP1"                      4 bytes, once at the start of the file
record 1
record 2
...
```

Each record is one line:

| Field | Size | Notes |
|---|---|---|
| nonce | 8 bytes | random (from `/dev/urandom`) |
| length | 4 bytes | big endian, size of `data` |
| data | `length` bytes | encrypted `"LL" + line + "\n"` |

## Encryption

- Cipher: RC4, the first 1024 key stream bytes are dropped (RC4-drop1024).
- RC4 key = `nonce` (8 bytes) + user key bytes (max 248 bytes, UTF-8).
- A new random nonce for each record, so two equal lines look different in the file.
- After decryption the data must start with `LL`. If not, the key is wrong.

This is light encryption. It is made to stop people reading the log as plain text. It does not protect against a skilled attacker.

## Why one record per line

The service never ends. Each line is written and flushed at once. If the server loses power, all complete records are still readable. A cut last record is reported as a warning and skipped.

## Code

- C: [src/rc4.h](../src/rc4.h), [src/livelog-decrypt.c](../src/livelog-decrypt.c)
- Windows: [windows/livelog-decrypt.ps1](../windows/livelog-decrypt.ps1) (C# inside PowerShell, same logic)
