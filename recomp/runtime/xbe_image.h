/* Loading default.xbe into guest memory at its link address. */
#pragma once
#include <stdint.h>

typedef struct {
    uint32_t base, size, entry, kernel_thunk, tls_dir;
    uint32_t num_sections, section_headers; /* guest addresses */
} XbeInfo;

/* Reads the XBE file and copies headers and sections to MEM(base). The
   range MEM(base)..MEM(base+size) must already be mapped read/write/exec.
   Returns 0 on failure after printing why. */
int xbe_load(const char *path, XbeInfo *info);
