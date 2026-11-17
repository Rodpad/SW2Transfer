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
- Click a file to download it, or tick several files and press Download.
  Downloads over 3.5 GB use one connection.
- Connections (default 8) and chunk size (default 2 MB) were the fastest
  combination in tests.
- Cancel all (web page), X twice (Switch) or `sw2xfer.py cancel` stops every
  transfer and copy, from any device; unfinished uploads and copies are
  deleted from the SD card.

Command line (Python 3; download it from `http://<switch-ip>:6967/sw2xfer.py`):

```sh
python3 sw2xfer.py --host <switch-ip> upload /SourceFileOrFolder /DestinationPath
python3 sw2xfer.py --host <switch-ip> download /SourceFileOrFolder /DestinationPath
python3 sw2xfer.py --host <switch-ip> upload game.iso MyFolder /roms/ps2   # several sources, last path = destination
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
writers of one file share one `FsFile` handle and use positional writes. TCP
buffers are raised to 4 MB, with a fallback if the socket service refuses.

Never use `std::thread::detach()` here: on the Switch, `pthread_detach` calls a
weak `__syscall_thread_detach` that libnx does not provide, so it jumps to
address 0. Background chores run on the copy worker (`opsBackground`).

## Building

```sh
export DEVKITPRO=/opt/devkitpro && make          # sw2transfer.nro
tests/host_build.sh && build-host/sw2transfer-host <dir> 6967   # same servers on the Mac (ASan)
```

Licences for the bundled fonts and libraries are in `romfs/licenses/`.
