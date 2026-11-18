# Companion services

A firmware's computer side (a web page, a phone app, a script) needs the same few things from every app: what is this
FM-1 running and what can it do, the app's files (projects, presets, patterns, songs) to and from the computer, and the
app's parameters to show and change. Without a common way each app invents its own SysEx for them, and every page and phone
app is written again for each firmware. The companion services are that common way, in the platform: an app declares
what it has, the platform speaks the protocol, and one generic page (`web/companion/`), one tool
(`tools/fm1_companion.py`) and the browser emulator serve every app.

Off unless the app asks: `fm1.toml companion = true` (`FM1B_COMPANION`) builds `device/companion.c`. An app without it
is built byte for byte as before (its package, its host simulator, its `app.wasm`).

| Service | The app gives | The computer can |
| --- | --- | --- |
| capability query (always, with `companion = true`) | `APP_NAME`, `APP_VERSION`, optional `APP_COMPANION_EXTRAS` | ask which platform services this firmware has, the app's name, version, its own tags and its backup's format |
| files (`APP_FILE_OBJS` in app.h: its store objects; `APP_FILES`: files of its own) | the store objects a computer may keep, by name and kind; or its own list with reads and writes | list them, read one in chunks, write one (checked by CRC-32 before it counts), delete one; refused while the app is busy (playing). A file is bytes: the page keeps whatever the app gives, whatever its format |
| parameters (`APP_PARAMS` in app.h) | a table of parameters (id, name, range, enum labels, unit, group) and get / set | describe them, get and set values, be told when one changes on the FM-1 |

## On the wire

The update's SysEx framing, as backup.c and mirror.c use it (felucca-bsp ota.c): `F0 pack7(00 59 cmd len24 body
~sum8(body)) F7`; pack7 carries 8-bit bodies, little-endian numbers. A request from the computer at a time (the device
keeps one frame): send the next after the answer. Every request is answered with the same cmd. The computer may send at
most 640 bytes between F0 and F7 (felucca-bsp usb.c's buffer): 512 bytes of file data a write. The commands:

| cmd | service | version (0x60 tells it) |
| --- | --- | --- |
| `0x60` | capability query | the answer's first byte (3) |
| `0x61` | files | 1 |
| `0x62` | parameters | 1 |
| `0x63` .. `0x6F` | kept for the companion's next services | |

The commands 0x60 to 0x6F are the companion's. An app's own commands in this framing (`APP_USB_CMD`) never use them,
with or without `companion = true`: with it, the platform takes the whole range, and answers nothing to the ones it does
not know yet. (0x11 and 0x30 are the update's, 0x41..0x44 backup's, restore's and the resources', 0x50..0x54 the
mirror's.) A firmware without the companion does not answer: a computer that hears nothing for 300 ms after `0x60` knows
the firmware has no companion services (an update's handshake, `0x11`, still tells it the identity).

### Versions

The capability query's answer carries the protocol's version (its first byte) and, from version 3, the version of each
command the firmware has, after everything else. A command's requests and answers change only with its version raised;
fields are only ever added at an answer's end, and a reader takes the ones it knows and ignores the rest. A client asks
0x60 first and then knows, for every command, what this firmware speaks, without a byte more in each request or answer.
A version 0 means the build does not have that service.

### 0x60 capability query

Request: an empty body (or one byte, the client's protocol version: ignored for now). Answer:

```
u8  version (3)
u32 services, a bit each:  0 the capability query   1 files            2 parameters      3 backup (backup.c)
                           4 mirror (mirror.c)      5 screen reader    6 Bluetooth MIDI  7 WiFi
                           8 USB audio in           9 USB audio out   10 the app's own SysEx (APP_SYSEX)
                          11 the app's own commands in this framing (APP_USB_CMD)     12 the second core
                          13 USB MIDI host         14 Bluetooth audio     15 the crash history (crash_history = true)
                          16 the debug stats       17 Bluetooth MIDI devices (ble_central)    18 the SD card
                          19 USB disk mode (msc)   20 a resource region   21 velocity and pressure (APP_TOUCH)
                          22 USB audio channels beyond two (usb_audio_channels)
u16 the largest read chunk (256), u16 the largest write chunk (512)
u16 files, u16 parameters  (their counts now; 0 without the service)
identity \0   name \0   version \0   extras \0
-- protocol 2, at the end (a version 1 reader stops after the extras and never sees it) --
4 bytes the backup's magic (APP_BACKUP_MAGIC, as backup.c writes it: 4 ASCII letters, the image's first 4 bytes),
        0 0 0 0 without backup (FM1B_BACKUP)
u16 the backup's format version (APP_BACKUP_VERSION), 0 without backup
ext \0  the backup files' extension without the dot (fm1.toml backup_ext, "bak" by default), "" without backup
-- protocol 3, at the end --
u8 n, then n x (u8 cmd, u8 version): each command's version (0x61 files, 0x62 parameters; 0: not in this build)
```

The crash history (`F0 7D 4C 48 F7`) and the debug stats (`F0 7D 4C 53 F7`) have their own SysEx (tools/fm1_remote.py,
the installer page's "Report a problem"); bits 15 and 16 say a firmware has them. Bits 3 and 4 say the same of backup
(web/backup) and the mirror (web/mirror).

identity is the package identity (`FM-1_NNNN`, the update handshake's), name `APP_NAME`, version `APP_VERSION`, extras
`APP_COMPANION_EXTRAS` ("" by default): the app's own tags for its own pages, `key=value` pairs split by `;` (e.g.
`editor=optimist/7;site=https://...`). A generic page shows what the bits say and leaves the extras to the app's pages.

The backup's format (magic, version, extension) is the platform's to tell, not an extras tag: a page or phone app that
reads a backup file or asks the FM-1 for one takes them from the answer (version 2). An older firmware (version 1)
answers without them: a client may then fall back to tags its app's own pages know (the extras), or to what it knows
of the firmware. Fields are only ever added at the answer's end and the version raised; a reader takes the fields it
knows and ignores the rest.

### 0x61 files

A file is a named object the app exposes, by index 0 .. count - 1: the store objects it lists (`APP_FILE_OBJS`) first,
in its order, then its own files (`APP_FILES`) (the name is what a computer keeps:
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

### Your saved objects as files

Most apps keep their work in the store already (`plat_store_save`, `plat_store_load`: presets, patterns, songs, a
project). To let a computer keep them as files, list them in app.h, with `companion = true` in fm1.toml:

```c
#define APP_FILE_OBJS {{OBJ_SONG1, PLAT_FK_PROJECT, PLAT_FF_READ | PLAT_FF_WRITE | PLAT_FF_DELETE, "song1"}, \
                       {OBJ_KIT, PLAT_FK_PRESET, PLAT_FF_READ | PLAT_FF_WRITE, "kit"}}
int app_file_busy(void);                           /* 1: no file written or deleted now (playing) */
void app_file_changed(uint32_t obj);               /* the computer wrote or emptied obj: load it again */
#define APP_FILE_OBJ_MAX 1024u                     /* optional: the largest a computer may write (PLAT_STORE_MAX) */
```

Each entry names an object, its kind, what a computer may do with it, and the name it shows. The platform does the
rest: it lists each object with its size, reads it from the current copy, and takes a write whole. The bytes gather
in a buffer, their CRC-32 is checked there once they all came, and only then does the platform save them as your app
would (`plat_store_save`: a new copy, the old one current until the new one is whole), and call `app_file_changed`.
A delete empties the object: your app finds it as at a first start. Writes and deletes wait while `app_file_busy`
says 1, unless an entry's flags have `PLAT_FF_PLAYING`, and while a backup streams. Never list `OBJ_PANEL`, the
unit's own calibration, or Bluetooth's pairings.

The buffer is `APP_FILE_OBJ_MAX` bytes of the pool (`PLAT_STORE_MAX`, 3,840, by default). With `backup = true` and no
`APP_FILE_OBJ_MAX` of the app's, the companion borrows backup's restore buffer instead and costs no RAM. A restore that
comes between a write's first and last frame changes that buffer; the CRC-32 check at the end then refuses the write
(status 5), and the computer sends it again.

Use `app_file_changed` to take the new content: load it as at power-on, redraw, and clear any "changed since the last
save" mark. The files carry your app's own format; the companion never looks inside, so a page keeps presets,
patterns or projects alike.

### Files of your own

For files that are not store objects (a sample in the data area, a text the app makes), use the hooks. They come after
the store objects in the list, and the two kinds may be used together:

```c
#define APP_COMPANION_EXTRAS "editor=myapp/1"      /* optional: the app's own tags (0x60) */

#define APP_FILES                                  /* files: */
int app_file(uint32_t i, plat_file_info_t *f);     /* file i (0..): 0 = there (name, kind, flags, size, room); -1 past the last */
int app_file_read(uint32_t i, uint32_t off, uint8_t *dst, uint32_t n);    /* 0 = read */
int app_file_begin(uint32_t i, uint32_t size);     /* a new content of size bytes is coming: 0 = ready */
int app_file_write(uint32_t i, uint32_t off, const uint8_t *src, uint32_t n);   /* in order; 0 = written */
int app_file_commit(uint32_t i, uint32_t size, uint32_t crc);   /* all of it came, CRC-32 checked: make it current */
int app_file_delete(uint32_t i);                   /* 0 = gone */
int app_file_busy(void);                           /* 1: no write or delete now (playing); one hook for both kinds */
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

Use `plat_file_*` to keep such a file in the data area (plat.h's windows) as two copies, A and B,
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
| an app without `companion = true` | 0: its package byte for byte as before (examples/ble_own's, sha256 checked, 2026-11-18) | 0 |
| the capability query (the template with `companion = true`) | +456 B | +592 B of the pool (the answer's buffers), 0 of .data / .bss |
| files, with `plat_file_*` (tests/companion_app: with its own hooks for three files) | +3,496 B | +32 B of .data / .bss, 0 of the pool |
| parameters (tests/companion_app: with its own table of six and its hooks) | +2,136 B | +64 B of .data / .bss; +4 B a parameter in the pool (APP_PARAM_MAX; 8 is +32 B), the app's labels its own |
| the template as it is (2026-11-18): the capability query and its settings as a file (`APP_FILE_OBJS`, one object) | +2,224 B | +32 B of .data / .bss; +4,432 B of the pool (the answer's buffers, and a write's buffer of 3,840 B: `APP_FILE_OBJ_MAX` makes it smaller, backup's restore buffer stands in for it with `backup = true`) |


## The page and the tools

Use the companion page (`web/companion/`, a site's `companion/` with `companion = true`) to keep an FM-1's files on a
computer. It finds the FM-1 over Web MIDI as the other pages do (web/fm1ports.js: ports with an FM-1's name, then the
ones that answer the screen reader's HELLO; never other gear, never a Bluetooth port), or the firmware's emulator in
another tab (its `port` part). It lists the files by kind with their sizes, and downloads, replaces or deletes one.
"Back up all" saves every readable file in one zip; "Restore from a backup" sends a zip's files back to the FM-1 by
name, and with "Send only what changed" it first asks the FM-1 for each file's CRC-32 and skips the ones that are the
same. Files can't be renamed: the app gives their names. The page shows the app's parameters too, when it has them.

The zip is plain (web/companion/zip.js writes it stored, with nothing to load): `files/NAME` for each file, and
`fm1-companion.json` with the firmware's identity, name and version and the list of files. Any zip tool opens it. The
page reads a zip back stored or deflated, so a backup zipped again by another tool still restores.

Use `tools/fm1_companion.py` (it needs mido with python-rtmidi, as the installer does) for the same from a script:
`info`, `ls`, `get NAME FILE`, `put FILE NAME`, `rm NAME`, `params`, `get-param ID`, `set-param ID VALUE`; also on a host
simulator (`NAME_host --serve`, `--serve-cmd`).
