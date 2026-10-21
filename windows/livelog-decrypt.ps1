# livelog-decrypt.ps1 - decrypt a livelog file on Windows.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File livelog-decrypt.ps1 <file> <key> [output-file]
#   or simply:  livelog-decrypt.cmd <file> <key> [output-file]
#
# Without output-file the plain text is shown in the console.
#
# Without any argument: the key is read from livelog.conf next to this script,
# and every *.log file in this folder that is still encrypted is decrypted.
# The original is kept as <name>.log.bak, the plain text is saved as <name>.log.
# Files that are already decrypted are skipped.
# No install is needed. It works with Windows PowerShell 5.1 and PowerShell 7.

param(
    [Parameter(Position = 0)][string]$File,
    [Parameter(Position = 1)][string]$Key,
    [Parameter(Position = 2)][string]$OutFile
)

$ErrorActionPreference = 'Stop'

$src = @'
using System;
using System.IO;

public static class LiveLogDecrypt
{
    const int MaxKey = 248;
    const int Drop = 1024;
    const uint MaxRecord = 16u * 1024u * 1024u;

    static int Fill(Stream s, byte[] b)
    {
        int off = 0;
        while (off < b.Length) {
            int n = s.Read(b, off, b.Length - off);
            if (n <= 0) break;
            off += n;
        }
        return off;
    }

    static void Rc4(byte[] hdr, byte[] key, byte[] data)
    {
        int kl = Math.Min(key.Length, MaxKey);
        byte[] k = new byte[8 + kl];
        Array.Copy(hdr, 0, k, 0, 8);
        Array.Copy(key, 0, k, 8, kl);

        byte[] s = new byte[256];
        for (int i = 0; i < 256; i++) s[i] = (byte)i;
        int j = 0;
        for (int i = 0; i < 256; i++) {
            j = (j + s[i] + k[i % k.Length]) & 255;
            byte t = s[i]; s[i] = s[j]; s[j] = t;
        }
        int x = 0, y = 0;
        for (int n = 0; n < Drop + data.Length; n++) {
            x = (x + 1) & 255;
            y = (y + s[x]) & 255;
            byte t = s[x]; s[x] = s[y]; s[y] = t;
            byte ks = s[(s[x] + s[y]) & 255];
            if (n >= Drop) data[n - Drop] ^= ks;
        }
    }

    public static int Run(string inPath, byte[] key, Stream output)
    {
        int records = 0;
        using (FileStream fs = new FileStream(inPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite)) {
            byte[] magic = new byte[4];
            if (Fill(fs, magic) != 4 || magic[0] != 'L' || magic[1] != 'L' || magic[2] != 'P' || magic[3] != '1')
                throw new Exception("this is not a livelog file");
            byte[] hdr = new byte[12];
            while (true) {
                int got = Fill(fs, hdr);
                if (got == 0) break;
                if (got < 12) { Console.Error.WriteLine("warning: last record is cut (file was not closed cleanly)"); break; }
                uint len = ((uint)hdr[8] << 24) | ((uint)hdr[9] << 16) | ((uint)hdr[10] << 8) | hdr[11];
                if (len < 3 || len > MaxRecord) throw new Exception("file is damaged (bad record length)");
                byte[] buf = new byte[len];
                if (Fill(fs, buf) < len) { Console.Error.WriteLine("warning: last record is cut (file was not closed cleanly)"); break; }
                Rc4(hdr, key, buf);
                if (buf[0] != 'L' || buf[1] != 'L') throw new Exception("wrong key");
                output.Write(buf, 2, (int)len - 2);
                records++;
            }
        }
        output.Flush();
        return records;
    }
}
'@

if (-not ('LiveLogDecrypt' -as [type])) {
    Add-Type -TypeDefinition $src -Language CSharp
}

function Get-ConfKey([string]$confPath) {
    $key = $null
    foreach ($line in [System.IO.File]::ReadAllLines($confPath, [System.Text.Encoding]::UTF8)) {
        $t = $line.Trim()
        if ($t -eq '' -or $t.StartsWith('#') -or $t.StartsWith(';')) { continue }
        $i = $t.IndexOf('=')
        if ($i -lt 0 -or $t.Substring(0, $i).Trim() -ne 'key') { continue }
        $v = $t.Substring($i + 1).Trim()
        if ($v.Length -ge 2 -and $v.StartsWith('"') -and $v.EndsWith('"')) { $v = $v.Substring(1, $v.Length - 2) }
        $key = $v
    }
    return $key
}

function Test-Encrypted([string]$path) {
    $fs = [System.IO.File]::Open($path, 'Open', 'Read', 'ReadWrite')
    try {
        $m = New-Object byte[] 4
        $n = $fs.Read($m, 0, 4)
        return ($n -eq 4 -and $m[0] -eq 76 -and $m[1] -eq 76 -and $m[2] -eq 80 -and $m[3] -eq 49)   # "LLP1"
    } finally { $fs.Close() }
}

# No arguments: decrypt every still encrypted *.log file in this folder.
function Convert-Folder {
    $dir = $PSScriptRoot
    $conf = Join-Path $dir 'livelog.conf'
    $k = $null
    if (Test-Path -LiteralPath $conf) { $k = Get-ConfKey $conf }
    if (-not $k) {
        [Console]::Error.WriteLine("No arguments, and no key found in $conf")
        [Console]::Error.WriteLine("usage: livelog-decrypt.cmd <file> <key> [output-file]")
        [Console]::Error.WriteLine("   or: livelog-decrypt.cmd   (with livelog.conf next to this script)")
        return 1
    }
    $keyBytes = [System.Text.Encoding]::UTF8.GetBytes($k)
    $done = 0; $skipped = 0; $failed = 0
    foreach ($f in Get-ChildItem -LiteralPath $dir -Filter '*.log' -File) {
        $path = $f.FullName
        if (-not (Test-Encrypted $path)) { $skipped++; continue }
        $bak = "$path.bak"
        $tmp = "$path.tmp"
        if (Test-Path -LiteralPath $bak) {
            [Console]::Error.WriteLine("${path}: skipped, $bak already exists")
            $failed++; continue
        }
        try {
            $out = [System.IO.File]::Create($tmp)
            try { $n = [LiveLogDecrypt]::Run($path, $keyBytes, $out) } finally { $out.Close() }
            [System.IO.File]::Move($path, $bak)
            [System.IO.File]::Move($tmp, $path)
            Write-Host "${path}: decrypted, $n lines (original kept as $bak)"
            $done++
        } catch {
            $msg = $_.Exception.Message
            if ($_.Exception.InnerException) { $msg = $_.Exception.InnerException.Message }
            if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Force }
            [Console]::Error.WriteLine("${path}: NOT decrypted (file is unchanged): $msg")
            $failed++
        }
    }
    Write-Host "${dir}: $done decrypted, $skipped already decrypted, $failed failed"
    if ($failed) { return 1 }
    return 0
}

try {
    if (-not $File) { exit (Convert-Folder) }
    if (-not $Key) { throw "usage: livelog-decrypt.cmd <file> <key> [output-file]" }
    $inPath = (Resolve-Path -LiteralPath $File).Path
    $keyBytes = [System.Text.Encoding]::UTF8.GetBytes($Key)
    if ($OutFile) {
        $outPath = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutFile)
        $out = [System.IO.File]::Create($outPath)
        try {
            $n = [LiveLogDecrypt]::Run($inPath, $keyBytes, $out)
        } finally {
            $out.Close()
        }
        Write-Host "$n lines written to $outPath"
    } else {
        $out = [Console]::OpenStandardOutput()
        [void][LiveLogDecrypt]::Run($inPath, $keyBytes, $out)
    }
} catch {
    $msg = $_.Exception.Message
    if ($_.Exception.InnerException) { $msg = $_.Exception.InnerException.Message }
    [Console]::Error.WriteLine("error: $msg")
    exit 1
}
