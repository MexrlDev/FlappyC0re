#!/usr/bin/env python3
"""Flappy Bird PS5 launcher (mirrors the Doom-PS pattern)."""
import argparse, datetime, os, platform, socket, sys, threading, time

DEFAULT_PS5_IP    = ""                              # fill in
DEFAULT_LAUNCHER  = "flappy.lua"
DEFAULT_SHELLCODE = "flappy.bin"
PAYLOAD_PORT      = 9026
LOG_PORT          = 9027
SC_PORT_LO        = 5001
SC_PORT_HI        = 5021
CHUNK             = 64 * 1024

IS_WINDOWS = os.name == "nt"
OS_NAME    = platform.system() or "Unknown"


def find_file(name, subdirs=("payloads", "lua", ".")):
    if os.path.isfile(name): return name
    for s in subdirs:
        c = os.path.join(s, os.path.basename(name))
        if os.path.isfile(c): return c
    return None


def get_local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        try: return socket.gethostbyname(socket.gethostname())
        except OSError: return "127.0.0.1"
    finally: s.close()


class LogServer(threading.Thread):
    def __init__(self, port):
        super().__init__(daemon=True)
        self.port = port
        self._stop_event = threading.Event()
        self.sock = None
    def run(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try: s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        except OSError: pass
        try: s.bind(("0.0.0.0", self.port))
        except OSError as e:
            print(f"[log] cannot bind UDP {self.port}: {e}")
            return
        s.settimeout(0.5)
        self.sock = s
        print(f"[log] UDP listening on 0.0.0.0:{self.port}", flush=True)
        while not self._stop_event.is_set():
            try: data, addr = s.recvfrom(65535)
            except socket.timeout: continue
            except OSError: break
            ts = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
            print(f"[{ts}] {addr[0]}  "
                  f"{data.decode('utf-8','replace').rstrip()}", flush=True)
        s.close()
    def stop(self):
        self._stop_event.set()
        if self.sock:
            try: self.sock.close()
            except OSError: pass


def send_lua(host, path, retries=5):
    if not os.path.isfile(path):
        print(f"[!] missing {path}"); return False
    with open(path, "rb") as f: data = f.read()
    print(f"[1] Sending {os.path.basename(path)} ({len(data):,} B)")
    for i in range(1, retries + 1):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(10)
        try:
            s.connect((host, PAYLOAD_PORT)); s.sendall(data); s.close()
            if i > 1: print(f"[1] Sent on attempt {i}")
            return True
        except Exception as e:
            try: s.close()
            except OSError: pass
            if i < retries:
                print(f"[1] attempt {i}/{retries} failed: {e}")
                time.sleep(1.0)
    return False


def _send_sc_once(host, port, data, size):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(0.5)
    try:
        s.connect((host, port))
    except OSError:
        s.close(); return False
    print(f"[sc] connected {host}:{port}, sending {size:,} bytes")
    s.settimeout(None)
    sent = 0; t0 = time.time()
    try:
        while sent < size:
            chunk = data[sent:sent+CHUNK]
            s.sendall(chunk)
            sent += len(chunk)
            pct = sent * 100 // size
            print(f"\r[sc] {sent:,}/{size:,} ({pct}%)", end="", flush=True)
        print()
        dt = max(time.time() - t0, 1e-6)
        print(f"[sc] done in {dt:.1f}s ({sent/dt/1024:.0f} KB/s)")
        s.close(); return True
    except OSError as e:
        print(f"\n[!] send broke at {sent:,}: {e}")
        try: s.close()
        except OSError: pass
        return False


def stream_shellcode(host, path, timeout=25, retries=3):
    if not os.path.isfile(path):
        print(f"[!] missing {path}"); return False
    with open(path, "rb") as f: data = f.read()
    size = len(data)
    for attempt in range(1, retries + 1):
        if attempt > 1:
            print(f"[sc] retry {attempt}/{retries}...")
            time.sleep(1.5)
        print(f"[sc] scanning {SC_PORT_LO}..{SC_PORT_HI-1}")
        deadline = time.time() + timeout
        while time.time() < deadline:
            for port in range(SC_PORT_LO, SC_PORT_HI):
                if _send_sc_once(host, port, data, size):
                    return True
            time.sleep(0.3)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", nargs="?", default=DEFAULT_PS5_IP)
    ap.add_argument("--launcher",  "-l", default=DEFAULT_LAUNCHER)
    ap.add_argument("--shellcode", "-s", default=DEFAULT_SHELLCODE)
    ap.add_argument("--no-log",    action="store_true")
    ap.add_argument("--local-ip",  default=None)
    ap.add_argument("--scport",    type=int, default=None)
    a = ap.parse_args()

    if not a.host:
        print("Provide the console IP: flappy_launcher.py <PS5_IP>")
        return 1

    print("=" * 60)
    print(" Flappy Bird PS5 launcher")
    print(f" Host OS: {OS_NAME}")
    print("=" * 60)

    lua = find_file(a.launcher)
    sc  = find_file(a.shellcode)
    if not lua: print(f"[!] {a.launcher} not found"); return 1
    if not sc:  print(f"[!] {a.shellcode} not found"); return 1

    local_ip = a.local_ip or get_local_ip()
    print(f"[*] console: {a.host}")
    print(f"[*] PC IP:   {local_ip}  (edit flappy.lua PC_IP to match)")

    log = None
    if not a.no_log:
        log = LogServer(LOG_PORT); log.start(); time.sleep(0.2)

    if not send_lua(a.host, lua):
        if log: log.stop()
        return 1
    time.sleep(1.0)

    ok = False
    if a.scport is not None:
        with open(sc, "rb") as f: data = f.read()
        ok = _send_sc_once(a.host, a.scport, data, len(data))
    else:
        ok = stream_shellcode(a.host, sc)

    if not ok:
        print("[!] shellcode send failed")
        if log: log.stop()
        return 1

    if log:
        print("\n[*] logs — Ctrl-C to stop")
        try:
            while log.is_alive(): time.sleep(0.5)
        except KeyboardInterrupt: pass
        finally: log.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
