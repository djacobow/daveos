# TCP files and directories

The optional `file-transfer` feature adds a binary file service on TCP port
1002. It requires `net` and `fatfs`; it is independent of the text console on
1000 and the flash OTA protocol on 1001. The full STM32 profile includes it.
The protocol and filesystem lease live in `lib/storage/`; the scheduler/TCP
adapter lives in `lib/net/file_transfer.hpp`.

## Usage

First initialize the card and mount it using the console:

```text
sd probe
fs mount rw
```

Upload a new file, then download it for an independent comparison:

```sh
python3 tools/files.py 192.168.1.207 put build/layout-h563/examples/stm32_console/application.ota /test-image.ota
python3 tools/files.py 192.168.1.207 get /test-image.ota build/readback.ota
cmp build/layout-h563/examples/stm32_console/application.ota build/readback.ota
```

Uploads require a read-write mount and never overwrite an existing path.
Downloads work on either read-only or read-write mounts. The host downloader
also refuses to overwrite an existing local file. Paths support spaces; quote
those arguments in the shell. Parent directories must already exist. The client also supports these basic
operations (no recursion, wildcards, overwrite, or automatic parent creation):

```sh
python3 tools/files.py 192.168.1.207 ls /
python3 tools/files.py 192.168.1.207 mkdir /updates
python3 tools/files.py 192.168.1.207 ls /updates
python3 tools/files.py 192.168.1.207 rm /updates/old.ota
python3 tools/files.py 192.168.1.207 rmdir /updates
```

`put` and `get` copy host→target and target→host respectively. `ls` prints
file/directory type, byte size, and name, and works on either mount mode.
`rm`, `mkdir`, and `rmdir` require rw. `rm` accepts files only; `rmdir` accepts
empty directories only. Metadata success follows FatFs synchronization;
mutations are not atomic against power loss and cannot be undone on disconnect.

For SD OTA, switch back to a read-only mount before preparing the package:

```text
fs unmount
fs mount
ota init /test-image.ota
```

Then use the existing `ota install` and explicit `board reset` workflow.
Uploading a file does not itself validate an OTA package, write MCU flash,
install firmware or reset the target.

## Ownership and failure

One connection and one transfer are supported at a time. A transfer reserves
the filesystem against console commands and SD OTA until completion or cleanup.
Media I/O runs from a self-rearming task, allowing the underlying SD driver to
yield. No ISR performs filesystem operations. The TCP adapter copies and
consumes input before entering filesystem calls; borrowed network buffers never
survive a yield. A second client is rejected by the existing TCP server.

Each upload chunk is acknowledged after writing it. Final success follows
size/CRC validation, sync and close. CRC32 checks the bytes passed to the
filesystem; use a download comparison or `ota init` to verify stored contents.
Downloads finish with a size/CRC acknowledgment after close. Chunk size is
1024 bytes; the last chunk may be shorter. Buffers are fixed, with no heap.

Disconnect, protocol error, checksum failure, or 30 seconds without progress
aborts an unfinished transfer. Cleanup closes its file, removes a partial
upload, then releases the filesystem reservation. It never removes a file
whose create request was rejected. Cleanup failure is reported as
`cleanup_failed` and logged; a remaining file needs explicit `fs rm` on a
writable mount. Retries never overwrite it. Power loss cannot run cleanup;
FAT writes are not atomic. A disconnect after durable completion can leave a
complete file whose success acknowledgment the host did not receive.

The inactivity deadline excludes time inside synchronous filesystem operations,
which have their own device timeouts. There are no automatic retries or resume.
Owners stopping the reusable module must call `stop()` and continue scheduler
dispatch until `stopped()` before destroying it or its filesystem. STM32 demo
shutdown remains terminal; normal transfers complete while the scheduler runs.

## Wire format (version 1)

All fields are unsigned 32-bit little-endian integers. A request or response
has a 24-byte header followed by up to 1024 payload bytes:

| Offset | Request | Response |
| --- | --- | --- |
| 0 | Magic `0x46534f44` (`DOSF`) | Same |
| 4 | Version `1` | Same |
| 8 | Operation | Status |
| 12 | Payload length | Payload length |
| 16 | Argument 0 | Offset, or total size for begin |
| 20 | Argument 1 | Running/final CRC32 |

Send one request and wait for its complete response before sending another.
The parser accepts arbitrary TCP fragmentation. CRC is CRC-32/ISO-HDLC, with
initial value zero, matching Python `binascii.crc32`; empty files have CRC zero.

| Operation | Payload | Argument 0 | Argument 1 |
| --- | --- | --- | --- |
| 1: upload | UTF-8 path, 1–255 bytes, no NUL | Total file bytes | Expected CRC32 |
| 2: write | 1–1024 binary bytes | Current offset | 0 |
| 3: finish upload | Empty | 0 | 0 |
| 4: download | Path | 0 | 0 |
| 5: read | Empty | 0 | 0 |
| 6: list directory | Path (`/` for root) | 0 | 0 |
| 7: next directory entry | Empty | 0 | 0 |
| 8: remove file | Path | 0 | 0 |
| 9: create directory | Path | 0 | 0 |
| 10: remove empty directory | Path | 0 | 0 |

Begin replies contain the total size and zero CRC. Write replies contain the
next offset and running CRC. Read replies contain up to 1024 bytes, the next
offset and running CRC. After receiving all file bytes, send one more read:
its empty reply confirms close/release and contains final size and CRC. Upload
finish similarly returns final size/CRC only after sync and close.

Directory begin and mutation replies have empty payloads and zero offset/CRC.
Each `next` reply carries one entry: little-endian u32 kind (0=file, 1=directory),
u32 byte size, then the UTF-8 name without NUL. Directory sizes are not useful
and should be ignored. Header offset/CRC remain zero. An empty payload ends the
listing after close/release. Listing holds the filesystem lease across pages;
disconnect, invalid requests, and idle timeout close it and release the lease.
The Python listing iterator must be exhausted or its socket closed.

Statuses: 0 `ok`, 1 `invalid`, 2 `busy`, 3 `not_ready`, 4 `read_only`,
5 `exists`, 6 `not_found`, 7 `io_error`, 8 `checksum_error`, 9 `timeout`,
10 `cleanup_failed`, 11 `denied` (wrong object type, nonempty directory, or
FatFs access denial). Close the connection on any error. Disconnect is also
how a client aborts an active transfer. This lab service has no authentication
or encryption, like the existing TCP console and OTA transport.

## Validation

Host tests use real FatFs over a file-backed FAT16 image, including fragmented
frames, binary/empty files, create-only and mount-mode checks, lease conflicts,
bad offsets/lengths/paths, checksum/timeout/disconnect cleanup, and injected
write/sync failure with unsuccessful cleanup. Python client tests cover bounded
streaming and fragmented replies, rejection, CRC errors, disconnects, and
uploading an already-positioned input stream.

H563 hardware passed upload and byte-exact downloads of a 290,114-byte OTA
package on rw and ro mounts, followed by successful SD OTA preparation (no
installation). It also passed duplicate-path protection, second-client/lease
rejection, disconnect, checksum and 30-second idle-timeout cleanup. Directory creation, paged listing on rw/ro mounts, file removal, empty-directory
removal, and nonempty-directory/read-only rejection also passed. Test files and
directories were removed and the volume unmounted. Stack painting observed 4,360 bytes and
zero heap attempts. ASan/UBSan passed 43/43, fake/no-logging 37/37, and both A/B
ARM builds passed. H755 was not flashed; power interruption and card removal
during transfer remain unqualified.
