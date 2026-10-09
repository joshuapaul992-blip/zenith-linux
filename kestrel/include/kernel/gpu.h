/* include/kernel/gpu.h -- native GPU drivers (NVIDIA, stage 1: detection) */
#ifndef KESTREL_GPU_H
#define KESTREL_GPU_H

#include <stddef.h>

/* Find NVIDIA display functions on the PCI bus and probe them: chip, VBIOS,
 * DisplayPort outputs and monitors, firmware display state. Registers the
 * "NVIDIA GPU" report section and VBIOS.ROM. Call after pci_init(). */
void   gpu_probe(void);
int    gpu_count(void);
/* Monitors lit by the experimental modeset (nvidia.modeset=1) become
 * display heads; call after the boot frame buffer is registered. */
void   gpu_register_displays(void);
size_t gpu_report(char *buf, size_t cap);   /* every probed GPU, as text */

#endif
