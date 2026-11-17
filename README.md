# SW2Transfer

Multi-connection file transfer server for the Switch 2 (hbmenu under kaiHEN).
One file can be read or written by many TCP connections at once, each moving a
different byte range, which is much faster over Wi-Fi than a single stream.

## Speed

| | Upload to Switch | Download from Switch |
|---|---|---|
| **SW2Transfer, 8 connections (default)** | **51 MB/s** | **43 MB/s** |
| SW2Transfer, 1 connection | 28 MB/s | 21 MB/s |

For comparison, the most I ever saw over FTP (kaiHEN ftpd) was 8.8 MB/s, so
SW2Transfer is about 5 times faster. Measured with a Mac and the Switch 2
both on Wi-Fi, moving a 256 MB file.

## Install

Copy `sw2transfer.nro` to `sdmc:/switch/sw2transfer/` and launch it
from hbmenu. The screen shows the console's address, live speed, and every
file in flight with its connection count. The console stays awake while the
app is open.

| Button | Action |
|---|---|
| X (twice) | Cancel all transfers and copies |
| Y | Help |
| + | Exit |

**No login:** while the app is open, anyone on your network can read, change
or delete files on the SD card. Only run it on a network you trust.

## Using it

Open `http://<switch-ip>:6967` in a browser on the same network (the Switch
screen shows the address; Y on the Switch and the page's Help button explain
everything with examples).

- Click folders to open them; uploads go into the folder you are in. Upload
  files, upload a folder, or drag files and folders onto the page.
- Tick items to download, copy, move or delete several at once; Copy to… and
  Move to… pick the destination by clicking through folders. Copies run on the
  Switch in the background with progress; moves never replace anything.
- Connections (default 8) and chunk size (default 2 MB) were the fastest
  combination in tests over Wi-Fi. Downloads over 3.5 GB use one connection.
- Cancel all (web page), X twice (Switch) or `sw2xfer.py cancel` stops every
  transfer and copy, from any device; unfinished uploads and copies are
  deleted from the SD card.

Command line (Python 3; download it from `http://<switch-ip>:6967/sw2xfer.py`):

```sh
python3 sw2xfer.py --host 192.168.1.168 upload /SourceFileOrFolder /DestinationPath
python3 sw2xfer.py --host 192.168.1.168 download /SourceFileOrFolder /DestinationPath
python3 sw2xfer.py --host 192.168.1.168 upload game.iso MyFolder /roms/ps2   # several sources, last path = destination
python3 sw2xfer.py --host 192.168.1.168 ls /path
python3 sw2xfer.py --host 192.168.1.168 cancel
python3 sw2xfer.py --host 192.168.1.168 bench /path/test.bin --levels 1,4,8
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
| `POST /api/cancel` | cancel all transfers and copies |
| `GET /api/screen` | PNG of the Switch screen (from memory, nothing saved) |
| `GET /sw2xfer.py`, `/favicon.png` | the client script, the app icon |

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
