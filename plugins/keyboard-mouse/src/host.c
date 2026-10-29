// Keyboard & Mouse for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "host.h"

#include <stdio.h>

static const struct {
    AzimuthBuild build;
    const char *name;
    uint32_t timestamp, image_size;
} hosts[] = {
    {AZIMUTH_104, "1.0.4", 0x6ae393ebu, 0x8a2000u},
    {AZIMUTH_102, "1.0.2", 0x6ae05dd1u, 0x9af000u},
};

#ifdef KBM_TEST
static unsigned char *test_base;
void azimuth_host_set_test_base(unsigned char *base) { test_base = base; }
#endif

unsigned char *azimuth_host_base(void) {
#ifdef KBM_TEST
    if (test_base) return test_base;
#endif
    return (unsigned char *)GetModuleHandleW(NULL);
}

static int headers(const unsigned char *base, IMAGE_NT_HEADERS64 *nt) {
    IMAGE_DOS_HEADER dos;
    return base && memory_read(base, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
           dos.e_lfanew >= 64 && dos.e_lfanew <= 4096 && memory_read(base + dos.e_lfanew, nt, sizeof(*nt)) &&
           nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
           !(nt->FileHeader.Characteristics & IMAGE_FILE_DLL) &&
           nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
}

AzimuthBuild azimuth_host_build(const unsigned char *base) {
    IMAGE_NT_HEADERS64 nt;
    if (!headers(base, &nt)) return AZIMUTH_UNKNOWN;
    for (unsigned i = 0; i < ARRAY_COUNT(hosts); ++i)
        if (nt.FileHeader.TimeDateStamp == hosts[i].timestamp && nt.OptionalHeader.SizeOfImage >= hosts[i].image_size)
            return hosts[i].build;
    return AZIMUTH_UNKNOWN;
}

const char *azimuth_host_name(AzimuthBuild build) {
    for (unsigned i = 0; i < ARRAY_COUNT(hosts); ++i)
        if (hosts[i].build == build) return hosts[i].name;
    return "unknown";
}

void azimuth_host_describe(const unsigned char *base, char *out, size_t size) {
    IMAGE_NT_HEADERS64 nt;
    if (headers(base, &nt))
        snprintf(out, size, "TimeDateStamp 0x%08lx, SizeOfImage 0x%lx", (unsigned long)nt.FileHeader.TimeDateStamp,
                 (unsigned long)nt.OptionalHeader.SizeOfImage);
    else
        snprintf(out, size, "not an x64 executable image");
}
