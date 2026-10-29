// Keyboard & Mouse for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Azimuth executable the game runs in (the process' main image):
// azimuth.exe 1.0.4 (TimeDateStamp 0x6ae393eb, SizeOfImage 0x8a2000) or 1.0.2
// (0x6ae05dd1, 0x9af000). A branded copy that only adds a section (the Forza
// Horizon 2 Builder's 1.0.2 exe) has a larger SizeOfImage and is accepted.
#ifndef KBM_HOST_H
#define KBM_HOST_H
#include "common.h"

typedef enum { AZIMUTH_UNKNOWN, AZIMUTH_102, AZIMUTH_104 } AzimuthBuild;

// The host image: the process' main module (a test build can set another).
unsigned char *azimuth_host_base(void);
AzimuthBuild azimuth_host_build(const unsigned char *base);
const char *azimuth_host_name(AzimuthBuild build);
// "TimeDateStamp 0x..., SizeOfImage 0x..." of `base` for the log.
void azimuth_host_describe(const unsigned char *base, char *out, size_t size);

#ifdef KBM_TEST
void azimuth_host_set_test_base(unsigned char *base);
#endif
#endif
