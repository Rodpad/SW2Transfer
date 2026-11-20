#!/usr/bin/env python3
"""sw2xfer: multi-connection uploads and downloads for SW2Transfer on the Switch 2.

Up to 4 files move at once, one connection each; connections with no file of
their own help a big file with its next chunks (HTTP ranged GETs / PUTs at
offsets). So many files go four at a time, and a file on its own is split
across all 8 connections.

  sw2xfer.py upload /SourceFileOrFolder /DestinationPath     to the Switch
  sw2xfer.py download /SourceFileOrFolder /DestinationPath   from the Switch
      (several sources are fine; the last path is the destination; folders copy whole)
  sw2xfer.py ls /path                              list a folder
  sw2xfer.py mkdir /path/new   |  rm /path [-r]  |  mv /a /b
  sw2xfer.py cancel                                stop every transfer and copy (any device)
  sw2xfer.py logout                                forget the saved password sign-in

If the Switch has a password, you are asked for it once; the sign-in is kept
for 12 hours (in ~/.config/sw2xfer, or %APPDATA%\\sw2xfer on Windows). For
scripts, set SW2XFER_PASSWORD instead.

If a file is already at the destination you are asked once, when its turn
comes: Overwrite all, Skip all or Cancel transfer (which removes the files it
had started). For scripts: --overwrite-existing or --skip-existing.
  sw2xfer.py bench [/remote/file] [--size 512M]    upload + download speed test (file deleted after)

Options: --host <switch-ip> (shown on the Switch screen; or set SW2XFER_HOST), --port 6967,
-n/--connections 8, --chunk 2M (fastest in tests: 8 connections, 2 MB chunks).
"""
import argparse
import collections
import getpass
import http.client
import json
import os
import queue
import sys
import threading
import time
import urllib.parse
import uuid

DEFAULT_HOST = os.environ.get("SW2XFER_HOST")  # the Switch's address; shown on its screen


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


def token_path():
    base = os.environ.get("APPDATA") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "sw2xfer", "tokens.json")


def load_tokens():
    try:
        with open(token_path()) as f:
            t = json.load(f)
        now = time.time()
        return {k: v for k, v in t.items() if v.get("expires", 0) > now + 60}  # drop expired ones
    except (OSError, ValueError):
        return {}


def save_tokens(t):
    path = token_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fd = os.open(path + ".tmp", os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)  # readable by you only
    with os.fdopen(fd, "w") as f:
        json.dump(t, f)
    os.replace(path + ".tmp", path)


class Client:
    def __init__(self, host, port, timeout=60):
        self.host, self.port, self.timeout = host, port, timeout
        self.local = threading.local()
        self.key = f"{host}:{port}"
        self.token = load_tokens().get(self.key, {}).get("token")
        self.auth_lock = threading.Lock()

    def login(self, seen):
        """Ask for the password (once, even with many connections waiting) and keep the sign-in."""
        with self.auth_lock:
            if self.token != seen:
                return  # another connection signed in meanwhile
            env = os.environ.get("SW2XFER_PASSWORD")
            for attempt in range(3):
                try:
                    pw = env if env is not None else getpass.getpass("Enter your password to connect to SW2Transfer: ")
                except EOFError:
                    raise RuntimeError("this Switch needs a password: run in a terminal, or set SW2XFER_PASSWORD")
                c = http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)
                try:
                    c.request("POST", "/api/login", headers={"X-SW2-Password": urllib.parse.quote(pw, safe="")})
                    r = c.getresponse()
                    j = json.loads(r.read() or b"{}")
                except (OSError, http.client.HTTPException, ValueError) as e:
                    raise RuntimeError(f"login: {e}")
                finally:
                    c.close()
                if r.status == 200 and j.get("ok"):
                    self.token = j.get("token") or None
                    t = load_tokens()
                    if self.token:
                        t[self.key] = {"token": self.token, "expires": j.get("expires", time.time() + 12 * 3600)}
                        save_tokens(t)
                    return
                if r.status == 429:
                    raise RuntimeError(j.get("error", "too many wrong passwords"))
                if env is not None:
                    raise RuntimeError("wrong password in SW2XFER_PASSWORD")
                print("Wrong password.", file=sys.stderr)
            raise RuntimeError("wrong password")

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
        logins = 0
        attempt = 0
        while attempt < tries:
            attempt += 1
            try:
                c = self.conn()
                h = dict(headers or {})
                seen = self.token
                if seen:
                    h["Authorization"] = "Bearer " + seen
                c.request(method, path, body=body, headers=h)
                r = c.getresponse()
                data = r.read()
                if r.status == 401 and logins < 3:
                    self.drop()
                    self.login(seen)
                    logins += 1
                    attempt -= 1  # signing in is not a failed try
                    continue
                if r.status == 410:
                    self.drop()
                    raise Cancelled("cancelled")
                if r.getheader("Connection", "").lower() == "close":
                    self.drop()
                return r, data
            except (OSError, http.client.HTTPException) as e:
                last = e
                self.drop()
                time.sleep(0.3 * attempt)
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


# How many files are in flight at once. From tests on the Switch 2: four files
# on one connection each beat splitting every file across 8 connections at any
# size; splitting only wins for a file on its own (or the last ones of a queue).
FILES_AT_ONCE = 4


class _File:
    def __init__(self, src, dst, size):
        self.src, self.dst, self.size = src, dst, size
        self.xid = uuid.uuid4().hex  # tags this file's requests so Cancel all can stop them
        self.fd = None
        self.ready = False      # set up (created / opened) and ready for chunks
        self.next_off = 0       # next chunk to hand out
        self.left = size        # bytes not yet moved
        self.busy = 0           # requests in flight
        self.started = False    # past the duplicate check: something may have been written


def transfer_batch(cl, jobs, n, chunk, upload, on_exists=None):
    """Moves jobs [(src, dst, size)] with n connections: up to FILES_AT_ONCE
    files in flight, each started by one connection; a connection with no file
    to start helps one in flight with its next chunk. So a queue of files goes
    four at a time, and a file on its own gets all n connections.

    A file already at the destination is found when its turn comes (not for
    the whole queue up front); on_exists "overwrite" or "skip" decides, else
    the user is asked once: Overwrite all, Skip all or Cancel transfer."""
    if not jobs:
        return 0
    total = sum(size for _, _, size in jobs)
    label = os.path.basename(jobs[0][0].rstrip("/")) if len(jobs) == 1 else f"{len(jobs)} files"
    prog = Progress(label, total)
    policy = [on_exists]
    ask_lock = threading.Lock()
    skipped = []

    def exists(f):
        if not upload:
            return os.path.exists(f.dst)
        r, _ = cl.request("HEAD", f"/dl{q(f.dst)}?x={f.xid}")
        return r.status == 200

    def decide(f):  # "overwrite" or "skip"; raises Cancelled for Cancel transfer
        with ask_lock:
            if policy[0] == "cancel" or errors:  # someone already cancelled (or it failed)
                raise Cancelled("transfer cancelled")
            if policy[0]:
                return policy[0]
            where = "on the Switch" if upload else "here"
            if not sys.stdin.isatty():
                raise RuntimeError(f"{f.dst} already exists {where}: add --overwrite-existing or --skip-existing")
            with prog.lock:  # keeps the progress line out of the question
                sys.stderr.write("\n")
                while True:
                    try:
                        a = input(f"{f.dst} already exists {where}. Overwrite all, Skip all or Cancel transfer? [o/s/c] ").strip().lower()
                    except EOFError:
                        a = "c"
                    if a[:1] in ("o", "s", "c"):
                        break
            if a[:1] == "c":
                policy[0] = "cancel"
                raise Cancelled("transfer cancelled")
            policy[0] = "overwrite" if a[:1] == "o" else "skip"
            return policy[0]
    pending = collections.deque(_File(*j) for j in jobs)
    active = []
    cv = threading.Condition()
    errors = []
    at_once = max(1, min(FILES_AT_ONCE, n))

    def whole(f):  # files up to one chunk: one request
        if upload:
            with open(f.src, "rb") as fh:
                data = fh.read()
            # no offset: the Switch creates (or replaces) the file in this one request
            r, body = cl.request("PUT", f"/ul{q(f.dst)}?total={len(data)}&x={f.xid}", body=data,
                                 headers={"Content-Length": str(len(data)), "Content-Type": "application/octet-stream"})
            if r.status != 200:
                raise RuntimeError(f"PUT {f.dst}: HTTP {r.status} {body[:200]!r}")
        else:
            r, data = cl.request("GET", f"/dl{q(f.src)}?x={f.xid}")
            if r.status != 200 or len(data) != f.size:
                raise RuntimeError(f"GET {f.src}: HTTP {r.status}, {len(data)} bytes")
            os.makedirs(os.path.dirname(os.path.abspath(f.dst)), exist_ok=True)
            with open(f.dst, "wb") as fh:  # written only once complete, so no half files
                fh.write(data)
        prog.add(f.size)

    def setup(f):  # bigger files: create / open, then chunks
        if upload:
            cl.api("POST", "/api/create", path=f.dst, size=f.size, x=f.xid)
            f.fd = os.open(f.src, os.O_RDONLY | BINARY)
        else:
            os.makedirs(os.path.dirname(os.path.abspath(f.dst)), exist_ok=True)
            f.fd = os.open(f.dst, os.O_RDWR | os.O_CREAT | os.O_TRUNC | BINARY, 0o644)
            os.ftruncate(f.fd, f.size)

    def move_chunk(f, off):
        ln = min(chunk, f.size - off)
        if upload:
            data = read_at(f.src, f.fd, ln, off)
            r, body = cl.request("PUT", f"/ul{q(f.dst)}?offset={off}&total={f.size}&x={f.xid}", body=data,
                                 headers={"Content-Length": str(len(data)), "Content-Type": "application/octet-stream"})
            if r.status != 200:
                raise RuntimeError(f"PUT {f.dst}@{off}: HTTP {r.status} {body[:200]!r}")
        else:
            r, data = cl.request("GET", f"/dl{q(f.src)}?x={f.xid}", headers={"Range": f"bytes={off}-{off + ln - 1}"})
            if r.status not in (200, 206) or len(data) != ln:
                raise RuntimeError(f"GET {f.src}@{off}: HTTP {r.status}, {len(data)} bytes")
            write_at(f.dst, f.fd, data, off)
        prog.add(ln)

    def close(f):
        if f.fd is not None:
            os.close(f.fd)
            f.fd = None

    def next_task():
        with cv:
            while not errors:
                if pending and len(active) < at_once:
                    f = pending.popleft()
                    active.append(f)
                    return ("start", f, 0)
                helpable = [f for f in active if f.ready and f.next_off < f.size]
                if helpable:
                    f = max(helpable, key=lambda f: f.size - f.next_off)  # the one with most left
                    off = f.next_off
                    f.next_off += chunk
                    f.busy += 1
                    return ("chunk", f, off)
                if not pending and not active:
                    return None
                cv.wait(0.5)
            return None

    def finish(f):  # with cv held
        close(f)
        active.remove(f)
        cv.notify_all()

    def worker():
        while True:
            t = next_task()
            if t is None:
                return
            kind, f, off = t
            try:
                if kind == "start":
                    if exists(f) and decide(f) == "skip":
                        skipped.append(f)
                        with prog.lock:
                            prog.total -= f.size
                        with cv:
                            active.remove(f)
                            cv.notify_all()
                        continue
                    f.started = True
                    if f.size <= chunk:
                        whole(f)
                        with cv:
                            f.left = 0
                            finish(f)
                    else:
                        setup(f)
                        with cv:
                            f.ready = True
                            cv.notify_all()
                else:
                    move_chunk(f, off)
                    with cv:
                        f.busy -= 1
                        f.left -= min(chunk, f.size - off)
                        if f.left <= 0:
                            finish(f)
            except Exception as e:  # noqa: BLE001
                with cv:
                    errors.append(e)
                    cv.notify_all()
                return

    ts = [threading.Thread(target=worker, daemon=True) for _ in range(max(1, n))]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    if errors:
        for f in list(active):
            close(f)
            if not f.started or f.left <= 0:
                continue
            try:  # do not leave a half-moved file behind
                if upload:
                    time.sleep(0.5)  # let the Switch notice the closed connection first
                    cl.api("POST", "/api/delete", path=f.dst)
                elif f.size > chunk and os.path.exists(f.dst):
                    os.unlink(f.dst)
            except (OSError, RuntimeError):
                pass
        raise errors[0]
    prog.show(final=True)
    if skipped:
        print(f"skipped {len(skipped)} file{'s' if len(skipped) > 1 else ''} already {'on the Switch' if upload else 'here'}", file=sys.stderr)
    return total - sum(f.size for f in skipped)


def remote_list(cl, path):
    return cl.api("GET", "/api/list", path=path)["entries"]


def remote_entry(cl, path):
    """The listing entry for path ({"name", "dir", "size", ...}), or None."""
    parent, name = os.path.split(path.rstrip("/")) if path.rstrip("/") else ("/", "")
    if not name:
        return {"name": "", "dir": True, "size": 0}
    try:
        for e in remote_list(cl, parent or "/"):
            if e["name"] == name:
                return e
    except RuntimeError:
        pass
    return None


def remote_kind(cl, path):
    e = remote_entry(cl, path)
    return None if e is None else ("dir" if e["dir"] else "file")


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
    jobs = []  # (local, remote, size)
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
                    jobs.append((os.path.join(root, f), rdir + "/" + f, os.path.getsize(os.path.join(root, f))))
        else:
            target = dest.rstrip("/") + "/" + os.path.basename(s) if into else dest
            jobs.append((s, target, os.path.getsize(s)))
    total += transfer_batch(cl, jobs, a.connections, a.chunk, upload=True, on_exists=a.on_exists)
    el = time.time() - t0
    print(f"uploaded {human(total)} in {el:.1f} s = {total / max(el, 1e-6) / (1 << 20):.2f} MB/s with {a.connections} connections")


def cmd_get(cl, a):
    *srcs, dest = a.paths if len(a.paths) > 1 else (a.paths + ["."])
    t0, total = time.time(), 0
    # Same rule as upload: several sources or an existing folder = "into".
    into = len(srcs) > 1 or os.path.isdir(dest) or dest.endswith(("/", os.sep))
    if into:
        os.makedirs(dest, exist_ok=True)

    jobs = []  # (remote, local, size)

    def get_tree(rpath, ldir):
        os.makedirs(ldir, exist_ok=True)
        for e in remote_list(cl, rpath):
            rp = rpath.rstrip("/") + "/" + e["name"]
            if e["dir"]:
                get_tree(rp, os.path.join(ldir, e["name"]))
            else:
                jobs.append((rp, os.path.join(ldir, e["name"]), e["size"]))

    for s in srcs:
        e = remote_entry(cl, s)
        if e is None:
            sys.exit(f"download: {s} not found on the Switch")
        if e["dir"]:
            get_tree(s, os.path.join(dest, os.path.basename(s.rstrip("/")) or "sd"))
        else:
            jobs.append((s, os.path.join(dest, os.path.basename(s)) if into else dest, e["size"]))
    total += transfer_batch(cl, jobs, a.connections, a.chunk, upload=False, on_exists=a.on_exists)
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
    p.add_argument("cmd", choices=["upload", "download", "ls", "mkdir", "rm", "mv", "cancel", "logout", "bench", "info", "put", "get"], metavar="command",
                   help="upload, download, ls, mkdir, rm, mv, cancel, logout, bench, info")
    p.add_argument("paths", nargs="*")
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--port", type=int, default=6967)
    p.add_argument("-n", "--connections", type=int, default=8)
    p.add_argument("--chunk", type=parse_size, default=2 << 20)
    p.add_argument("-r", "--recursive", action="store_true")
    p.add_argument("--overwrite-existing", dest="on_exists", action="store_const", const="overwrite",
                   help="replace files that already exist at the destination (no question)")
    p.add_argument("--skip-existing", dest="on_exists", action="store_const", const="skip",
                   help="leave files that already exist at the destination (no question)")
    p.add_argument("--size", type=parse_size, default=256 << 20, help="bench file size")
    p.add_argument("--levels", type=lambda s: [int(x) for x in s.split(",")], default=[1, 4, 8], help="bench connection counts")
    a = p.parse_args()
    if not a.host:
        p.error("give the Switch's address with --host <switch-ip> (it is shown on the Switch screen), or set SW2XFER_HOST")
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
        elif a.cmd == "logout":
            t = load_tokens()
            t.pop(cl.key, None)
            save_tokens(t)
            print(f"signed out of {cl.key}")
        elif a.cmd == "cancel":
            cl.api("POST", "/api/cancel")
            print("cancelled all transfers and copies")
        elif a.cmd == "bench":
            cmd_bench(cl, a)
        elif a.cmd == "info":
            info = cl.api("GET", "/api/info")
            print(json.dumps(info, indent=2))
            if info.get("background"):
                print("Served by the SW2Transfer background plugin (runs from startup, without the app open).")
    except Cancelled:
        sys.exit("\ncancelled (unfinished files removed)")
    except RuntimeError as e:
        sys.exit(f"error: {e}")
    except KeyboardInterrupt:
        sys.exit("\ninterrupted")


if __name__ == "__main__":
    main()
