// Language Support for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Subtitles for spoken lines the game shows none for (Ben, Ashley, ANNA).
// Ported from features/voice_subtitles.c of the Extra Options fork
// (0.3.2-openbox), itself ported from the Ukrainian localization's standalone
// winmm.dll proxy.
//
// The speech parameter constructors and SubtitleDataSource::Update are hooked
// by the machine code in voice_subtitles_code.h (source:
// tools/voice_subtitles/voice_hooks.py): a voice event whose last path
// component is an id of the VoiceSubtitles.str table shows that text, for
// <id>~t seconds (the sample length) plus a margin, between 2 and 60 s. The
// display time follows the system clock (KUSER_SHARED_DATA.InterruptTime), so
// it does not depend on the frame rate.
//
// Unlike the fork, the code is also relocated for a game image that is not at
// its preferred base, and the five entries are written with every other
// thread of the process suspended (the plugin starts while the game runs).
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "lang_support.h"

#include <tlhelp32.h>
#include "voice_subtitles_code.h"

#define TABLE_FILE "VoiceSubtitles.str"
#define TABLE_FILE_W L"VoiceSubtitles.str"
#define GAME_PREFERRED_BASE 0x140000000ull  // the hook code is assembled for it
#define SET_TEXT_RVA 0xc3a4b0u              // ICinematicSubtitlesDisplay::SetText (voice_hooks.py SET_TEXT_VA)
#define TICKS_PER_SECOND 10000000.0         // KUSER_SHARED_DATA.InterruptTime units
#define DEFAULT_SECONDS 3.0
#define MIN_SECONDS 2.0
#define MAX_SECONDS 60.0
#define MARGIN_SECONDS 0.6
#define MAX_ARCHIVE_SIZE (512u << 20)
#define REACH 0x7ff00000ll  // rel32 reach, with a margin
#define MAX_THREADS 4096

typedef struct {
    unsigned long long hash, text;
    unsigned duration, reserved;
} VoiceEntry;

// The first 16 bytes of each hooked function in the game's executable (the
// fork checked the replaced bytes only), and SetText, which the code calls.
static const struct {
    uint32_t rva;
    unsigned char bytes[16];
} guards[] = {
    {0xf50ee0u, {0x4c, 0x89, 0x4c, 0x24, 0x20, 0x4c, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48}},
    {0xf51100u, {0x4c, 0x89, 0x4c, 0x24, 0x20, 0x4c, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48}},
    {0xf512a0u, {0x4c, 0x89, 0x4c, 0x24, 0x20, 0x4c, 0x89, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10, 0x48}},
    {0xc3df10u, {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8b, 0xec, 0x48, 0x83, 0xec, 0x58}},
    {0x85afa0u, {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24}},
    {SET_TEXT_RVA, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48}},
};
static const char *const guard_names[] = {
    "the PlaySpeech parameter constructor 0xf50ee0",      "the PreparePlayVO parameter constructor 0xf51100",
    "the PreparePlay3DVO parameter constructor 0xf512a0", "SubtitleDataSource::Update 0xc3df10",
    "the DialogueScript parser 0x85afa0",                 "ICinematicSubtitlesDisplay::SetText 0xc3a4b0",
};

static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }

static unsigned rd32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

static uint32_t site_rva(unsigned i) { return (uint32_t)(vc_hooks[i].site - GAME_PREFERRED_BASE); }

const char *voice_check(const unsigned char *game, uint32_t size) {
    static char why[160];
    if (!game) return "no game image";
    if (size != GAME_IMAGE_SIZE) return "the game image is not forza_durango_release_final.exe (image size)";
    for (unsigned i = 0; i < ARRAY_COUNT(guards); ++i) {
        if (!fh2_bytes_equal(game + guards[i].rva, guards[i].bytes, sizeof(guards[i].bytes))) {
            unsigned char first = 0;
            fh2_safe_read(game + guards[i].rva, &first, 1);
            snprintf(why, sizeof(why), "%s is not as inspected%s", guard_names[i],
                      first == 0xe9 || first == 0xe8 || first == 0xff ? " (another module hooks it)" : "");
            why[sizeof(why) - 1] = 0;
            return why;
        }
    }
    // The table of voice_subtitles_code.h agrees with the guards.
    for (unsigned i = 0; i < VC_N_HOOKS; ++i)
        if (site_rva(i) != guards[i].rva || memcmp(vc_hooks[i].orig, guards[i].bytes, vc_hooks[i].ndisp))
            return "the hook code's sites differ from the guards (voice_subtitles_code.h)";
    return NULL;
}

// ------------------------------------------------------------------ table

static unsigned char *load_file(const wchar_t *path, DWORD *size) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER length;
    unsigned char *data = NULL;
    DWORD read = 0;
    if (GetFileSizeEx(file, &length) && length.QuadPart > 0 && length.QuadPart <= MAX_ARCHIVE_SIZE) {
        data = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)length.QuadPart);
        if (data && (!ReadFile(file, data, (DWORD)length.QuadPart, &read, NULL) || read != (DWORD)length.QuadPart)) {
            HeapFree(GetProcessHeap(), 0, data);
            data = NULL;
        }
    }
    CloseHandle(file);
    *size = read;
    return data;
}

static int name_equal_ignore_case(const unsigned char *name, unsigned length, const char *text) {
    if (length != strlen(text)) return 0;
    for (unsigned i = 0; i < length; ++i) {
        unsigned char a = name[i], b = (unsigned char)text[i];
        if (a >= 'A' && a <= 'Z') a = (unsigned char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (unsigned char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

// Data of a stored entry in a zip image; NULL when missing or compressed.
static const unsigned char *zip_entry(const unsigned char *zip, DWORD size, const char *name, DWORD *entry_size,
                                      const char **why) {
    if (size < 22) return NULL;
    DWORD end = size - 22, lowest = size > 70000 ? size - 70000 : 0;
    while (end > lowest && rd32(zip + end) != 0x06054b50u) --end;
    if (rd32(zip + end) != 0x06054b50u) return NULL;
    unsigned count = rd16(zip + end + 10), position = rd32(zip + end + 16);
    for (unsigned i = 0; i < count && (unsigned long long)position + 46 <= size; ++i) {
        unsigned name_length = rd16(zip + position + 28), extra = rd16(zip + position + 30),
                 comment = rd16(zip + position + 32), local = rd32(zip + position + 42);
        if ((unsigned long long)position + 46 + name_length > size) return NULL;
        if (name_equal_ignore_case(zip + position + 46, name_length, name)) {
            unsigned method = rd16(zip + position + 10), stored = rd32(zip + position + 20);
            if (method != 0) {
                *why = TABLE_FILE " is compressed in the archive; it must be stored";
                return NULL;
            }
            if ((unsigned long long)local + 30 > size) return NULL;
            unsigned long long data = (unsigned long long)local + 30 + rd16(zip + local + 26) + rd16(zip + local + 28);
            if (data + stored > size) return NULL;
            *entry_size = stored;
            return zip + data;
        }
        position += 46 + name_length + extra + comment;
    }
    return NULL;
}

static unsigned long long fnv1a64(const char *text, size_t length) {
    unsigned long long hash = 0xcbf29ce484222325ull;
    while (length--) hash = (hash ^ (unsigned char)*text++) * 0x100000001b3ull;
    return hash;
}

static unsigned display_ticks(double seconds) {
    if (!(seconds >= MIN_SECONDS - MARGIN_SECONDS)) seconds = MIN_SECONDS - MARGIN_SECONDS;
    if (seconds > MAX_SECONDS) seconds = MAX_SECONDS;
    return (unsigned)(long long)((seconds + MARGIN_SECONDS) * TICKS_PER_SECOND + 0.5);
}

// "<digits>[.<digits>]" in UTF-16 (the <id>~t rows); negative or junk gives 0.
static double parse_seconds(const unsigned char *text, const unsigned char *end) {
    double value = 0, scale = 0;
    for (; text + 1 < end && rd16(text); text += 2) {
        unsigned c = rd16(text);
        if (c == '.' || c == ',') {
            if (scale) break;
            scale = 1;
        } else if (c >= '0' && c <= '9') {
            if (scale) {
                scale /= 10;
                value += (c - '0') * scale;
            } else {
                value = value * 10 + (c - '0');
            }
        } else if (c != ' ') {
            break;
        }
    }
    return value;
}

// LSB2 string table -> hook table and text pool. `pool_address` is where the
// hook code sees the pool. Returns the entry count.
static unsigned fill_table(const unsigned char *table, DWORD size, VoiceEntry *entries, unsigned char *pool,
                           unsigned long long pool_address, const char **why) {
    const unsigned char *end = table + size;
    if (size < 40 || memcmp(table, "LSB2", 4)) {
        *why = TABLE_FILE " is not an LSB2 string table";
        return 0;
    }
    unsigned text_section = rd32(table + 20), id_section = rd32(table + 24);
    if ((unsigned long long)text_section + 8 > size || (unsigned long long)id_section + 8 > size) {
        *why = TABLE_FILE " is truncated";
        return 0;
    }
    unsigned text_count = rd32(table + text_section + 4), id_count = rd32(table + id_section + 4);
    const unsigned char *texts = table + text_section + 8, *ids = table + id_section + 8;
    if ((unsigned long long)text_section + 8 + 6ull * (text_count + 1ull) > size ||
        (unsigned long long)id_section + 8 + 8ull * id_count > size) {
        *why = TABLE_FILE " is truncated";
        return 0;
    }
    const unsigned char *blob = texts + 6ull * (text_count + 1), *names = ids + 8ull * id_count;
    unsigned count = 0, used = 0;
    // Pass 0 adds the texts, pass 1 sets the durations from the <id>~t rows.
    for (int pass = 0; pass < 2; ++pass) {
        for (unsigned i = 0; i < id_count; ++i) {
            unsigned hash = rd16(ids + 8 * i), name_offset = rd32(ids + 8 * i + 4), k = 0;
            if (name_offset >= (size_t)(end - names)) continue;
            const char *id = (const char *)names + name_offset;
            size_t id_length = 0;
            while ((const unsigned char *)id + id_length < end && id[id_length]) ++id_length;
            if ((const unsigned char *)id + id_length >= end) continue;
            int is_time = id_length > 2 && id[id_length - 2] == '~' && id[id_length - 1] == 't';
            if (is_time != pass) continue;
            while (k < text_count && rd16(texts + 6 * k) != hash) ++k;
            if (k >= text_count) continue;
            unsigned long long first_offset = 2ull * rd32(texts + 6 * k + 2),
                               last_offset = 2ull * rd32(texts + 6 * (k + 1) + 2);
            if (first_offset > last_offset || last_offset > (unsigned long long)(end - blob)) continue;
            const unsigned char *first = blob + first_offset, *last = blob + last_offset;
            if (!is_time) {
                unsigned bytes = (unsigned)(last - first);
                if (count >= VC_TABLE_MAX || used + bytes + 2 > VC_POOL_SIZE) {
                    say("voice subtitle table full at %u lines; the rest is left out", count);
                    return count;
                }
                memcpy(pool + used, first, bytes);
                pool[used + bytes] = pool[used + bytes + 1] = 0;
                entries[count].hash = fnv1a64(id, id_length);
                entries[count].text = pool_address + used;
                entries[count].duration = display_ticks(DEFAULT_SECONDS);
                entries[count].reserved = 0;
                used += bytes + 2;
                ++count;
            } else {
                unsigned long long owner = fnv1a64(id, id_length - 2);
                for (unsigned j = 0; j < count; ++j) {
                    if (entries[j].hash == owner) {
                        entries[j].duration = display_ticks(parse_seconds(first, last));
                        break;
                    }
                }
            }
        }
    }
    if (!count) *why = TABLE_FILE " has no lines";
    return count;
}

// Loose <stringtables>\<archive>\VoiceSubtitles.str first, then the stored
// entry in <stringtables>\<archive>.zip. *file is to be freed.
static const unsigned char *load_table(const wchar_t *stringtables, const wchar_t *archive, unsigned char **file,
                                       DWORD *table_size, const char **why) {
    wchar_t path[LANG_PATH];
    DWORD size = 0;
    *file = NULL;
    *why = "no " TABLE_FILE " in the pack (media\\stripped\\stringtables\\<archive>\\ or <archive>.zip)";
    if (_snwprintf(path, ARRAY_COUNT(path), L"%ls\\%ls\\" TABLE_FILE_W, stringtables, archive) > 0 &&
        (*file = load_file(path, &size)) != NULL) {
        *table_size = size;
        return *file;
    }
    if (_snwprintf(path, ARRAY_COUNT(path), L"%ls\\%ls.zip", stringtables, archive) > 0 &&
        (*file = load_file(path, &size)) != NULL) {
        const unsigned char *table = zip_entry(*file, size, TABLE_FILE, table_size, why);
        if (table) return table;
        HeapFree(GetProcessHeap(), 0, *file);
        *file = NULL;
    }
    return NULL;
}

// ------------------------------------------------------------------- cave

static long long game_delta(const unsigned char *game) {
    return (long long)((uintptr_t)game - GAME_PREFERRED_BASE);
}

// Lowest and highest game address the cave code reaches by rel32: the hook
// sites (jumps into the cave) and the targets of the code's calls and jumps.
static void game_targets(const unsigned char *game, unsigned long long *low, unsigned long long *high) {
    *low = ~0ull;
    *high = 0;
    for (unsigned i = 0; i < VC_N_HOOKS + VC_N_REL_REL32; ++i) {
        unsigned long long target;
        if (i < VC_N_HOOKS) {
            target = vc_hooks[i].site;
        } else {
            unsigned offset = vc_rel_rel32[i - VC_N_HOOKS];
            int displacement;
            memcpy(&displacement, vc_code + (offset - VC_OFF_CODE), 4);
            target = VC_LINK_CAVE + offset + 4 + (long long)displacement;
        }
        target += (unsigned long long)game_delta(game);
        if (target < *low) *low = target;
        if (target > *high) *high = target;
    }
}

static int cave_reaches(unsigned long long cave, unsigned long long low, unsigned long long high) {
    return (long long)(cave + VC_CAVE_SIZE - low) < REACH && (long long)(high - cave) < REACH;
}

// Commits the cave in the nearest free memory past the game image (then
// below it) from which every rel32 of the hook code reaches its target.
static unsigned char *allocate_cave(const unsigned char *game) {
    SYSTEM_INFO system;
    unsigned long long low, high, granularity;
    GetSystemInfo(&system);
    granularity = system.dwAllocationGranularity ? system.dwAllocationGranularity : 0x10000;
    game_targets(game, &low, &high);
    for (unsigned long long address = (uintptr_t)game + GAME_IMAGE_SIZE;;) {  // upwards from the image's end
        MEMORY_BASIC_INFORMATION region;
        address = (address + granularity - 1) & ~(granularity - 1);
        if (!cave_reaches(address, low, high) || !VirtualQuery((void *)(uintptr_t)address, &region, sizeof(region)))
            break;
        unsigned long long end = (uintptr_t)region.BaseAddress + region.RegionSize;
        if (region.State == MEM_FREE && address + VC_CAVE_SIZE <= end) {
            void *cave = VirtualAlloc((void *)(uintptr_t)address, VC_CAVE_SIZE, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
            if ((uintptr_t)cave == address) return (unsigned char *)cave;
            if (cave) VirtualFree(cave, 0, MEM_RELEASE);
        }
        if (end <= address) break;
        address = end;
    }
    for (unsigned long long top = (uintptr_t)game; top > VC_CAVE_SIZE + granularity;) {  // downwards from its base
        MEMORY_BASIC_INFORMATION region;
        if (!VirtualQuery((void *)(uintptr_t)(top - 1), &region, sizeof(region))) break;
        unsigned long long base = (uintptr_t)region.BaseAddress;
        unsigned long long address = (top - VC_CAVE_SIZE) & ~(granularity - 1);
        if (!cave_reaches(address, low, high)) break;
        if (region.State == MEM_FREE && address >= base) {
            void *cave = VirtualAlloc((void *)(uintptr_t)address, VC_CAVE_SIZE, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
            if ((uintptr_t)cave == address) return (unsigned char *)cave;
            if (cave) VirtualFree(cave, 0, MEM_RELEASE);
        }
        if (base >= top) break;
        top = base;
    }
    return NULL;
}

// Copies the hook code into the cave and applies the relocation table: the
// cave's own pointers move with the cave, the rel32s to game code with the
// cave and the game image.
static void place_code(unsigned char *cave, const unsigned char *game) {
    long long delta = (long long)((uintptr_t)cave - VC_LINK_CAVE);
    memcpy(cave, vc_head, sizeof(vc_head));
    memcpy(cave + VC_OFF_CODE, vc_code, sizeof(vc_code));
    for (unsigned i = 0; i < VC_N_REL_ABS64; ++i) {
        unsigned long long value;
        memcpy(&value, cave + vc_rel_abs64[i], 8);
        value += (unsigned long long)delta;
        memcpy(cave + vc_rel_abs64[i], &value, 8);
    }
    for (unsigned i = 0; i < VC_N_REL_REL32; ++i) {
        int value;
        memcpy(&value, cave + vc_rel_rel32[i], 4);
        value = (int)(value - delta + game_delta(game));  // cave_reaches() keeps it in range
        memcpy(cave + vc_rel_rel32[i], &value, 4);
    }
}

// --------------------------------------------------------------- writing

static HANDLE suspended[MAX_THREADS];

static void resume_all(int count) {
    while (count-- > 0) {
        ResumeThread(suspended[count]);
        CloseHandle(suspended[count]);
    }
}

// Suspends every other thread of the process; -1 (all resumed) when one is
// stopped inside a byte about to change (at the first byte is fine: it then
// runs the whole new instruction).
static int suspend_others(unsigned char *const *sites) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return -1;
    DWORD self = GetCurrentThreadId(), process = GetCurrentProcessId();
    int found = 0, busy = 0;
    THREADENTRY32 entry;
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more && found < MAX_THREADS; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        if (SuspendThread(thread) == (DWORD)-1) {
            CloseHandle(thread);
            continue;
        }
        suspended[found++] = thread;
        CONTEXT context;
        memset(&context, 0, sizeof(context));
        context.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(thread, &context)) {  // also waits until it has really stopped
            for (unsigned i = 0; i < VC_N_HOOKS; ++i)
                if (context.Rip > (DWORD64)(uintptr_t)sites[i] &&
                    context.Rip < (DWORD64)(uintptr_t)(sites[i] + vc_hooks[i].ndisp))
                    busy = 1;
        } else {
            busy = 1;
        }
    }
    CloseHandle(snapshot);
    if (busy) {
        resume_all(found);
        return -1;
    }
    return found;
}

// All five entries or none; NULL or why not.
static const char *write_hooks(unsigned char *game, const unsigned char *cave) {
    unsigned char *sites[VC_N_HOOKS], patches[VC_N_HOOKS][8];
    DWORD protections[VC_N_HOOKS], ignored;
    for (unsigned i = 0; i < VC_N_HOOKS; ++i) {
        sites[i] = game + site_rva(i);
        long long relative = (long long)((uintptr_t)cave + vc_hooks[i].hook) - (long long)((uintptr_t)sites[i] + 5);
        if (relative != (int)relative || vc_hooks[i].ndisp < 5 || vc_hooks[i].ndisp > 8)
            return "the hook code is out of reach of the speech hooks";
        int displacement = (int)relative;
        patches[i][0] = 0xe9;
        memcpy(patches[i] + 1, &displacement, 4);
        for (unsigned k = 5; k < 8; ++k) patches[i][k] = 0x90;
    }
    for (unsigned i = 0; i < VC_N_HOOKS; ++i) {
        if (!VirtualProtect(sites[i], vc_hooks[i].ndisp, PAGE_EXECUTE_READWRITE, &protections[i])) {
            while (i--) VirtualProtect(sites[i], vc_hooks[i].ndisp, protections[i], &ignored);
            return "the speech hooks could not be made writable";
        }
    }
    int threads = -1;
    for (int attempt = 0; attempt < 200 && threads < 0; ++attempt) {
        threads = suspend_others(sites);
        if (threads < 0) Sleep(attempt < 20 ? 0 : 1);
    }
    const char *problem = threads < 0 ? "a game thread kept running inside the speech hooks" : NULL;
    for (unsigned i = 0; !problem && i < VC_N_HOOKS; ++i)
        if (memcmp(sites[i], vc_hooks[i].orig, vc_hooks[i].ndisp))
            problem = "another module changed the speech hooks at the same time";
    if (!problem) {
        for (unsigned i = 0; i < VC_N_HOOKS; ++i) {
            memcpy(sites[i], patches[i], vc_hooks[i].ndisp);
            FlushInstructionCache(GetCurrentProcess(), sites[i], vc_hooks[i].ndisp);
        }
    }
    if (threads >= 0) resume_all(threads);
    for (unsigned i = 0; i < VC_N_HOOKS; ++i) VirtualProtect(sites[i], vc_hooks[i].ndisp, protections[i], &ignored);
    return problem;
}

static unsigned char *installed_cave;

unsigned char *voice_cave(void) { return installed_cave; }

unsigned voice_install(unsigned char *game, const wchar_t *stringtables, const wchar_t *archive, const char **why) {
    const char *problem = voice_check(game, GAME_IMAGE_SIZE);
    if (problem) {
        *why = problem;
        return 0;
    }
    if (installed_cave) {
        *why = "the voice subtitles are already installed";
        return 0;
    }
    unsigned char *file = NULL;
    DWORD table_size = 0;
    const unsigned char *table = load_table(stringtables, archive, &file, &table_size, why);
    if (!table) return 0;
    unsigned char *cave = allocate_cave(game);
    unsigned long long count = 0;
    if (!cave) {
        HeapFree(GetProcessHeap(), 0, file);
        *why = "no free memory for the hook code within 2 GB of the game image";
        return 0;
    }
    place_code(cave, game);
    count = fill_table(table, table_size, (VoiceEntry *)(cave + VC_OFF_TABLE), cave + VC_OFF_POOL,
                       (uintptr_t)cave + VC_OFF_POOL, why);
    HeapFree(GetProcessHeap(), 0, file);
    if (!count) {
        VirtualFree(cave, 0, MEM_RELEASE);
        return 0;
    }
    // Published before the hooks go in: they match only from now on.
    memcpy(cave + VC_OFF_COUNT, &count, 8);
    problem = write_hooks(game, cave);
    if (problem) {
        VirtualFree(cave, 0, MEM_RELEASE);  // nothing jumps into it
        *why = problem;
        return 0;
    }
    installed_cave = cave;
    return (unsigned)count;
}

#ifdef LANG_SUPPORT_TEST


static VoiceEntry test_entries[VC_TABLE_MAX];
static unsigned char test_pool[VC_POOL_SIZE];
static unsigned test_count;

unsigned voice_test_table(const wchar_t *stringtables, const wchar_t *archive, const char **why) {
    unsigned char *file = NULL;
    DWORD size = 0;
    *why = NULL;
    test_count = 0;
    const unsigned char *table = load_table(stringtables, archive, &file, &size, why);
    if (table) {
        *why = NULL;
        test_count = fill_table(table, size, test_entries, test_pool, (uintptr_t)test_pool, why);
    }
    if (file) HeapFree(GetProcessHeap(), 0, file);
    return test_count;
}

int voice_test_entry(unsigned index, unsigned long long *hash, const wchar_t **text, unsigned *duration) {
    if (index >= test_count) return 0;
    *hash = test_entries[index].hash;
    *text = (const wchar_t *)(uintptr_t)test_entries[index].text;
    *duration = test_entries[index].duration;
    return 1;
}
#endif
