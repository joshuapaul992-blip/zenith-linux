# Linux drivers, vendored unmodified

These files are copied byte for byte from the Linux kernel, tag **v6.12**
(`https://raw.githubusercontent.com/torvalds/linux/v6.12/<path>`). They are
built through Kestrel's Linux KPI (`linuxkpi/`, see the "Linux drivers"
section of `kestrel/README.md`) against the KPI's own `<linux/...>` headers;
none of Linux's headers are copied. There are no patches: if one is ever
needed it goes in `patches/` next to this file, applied to a copy at build
time, so the files here stay as upstream has them.

| File here | Upstream path (v6.12) | License (SPDX) | SHA-256 |
|---|---|---|---|
| `drivers/misc/pvpanic/pvpanic.c` | `drivers/misc/pvpanic/pvpanic.c` | GPL-2.0+ | `ee907764d7540598b2189a43904c161c9dfb8575c24cc939c4ca54c5aba34659` |
| `drivers/misc/pvpanic/pvpanic.h` | `drivers/misc/pvpanic/pvpanic.h` | GPL-2.0+ | `3c0b1d6d904ce1974145c5a909552bc4ca8c0d09b055d01734efd82c357a3aa9` |
| `drivers/misc/pvpanic/pvpanic-pci.c` | `drivers/misc/pvpanic/pvpanic-pci.c` | GPL-2.0+ | `9ccb6e1c335ae93ca9a485fd6ae85f23d2fddd1b61a66f14a8f12e33c00c7edd` |
| `drivers/watchdog/i6300esb.c` | `drivers/watchdog/i6300esb.c` | GPL-2.0-or-later | `ce08519e7df3003a0e1baa79eb21fe9f2e50312d05487cb0b400745820453107` |

Each file keeps its SPDX line and copyright notices. GPL-2.0-or-later code
may be combined with Kestrel, which is GPL-3.0 (the repository's `LICENSE`):
the combination is distributed under GPLv3.

## What may be vendored

- **Yes:** files whose SPDX line is `GPL-2.0+` / `GPL-2.0-or-later`, MIT,
  BSD (2- or 3-clause), or a dual license with one of those (e.g.
  `GPL-2.0 OR MIT`, `Dual BSD/GPL`).
- **No:** `GPL-2.0` / `GPL-2.0-only` files, including `GPL-2.0 WITH
  Linux-syscall-note` UAPI headers. GPLv2-only code cannot be combined with
  GPLv3 code. Most of Linux's core (and most of its headers) is GPLv2-only,
  which is why the KPI's headers are written from scratch.
  For example `include/uapi/misc/pvpanic.h` (GPL-2.0 WITH Linux-syscall-note)
  is not copied: `linuxkpi/include/uapi/misc/pvpanic.h` defines the three
  event bits itself, from QEMU's documented device interface.
- Check the SPDX line of every file, not the directory: licenses differ file
  by file. `MODULE_LICENSE("GPL")` means "GPL v2 or later" in Linux's own
  convention, but the SPDX line is what counts.
