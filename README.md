# SW2Transfer

Multi-connection file transfer and file management server for the Switch 2
(hbmenu under kaiHEN).
One file can be read or written by many TCP connections at once, each moving a
different byte range, which is much faster than a single stream.

## Speed

| | Upload to Switch | Download from Switch |
|---|---|---|
| **SW2Transfer, 8 connections (default)** | **51 MB/s** | **43 MB/s** |
| SW2Transfer, 1 connection | 28 MB/s | 21 MB/s |
| FTP (kaiHEN ftpd) | 8.6 MB/s | 9.5 MB/s |

SW2Transfer is about 5 times faster than FTP. Before these tests, the most I had
ever seen over FTP was 8.8 MB/s. All measured with a Mac and the Switch 2 both
on Wi-Fi, moving a 256 MB file (every transfer checked byte for byte).

Many small files (100 typical emulator files: saves, configs, box art and ROMs,
21.6 MB in all):

| | Time | Speed |
|---|---|---|
| **SW2Transfer 1.4.3** | **0.9 s** | **23 MB/s** |
| SW2Transfer 1.4.0 | 3.0 s | 7.2 MB/s |
| FTP (kaiHEN ftpd) | 3.9 s | 5.5 MB/s |

Up to 4 files move at once, one connection each; connections with no file of
their own help a big file with its next chunks, so a file on its own still uses
all 8 connections. In tests, 4 files at once beat splitting each file across 8
connections at every size from 256 KB to 64 MB.

## Install

Copy `sw2transfer.nro` to `sdmc:/switch/sw2transfer/` and launch it
from hbmenu. The screen shows the console's address, live speed, and every
file in flight with its connection count. The console stays awake while the
app is open.

| Button | Action |
|---|---|
| − | Set, change or remove the password |
| X (twice) | Cancel all transfers and copies |
| Y | Help |
| + | Exit |

## Password protection

**Password protection is off by default.** Until you set a password, anyone
on your network can read, change or delete files on the SD card while the app
is open.

To turn it on, press − on the Switch and type a password on the on-screen
keyboard (twice, to confirm). To remove it, press − again and leave it blank.
With a password set:

- the web page asks for it and keeps you signed in on that browser for
  12 hours (Sign out ends it early);
- `sw2xfer.py` asks once and keeps the sign-in for 12 hours, in
  `~/.config/sw2xfer/` (`%APPDATA%\sw2xfer\` on Windows); `sw2xfer.py logout`
  forgets it, and `SW2XFER_PASSWORD` supplies it to scripts;
- changing or removing the password signs everyone out;
- after 5 wrong passwords in a row, sign-in is refused for 30 seconds.

The password is not stored, only a salted PBKDF2-SHA256 hash, in
`/switch/sw2transfer/settings.json`. Traffic is plain HTTP, so the password
crosses your network unencrypted: it keeps people on your network out, it is
not protection for an untrusted network.

## Transferring files

Open `http://<switch-ip>:6967` in a browser on the same network (the Switch
screen shows the address; Y on the Switch and the page's Help button explain
everything with examples).

- Uploads go into the folder you are in: Upload files, Upload folder, or drag
  files and folders onto the page (subfolders are kept).
- Each file has a Download button; clicking a file ticks it, and Download in
  the bar saves all ticked files. Downloads over 3.5 GB use one connection.
- Connections (default 8) and chunk size (default 2 MB) were the fastest
  combination in tests. Up to 4 files upload at once, one connection each
  (files up to one chunk in one request), so folders of many small files are
  quick too; a big file on its own gets every connection.
- A file that is already there is found when its turn comes (a big queue is
  not checked up front). You are asked once: Overwrite all, Skip all or Cancel
  transfer, which stops that upload and removes the files it had started. On
  the command line, `--overwrite-existing` or `--skip-existing` decide ahead of time.
- Cancel all (web page), X twice (Switch) or `sw2xfer.py cancel` stops every
  transfer and copy, from any device; unfinished uploads and copies are
  deleted from the SD card.

Command line (Python 3; download it from `http://<switch-ip>:6967/sw2xfer.py`):

```sh
python3 sw2xfer.py --host <switch-ip> upload /SourceFileOrFolder /DestinationPath
python3 sw2xfer.py --host <switch-ip> download /SourceFileOrFolder /DestinationPath
python3 sw2xfer.py --host <switch-ip> upload game.iso MyFolder /roms/ps2   # several sources, last path = destination
python3 sw2xfer.py --host <switch-ip> upload MyFolder /roms --overwrite-existing   # or --skip-existing
python3 sw2xfer.py --host <switch-ip> cancel
python3 sw2xfer.py --host <switch-ip> logout     # forget the saved password sign-in
python3 sw2xfer.py --host <switch-ip> bench /path/test.bin --levels 1,4,8
```

## File management

On the web page:

- Click folders to open them; the path at the top jumps back up.
- Name, Size, Created and Modified columns: click a header to sort by it,
  click again to reverse. Folders stay above files.
- New folder makes a folder here; each row has Rename and Delete (deleting a
  folder removes everything in it).
- Tick items (the box in the bar ticks the whole folder) to Copy to…, Move
  to… or Delete several at once. Copy to… and Move to… pick the destination by
  clicking through folders, no typing.
- Copies run on the Switch in the background, with progress under Transfers;
  a copy into the same folder is named "name copy". Moves are instant and
  never replace an existing item, and a folder cannot go inside itself.

Command line:

```sh
python3 sw2xfer.py --host <switch-ip> ls /path
python3 sw2xfer.py --host <switch-ip> mkdir /path/new
python3 sw2xfer.py --host <switch-ip> rm -r /path/old
python3 sw2xfer.py --host <switch-ip> mv /path/a /path/b
```

## HTTP API

| Request | Effect |
|---|---|
| `GET /api/info` | name, version, free bytes |
| `GET /api/list?path=/x` | folder listing |
| `GET /dl/<path>` (+ `Range: bytes=a-b`) | file or a range of it (206) |
| `POST /api/create?path=&size=` | create or replace a file, preallocated |
| `PUT /ul/<path>?offset=N` | write the body at offset N (no truncation) |
| `POST /api/mkdir?path=`, `/api/delete?path=[&recursive=1]`, `/api/rename?from=&to=` | rename/move never replaces |
| `POST /api/copy?from=&to=` | background copy of a file or folder; progress at `GET /api/jobs` |
| `POST /api/login` (header `X-SW2-Password`, URL-encoded) | returns a 12-hour token and sets the `sw2t` cookie |
| `POST /api/logout` | clears the cookie |
| `POST /api/cancel` | cancel all transfers and copies |
| `GET /api/screen` | PNG of the Switch screen (from memory, nothing saved) |
| `GET /sw2xfer.py`, `/favicon.png` | the client script, the app icon |

With a password set, every request except the page, its icon, the client
script, `info`, `login` and `logout` needs `Authorization: Bearer <token>` or
the cookie, and gets 401 otherwise. Other websites cannot call the API (no
cross-origin headers).

A multi-connection upload is `create` followed by parallel `PUT`s at different
offsets. Clients tag each file's requests with `?x=<id>`; after a cancel those
ids are refused with HTTP 410, so clients stop instead of retrying.

## Design

The Switch socket service allows only a few concurrent IPC sessions (libnx caps
it at 16, and a blocked call holds one), so there is no thread per connection.
Four network threads poll non-blocking sockets. A disk pool of four threads
reads and writes the SD in 1 MB chunks, triple-buffered per connection. All
writers of one file share one `FsFile` handle and use positional writes. Socket
buffers are 1 MB.

The socket service keeps one memory pool for all of an app's sockets, sized
`sb_efficiency` x (send max + receive max). Every open socket takes part of it,
and when it runs out every socket on the console stalls, ftpd included, until
the app quits. With the old 4 MB buffers that happened at about 12 connections.
1 MB buffers are just as fast (46 MB/s on one connection, 54 MB/s on 8), so the
pool is set for many small sockets (efficiency 32), and the server never has
more than 16 connections open (more wait in a short listen queue), 8 transfers
moving data, or 24 files open.

Never use `std::thread::detach()` here: on the Switch, `pthread_detach` calls a
weak `__syscall_thread_detach` that libnx does not provide, so it jumps to
address 0. Background chores run on the copy worker (`opsBackground`).

## Building

```sh
export DEVKITPRO=/opt/devkitpro && make          # sw2transfer.nro
tests/host_build.sh && build-host/sw2transfer-host <dir> 6967   # same servers on the Mac (ASan)
```

Licences for the bundled fonts and libraries are in `romfs/licenses/`.
