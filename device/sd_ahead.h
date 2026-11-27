/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Henri David */
/* An app's read stream read ahead in SPI1's interrupt (fm1.toml sd_read_ahead = N: FM1B_SD_AHEAD, N blocks; included
 * by sd.c). Without it a stream's blocks move only while the main loop calls plat_sd_read_next: held up by the render
 * (the audio's interrupt, up to a block's time), the card waits, its clock stopped. With it the card streams on: after
 * the CMD18, SPI1's interrupt (above the render) looks for each block's start token and takes the block by DMA into a
 * free slot; plat_sd_read_next copies the oldest slot out and checks its CRC16 there. It stops with the slots full, at
 * the run's planned end (plat_sd_read_open's n: the CMD12 then goes in between blocks), at an error token or a silence
 * (SD_SILENT_MS); sd_stream_close stops it before any other byte goes out. Measured with an earlier driver on one FM-1
 * (2026-11-05, Optimist's drum streaming under a dense kit, the render 3.9 of 5.8 ms a block): 8-block runs 366 KB/s
 * polled, 702 KB/s this way.
 *
 * Every transfer is a multiple of 4 bytes into 4-byte aligned main RAM (the DMA's rule):
 *   HUNT  SDA_HUNT bytes, scanned for the token (0xFE; another byte not 0xFF: an error token, the end). The bytes
 *         after it are the block's head (copied by the CPU); the rest of the block and its CRC16 come by one DMA
 *         into the slot's tail. No token: another hunt.
 *   DATA  the slot full. The DMA's rounding bytes past the CRC16 (0 to 3) are the next hunt's first bytes. */
#define SDA_PRIO 4u                        /* above the audio (ALNK0 3), the input scan's own (TIMER5 4): never inside
                                            * usb_poll. At 5 (inside it, every ~7 us while the card waits) the USB MIDI
                                            * IN endpoint went silent on one FM-1 within minutes of a start, 3 starts of
                                            * 3, until a power cycle; at 4: none in 2 starts (2026-11-19). Why the
                                            * preemption does it is not known */
#define SDA_HUNT 16u                       /* bytes a hunt: ~7 us at 20 MHz between interrupts while the card waits */
#define SDA_TAIL 520u                      /* 514 + rounding, a multiple of 4 */
enum { SDA_OFF, SDA_HUNTING, SDA_DATA, SDA_FULL, SDA_ERR };

typedef struct {
    uint8_t tail[SDA_TAIL] __attribute__((aligned(4)));   /* the block's rest, its CRC16, the rounding */
    uint8_t head[SDA_HUNT];                /* its first k bytes, from the hunt that found the token */
    uint32_t k, len;                       /* head bytes; the tail DMA's length */
} sda_slot_t;
static sda_slot_t sda_slot[FM1B_SD_AHEAD] PLAT_POOL;
static uint8_t sda_hunt[SDA_HUNT] __attribute__((aligned(4))) PLAT_POOL;

static struct {
    volatile uint8_t state;
    uint8_t attached;                      /* the vector set (sd_ahead_attach, at the start) */
    volatile uint32_t wr, rd;              /* slots filled, taken */
    uint32_t limit;                        /* blocks this run may read (0: no limit) */
    uint32_t t0;                           /* the hunt's start (SD_TICKS) */
    uint32_t carry_n;
    uint8_t carry[4];
    uint32_t irqs, blocks, runs, errs;     /* for a console */
} sdx;

void isr_spi1(void);                       /* felucca-bsp chip/fm1_isr.S: FM1_ISR isr_spi1, fm1_spi1_irq */
void fm1_spi1_irq(void);

static void sdx_stop_hw(void)
{
    fm1_sd_rx_irq_stop();
    fm1_irq_mask(FM1_IRQ_SPI1);
}

static void sdx_fail(void)
{
    sdx.state = SDA_ERR;
    sdx.errs++;
    sdx_stop_hw();
}

/* b[0..n) of the hunt: a token starts the block's tail DMA, an error token ends the run, nothing: another hunt */
static void sdx_feed(const uint8_t *b, uint32_t n)
{
    sda_slot_t *s = &sda_slot[sdx.wr % FM1B_SD_AHEAD];
    uint32_t i, k;
    for (i = 0; i < n; i++) {
        if (b[i] == 0xFEu) {
            k = n - i - 1u;
            for (i++; i < n; i++)
                s->head[i - (n - k)] = b[i];
            s->k = k;
            s->len = (514u - k + 3u) & ~3u;
            sdx.state = SDA_DATA;
            fm1_sd_rx_irq_start(s->tail, s->len);
            return;
        }
        if (b[i] != 0xFFu) {
            sdx_fail();
            return;
        }
    }
    if (SD_TICKS() - sdx.t0 >= SD_SILENT_MS * 1000u * SD_TICKS_PER_US) {
        sdx_fail();
        return;
    }
    fm1_sd_rx_irq_start(sda_hunt, SDA_HUNT);
}

static void sdx_hunt(void)                 /* the next block's hunt, from the bytes carried over */
{
    uint32_t n = sdx.carry_n;
    sdx.carry_n = 0;
    sdx.t0 = SD_TICKS();
    sdx.state = SDA_HUNTING;
    sdx_feed(sdx.carry, n);
}

void fm1_spi1_irq(void)
{
    sda_slot_t *s;
    uint32_t i;
    fm1_sd_rx_irq_ack();
    sdx.irqs++;
    if (sdx.state == SDA_HUNTING) {
        sdx_feed(sda_hunt, SDA_HUNT);
    } else if (sdx.state == SDA_DATA) {
        s = &sda_slot[sdx.wr % FM1B_SD_AHEAD];
        for (i = 514u - s->k; i < s->len; i++)
            sdx.carry[sdx.carry_n++] = s->tail[i];
        sdx.wr++;
        sdx.blocks++;
        if (sdx.wr - sdx.rd < FM1B_SD_AHEAD && (!sdx.limit || sdx.wr < sdx.limit))
            sdx_hunt();
        else
            sdx.state = SDA_FULL;
    }
}

/* the vector, once, at the start (main_fm1.c, before the guard write-protects the vectors); the source stays masked
 * until a run */
static void sd_ahead_attach(void)
{
    uint32_t f = irq_save();
    fm1_irq_attach(FM1_IRQ_SPI1, isr_spi1, SDA_PRIO);
    fm1_irq_mask(FM1_IRQ_SPI1);
    sdx.attached = 1;
    irq_restore(f);
}

static void sd_ahead_begin(uint32_t limit) /* after a CMD18 answered: its first block's hunt */
{
    uint32_t f;
    if (!sdx.attached || !sd.dma)          /* (only where the start proved SPI1's DMA reads) */
        return;
    f = irq_save();
    sdx.wr = sdx.rd = 0;
    sdx.limit = limit;
    sdx.carry_n = 0;
    sdx.runs++;
    fm1_irq_unmask(FM1_IRQ_SPI1);
    sdx_hunt();
    irq_restore(f);
}

static void sd_ahead_end(void)             /* stopped, a transfer in flight waited out: the bus the polled path's */
{
    uint32_t f, n;
    if (sdx.state == SDA_OFF)
        return;
    f = irq_save();
    if (sdx.state == SDA_HUNTING || sdx.state == SDA_DATA)
        for (n = 0; n < 1000000u && !fm1_sd_dma_done(); n++)
            ;
    sdx_stop_hw();
    sdx.state = SDA_OFF;
    irq_restore(f);
}

/* the oldest slot into p: 0, SD_E_CRC (damaged), SD_E_BUSY (none yet), SD_E_READ (the run ended: an error token or
 * a silence, every slot taken) */
static int sd_ahead_take(uint8_t *p)
{
    sda_slot_t *s;
    uint32_t i, k, f;
    uint16_t got;
    if (sdx.rd == sdx.wr)
        return sdx.state == SDA_ERR ? SD_E_READ : SD_E_BUSY;
    s = &sda_slot[sdx.rd % FM1B_SD_AHEAD];
    k = s->k;
    for (i = 0; i < k; i++)
        p[i] = s->head[i];
    for (i = k; i < 512u; i++)
        p[i] = s->tail[i - k];
    got = (uint16_t)(s->tail[512u - k] << 8 | s->tail[513u - k]);
    f = irq_save();
    sdx.rd++;
    if (sdx.state == SDA_FULL && (!sdx.limit || sdx.wr < sdx.limit))
        sdx_hunt();                        /* (a slot free again) */
    irq_restore(f);
    if (got != sd_crc16(p, 512u)) {
        sd.crc_errors++;
        return SD_E_CRC;
    }
    return 0;
}

#define sd_ahead_on() (sdx.state != SDA_OFF)

void plat_sd_ahead(uint32_t *c)            /* c[4]: interrupts, blocks, runs, runs ended by an error or a silence */
{
    c[0] = sdx.irqs;
    c[1] = sdx.blocks;
    c[2] = sdx.runs;
    c[3] = sdx.errs;
}
