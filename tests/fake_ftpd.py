#!/usr/bin/env python3
"""Tiny FTP server for the livelog tests (not for real use).

usage: fake_ftpd.py ROOT PORTFILE [--fail-stor N] [--no-epsv]
  ROOT         folder that is the FTP root
  PORTFILE     the port number is written here when the server is ready
  --fail-stor  the first N STOR commands get a 550 error
  --no-epsv    EPSV is refused (the client must use PASV)
Login: user "u", password "p".
"""
import os
import socket
import sys
import threading

root = os.path.abspath(sys.argv[1])
fail_stor = int(sys.argv[sys.argv.index("--fail-stor") + 1]) if "--fail-stor" in sys.argv else 0
no_epsv = "--no-epsv" in sys.argv
lock = threading.Lock()


def serve(conn):
    f = conn.makefile("rwb", buffering=0)
    cwd = root
    data_srv = None
    authed = False
    global fail_stor

    def say(text):
        f.write((text + "\r\n").encode())

    say("220 fake ftpd")
    while True:
        line = f.readline()
        if not line:
            break
        cmd, _, arg = line.decode().rstrip("\r\n").partition(" ")
        cmd = cmd.upper()
        if cmd == "USER":
            say("331 password please")
        elif cmd == "PASS":
            authed = True
            say("230 ok")
        elif not authed:
            say("530 login first")
        elif cmd == "TYPE":
            say("200 type set")
        elif cmd == "CWD":
            target = root if arg == "/" else os.path.join(cwd, arg)
            if os.path.isdir(target):
                cwd = os.path.abspath(target)
                say("250 ok")
            else:
                say("550 no such folder")
        elif cmd == "MKD":
            os.makedirs(os.path.join(cwd, arg), exist_ok=True)
            say("257 created")
        elif cmd in ("EPSV", "PASV"):
            if cmd == "EPSV" and no_epsv:
                say("502 not implemented")
                continue
            data_srv = socket.socket()
            data_srv.bind(("127.0.0.1", 0))
            data_srv.listen(1)
            port = data_srv.getsockname()[1]
            if cmd == "EPSV":
                say("229 Entering Extended Passive Mode (|||%d|)" % port)
            else:
                say("227 Entering Passive Mode (10,9,8,7,%d,%d)" % (port // 256, port % 256))
        elif cmd == "SIZE":
            p = os.path.join(cwd, arg)
            if os.path.isfile(p):
                say("213 %d" % os.path.getsize(p))
            else:
                say("550 no such file")
        elif cmd == "STOR":
            with lock:
                fail = fail_stor > 0
                if fail:
                    fail_stor -= 1
            if fail or data_srv is None:
                say("550 upload refused")
                continue
            say("150 send it")
            d, _ = data_srv.accept()
            data_srv.close()
            data_srv = None
            with open(os.path.join(cwd, arg), "wb") as out:
                while True:
                    chunk = d.recv(65536)
                    if not chunk:
                        break
                    out.write(chunk)
            d.close()
            say("226 done")
        elif cmd == "QUIT":
            say("221 bye")
            break
        else:
            say("502 not implemented")
    conn.close()


srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 0))
srv.listen(5)
with open(sys.argv[2], "w") as pf:
    pf.write(str(srv.getsockname()[1]))
while True:
    c, _ = srv.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()
