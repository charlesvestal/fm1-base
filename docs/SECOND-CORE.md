# Using both cores

The FM-1's chip has two cores. Your app runs on the first. The second core, when you turn it on, is a worker for
the render: in each audio block you hand it one job, do your own share on the first core, and wait for it. LOOM-8
renders half its voices and its effects this way. The second core comes from felucca-bsp's `chip/fm1_cpu1.h`
(Melodee's, by keremimo, under MPL-2.0 by agreement on felucca-bsp#5); fm1-base adds when it may start and the
calls in `include/plat.h`.

## Turn it on

Add this to your app's `fm1.toml`:

```toml
cpu2 = true
```

Two more keys change how it behaves. `tools/build.py` describes every key.

- `cpu2_fallback = true` keeps the FM-1 running on one core after the second core fails, until it is switched off.
  Without it, a failure restarts the FM-1 (see "When it fails").
- `cpu2_stack = 4096` gives the second core's jobs a bigger stack, in bytes (a multiple of 8, 1024 to 16384). The
  default is 2816. Use `plat_cpu2_stack()` to see how deep your jobs went.

Then start it in `app_init`. It can only start at power-on, before the audio and the timers, and `app_init` is that
moment:

```c
void app_init(void)
{
    /* ... */
    if (plat_cpu2_set(1) == 0)
        two_cores = 1;              /* else one core: do all the work yourself */
}
```

`plat_cpu2_set(1)` returns 0 when the second core runs, and -1 when it doesn't: it didn't answer, it was barred
after an earlier failure, or it's too late to start it. Your render has to work either way.

## Hand it a job

Split your render into two parts that don't write to the same memory, such as two halves of the voices, each into
its own buffer. In the render, hand one part to the second core, do the other, wait, and then mix:

```c
static void half_job(uint32_t arg)       /* runs on the second core */
{
    render_voices(4, 8, buf2);          /* voices 4..7 into its own buffer */
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    int on2 = plat_cpu2_on() && plat_cpu2_run(half_job, 0);
    render_voices(0, 4, buf1);          /* voices 0..3, here */
    if (!on2 || plat_cpu2_wait() != 0)
        half_job(0);                    /* it didn't take the job, or it dropped it: do it here */
    mix(buf1, buf2, lr, n, master_q12);
}
```

These calls do the work:

- `plat_cpu2_run(fn, arg)` hands it `fn(arg)` and returns 1, or returns 0 when it can't take the job. Then do the
  work yourself.
- `plat_cpu2_wait()` waits for the job and returns 0. It returns -1 only with `cpu2_fallback`, when the second core
  was given up and the job dropped. Then do the work yourself.
- `plat_cpu2_busy()` tells you whether the job is still running, without waiting.

It takes one job at a time, and every job must be back within 8 ms. A job that takes longer counts as a failure.
Keep each job well inside one audio block.

## Share data safely

The two cores run at once, so treat anything both of them touch as shared:

- Give each core its own output buffer, and combine them only after `plat_cpu2_wait()`.
- When a job and the first core pass flags or counters while both run (a queue of voices that each core takes from,
  as LOOM-8's `engine.c` does), make them `volatile`, and call `plat_cpu2_sync()` between a write and the read that
  depends on it on the other core.
- Use `plat_core()` when one function runs on either core and needs a scratch buffer per core. It returns 0 on the
  first core and 1 on the second.
- Don't change what a job reads (a voice's settings, a table) from the main loop while a job may be running. Change
  it in the render, before you hand the job over.

## Where its code lives

A job can be ordinary code in the flash. LOOM-8's jobs are. Both cores share one instruction cache.

For a hot job you want out of the way of the cache's misses, put it in RAM in section `.c1_text`:

```c
__attribute__((section(".c1_text"))) static void half_job(uint32_t arg) { /* ... */ }
```

`.c1_text` is copied to RAM at boot with `.ram_text`. Unlike `PLAT_RAM_CODE`, the build doesn't check its calls into
the flash, because the second core only runs while the first core waits for it in the audio interrupt, and the flash
is never off then. The RAM code region is shared, 32 KiB (24 KiB with Wi-Fi), so keep it for code that pays for its
room.

## When it fails

Some FM-1 units misread memory on the second core. fm1-base checks every job, and treats these as failures: a job
it misread, a fault on the second core, a job not back within 8 ms, and no answer at its start.

Without `cpu2_fallback`, a failure restarts the FM-1, and the crash record says why (vector 0x101 to 0x105, the same
numbers `plat_cpu2_gave_up()` returns). Two failed starts in a row bring up safe mode, as any crash at start does.
Read the record with `tools/fm1_remote.py log`, or with "Report a problem" on the installer page.

With `cpu2_fallback = true`, a failure doesn't restart the FM-1. The second core is held, `plat_cpu2_wait()` returns
-1 for the job it dropped, and the FM-1 runs on one core until it's switched off, or until a new build is installed.

Your app can also offer one core as a setting, as LOOM-8 does on its Debug page:

```c
if (plat_cpu2_barred() && !proj.one_core)
    proj.one_core = 1;                  /* it failed with two cores: one from now on */
if (!proj.one_core)
    plat_cpu2_set(1);
```

These calls tell you what happened:

- `plat_cpu2_gave_up()` returns why it's off: 0 it isn't, 1 it misread a job, 2 a job was late, 3 it's barred, 4 it
  faulted, 5 it didn't answer at its start.
- `plat_cpu2_barred()` returns 1 when it stays off until power-off.
- `plat_cpu2_bad()` counts the jobs it misread.

## The supplies

fm1-base sets the chip's supply rails for you. At boot, before anything loads them, it raises VDC14 to 1.60 V and
SYSVDD to level 15, M-VAVE's own levels (`device/main_fm1.c`), and reads them back. At the boot loader's lower
levels, flash reads come back wrong under load. felucca-bsp's `chip/fm1_sys.h` explains the rails. Your app doesn't
set them, and there's no call to read them. `tools/fm1_remote.py log` prints them as the boot loader left them and as
they are now.

`cpu_clock = 396` in `fm1.toml` runs the CPU at 396 MHz instead of the stock 360. It's off by default and past
JieLi's rating for mass production, so try it on several FM-1s before a release. The build warns you on every
build that has it.

## In the simulator

The host simulator has no second core. `plat_cpu2_run` runs the job at once, in order, so the result is the same as
on the FM-1, and `plat_core()` returns 1 while it runs. An app that wants its own simulation of the second core
defines `APP_HOST_CPU2` and provides all the `plat_cpu2_*` calls itself, as LOOM-8's `host.c` does.

## Check it on the FM-1

- Measure with real notes sounding. `plat_cpu_pct` shows the render's load; compare one core and two.
- Check `plat_cpu2_stack()` after your heaviest case, and raise `cpu2_stack` if it comes near the limit.
- Run it long, on more than one FM-1 if you can, and read `tools/fm1_remote.py crashes` afterwards.
