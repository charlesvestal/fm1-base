# Companion services

A firmware's computer side (a web page, a phone app, a script) needs the same few things from every app: what is this
FM-1 running and what can it do, the app's files (projects, presets, samples) to and from the computer, and the app's
parameters to show and change. Without a common way each app invents its own SysEx for them, and every page and phone
app is written again for each firmware. The companion services are that common way, in the platform: an app declares
what it has, the platform speaks the protocol, and one generic page (`web/companion/`), one tool
(`tools/fm1_companion.py`) and the browser emulator serve every app.

Off unless the app asks: `fm1.toml companion = true` (`FM1B_COMPANION`) builds `device/companion.c`. An app without it
is built byte for byte as before (its package, its host simulator, its `app.wasm`).

| Service | The app gives | The computer can |
| --- | --- | --- |
| capability query (always, with `companion = true`) | `APP_NAME`, `APP_VERSION`, optional `APP_COMPANION_EXTRAS` | ask which platform services this firmware has, the app's name, version, its own tags and its backup's format |
| files (`APP_FILES` in app.h) | a list of named objects and their reads and writes | list them, read one in chunks, write one (checked by CRC-32 before it counts, resumable), delete one; refused while the app is busy (playing) |
| parameters (`APP_PARAMS` in app.h) | a table of parameters (id, name, range, enum labels, unit, group) and get / set | describe them, get and set values, be told when one changes on the FM-1 |

## On the wire

The update's SysEx framing, as backup.c and mirror.c use it (felucca-bsp ota.c): `F0 pack7(00 59 cmd len24 body
~sum8(body)) F7`; pack7 carries 8-bit bodies, little-endian numbers. A request from the computer at a time (the device
keeps one frame): send the next after the answer. Every request is answered with the same cmd. The computer may send at
most 640 bytes between F0 and F7 (felucca-bsp usb.c's buffer): 512 bytes of file data a write. The commands:

| cmd | service |
| --- | --- |
| `0x60` | capability query |
| `0x61` | files |
| `0x62` | parameters |

(0x11 and 0x30 are the update's, 0x41..0x43 backup's, 0x50..0x54 the mirror's.) A firmware without the service does not
answer: a computer that hears nothing for 300 ms after `0x60` knows the firmware has no companion services (an update's
handshake, `0x11`, still tells it the identity).

### 0x60 capability query

Request: an empty body (or one byte, the client's protocol version: ignored for now). Answer:

```
u8  version (2)
u32 services, a bit each:  0 the capability query   1 files            2 parameters      3 backup (backup.c)
                           4 mirror (mirror.c)      5 screen reader    6 Bluetooth MIDI  7 WiFi
                           8 USB audio in           9 USB audio out   10 the app's own SysEx (APP_SYSEX)
                          11 the app's own commands in this framing (APP_USB_CMD)     12 the second core
                          13 USB MIDI host         14 Bluetooth audio
u16 the largest read chunk (256), u16 the largest write chunk (512)
u16 files, u16 parameters  (their counts now; 0 without the service)
identity \0   name \0   version \0   extras \0
-- protocol 2, at the end (a version 1 reader stops after the extras and never sees it) --
4 bytes the backup's magic (APP_BACKUP_MAGIC, as backup.c writes it: 4 ASCII letters, the image's first 4 bytes),
        0 0 0 0 without backup (FM1B_BACKUP)
u16 the backup's format version (APP_BACKUP_VERSION), 0 without backup
ext \0  the backup files' extension without the dot (fm1.toml backup_ext, "bak" by default), "" without backup
```

identity is the package identity (`FM-1_NNNN`, the update handshake's), name `APP_NAME`, version `APP_VERSION`, extras
`APP_COMPANION_EXTRAS` ("" by default): the app's own tags for its own pages, `key=value` pairs split by `;` (e.g.
`editor=optimist/7;site=https://...`). A generic page shows what the bits say and leaves the extras to the app's pages.

The backup's format (magic, version, extension) is the platform's to tell, not an extras tag: a page or phone app that
reads a backup file or asks the FM-1 for one takes them from the answer (version 2). An older firmware (version 1)
answers without them: a client may then fall back to tags its app's own pages know (the extras), or to what it knows
of the firmware. Fields are only ever added at the answer's end and the version raised; a reader takes the fields it
knows and ignores the rest.

### 0x61 files

A file is a named object the app exposes, by index 0 .. count - 1 in the app's order (the name is what a computer keeps:
an index may name another file after the app's list changed). The first body byte is the operation; every answer is
`op, status, u16 index, ...`. Status: 0 ok, 1 busy (the app plays: stop first), 2 no such file, 3 not allowed (read
only, not deletable), 4 out of range (offset, size above the file's room), 5 CRC-32 mismatch, 6 the flash refused, 7 out
of order (no write begun for this file, or a gap), 8 a malformed request, 9 an unknown operation.

| op | request (after op) | answer (after op, status, index) |
| --- | --- | --- |
| 0 list | u16 index | u16 count; if there: u8 kind, u8 flags, u32 size, u32 room, name \0 |
| 1 read | u16 index, u32 offset, u16 n | u32 offset, the bytes (n at most, fewer at the end, 256 at most) |
| 2 sum | u16 index | u32 size, u32 CRC-32 of the whole file (read from the flash now) |
| 3 begin | u16 index, u32 size, u32 CRC-32 | u32 next: the offset to send from (0; more when it resumes) |
| 4 data | u16 index, u32 offset, the bytes (512 at most) | u32 next |
| 5 commit | u16 index | u32 size, u32 CRC-32 |
| 6 delete | u16 index | (nothing) |

kind: 0 other, 1 project, 2 preset, 3 sample, 4 bank, 5 pattern, 6 settings, 7 text, 8 log. flags: bit 0 readable,
1 writable, 2 deletable, 3 written while playing too (not refused when busy). size 0: empty. CRC-32 is zlib's.

A read is by offset: resumable as it is, any chunk again. A write: begin with the whole file's size and CRC-32, the data
in order, commit. The device keeps a running CRC-32 of what came, and at commit the app's copy is made current only if
it matches (the app checks its flash too). A data frame again (its answer lost) is answered again, not written twice;
one past next is refused with next (7): the computer goes on from there. A begin with the same index, size and CRC-32
while that write is under way answers where it stands: a cable pulled mid-write resumes where it stopped (while the
FM-1 stays on; the transfer is in RAM). Another begin abandons it (nothing changed: the old copy stays current). Begin,
data, commit and delete are refused while the app says busy (`app_file_busy`), unless the file's flag 3 says otherwise.

### 0x62 parameters

A parameter has an id (u16, the app's own, kept from one release to the next), a name, a type (0 a number, 1 one of
labels), min, max and default (i32), a unit, a group (an index into the app's groups: a page's sections) and flags (bit 0
read only). An enum's labels are for min .. max in order.

| op | request (after op) | answer (after op, status) |
| --- | --- | --- |
| 0 info | u16 index | u16 index, u16 count; if there: u16 id, u8 type, u8 group, u8 flags, i32 min, i32 max, i32 default, u16 labels, name \0, unit \0 |
| 1 labels | u16 index, u16 from | u16 index, u16 from, u8 n, n labels each \0 (as many as fit 200 bytes) |
| 2 group | u8 group | u8 group, u8 count, name \0 |
| 3 get | n x u16 id (32 at most) | u8 n, n x (u16 id, i32 value) (an id the app has not: left out, status 2) |
| 4 set | u16 id, i32 value | u16 id, i32 the value now (clamped, or as the app took it) |
| 5 watch | u8 1 on, 0 off | u8 on |
| 6 changed (device -> computer) | | u8 n, n x (u16 id, i32 value) |

Statuses as for files. While watched, the device looks at every value 20 times a second and sends what changed (on the
panel, by MIDI, by the app itself) in op 6 frames, 16 values a frame, when the SysEx ring has room (never waiting: the
music first). A set from the computer is not sent back. Watching ends with watch 0, a new USB connection, or 10 s without
any request: a page keeps it with watch 1 every few seconds.

## The app's side

app.h, with `companion = true` in fm1.toml (plat.h has the types and the helper):

```c
#define APP_COMPANION_EXTRAS "editor=myapp/1"      /* optional: the app's own tags (0x60) */

#define APP_FILES                                  /* files: */
int app_file(uint32_t i, plat_file_info_t *f);     /* file i (0..): 0 = there (name, kind, flags, size, room); -1 past the last */
int app_file_read(uint32_t i, uint32_t off, uint8_t *dst, uint32_t n);    /* 0 = read */
int app_file_begin(uint32_t i, uint32_t size);     /* a new content of size bytes is coming: 0 = ready */
int app_file_write(uint32_t i, uint32_t off, const uint8_t *src, uint32_t n);   /* in order; 0 = written */
int app_file_commit(uint32_t i, uint32_t size, uint32_t crc);   /* all of it came, CRC-32 checked: make it current */
int app_file_delete(uint32_t i);                   /* 0 = gone */
int app_file_busy(void);                           /* 1: no write or delete now (playing) */
#define APP_FILE_COUNT                             /* optional: uint32_t app_file_count(void), the files' number, for an
                                                      app whose app_file is costly (the list asks it once, not a file each) */

#define APP_PARAMS                                 /* parameters: */
#define APP_PARAM_MAX 64                           /* the most there are (watching keeps a value each) */
int app_param(uint32_t i, plat_param_t *p);        /* parameter i (0..): 0 = there; -1 past the last */
const char *app_param_group(uint32_t g);           /* group g's name; 0 past the last */
int32_t app_param_get(uint32_t i);
int32_t app_param_set(uint32_t i, int32_t v);      /* -> the value now */
```

All of it runs in the main loop (a request is taken where the update's handshake is). A value the render reads is the
app's to share safely (plat_lock), as for its own UI.

**Files on the flash, the easy way: `plat_file_*`.** A file in the data area (plat.h's windows) as two copies, A and B,
each a whole number of 4 KiB sectors: `plat_file_t f = {A, B, BYTES}`. The current copy is the newer one whose head is
whole; a write goes to the other one (its sectors erased as the data reaches them) and its head (32 bytes: magic, a
sequence number, the size, the CRC-32 of the data, the head's own CRC-32) is written last, after the data was read
back and checked, so a power cut leaves the old file current. `plat_file_size`, `_read`, `_data` (read in place),
`_begin`, `_write`, `_commit`, `_delete` map one to one onto `app_file_*`. A file's room is BYTES - 32. In XDATA0 and
XDATA1 (plat.h) the update loader clears a sector whose bytes 0xF00..0xF07 read as its record: a file's data could do so
(2^-32 for a sector of random bytes, a CRC-16 and two letters); keep such files in the data area proper when that
matters.

## Size

Measured on the FM-1's build (JieLi toolchain, AC79 reference SDK; tools/build.py's image size), 2026-10-27:

| | flash | RAM |
| --- | --- | --- |
| an app without `companion = true` | 0: its package byte for byte as before (the template's, sha256 checked) | 0 |
| the capability query (the template with `companion = true`) | +456 B | +592 B of the pool (the answer's buffers), 0 of .data / .bss |
| files, with `plat_file_*` (tests/companion_app: with its own hooks for three files) | +3,496 B | +32 B of .data / .bss, 0 of the pool |
| parameters (tests/companion_app: with its own table of six and its hooks) | +2,136 B | +64 B of .data / .bss; +4 B a parameter in the pool (APP_PARAM_MAX; 8 is +32 B), the app's labels its own |


## Tools

- `tools/fm1_companion.py` (needs mido with python-rtmidi, as the installer): `info`, `ls`, `get NAME FILE`, `put FILE
  NAME`, `rm NAME`, `params`, `get-param ID`, `set-param ID VALUE`; also on a host simulator (`NAME_host --serve`,
  `--serve-cmd`).
- `web/companion/` (the site's `companion/` page): the same against the FM-1 over Web MIDI, or the browser emulator
  (its `port` part).
