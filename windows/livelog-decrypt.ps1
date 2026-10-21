# livelog-decrypt.ps1 - decrypt a livelog file on Windows.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File livelog-decrypt.ps1 <file> <key> [output-file]
#   or simply:  livelog-decrypt.cmd <file> <key> [output-file]
#
# Without output-file the plain text is shown in the console.
# No install is needed. It works with Windows PowerShell 5.1 and PowerShell 7.

param(
    [Parameter(Mandatory = $true, Position = 0)][string]$File,
    [Parameter(Mandatory = $true, Position = 1)][string]$Key,
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

try {
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
