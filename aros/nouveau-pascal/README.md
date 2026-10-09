# NVIDIA Pascal support for the AROS nouveau driver

A patch series for the `nouveau.hidd` that deadwood ported to AROS
([deadw00d/AROS](https://github.com/deadw00d/AROS), `workbench/hidds/nouveau`).
It makes GeForce 10-series and Quadro P-series cards get through
initialisation, and gives them a working display when the NVIDIA firmware
is missing.

This is a side project kept in the Zenith Linux repository. It does not
touch the Zenith Linux build.

> **Status: untested on real hardware.** The changes have been syntax-checked
> with a host compiler and the core fix has been run against a simulated
> SEC2 falcon. They have not been built with the AROS toolchain and have not
> been run on an NVIDIA card. See [Verification](#verification).

## Cards

| Chip  | nvkm id | GeForce | Quadro |
|-------|---------|---------|--------|
| GP102 | 0x132   | GTX 1080 Ti, Titan X (Pascal), Titan Xp | P6000 |
| GP104 | 0x134   | GTX 1070, GTX 1070 Ti, GTX 1080 | P4000, P5000 |
| GP106 | 0x136   | GTX 1060 | P2000, P2200 |
| GP107 | 0x137   | GTX 1050, GTX 1050 Ti | P400, P600, P620, P1000 |
| GP108 | 0x138   | GT 1030 | |

The driver takes any NVIDIA display controller of PCI class `0x0300` and
identifies the chip from the GPU itself, not from a list of products. A
Quadro takes the same code path and uses the same firmware as the GeForce
with the same chip. Quadros come with their own display caveats, see
[Quadro cards](#quadro-cards).

GP100 (Quadro GP100, Tesla P100) runs secure boot on the PMU like Maxwell 2,
so patches `0001` and `0002` don't affect it. It is untested.
GP10B (Tegra X2) is out of scope.

## What was wrong

The port is based on Linux 5.4.302 DRM, so the Pascal parts of nvkm (GP102
display, GP100 MMU, GP102 GR, SEC2, secure boot) were already compiled in,
and the hidd already mapped chipset `0x13x` to `NV_PASCAL` and the Fermi-style
2D path. Four things stood between that and a working card.

**1. Pascal hung forever during graphics engine init.**
On GP102 to GP108, secure boot runs NVIDIA's ACR on the SEC2 falcon. SEC2
then runs an NVIDIA RTOS. FECS and GPCCS, the falcons behind the graphics
engine, are started by posting `ACR_CMD_BOOTSTRAP_FALCON` to that RTOS
through a message queue, on every GR init. In the port that message queue
was stubbed out with `NOT_IMPLEMENTED_STOP`, which logs and then loops in
`while(1) Delay(1)`. With firmware installed, the first call,
`nvkm_msgqueue_ctor()`, never returned.

Maxwell 2 (GM20x) also needs signed firmware but runs its ACR on the PMU
without an RTOS, which is why only Pascal hit this.

The stubs were there because the compat layer had no `struct completion`
and `get_jiffies()` is itself unimplemented.

**2. Commands would have gone to the wrong place.**
Inside the same stub, `cmd_write()` lost its call to `cmd_queue_open()`.
That call takes the queue lock and reads the write position. Without it,
commands were written at a stale offset and a semaphore that was never
obtained was released.

**3. No firmware meant broken drawing instead of plain software drawing.**
Without firmware nvkm leaves the graphics engine out (`-ENODEV`) and
display keeps working. The hidd stored the BOOL result of acceleration setup
in a `LONG` and tested it for `< 0`, so the failure was never noticed and 2D
commands kept going to a GPU channel whose engine objects were never set up.

**4. MST-capable DisplayPort monitors would hang the driver.**
This one is not Pascal-specific, but Quadros only have DisplayPort
outputs. MST (multi-stream) was enabled by default, so a DP 1.2+ monitor
that advertises it (daisy-chain-capable monitors, MST hubs and docks) was
switched into MST mode. The MST code that follows is stubbed with
`NOT_IMPLEMENTED_STOP` (`drm_dp_mst_wait_tx_reply()`,
`drm_dp_validate_guid()`, `nv50_mstc_get_modes()`). It would hang the work
queue process that also runs display handling and SEC2 messages.

## The patches

| Patch | Change |
|-------|--------|
| `0001` | Adds completions to `drm-compat`: `init`/`reinit_completion`, `complete`, `complete_all`, `try_wait_for_completion`, `completion_done`, `wait_for_completion[_timeout]`, `DECLARE_COMPLETION_ONSTACK`, plus `msecs_to_jiffies`. Waiters poll in 1 ms steps, because `get_jiffies()` is unimplemented and `complete()` can run from the interrupt handler or the work queue process. |
| `0002` | Restores the falcon message queues. `msgqueue.h`, `msgqueue_0148cdec.c` (SEC2) and `msgqueue_0137c63d.c` (PMU) go back to the upstream 5.4.302 sources. `msgqueue.c` keeps one AROS change: the queue-full retry loop uses `udelay()` instead of jiffies. The msgqueue sections of `patches/drm-aros.diff` are refreshed to match. |
| `0003` | Adds `carddata->accel_enabled`. When acceleration setup fails, the hidd skips the GART buffer and pattern setup, sends `Clear`, `FillRect` and `CopyBox` to the software implementation, and refuses to create a Gallium object so Mesa uses its software renderer. Image transfers already take the CPU path without a GART buffer. Cards whose acceleration comes up take exactly the same path as before. |
| `0004` | Defaults `nouveau_mst` to 0 on AROS, which is what `nouveau.mst=0` does on Linux. MST-capable monitors are then driven in single-stream mode like any other DP monitor. The `nouveau_dp.c` section of `patches/drm-aros.diff` is refreshed to match. |

All four are against `deadw00d/AROS` master at
`5b5fd4cfd4f039927c0d1bb9cdd7f4fc8644d23e` (2026-10-01). The patches
record it as `base-commit`.

## Applying and building

```sh
git clone https://github.com/deadw00d/AROS.git
./apply.sh AROS            # git am --3way of patches/*.patch
```

Then build as described in `AROS/INSTALL.md`: copy `scripts/rebuild.sh`
next to the checkout, build the toolchain (option 1), then the target you
use, for example `core-pc-x86_64` (option 3).

## Firmware

Graphics acceleration on Pascal needs NVIDIA-signed firmware from
[linux-firmware](https://gitlab.com/kernel-firmware/linux-firmware).
The driver loads it from `DEVS:Firmware/nvidia/<chip>/`:

```
acr/   bl.bin  ucode_load.bin  ucode_unload.bin  unload_bl.bin
gr/    fecs_bl.bin  fecs_inst.bin  fecs_data.bin  fecs_sig.bin
       gpccs_bl.bin  gpccs_inst.bin  gpccs_data.bin  gpccs_sig.bin
       sw_ctx.bin  sw_nonctx.bin  sw_bundle_init.bin  sw_method_init.bin
nvdec/ scrubber.bin
sec2/  desc.bin  image.bin  sig.bin   (+ desc-1, image-1, sig-1 except on gp108)
```

From a Linux system with `linux-firmware` installed:

```sh
./install-firmware.sh /lib/firmware /path/to/AROS/Devs/Firmware
```

This copies `nvidia/gp102` to `nvidia/gp108`. It follows symlinks and
unpacks `.xz` and `.zst` files, since distributions often ship the firmware
that way and AROS needs plain files.

Without firmware the card should still give a display, drawn by the CPU,
because of patch `0003`.

## Quadro cards

The GPU side is the same as on GeForce. The outputs are what differ: Pascal
Quadros only have DisplayPort outputs, except P5000, P6000 and GP100, which
also have one DVI-D port. DisplayPort is the least proven part of
deadwood's port:

- In July 2026 the connector code was enabled, with the note "DisplayPort
  still can't be used though" (`31204f09`). A few days later i2c-over-AUX
  was added, "needed for DisplayPort output" (`1c452aa0`). No commit
  confirms that single-stream DP output works end to end.
- Patch `0004` turns MST off. A daisy chain then shows only the first
  monitor.

Safest setups, in order:

1. The DVI-D port on a P5000, P6000 or GP100.
2. A passive DP-to-HDMI or DP-to-DVI adapter. Upstream nouveau usually
   drives these through the HDMI/DVI (TMDS) path, which this port uses more.
   Not confirmed on AROS.
3. A plain single-stream DP monitor.

Not covered:

- Quadro T400, T600, T1000 and the Quadro RTX cards are Turing, not Pascal.
  The hidd rejects their chipset.
- Laptop Quadros in Optimus systems often appear as PCI class `0x0302`
  (3D controller). The AROS driver only looks for `0x0300`.

## What to look for on real hardware

The driver's debug output (`bug()`) shows errors from nvkm. Debug-level
messages are compiled out (`CONFIG_NOUVEAU_DEBUG` is 3).

- **Works:** no `NOT IMPLEMENTED STOP`, no
  `Acceleration not available, using software rendering`, and fast window
  dragging and scrolling.
- `FIRMWARE: failed to open firmware file: DEVS:Firmware/nvidia/<chip>/sec2/image-1.bin`
  and similar for `desc-1`/`sig-1`: harmless when the file without `-1`
  exists. The driver tries the newer version first.
- Any other `failed to open firmware file`: that file is missing, so
  acceleration is off.
- `secboot: error during falcon reset: -60` (`-ETIMEDOUT` in AROS's
  libc): SEC2 did not answer within 1 s. The likely cause is the SEC2 interrupt not reaching the driver.
  Check MSI and interrupt routing first.
- `Acceleration not available, using software rendering` with all firmware
  present: graphics engine init failed. The lines before it say why.
- A clean log but a black DisplayPort screen: DP output itself. Try DVI or
  a passive adapter, see [Quadro cards](#quadro-cards).

Expect lower 3D performance than on Linux with the NVIDIA driver. nouveau
cannot reclock Pascal, so the GPU stays at its boot clocks.

## Verification

What was done:

- **Host syntax/type check** (`verify/syntax-check.sh <AROS checkout>`).
  The changed DRM translation units are compiled with `-D__AROS__` and
  `-Werror=implicit-function-declaration` against the driver's real
  headers. `verify/shim/` stands in only for AROS system headers that an
  AROS build generates. It covers the falcon message queue sources and
  `nouveau_dp.c`. As a control, the restored upstream message queue
  compiled against the original compat headers fails with the errors that
  originally led to the stubs.
- **Simulation** (`verify/run-sim.sh <AROS checkout>`). The real
  `msgqueue.c`, `msgqueue_0148cdec.c` and `msgqueue_0137c63d.c`, built for
  AROS, run against a fake SEC2 falcon (`verify/sim/harness.c`). A firmware
  thread speaks the RTOS protocol: init message, command ring with REWIND
  markers, ACR replies. A second thread plays the work queue that the SEC2
  interrupt schedules. `get_jiffies()` aborts if called. It checks:
  - the completion primitives,
  - booting FECS and GPCCS when the RTOS comes up late,
  - 40 further boots with the command ring wrapping 27 times,
  - slow replies, a 1 s timeout when SEC2 never answers,
  - re-initialisation.

  All checks pass. Run with `--original` against an unpatched checkout,
  the same harness stops in `NOT IMPLEMENTED STOP nvkm_msgqueue_ctor`.
- **Review only** for patch `0003`. The hidd sources need generated
  OOP/HIDD/Gallium headers and were not compiled.
- **Code tracing** for patch `0004`. With MST off, the other MST entry
  points stay out of the stubs. On unplug, `nv50_mstm_remove()` returns
  early. On a DP short pulse, `nv50_mstm_service()` only reads the ESI
  registers, as it already did for non-MST monitors. Not run.

What was not done:

- No build with the AROS cross toolchain. The environment these patches
  were written in could not reach the GNU and unicode.org download hosts
  that the toolchain build needs.
- No run on an NVIDIA card.

## Known limitations

- **Late replies after a timeout.** This is the same as upstream Linux. If
  SEC2 answers after `acr_boot_falcon()` has timed out, the reply handler
  signals a completion on a stack frame that is gone. This can only happen
  after a timeout has already been reported.
- **Polled completions.** They poll every 1 ms. They are only used on the
  init path, where that is fine.
- **MST is off.** DP daisy chains show one monitor until the MST helpers
  are implemented.
- **Out of scope.** GP10B and reclocking. GP100 is untested.

## Files

```
patches/            git format-patch series for deadw00d/AROS
apply.sh            applies the series to a checkout
install-firmware.sh copies Pascal firmware from a Linux firmware tree
verify/             host syntax check and message queue simulator
```
