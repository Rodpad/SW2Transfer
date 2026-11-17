#!/usr/bin/env python3
"""sw2xfer: multi-connection uploads and downloads for SW2Transfer on the Switch 2.

Each file is split into chunks that N connections fetch or send in parallel
(HTTP ranged GETs / PUTs at offsets), which is much faster than a
single connection.

  sw2xfer.py upload /SourceFileOrFolder /DestinationPath     to the Switch
  sw2xfer.py download /SourceFileOrFolder /DestinationPath   from the Switch
      (several sources are fine; the last path is the destination; folders copy whole)
  sw2xfer.py ls /path                              list a folder
  sw2xfer.py mkdir /path/new   |  rm /path [-r]  |  mv /a /b
  sw2xfer.py cancel                                stop every transfer and copy (any device)
  sw2xfer.py bench [/remote/file] [--size 512M]    upload + download speed test (file deleted after)

Options: --host (or SW2XFER_HOST, default 192.168.1.168), --port 6967,
-n/--connections 8, --chunk 2M (fastest in tests: 8 connections, 2 MB chunks).
"""
import argparse
import http.client
import json
import os
import queue
import sys
import threading
import time
import urllib.parse
import uuid

DEFAULT_HOST = os.environ.get("SW2XFER_HOST", "192.168.1.168")


def parse_size(s):
    s = s.strip().upper()
    mult = 1
    for suf, m in (("K", 1 << 10), ("M", 1 << 20), ("G", 1 << 30)):
        if s.endswith(suf) or s.endswith(suf + "B"):
            s = s.rstrip("B")[:-1]
            mult = m
            break
    return int(float(s) * mult)


def human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if abs(n) < 1024 or unit == "GB":
            return f"{n:.1f} {unit}" if unit != "B" else f"{n} B"
        n /= 1024


class Cancelled(RuntimeError):
    """The transfer was cancelled (Cancel all on the Switch, the web page or `cancel`)."""


class Client:
    def __init__(self, host, port, timeout=60):
        self.host, self.port, self.timeout = host, port, timeout
        self.local = threading.local()

    def conn(self):
        c = getattr(self.local, "c", None)
        if c is None:
            c = http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)
            self.local.c = c
        return c

    def drop(self):
        c = getattr(self.local, "c", None)
        if c:
            c.close()
        self.local.c = None

    def request(self, method, path, body=None, headers=None, tries=3):
        last = None
        for attempt in range(tries):
            try:
                c = self.conn()
                c.request(method, path, body=body, headers=headers or {})
                r = c.getresponse()
                data = r.read()
                if r.status == 410:
                    self.drop()
                    raise Cancelled("cancelled")
                if r.getheader("Connection", "").lower() == "close":
                    self.drop()
                return r, data
            except (OSError, http.client.HTTPException) as e:
                last = e
                self.drop()
                time.sleep(0.3 * (attempt + 1))
        raise RuntimeError(f"{method} {path}: {last}")

    def api(self, method, endpoint, **params):
        r, data = self.request(method, endpoint + "?" + urllib.parse.urlencode(params))
        try:
            j = json.loads(data or b"{}")
        except ValueError:
            j = {"ok": False, "error": data[:200].decode(errors="replace")}
        if r.status >= 300 or j.get("ok") is False:
            raise RuntimeError(f"{endpoint} {params}: {j.get('error', r.status)}")
        return j


def q(path):
    return urllib.parse.quote(path)


class Progress:
    def __init__(self, label, total):
        self.label, self.total, self.done = label, total, 0
        self.t0 = self.last = time.time()
        self.lock = threading.Lock()

    def add(self, n):
        with self.lock:
            self.done += n
            now = time.time()
            if now - self.last > 0.25 or self.done >= self.total:
                self.last = now
                self.show()

    def show(self, final=False):
        el = max(time.time() - self.t0, 1e-6)
        pct = 100.0 * self.done / self.total if self.total else 100.0
        sp = self.done / el / (1 << 20)
        end = "\n" if final else "\r"
        sys.stderr.write(f"\r  {self.label[:48]:<48} {pct:5.1f}%  {human(self.done):>10}  {sp:7.2f} MB/s ")
        sys.stderr.write(end)
        sys.stderr.flush()


def run_parallel(n, jobs, fn):
    """Runs fn(job) over jobs with n worker threads; re-raises the first error."""
    qq = queue.Queue()
    for j in jobs:
        qq.put(j)
    errors = []

    def worker():
        while not errors:
            try:
                j = qq.get_nowait()
            except queue.Empty:
                return
            try:
                fn(j)
            except Exception as e:  # noqa: BLE001
                errors.append(e)
                return

    ts = [threading.Thread(target=worker, daemon=True) for _ in range(min(n, max(1, len(jobs))))]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    if errors:
        raise errors[0]


BINARY = getattr(os, "O_BINARY", 0)  # Windows: no newline translation


def read_at(path, fd, n, off):
    """Positional read; Windows has no os.pread, so open the file per chunk there."""
    if hasattr(os, "pread"):
        return os.pread(fd, n, off)
    with open(path, "rb") as f:
        f.seek(off)
        return f.read(n)


def write_at(path, fd, data, off):
    """Positional write; Windows has no os.pwrite, so open the file per chunk there."""
    if hasattr(os, "pwrite"):
        os.pwrite(fd, data, off)
        return
    with open(path, "r+b") as f:
        f.seek(off)
        f.write(data)


def upload_file(cl, local, remote, n, chunk):
    size = os.path.getsize(local)
    xid = uuid.uuid4().hex  # tags this file's requests so Cancel all can stop them
    cl.api("POST", "/api/create", path=remote, size=size, x=xid)
    prog = Progress(os.path.basename(local), size)
    fd = os.open(local, os.O_RDONLY | BINARY)
    try:
        def put(off):
            ln = min(chunk, size - off)
            data = read_at(local, fd, ln, off)
            r, body = cl.request("PUT", f"/ul{q(remote)}?offset={off}&total={size}&x={xid}", body=data,
                                 headers={"Content-Length": str(len(data)), "Content-Type": "application/octet-stream"})
            if r.status != 200:
                raise RuntimeError(f"PUT {remote}@{off}: HTTP {r.status} {body[:200]!r}")
            prog.add(ln)
        offsets = list(range(0, size, chunk)) or []
        run_parallel(n, offsets, put)
    finally:
        os.close(fd)
    prog.show(final=True)
    return size


def download_file(cl, remote, local, n, chunk):
    xid = uuid.uuid4().hex
    r, _ = cl.request("HEAD", f"/dl{q(remote)}?x={xid}")
    if r.status != 200:
        raise RuntimeError(f"{remote}: HTTP {r.status}")
    size = int(r.getheader("Content-Length", "0"))
    os.makedirs(os.path.dirname(os.path.abspath(local)), exist_ok=True)
    fd = os.open(local, os.O_RDWR | os.O_CREAT | os.O_TRUNC | BINARY, 0o644)
    os.ftruncate(fd, size)
    prog = Progress(os.path.basename(remote), size)
    try:
        def get(off):
            end = min(off + chunk, size) - 1
            r, data = cl.request("GET", f"/dl{q(remote)}?x={xid}", headers={"Range": f"bytes={off}-{end}"})
            if r.status not in (200, 206) or len(data) != end - off + 1:
                raise RuntimeError(f"GET {remote}@{off}: HTTP {r.status}, {len(data)} bytes")
            write_at(local, fd, data, off)
            prog.add(len(data))
        run_parallel(n, list(range(0, size, chunk)), get)
    except BaseException:
        os.close(fd)
        fd = None
        os.unlink(local)  # do not leave a half-downloaded file behind
        raise
    finally:
        if fd is not None:
            os.close(fd)
    prog.show(final=True)
    return size


def remote_list(cl, path):
    return cl.api("GET", "/api/list", path=path)["entries"]


def remote_kind(cl, path):
    parent, name = os.path.split(path.rstrip("/")) if path.rstrip("/") else ("/", "")
    if not name:
        return "dir"
    try:
        for e in remote_list(cl, parent or "/"):
            if e["name"] == name:
                return "dir" if e["dir"] else "file"
    except RuntimeError:
        pass
    return None


def cmd_put(cl, a):
    *srcs, dest = a.paths
    if not srcs:
        sys.exit("upload: give local files or folders, then the destination folder on the Switch")
    t0, total = time.time(), 0
    # Several sources, a folder source or a trailing "/" mean "into this
    # folder" (created if needed); one file to a new name means "save as".
    into = len(srcs) > 1 or dest.endswith("/") or any(os.path.isdir(x) for x in srcs) or remote_kind(cl, dest) == "dir"
    if into:
        cl.api("POST", "/api/mkdir", path=dest.rstrip("/") or "/")
    for s in srcs:
        s = s.rstrip("/\\") or s
        if os.path.isdir(s):
            base = os.path.basename(os.path.abspath(s))
            for root, _dirs, files in os.walk(s):
                rel = os.path.relpath(root, s).replace(os.sep, "/")  # Switch paths use "/"
                rdir = "/".join(p for p in (dest.rstrip("/"), base, "" if rel == "." else rel) if p)
                cl.api("POST", "/api/mkdir", path=rdir or "/")
                for f in sorted(files):
                    if f.startswith("._") or f == ".DS_Store":
                        continue
                    total += upload_file(cl, os.path.join(root, f), rdir + "/" + f, a.connections, a.chunk)
        else:
            target = dest.rstrip("/") + "/" + os.path.basename(s) if into else dest
            total += upload_file(cl, s, target, a.connections, a.chunk)
    el = time.time() - t0
    print(f"uploaded {human(total)} in {el:.1f} s = {total / max(el, 1e-6) / (1 << 20):.2f} MB/s with {a.connections} connections")


def cmd_get(cl, a):
    *srcs, dest = a.paths if len(a.paths) > 1 else (a.paths + ["."])
    t0, total = time.time(), 0
    # Same rule as upload: several sources or an existing folder = "into".
    into = len(srcs) > 1 or os.path.isdir(dest) or dest.endswith(("/", os.sep))
    if into:
        os.makedirs(dest, exist_ok=True)

    def get_tree(rpath, ldir):
        nonlocal total
        os.makedirs(ldir, exist_ok=True)
        for e in remote_list(cl, rpath):
            rp = rpath.rstrip("/") + "/" + e["name"]
            if e["dir"]:
                get_tree(rp, os.path.join(ldir, e["name"]))
            else:
                total += download_file(cl, rp, os.path.join(ldir, e["name"]), a.connections, a.chunk)

    for s in srcs:
        kind = remote_kind(cl, s)
        if kind == "dir":
            get_tree(s, os.path.join(dest, os.path.basename(s.rstrip("/")) or "sd"))
        elif kind == "file":
            target = os.path.join(dest, os.path.basename(s)) if into else dest
            total += download_file(cl, s, target, a.connections, a.chunk)
        else:
            sys.exit(f"download: {s} not found on the Switch")
    el = time.time() - t0
    print(f"downloaded {human(total)} in {el:.1f} s = {total / max(el, 1e-6) / (1 << 20):.2f} MB/s with {a.connections} connections")


def cmd_ls(cl, a):
    path = a.paths[0] if a.paths else "/"
    for e in sorted(remote_list(cl, path), key=lambda e: (not e["dir"], e["name"].lower())):
        print(f"{'<dir>':>12}  {e['name']}/" if e["dir"] else f"{e['size']:>12}  {e['name']}")


def cmd_bench(cl, a):
    import tempfile
    size = a.size
    remote = a.paths[0] if a.paths else "/sw2transfer-bench.bin"
    if remote_kind(cl, remote) is not None:
        sys.exit(f"bench: {remote} already exists; pick a new file name")
    with tempfile.TemporaryDirectory() as td:
        src, back = os.path.join(td, "up.bin"), os.path.join(td, "down.bin")
        with open(src, "wb") as f:
            left = size
            while left:
                n = min(left, 8 << 20)
                f.write(os.urandom(n))
                left -= n
        for n in a.levels:
            t0 = time.time()
            upload_file(cl, src, remote, n, a.chunk)
            up = size / (time.time() - t0) / (1 << 20)
            t0 = time.time()
            download_file(cl, remote, back, n, a.chunk)
            down = size / (time.time() - t0) / (1 << 20)
            same = open(src, "rb").read() == open(back, "rb").read()
            print(f"{n:2d} connections: upload {up:7.2f} MB/s, download {down:7.2f} MB/s, {'verified' if same else 'MISMATCH'}")
    cl.api("POST", "/api/delete", path=remote)


def main():
    p = argparse.ArgumentParser(description="Multi-connection transfers to SW2Transfer on the Switch 2.",
                                formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    p.add_argument("cmd", choices=["upload", "download", "ls", "mkdir", "rm", "mv", "cancel", "bench", "info", "put", "get"], metavar="command",
                   help="upload, download, ls, mkdir, rm, mv, cancel, bench, info")
    p.add_argument("paths", nargs="*")
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--port", type=int, default=6967)
    p.add_argument("-n", "--connections", type=int, default=8)
    p.add_argument("--chunk", type=parse_size, default=2 << 20)
    p.add_argument("-r", "--recursive", action="store_true")
    p.add_argument("--size", type=parse_size, default=256 << 20, help="bench file size")
    p.add_argument("--levels", type=lambda s: [int(x) for x in s.split(",")], default=[1, 4, 8], help="bench connection counts")
    a = p.parse_args()
    cl = Client(a.host, a.port)
    try:
        if a.cmd in ("upload", "put"):  # put/get: older names
            cmd_put(cl, a)
        elif a.cmd in ("download", "get"):
            cmd_get(cl, a)
        elif a.cmd == "ls":
            cmd_ls(cl, a)
        elif a.cmd == "mkdir":
            for x in a.paths:
                cl.api("POST", "/api/mkdir", path=x)
                print(f"made {x}")
        elif a.cmd == "rm":
            for x in a.paths:
                cl.api("POST", "/api/delete", path=x, recursive="1" if a.recursive else "0")
                print(f"deleted {x}")
        elif a.cmd == "mv":
            cl.api("POST", "/api/rename", **{"from": a.paths[0], "to": a.paths[1]})
            print(f"moved {a.paths[0]} -> {a.paths[1]}")
        elif a.cmd == "cancel":
            cl.api("POST", "/api/cancel")
            print("cancelled all transfers and copies")
        elif a.cmd == "bench":
            cmd_bench(cl, a)
        elif a.cmd == "info":
            print(json.dumps(cl.api("GET", "/api/info"), indent=2))
    except Cancelled:
        sys.exit("\ncancelled")
    except RuntimeError as e:
        sys.exit(f"error: {e}")
    except KeyboardInterrupt:
        sys.exit("\ninterrupted")


if __name__ == "__main__":
    main()
