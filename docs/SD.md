# The SD card

A firmware with `sd = true` in its fm1.toml can read and write files on a microSD card: samples, songs, banks, a
library bigger than the FM-1's 1 MiB flash. Neither the FM-1 nor the SMK-37 has a card slot, so this needs a card mod.
With `msc = true` as well, the card can also be a USB drive for a computer (`plat_sd_disk`).

**Status:** run on an SMK-37 Pro with a hand-wired card (32 GB SDHC, FAT32), on its board layer (the `smk37-inhouse`
branches): the card at 15 MHz with DMA and no CRC errors; files at 1.35 MB/s reading and 1.2 MB/s writing; as a USB
drive 1.0 MB/s reading and ~790 KB/s writing (Linux, `dd` with direct I/O), the data read back the same; the card
pulled and put back while a USB drive, with the computer idle and in the middle of a copy. Not run yet on an FM-1
with the m-card board. On the host, the protocol and the file system run against a modelled card
(`tests/sd_test.c`) and the example runs in the simulator.

## The hardware

**FM-1:** the [m-card](https://github.com/jvitkauskas/m-card) board. It plugs in between the LCD's flat cable and the
main board, so nothing is soldered to the FM-1. The card shares the LCD's SPI1:

| Card | FM-1 | Through |
| --- | --- | --- |
| CLK | PC9, SPI1's clock | 470 R |
| DI (CMD) | PC10, SPI1's data out | 470 R, 47k pull-up |
| DO (DAT0) | PC8, the panel's D/C | 220 R, 47k pull-up |
| CS | NOT PC7, the panel's CS (an inverter) | 47k pull-down before the inverter |

The card is selected whenever the panel isn't. There's no card-detect line: a card is found by asking it.

**SMK-37:** no board yet. A hand-wired mod as m-bootloader's: CLK PB6, DI PB7, DO PB5 (the panel's D/C), CS = NOT PC8
(felucca-bsp `boards/smk37/fm1_sd_hw.h`, on the SMK-37 branch). A pull-up on DI and a series resistor on DO, as on
m-card, are wanted.

## Using it

```toml
sd = true                     # fm1.toml
```

```c
#include "fatfs/ff.h"         /* FatFs: f_open, f_read, f_write, f_lseek, f_opendir, f_readdir, f_expand ... */

if (plat_sd_start() == 0) {   /* the card found, started and its FAT mounted (drive "") */
    FIL f;
    UINT n;
    if (f_open(&f, "SAMPLES/KICK.QOA", FA_READ) == FR_OK) {
        f_read(&f, buf, sizeof buf, &n);
        f_close(&f);
    }
}
```

- `plat_sd_start()`: 0; `SD_E_NOFS` when the card works but has no FAT (raw sectors only); another `SD_E_*` when
  there's no usable card. Call it again after a card change or an error: nothing tells the firmware a card came or
  went. `plat_sd_what(e)` puts any answer in words for the screen.
- `plat_sd_info()`: the size, the clock the card runs at, whether reads use DMA, and the counts of CRC errors,
  retries and timeouts since power-on: what to look at on a new mod.
- `plat_sd_read` / `plat_sd_write`: raw 512-byte sectors, for an app with its own layout (or no FAT at all).
- FatFs is R0.16 with ChaN's patches 1 and 2, configured in `lib/fatfs/ffconf.h`: long names in UTF-8, fast seek
  (`f_lseek` with a cluster table, for streaming without reading the FAT), `f_expand` (a contiguous file), one shared
  sector window (`FF_FS_TINY`), no clock (files are dated 2026-01-01). FAT12, FAT16 and FAT32. exFAT (SDXC cards as
  they come, 64 GB and up) with `-DFM1B_SD_EXFAT=1` in the build's flags; or format the card FAT32.

**Main loop only, from `app_ui_init` on.** The card shares SPI1 with the screen, so every call is a session between
two LCD transfers and blocks until it's done: never from `app_render`, `app_tick` or the second core. Rough times at
the top clock: a sector ~0.3 ms, the start up to ~1 s.

**Buffers:** in RAM. A 4-byte-aligned buffer in the main RAM is filled by DMA; anything else goes through a
512-byte bounce buffer, a little slower. Whole-sector reads (`f_read` of 512-byte multiples at 512-byte offsets) go
straight into the caller's buffer.

**A card that's gone or stuck.** Every call has a time budget (a read or write 1.5 s with its retries, the start 4 s),
and a read or write that fails every try takes the card for lost: `plat_sd_state()` turns negative and every call
after fails at once, until `plat_sd_start()` finds a card again. So a card pulled in the middle of a long transfer
costs one budget, not one per sector (the main loop's watchdog is 8 s).

## As a USB drive

```c
plat_disk_mode(plat_sd_disk());    /* after plat_sd_start: the card's files unmounted, the computer's now */
/* app_loop (APP_LOOP): */
if (plat_sd_disk_poll() == PLAT_DISK_EJECTED) { plat_disk_mode(0); plat_sd_start(); }
```

`plat_sd_disk()` is a ready-made `plat_disk_t` for the card. Reads run ahead in a stream (CMD18) and a WRITE's
blocks go out in one (CMD25), each block moved by DMA while disk mode waits on USB, so the card and USB work at
once. A WRITE's blocks are on the card before the computer hears it succeeded. A read that fails reaches the computer
as a medium error, not zeros. The card pulled while it's a drive: the computer hears "no medium" (as from a card
reader), within a second when it is quiet (the card is asked for its status once a second), at once in the middle of
a transfer; put back (or another card), it's found within 2 s and the computer hears "medium changed" and reads it
again; what was left of a command started on the old card never reaches the new one. 32 KiB of the pool
(`APP_SD_DISK_RA`, `APP_SD_DISK_WB`: 32 blocks each).

For this, `plat_disk_t` has optional fields beyond the app's blocks: `sync` (held writes written before the WRITE is
answered), `work` (called while disk mode waits on USB), `read_fails` (a read's 0 is an error, not zeros) and
`present` (the medium there or not, and which). Disk mode also moves its USB packets from the main loop while it waits on them
(felucca-bsp `usb_msc_pump`), not only at the 2 kHz poll, which alone allowed ~128 KB/s.

## Streams for an app

For an app that moves many blocks a few at a time from its main loop (samples played from the card, a recording),
on sectors it found through FatFs (a contiguous file: `f_expand`, then its first sector):

```c
plat_sd_read_open(sector, n);                /* n blocks planned (0: to the card's end) */
r = plat_sd_read_next(buf, 50);              /* 0; SD_E_BUSY: not begun within 50 us, call again later */
plat_sd_close();
plat_sd_write_open(sector);
r = plat_sd_write_next(buf);                 /* 0 sent; SD_E_BUSY: the card still writes the last one */
plat_sd_close();                             /* the last block on the card */
```

A read never waits past `wait_us` for its block to begin, so the main loop keeps its rhythm; the bus stays the card's
between calls until the LCD draws, which ends the read (the next call opens it again where it was). A write sends one
block per call and asks the card's busy without waiting on it; the LCD may draw between blocks (the write stays open,
the card deselected, its busy waited for first so its DO never pulls against D/C). A damaged block: `SD_E_CRC`, the
same block at the next call. Any other card call ends a stream first.

Options (fm1.toml, all off by default):
- `sd_read_ahead = N`: a read's blocks taken in SPI1's interrupt, up to N ahead (~544 B of the pool each), so the
  card streams on while the render holds the main loop up (felucca-bsp `fm1_sd_rx_irq_*`, `isr_spi1`).
- `sd_clock_fine = true`: the clock search through 20, 15, 12, 10, 8.57, 7.5, 6.67, 6 and 5 MHz, for a board whose
  ceiling falls between the usual steps.
- `sd_sck_hd = true`: SCK driven hard during the card's sessions (felucca-bsp `FM1_SD_SCK_HD`).

## How it works

`device/sd.c` is the protocol, `lib/sd_fs.c` the file system's glue and the USB drive, felucca-bsp's
`boards/<board>/fm1_sd_hw.h` the pins and SPI1 (fm1-base keeps every register access in felucca-bsp).

- **The bus.** A session sends the panel a NOP first, so the bytes clocked later while the card is deselected (the
  panel selected, D/C pulled high: data) follow no command and are ignored. It then turns D/C into SPI1's data input
  (the card's DO) and SPI1 into separate in and out lines. At the end SPI1 goes back as the panel uses it. After each
  LCD command D/C is left high (`fm1_sd_park`): the selected card's DO idles high, and the two don't pull against
  each other through the 220 R. A stream left open (the USB drive's) keeps the bus; the LCD's next transfer
  (`lcd_sync`) ends it first.
- **The start.** A reset restarts the chip, not the card, so a transfer the firmware before left open (a crash, a
  watchdog; an update and `plat_restart` end it before they reset) is ended first: 515 clocks finish a half-sent
  write block, CMD12 ends a multi-block read (which heeds nothing else: a CMD0 would get its data) or a refused write,
  the stop token a multi-block write, each followed by the card's busy (up to 600 ms). A card still busy gets no CMD0
  (resetting a card that programs can damage it): the start fails and is tried again. The half block's CRC is then
  almost always wrong, so it is refused; 1 time in 65536 that sector gets the block's 0xFF tail, as a power loss in
  the middle of a write would leave it (nothing can cut the card's power to do better).
  Then 400 kHz, CMD0, CMD8, ACMD41, CMD58: SDSC v1 and v2, SDHC and SDXC (no MMC). Then CRCs on (CMD59):
  the card checks every command and every block written, and every block read is checked here (CRC-16). A clock too
  fast or a poor contact shows as an error, never as wrong data.
- **The clock.** The fastest of 20, 15, 10 and 5 MHz (LSB / 3, 4, 6, 12; 10 % more at `cpu_clock = 396`), else
  400 kHz, at which the partition table, the first boot sector and 16 sectors of the FAT read with good CRCs (blank
  sectors prove nothing: all zeros pass a CRC however they're misread). The card is started again before each step
  down, as a failed step can leave it in the middle of a transfer. Later, two failures in a row step it down one.
- **DMA.** What SPI1 sends while it receives by DMA isn't established (a card wants 0xFF there; m-card pulls the line
  up). So reads go by DMA only after those reads came in right that way twice; otherwise the card is started again and
  reads go byte by byte (about 1 MB/s). Writes use DMA from aligned RAM.
- **Errors.** A transfer is tried up to 4 times, with a restart of the card in between. Each `SD_E_*` says what
  happened (`include/plat.h`).

## Testing

- `tests/sd_test.c` (in `tests/run_tests.sh`): `device/sd.c` against `tests/sd_card_model.h`, a card byte by byte
  in SPI mode, which checks every command's CRC7 and every written block's CRC16. SDHC, SDSC v2 and v1 cards, one
  sector and runs, aligned and unaligned buffers, a FAT32 and two FAT16 volumes made, filled and read back after a
  new start, long names, fast seek, a contiguous file; the clock stepping down, DMA refused, damaged blocks; the USB
  drive's streams with the LCD drawing in between, its writes, a damaged block in a stream; a card pulled, stuck busy
  and put back (each error within its budget), pulled from a quiet USB drive (noticed within a second), and left in
  the middle of a read or a write by a restart (between blocks, inside one, after a refused one: no command to a
  busy card). Its
  FAT32 image is checked with mtools when they're installed.
- `tests/sd_stream_test.c`: an app's streams on the same model, with and without `sd_read_ahead` (the interrupt run
  by the test): waits within their budget while the card is slow, the LCD between blocks, damaged and refused blocks,
  the run's end and the card's, a card pulled or stuck; the clock a 9 MHz card ends at with `sd_clock_fine`.
- `tests/fatfs_test.c`: lib/fatfs's changes on a volume in memory (`f_expand` round the volume's end,
  `f_unlink_entry` after a rename cut by a power loss).
- `examples/sd`: on the device, the test of a mod. It shows the card's size, clock, DMA and error counts, lists the
  top folder, runs a speed test (REC: a 1 MiB contiguous file written, read back, checked) and shows the card to a
  computer as a USB drive (SAVE). The buttons also come as USB MIDI notes (60 PLAY, 62 REC, 64 SEL, 65 SAVE) for
  tests at a distance. In the simulator it runs on an image (`FM1B_SD_IMAGE`, or the script's `sd FILE` / `sd out`):
  `examples/sd/tests/sd.txt`.

What to check on a unit: the example's clock (20 MHz possible on m-card, 15 MHz on the SMK-37's wiring), DMA (yes or
no: either works), zero CRC errors over a few speed tests, the screen still right while the card is used, and as a
USB drive: a file copied on and back unchanged, the card pulled and put back.
