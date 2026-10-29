// Custom Radio for Forza Horizon 2 Extra Options: the radio station.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from the custom radio of the Extra Options fork (features/radio_station.c,
// pull request "custom radio"), as a mod-loader plugin.
#include "radio.h"
#include "hooks.h"
#include "mp3.h"

/* Native "My Music" radio station.

   The game reads media\audio\RadioInfo_<lang>.xml through its CXML4Resource
   loader and builds one station per <RadioStation>. Each playlist entry names
   an FMOD Studio event plus a SoundName key; track playback (0x1018840) asks
   Studio::System::getSoundInfo(key) for the stream parameters, opens the sound
   with System::createSound(... | CREATESTREAM | NONBLOCKING), takes the FSB
   subsound once it is ready (0x1022180) and hands it to the event's programmer
   instrument. Everything therefore plays on the game's radio bus, with its
   volume, ducking, pause and station banner.

   Hooks (all verified byte-for-byte first; any mismatch leaves the game alone,
   see hooks.h; each one calls whatever was at its site before):
   - CXML4Resource read slot: after the file is read, RadioInfo text gets one
     extra <RadioStation> whose entries use FH2MUSIC_<n> sound keys.
   - getSoundInfo call in track playback: FH2MUSIC_<n> keys answer with the MP3
     file loaded in memory (FMOD_OPENMEMORY_POINT); other keys go to FMOD.
   - getSubSound call in the open poll: a plain MP3 stream has no subsounds, so
     our marked index returns the stream itself.
   - radio player update slot: the RadioNext/RadioRestart keys (F7/F6) move
     the playing custom track's event timeline (next track / restart), on the
     radio's own thread.
   - RadioModule::IsStationUnlocked call sites (D-pad cycle, state machine, UI):
     the custom station always counts as unlocked, whatever the progression
     (and every station with UnlockAllStations=1).
   - RadioModule::ChangeStation call sites (D-pad): the custom station is part
     of the cycle.
   - The RadioStation key (F9) on the game thread, from the local car update:
     calls the game's own RadioModule::SetStation(name) path used by
     RadioSetStation state nodes, which hands the name to the deferred audio
     manager.
   Settings: CustomRadio.ini beside the plugin. */
#define SOUND_KEY_PREFIX "FH2MUSIC_"
#define SILENCE_KEY "FH2MUSIC_SILENCE"

enum {
    ENTRY_LENGTH_MS = 3600000,
    NAME_PADDING = 96,
    PLAYER_HOLDER_OFFSET = 0x30,
    STATION_SLOTS_OFFSET = 8,
    STATION_SLOT_SIZE = 0x20,
    STATION_SLOT_COUNT = 3,
    SLOT_CLOCK_OFFSET = 0x18,
    FREEROAM_PLAYLIST_OFFSET = 0x68,
    EVENT_PLAYLIST_OFFSET = 0x78,
    PLAYLIST_CURSOR_OFFSET = 0x68,
    STATION_HISTORY_OFFSET = 0x160,
    GUARD_WINDOW_MS = 2000,
    GUARD_LIMIT = 6,
    GUARD_PAUSE_MS = 30000,
    ENTRY_DISPLAY_OFFSET = 0x40,
    ENTRY_ARTIST_OFFSET = 0x60,
    PLAYER_START_OFFSET = 0x28,
    PLAYER_POSITION_OFFSET = 0x2c,
    END_MARGIN_MS = 400,
    BACK_WINDOW_MS = 3000,
    MODULE_WRAPPER_OFFSET = 0x60,
    MODULE_MAP_OFFSET = 0x68,
    WRAPPER_CURRENT_OFFSET = 0x38,
    MAP_NODE_NAME_OFFSET = 0x28,
    MAX_MAP_STATIONS = 32,
    STATION_NUMBER_OFFSET = 0x108,
    PLAYER_INSTANCE_OFFSET = 0x10,
    PLAYER_STATE_OFFSET = 0x20,
    PLAYER_STATE_PLAYING = 3,
    MAX_TRACKS = 2048,
    MIN_TRACK_MS = 3000,
    MAX_FILE_SIZE = 256 * 1024 * 1024,
    BUFFER_SLOTS = 5,
    BUFFER_IDLE_MS = 15000,
    PLAYER_SLOTS = 8,
    REQUEST_TTL_MS = 1000,
    SUBSOUND_MARK = 0x46480000,
    SUBSOUND_MASK = 0xffff0000,
    SILENCE_INDEX = 0xffff,
    SILENCE_RATE = 22050,
    SILENCE_SAMPLES = SILENCE_RATE * 4 / 10,
    FMOD_OPENMEMORY_POINT = 0x10000000,
    FMOD_ACCURATETIME = 0x4000,
    EXINFO_SIZE = 0xd8
};

// A game std::shared_ptr (object, control block).
typedef struct {
    void *object, *counter;
} GameShared;

typedef unsigned char (*XmlReadFn)(void *, void *, void *, void *);
typedef void (*PlayerUpdateFn)(void *, float);
typedef int (*SoundInfoFn)(void *, const char *, void *);
typedef int (*SubSoundFn)(void *, int, void **);
typedef int (*SetTimelineFn)(void *, int);
typedef int (*SetVolumeFn)(void *, float);
typedef void (*SetStationFn)(void *, const void *);
typedef unsigned char (*StationUnlockedFn)(void *, const void *);
typedef void (*ChangeStationFn)(void *, int);
typedef void (*ModuleGetterFn)(void *, GameShared *);
typedef void *(*GameAllocFn)(size_t);
typedef void (*GameFreeFn)(void *);
typedef void (*ObjectActionFn)(void *);

typedef struct {
    void *data;
    unsigned track;
    uint64_t handed_at;  // last time it was served or still playing
    DWORD size;
    int handed;          // given to FMOD at least once
    int pcm;
} SoundBuffer;

typedef struct {
    void *player;
    unsigned track;
} PlayerTrack;

static unsigned char *image;
static volatile LONG installed;
static XmlReadFn original_read;
static PlayerUpdateFn original_update;
static SoundInfoFn original_sound_info;
static SubSoundFn original_subsound;
static SetTimelineFn set_timeline;
static SetVolumeFn set_volume;
static float gain = 1.0f;
// Alternative playback modes kept off (they were for bisecting behaviour).
static const int single_entry = 0, zero_offset_always = 0, rename_entries = 0, shift_keys = 0;
// UnlockAllStations.
static int unlock_all;
static volatile LONG unlock_all_live;
// PcmDecode: decode MP3s with Windows' codec instead of FMOD's.
static int pcm_decode = 1;
// Radio-thread playback state.
//  - per-track mode (default): the radio picks entry e by its own station clock
//    and is served file (e + shift); F7/F6 move `shift`.
//  - single-entry mode: one long entry, always served the file at `cursor`.
static unsigned cursor, served_track, served_entry, served_end;
static int shift, served_valid, served_done, served_position, served_moved;
static volatile LONG zero_offset_pending;
// The playlist entry last served (its SoundName text is the entry's first field).
static const void *served_entry_address;
// F7/F6 transitions: further presses wait for the next serve; a watchdog on the
// game thread re-tunes the station if no serve follows.
static volatile LONG serve_count, transition_serve;
static volatile LONG64 transition_at;
// RadioNext/RadioRestart presses from the game thread (GetTickCount64 of the
// press, 0 = none), taken by the playing custom player's update within 1 s.
static volatile LONG64 next_request, restart_request;
// Playlist next-pick hook (radio thread): the custom station always continues
// in folder order from the served entry, wrapping at both ends; F6 asks for -1.
// The step holds until the next serve: one transition runs GetNext more than
// once (the item actually played is not the first pick), so every pick until
// then must aim at the same entry.
typedef void *(*PlaylistNextFn)(void *, GameShared *, int);
static PlaylistNextFn original_next;
static volatile LONG next_step = 1;
static int wrap_logged;
// The station re-picks (0x1009090) while a pick is in its recently-played list
// (station+0x160); for the steered custom station that list is ignored, or a
// step back to a played track would loop forever on the radio thread.
typedef void **(*HistoryFindFn)(void **, void *, void *, void *);
static HistoryFindFn original_find;
// File position (ms) the playing custom track started from (its live offset or 0).
static int played_from;
static uint64_t guard_window, guard_until;
static unsigned guard_count;
static SetStationFn set_station;
static StationUnlockedFn original_unlocked;
static ChangeStationFn original_change;
static ModuleGetterFn module_getter;
static GameAllocFn game_alloc;
static GameFreeFn game_free;

static wchar_t folder[FOLDER_CAPACITY];
static char station_name[TEXT_CAPACITY];
static char injected_name[TEXT_CAPACITY];

// MSVC std::string as the game lays it out.
typedef struct {
    union {
        char inline_text[16];
        char *heap_text;
    };
    size_t size, capacity;
} GameString;

static SRWLOCK lock = SRWLOCK_INIT;
static Track *tracks;
static unsigned track_count;
static int scanned;
static volatile LONG station_number;
static SoundBuffer buffers[BUFFER_SLOTS];
static PlayerTrack players[PLAYER_SLOTS];
static unsigned char silence[44 + SILENCE_SAMPLES * 2];

static void release_shared(GameShared *value) {
    unsigned char *counter = value->counter;
    if (counter && InterlockedDecrement((volatile LONG *)(counter + 8)) == 0) {
        ((ObjectActionFn)((uintptr_t *)*(void **)counter)[0])(counter);
        if (InterlockedDecrement((volatile LONG *)(counter + 12)) == 0) {
            ((ObjectActionFn)((uintptr_t *)*(void **)counter)[1])(counter);
        }
    }
    value->object = value->counter = NULL;
}

// --- text helpers ---------------------------------------------------------------

static int lower(int character) {
    return character >= 'A' && character <= 'Z' ? character + 'a' - 'A' : character;
}

static const char *find_text(const char *text, size_t length, const char *needle, int last) {
    size_t count = text_length(needle);
    const char *found = NULL;
    for (size_t index = 0; count && index + count <= length; ++index) {
        size_t matched = 0;
        while (matched < count && lower(text[index + matched]) == lower(needle[matched])) {
            ++matched;
        }
        if (matched == count) {
            found = text + index;
            if (!last) {
                return found;
            }
        }
    }
    return found;
}

// Growing output buffer for the generated XML.
typedef struct {
    char *data;
    size_t length, capacity;
    int failed;
} Output;

static void put(Output *out, const char *text, size_t length) {
    if (out->failed || out->length + length + 1 > out->capacity) {
        out->failed = 1;
        return;
    }
    memory_copy(out->data + out->length, text, length);
    out->length += length;
    out->data[out->length] = 0;
}

static void put_text(Output *out, const char *text) {
    put(out, text, text_length(text));
}

static void put_number(Output *out, unsigned value) {
    char digits[12];
    char *at = digits + sizeof(digits);
    do {
        *--at = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    put(out, at, (size_t)(digits + sizeof(digits) - at));
}

// The game's own RadioInfo files carry raw '&' in attributes ("Camo & Crooked"),
// so its parser does not decode entities: keep '&' and replace the characters
// that would end an attribute or tag.
static void put_escaped(Output *out, const char *text) {
    for (; *text; ++text) {
        char character = *text == '"' ? '\'' : *text == '<' ? '(' : *text == '>' ? ')' : *text;
        if ((unsigned char)character >= 32) {
            put(out, &character, 1);
        }
    }
}

// --- tracks ---------------------------------------------------------------------

static int track_path(unsigned index, wchar_t *path, size_t capacity) {
    path[0] = 0;
    return wide_append(path, capacity, folder) && wide_append(path, capacity, L"\\") &&
           wide_append(path, capacity, tracks[index].name);
}

static void scan_tracks(void) {
    static wchar_t search[FOLDER_CAPACITY + 8], path[FOLDER_CAPACITY + MAX_PATH + 2];
    scanned = 1;
    tracks = HeapAlloc(GetProcessHeap(), 0, MAX_TRACKS * sizeof(*tracks));
    search[0] = 0;
    if (!tracks || !wide_append(search, ARRAY_COUNT(search), folder) ||
        !wide_append(search, ARRAY_COUNT(search), L"\\*.mp3")) {
        return;
    }
    WIN32_FIND_DATAW entry;
    HANDLE find = FindFirstFileW(search, &entry);
    if (find == INVALID_HANDLE_VALUE) {
        log_append_wide("CUSTOM RADIO: no .mp3 files in ", folder);
        return;
    }
    do {
        size_t length = wide_length(entry.cFileName);
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || length >= MAX_PATH ||
            track_count >= MAX_TRACKS || entry.nFileSizeHigh ||
            entry.nFileSizeLow > MAX_FILE_SIZE) {
            continue;
        }
        Track *track = &tracks[track_count];
        memory_copy(track->name, entry.cFileName, (length + 1) * sizeof(wchar_t));
        if (!track_path(track_count, path, ARRAY_COUNT(path))) {
            continue;
        }
        mp3_describe(path, track);
        if (track->duration < MIN_TRACK_MS) {
            log_append_wide("CUSTOM RADIO: skipped (unknown length): ", track->name);
            continue;
        }
        ++track_count;
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    log_append_value("CUSTOM RADIO: playable MP3 files: ", (int)track_count);
}

// --- XML injection ------------------------------------------------------------

static unsigned highest_station_number(const char *text, size_t length) {
    unsigned highest = 0;
    const char *at = text, *end = text + length;
    while ((at = find_text(at, (size_t)(end - at), "Number=\"", 0)) != NULL) {
        at += 8;
        unsigned value = 0;
        while (at < end && *at >= '0' && *at <= '9' && value < 100000) {
            value = value * 10 + (unsigned)(*at++ - '0');
        }
        if (value > highest) {
            highest = value;
        }
    }
    return highest;
}

// Names are padded with spaces so the game's strings get heap buffers that can
// later be rewritten in place with the title of the file actually served.
static void put_name(Output *out, const char *text) {
    size_t mark = out->length;
    put_escaped(out, text);
    while (rename_entries && out->length - mark < NAME_PADDING && !out->failed) {
        put(out, " ", 1);
    }
}

static void put_entry(Output *out, unsigned key, unsigned length, const char *title,
                      const char *artist) {
    unsigned drop = length / 3 < 10000 ? length / 3 : 10000;
    put_text(out, "        <Entry EventName=\"/Master/Radio/Track\" SoundName=\"" SOUND_KEY_PREFIX);
    put_number(out, key);
    put_text(out, "\" SampleLength=\"");
    put_number(out, length);
    put_text(out, "\" DisplayName=\"");
    put_name(out, title);
    put_text(out, "\" Artist=\"");
    put_name(out, artist);
    put_text(out, "\" TrackStart=\"0\" DJDrop=\"");
    put_number(out, drop);
    put_text(out, "\" TrackDrop=\"");
    put_number(out, drop);
    put_text(out, "\" TrackBreakDown=\"");
    put_number(out, length / 2);
    put_text(out, "\" StingerStart=\"");
    put_number(out, length - 1500);
    put_text(out, "\" DJStart=\"");
    put_number(out, length - 1000);
    put_text(out, "\" End=\"");
    put_number(out, length);
    put_text(out, "\"/>\r\n");
}

static void put_tracks(Output *out, const char *type) {
    put_text(out, "      <Playlist Type=\"");
    put_text(out, type);
    put_text(out, "\">\r\n");
    if (single_entry) {
        put_entry(out, 0, ENTRY_LENGTH_MS, "", "");
    } else {
        for (unsigned index = 0; index < track_count; ++index) {
            put_entry(out, index, tracks[index].duration, tracks[index].title,
                      tracks[index].artist);
        }
    }
    put_text(out, "      </Playlist>\r\n");
}

static void put_stingers(Output *out, const char *type) {
    put_text(out, "      <Playlist Type=\"");
    put_text(out, type);
    put_text(out, "\">\r\n        <Entry EventName=\"/Master/Radio/Stinger\" SoundName=\"" SILENCE_KEY
                  "\" SampleLength=\"400\" StartNextTrack=\"200\" End=\"400\"/>\r\n"
                  "      </Playlist>\r\n");
}

static int build_station(Output *out, unsigned number, int polish) {
    put_text(out, "    <RadioStation Name=\"");
    size_t mark = out->length;
    put_escaped(out, station_name[0] ? station_name : polish ? "MOJA MUZYKA" : "MY MUSIC");
    if (!out->failed && out->length - mark < sizeof(injected_name)) {
        memory_copy(injected_name, out->data + mark, out->length - mark);
        injected_name[out->length - mark] = 0;
    }
    put_text(out, "\" Number=\"");
    put_number(out, number);
    put_text(out, "\" RecentlyPlayedListSize=\"-1\">\r\n");
    put_tracks(out, "FreeroamTracks");
    put_tracks(out, "EventTracks");
    put_stingers(out, "Stingers");
    put_stingers(out, "ShortStingers");
    put_text(out, "      <Playlist Type=\"DJ\">\r\n      </Playlist>\r\n    </RadioStation>\r\n");
    return !out->failed;
}

static const char *resource_name(const unsigned char *resource, size_t *length) {
    const char *name = (const char *)(resource + 0xb8);
    size_t capacity;
    if (!memory_read(resource + 0xc8, length, sizeof(*length)) ||
        !memory_read(resource + 0xd0, &capacity, sizeof(capacity)) || !*length ||
        *length > 4096 || capacity < *length) {
        return NULL;
    }
    if (capacity >= 16 && !memory_read(resource + 0xb8, &name, sizeof(name))) {
        return NULL;
    }
    return name;
}

static void inject_station(unsigned char *resource) {
    size_t name_length;
    const char *name = resource_name(resource, &name_length);
    if (!name || !find_text(name, name_length, "radioinfo", 0)) {
        return;
    }
    char **buffer = (char **)(resource + 0xe0);
    uint32_t *length = (uint32_t *)(resource + 0xe8);
    if (!*buffer || !*length || *length > 8u * 1024u * 1024u) {
        return;
    }
    AcquireSRWLockExclusive(&lock);
    if (!scanned) {
        scan_tracks();
    }
    ReleaseSRWLockExclusive(&lock);
    if (!track_count) {
        return;
    }
    const char *close = find_text(*buffer, *length, "</RadioStations>", 1);
    if (!close) {
        log_append("CUSTOM RADIO: RadioInfo layout not recognised; station not added.\r\n");
        return;
    }
    while (close > *buffer && (close[-1] == ' ' || close[-1] == '\t')) {
        --close;  // insert at the start of the closing tag's line
    }
    unsigned number = highest_station_number(*buffer, *length) + 1;
    Output station = {0};
    station.capacity = (size_t)track_count * 2 * (3 * TEXT_CAPACITY * 2 + 512) + 4096;
    station.data = HeapAlloc(GetProcessHeap(), 0, station.capacity);
    if (!station.data ||
        !build_station(&station, number, find_text(name, name_length, "_pl.", 0) != NULL)) {
        log_append("CUSTOM RADIO: station XML could not be built.\r\n");
        if (station.data) {
            HeapFree(GetProcessHeap(), 0, station.data);
        }
        return;
    }
    size_t before = (size_t)(close - *buffer), total = *length + station.length;
    char *replacement = game_alloc(total + 1);
    if (replacement) {
        memory_copy(replacement, *buffer, before);
        memory_copy(replacement + before, station.data, station.length);
        memory_copy(replacement + before + station.length, *buffer + before, *length - before);
        replacement[total] = 0;
        char *original = *buffer;
        *buffer = replacement;
        *length = (uint32_t)total;
        game_free(original);
        InterlockedExchange(&station_number, (LONG)number);
        log_append_value("CUSTOM RADIO: custom station added as number ", (int)number);
    } else {
        log_append("CUSTOM RADIO: station XML allocation failed.\r\n");
    }
    HeapFree(GetProcessHeap(), 0, station.data);
}

static unsigned char xml_read_hook(void *resource, void *a, void *b, void *c) {
    unsigned char result = original_read(resource, a, b, c);
    if (result) {
        inject_station(resource);
    }
    return result;
}

// --- FMOD sound hooks ---------------------------------------------------------

static void build_silence(void) {
    static const unsigned char header[44] = {
        'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
        16, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 16, 0,
        'd', 'a', 't', 'a', 0, 0, 0, 0};
    uint32_t data = SILENCE_SAMPLES * 2, riff = data + 36, rate = SILENCE_RATE,
             bytes = SILENCE_RATE * 2;
    memory_copy(silence, header, sizeof(header));
    memory_copy(silence + 4, &riff, 4);
    memory_copy(silence + 24, &rate, 4);
    memory_copy(silence + 28, &bytes, 4);
    memory_copy(silence + 40, &data, 4);
}

static volatile LONG prefetch_request = -1;

static void *read_track_file(unsigned index, DWORD *size) {
    wchar_t path[FOLDER_CAPACITY + MAX_PATH + 2];
    if (!track_path(index, path, ARRAY_COUNT(path))) {
        return NULL;
    }
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    LARGE_INTEGER length;
    void *data = NULL;
    *size = 0;
    if (GetFileSizeEx(file, &length) && length.QuadPart > 0 && length.QuadPart <= MAX_FILE_SIZE) {
        data = HeapAlloc(GetProcessHeap(), 0, (SIZE_T)length.QuadPart);
        if (data && (!ReadFile(file, data, (DWORD)length.QuadPart, size, NULL) ||
                     *size != (DWORD)length.QuadPart)) {
            HeapFree(GetProcessHeap(), 0, data);
            data = NULL;
        }
    }
    CloseHandle(file);
    return data;
}

// --- buffer cache (lock held by callers of the cache_* helpers) --------------------
// Buffers given to FMOD stay alive while their track plays and for 15 s after;
// prefetched ones FMOD never saw can be dropped at any time.

static SoundBuffer *cache_find(unsigned index) {
    for (unsigned i = 0; i < BUFFER_SLOTS; ++i) {
        if (buffers[i].data && buffers[i].track == index) {
            return &buffers[i];
        }
    }
    return NULL;
}

static SoundBuffer *cache_free_slot(unsigned keep) {
    uint64_t now = GetTickCount64();
    SoundBuffer *slot = NULL;
    for (unsigned i = 0; i < BUFFER_SLOTS && !slot; ++i) {
        if (!buffers[i].data) {
            slot = &buffers[i];
        }
    }
    for (unsigned i = 0; i < BUFFER_SLOTS && !slot; ++i) {
        if (!buffers[i].handed && buffers[i].track != keep) {
            slot = &buffers[i];
        }
    }
    for (unsigned i = 0; i < BUFFER_SLOTS && !slot; ++i) {
        if (buffers[i].track != keep && buffers[i].track != served_track &&
            now - buffers[i].handed_at > BUFFER_IDLE_MS) {
            slot = &buffers[i];
        }
    }
    if (slot && slot->data) {
        HeapFree(GetProcessHeap(), 0, slot->data);
        memory_zero(slot, sizeof(*slot));
    }
    return slot;
}

// Copies a cached buffer out, marking it as given to FMOD when `hand` is set.
static void cache_take(SoundBuffer *slot, int hand, void **data, DWORD *size, int *pcm) {
    if (hand) {
        slot->handed = 1;
        slot->handed_at = GetTickCount64();
    }
    *data = slot->data;
    *size = slot->size;
    *pcm = slot->pcm;
}

// Reads (and decodes) a track into the cache. Everything that touches a slot
// happens under the lock, so the prefetch thread can never free a buffer that
// was just handed out.
static int cache_load(unsigned index, int hand, void **out, DWORD *out_size, int *out_pcm) {
    AcquireSRWLockExclusive(&lock);
    SoundBuffer *found = cache_find(index);
    if (found) {
        cache_take(found, hand, out, out_size, out_pcm);
    }
    ReleaseSRWLockExclusive(&lock);
    if (found) {
        return 1;
    }
    DWORD size = 0, pcm_size = 0;
    void *raw = read_track_file(index, &size), *data = raw;
    int pcm = 0;
    if (raw && pcm_decode) {
        void *decoded = mp3_decode(raw, size, &pcm_size);
        if (decoded) {
            HeapFree(GetProcessHeap(), 0, raw);
            data = decoded;
            size = pcm_size;
            pcm = 1;
        }
    }
    if (!data) {
        return 0;
    }
    AcquireSRWLockExclusive(&lock);
    SoundBuffer *slot = cache_find(index);
    if (slot) {
        HeapFree(GetProcessHeap(), 0, data);  // loaded meanwhile by the other thread
    } else if ((slot = cache_free_slot(index)) != NULL) {
        *slot = (SoundBuffer){data, index, GetTickCount64(), size, 0, pcm};
    } else {
        static int busy_logged;
        if (!busy_logged) {
            busy_logged = 1;  // once per session; it can recur on every track switch
            log_append("CUSTOM RADIO: all track buffers busy; one is kept outside the cache.\r\n");
        }
        static SoundBuffer loose;
        loose = (SoundBuffer){data, index, GetTickCount64(), size, 1, pcm};
        slot = &loose;
    }
    cache_take(slot, hand, out, out_size, out_pcm);
    ReleaseSRWLockExclusive(&lock);
    return 1;
}

static void cache_touch(unsigned index) {
    AcquireSRWLockExclusive(&lock);
    SoundBuffer *slot = cache_find(index);
    if (slot) {
        slot->handed_at = GetTickCount64();
    }
    ReleaseSRWLockExclusive(&lock);
}

// Loads the track into a buffer that stays alive while FMOD streams from it.
static void *load_track(unsigned index, DWORD *size) {
    void *data;
    int pcm;
    if (!cache_load(index, 1, &data, size, &pcm)) {
        return NULL;
    }
    log_append_wide(pcm ? "CUSTOM RADIO: streaming (PCM) " : "CUSTOM RADIO: streaming (MP3) ",
                    tracks[index].name);
    // Decode the following track in the background.
    InterlockedExchange(&prefetch_request, (LONG)((index + 1) % track_count));
    return data;
}

static DWORD WINAPI prefetch_thread(void *parameter) {
    (void)parameter;
    for (;;) {
        Sleep(200);
        LONG request = InterlockedExchange(&prefetch_request, -1);
        void *data;
        DWORD size;
        int pcm;
        if (request >= 0 && (unsigned)request < track_count) {
            cache_load((unsigned)request, 0, &data, &size, &pcm);
        }
    }
}

static int parse_key_index(const char *key, unsigned *index) {
    size_t prefix = sizeof(SOUND_KEY_PREFIX) - 1;
    if (!key || !memory_equal(key, SOUND_KEY_PREFIX, prefix)) {
        return 0;
    }
    if (text_equal_ignore_case(key, SILENCE_KEY)) {
        *index = SILENCE_INDEX;
        return 1;
    }
    unsigned value = 0;
    const char *digit = key + prefix;
    if (!*digit) {
        return 0;
    }
    for (; *digit; ++digit) {
        if (*digit < '0' || *digit > '9' || value > MAX_TRACKS) {
            return 0;
        }
        value = value * 10 + (unsigned)(*digit - '0');
    }
    *index = value;
    return value < track_count;
}

// Rewrites a game std::string in place, never reallocating (heap buffers only).
static void rewrite_game_string(unsigned char *value, const char *text) {
    GameString string;
    if (!memory_read(value, &string, sizeof(string)) || string.capacity < 16 ||
        string.size > string.capacity || !string.heap_text ||
        !memory_is_writable(string.heap_text, string.capacity + 1)) {
        return;
    }
    size_t length = text_length(text);
    if (length > string.capacity) {
        length = string.capacity;
        while (length && ((unsigned char)text[length] & 0xc0) == 0x80) {
            --length;  // do not cut a UTF-8 sequence
        }
    }
    memory_copy(string.heap_text, text, length);
    string.heap_text[length] = 0;
    memory_copy(value + 0x10, &length, sizeof(length));
}

// The key the radio passes is the SSO text inside the playlist entry itself
// (SoundName is the entry's first field), so the entry can be renamed.
static void rename_entry(const char *key, const Track *track) {
    unsigned char *entry = (unsigned char *)key;
    size_t size, capacity;
    if (!memory_read(entry + 0x10, &size, sizeof(size)) ||
        !memory_read(entry + 0x18, &capacity, sizeof(capacity)) || capacity != 15 ||
        size != text_length(key)) {
        return;
    }
    rewrite_game_string(entry + ENTRY_DISPLAY_OFFSET, track->title);
    rewrite_game_string(entry + ENTRY_ARTIST_OFFSET, track->artist);
}

// Stops serving for a while if the radio keeps re-requesting (a failing or
// instantly ending stream would otherwise spin).
static int loop_guard(void) {
    uint64_t now = GetTickCount64();
    if (now < guard_until) {
        return 1;
    }
    if (now - guard_window > GUARD_WINDOW_MS) {
        guard_window = now;
        guard_count = 0;
    }
    if (++guard_count > GUARD_LIMIT) {
        guard_until = now + GUARD_PAUSE_MS;
        log_append("CUSTOM RADIO: loop guard: the radio re-requested the custom track too often; "
                   "serving paused for 30 s. Please send this log.\r\n");
        return 1;
    }
    return 0;
}

static int sound_info_hook(void *system, const char *key, void *info) {
    unsigned index;
    if (!parse_key_index(key, &index)) {
        return original_sound_info(system, key, info);
    }
    void *data;
    DWORD size;
    if (index == SILENCE_INDEX) {
        data = silence;
        size = sizeof(silence);
    } else {
        if (loop_guard()) {
            return 18;  // any FMOD error: the radio treats it as a failed item.
        }
        unsigned entry = index, file;
        if (single_entry) {
            // A request after the previous file reached its end means "next".
            if (served_valid && !served_done &&
                served_position + (int)BACK_WINDOW_MS >= (int)tracks[served_track].duration) {
                cursor = (served_track + 1) % track_count;
            }
            file = cursor % track_count;
        } else {
            int n = (int)track_count;
            file = (unsigned)((((int)entry + shift) % n + n) % n);
        }
        data = load_track(file, &size);
        if (!data) {
            log_append("CUSTOM RADIO: track could not be read; skipping.\r\n");
            cursor = (file + 1) % track_count;
            return 18;
        }
        // A transition first re-serves the playing entry; only the next entry
        // ends the step.
        int same = served_valid && !single_entry && entry == served_entry;
        LONG step = same ? 1 : InterlockedExchange(&next_step, 1);
        if (!same) {
            wrap_logged = 0;
        }
        if (step < 0 && !single_entry && served_valid &&
            entry != (served_entry + track_count - 1) % track_count) {
            log_append_value("CUSTOM RADIO: previous track missed; the radio served entry ", (int)entry);
        }
        served_track = file;
        served_entry = entry;
        served_entry_address = key;
        InterlockedIncrement(&serve_count);
        InterlockedExchange64(&transition_at, 0);
        served_end = single_entry ? ENTRY_LENGTH_MS : tracks[entry].duration;
        served_valid = 1;
        served_done = 0;
        served_position = 0;
        served_moved = single_entry || file != entry;
        InterlockedExchange(&zero_offset_pending, served_moved || zero_offset_always);
        if (rename_entries) {
            rename_entry(key, &tracks[file]);
        }
        index = file;
    }
    // FMOD_STUDIO_SOUND_INFO: name_or_data, mode, FMOD_CREATESOUNDEXINFO, subsoundindex.
    unsigned char *fields = info;
    uint32_t mode = FMOD_OPENMEMORY_POINT | FMOD_ACCURATETIME, exinfo_size = EXINFO_SIZE;
    int marker = (int)(SUBSOUND_MARK | index);
    memory_zero(fields, 0xf0);
    memory_copy(fields, &data, sizeof(data));
    memory_copy(fields + 8, &mode, 4);
    memory_copy(fields + 0x10, &exinfo_size, 4);
    memory_copy(fields + 0x14, &size, 4);
    memory_copy(fields + 0xe8, &marker, 4);
    return 0;
}

// `subsound` points into the radio's 0x28-byte request: +0x10 is the output
// slot and +0x20 the player that owns the request.
static int subsound_hook(void *sound, int index, void **subsound) {
    void *player = NULL;
    memory_read((unsigned char *)subsound + 0x10, &player, sizeof(player));
    int ours = ((unsigned)index & SUBSOUND_MASK) == SUBSOUND_MARK;
    unsigned track = (unsigned)index & 0xffff;
    if (player) {
        PlayerTrack *free_slot = NULL;
        for (unsigned i = 0; i < PLAYER_SLOTS; ++i) {
            if (players[i].player == player) {
                free_slot = &players[i];
                break;
            }
            if (!players[i].player && !free_slot) {
                free_slot = &players[i];
            }
        }
        if (free_slot) {
            *free_slot = (PlayerTrack){ours && track != SILENCE_INDEX ? player : NULL, track};
        }
    }
    if (!ours) {
        return original_subsound(sound, index, subsound);
    }
    *subsound = sound;
    if (player && track != SILENCE_INDEX) {
        // The radio resumes an entry at its "live" offset. That offset belongs to
        // the entry, so a different file (or the long single entry) starts at 0.
        int start = 0, live = 0;
        void *instance = NULL;
        unsigned char *fields = player;
        memory_read(fields + PLAYER_START_OFFSET, &live, 4);
        if (InterlockedExchange(&zero_offset_pending, 0) &&
            memory_is_writable(fields + PLAYER_START_OFFSET, 4)) {
            memory_copy(fields + PLAYER_START_OFFSET, &start, 4);
            played_from = 0;
            log_append_value("CUSTOM RADIO: live start offset reset to 0; was ms ", live);
        } else {
            played_from = live;
            log_append_value("CUSTOM RADIO: playing from live offset ms ", live);
        }
        if (set_volume && gain != 1.0f &&
            memory_read(fields + PLAYER_INSTANCE_OFFSET, &instance, sizeof(instance)) && instance) {
            set_volume(instance, gain);
        }
    }
    return 0;
}

// --- RadioNext/RadioRestart on the radio thread -----------------------------------

// Ends the current file: the radio then asks for the long entry again and is
// answered with the file at the (already moved) cursor.
static void finish_current(void *instance) {
    served_done = 1;
    set_timeline(instance, (int)served_end - 2000);
}

static int has_vtable(const void *object, unsigned rva) {
    void *table;
    return object && memory_read(object, &table, sizeof(table)) && table == image + rva;
}

// The custom station object, found in RadioSystem's station list by number.
static unsigned char *custom_station(void) {
    unsigned char *system, *begin, *end;
    LONG number = InterlockedCompareExchange(&station_number, 0, 0);
    if (!number || !memory_read(image + RVA_RADIO_SYSTEM_GLOBAL, &system, sizeof(system)) ||
        !has_vtable(system, RVA_RADIO_SYSTEM_VTABLE) ||
        !memory_read(system + 0x30, &begin, sizeof(begin)) ||
        !memory_read(system + 0x38, &end, sizeof(end)) || end < begin ||
        (size_t)(end - begin) > 64 * 16) {
        return NULL;
    }
    for (unsigned char *at = begin; at < end; at += 16) {
        unsigned char *station;
        int value;
        if (memory_read(at, &station, sizeof(station)) && has_vtable(station, RVA_STATION_VTABLE) &&
            memory_read(station + STATION_NUMBER_OFFSET, &value, 4) && value == number) {
            return station;
        }
    }
    return NULL;
}

// The station's live clock (ms, float) for the playlist item `holder`: the
// station advances it every tick and moves to the next item at SampleLength.
static float *station_clock(unsigned char *station, void *holder) {
    for (unsigned i = 0; i < STATION_SLOT_COUNT; ++i) {
        unsigned char *slot = station + STATION_SLOTS_OFFSET + i * STATION_SLOT_SIZE;
        void *item;
        if (memory_read(slot + 8, &item, sizeof(item)) && item == holder &&
            memory_is_writable(slot + SLOT_CLOCK_OFFSET, 4)) {
            return (float *)(slot + SLOT_CLOCK_OFFSET);
        }
    }
    // Fallback: the only running slot (byte +0x1c) is the one playing.
    float *running = NULL;
    for (unsigned i = 0; i < STATION_SLOT_COUNT; ++i) {
        unsigned char *slot = station + STATION_SLOTS_OFFSET + i * STATION_SLOT_SIZE;
        unsigned char active = 0;
        if (memory_read(slot + 0x1c, &active, 1) && active) {
            if (running || !memory_is_writable(slot + SLOT_CLOCK_OFFSET, 4)) {
                return NULL;
            }
            running = (float *)(slot + SLOT_CLOCK_OFFSET);
        }
    }
    if (running) {
        log_append("CUSTOM RADIO: station clock matched by running slot.\r\n");
    }
    return running;
}

typedef void (*ItemEntryFn)(void *, GameShared *);

// The playlist entry an item points to (item->vtable[2] returns it shared).
static const void *item_entry(unsigned char *items, int count, int index) {
    void *item, **table;
    uintptr_t method;
    if (index < 0 || index >= count || !memory_read(items + index * 16, &item, sizeof(item)) ||
        !item || !memory_read(item, &table, sizeof(table)) ||
        !memory_read(table + 2, &method, sizeof(method)) || method < (uintptr_t)image + 0x1000 ||
        method >= (uintptr_t)image + 0x1dcc000) {
        return NULL;
    }
    GameShared out = {0};
    ((ItemEntryFn)method)(item, &out);
    const void *entry = out.object;
    release_shared(&out);
    return entry;
}

// Steps the playlist that is playing the served entry back, so that its next
// pick (cursor + 1, through the shuffle order when there is one) is the item
// played before it.
static int playlist_back(unsigned char *station) {
    static const unsigned offsets[] = {FREEROAM_PLAYLIST_OFFSET, EVENT_PLAYLIST_OFFSET};
    if (!served_entry_address) {
        return 0;
    }
    for (unsigned p = 0; p < ARRAY_COUNT(offsets); ++p) {
        unsigned char *list, *items, *items_end;
        int *order, *order_end, position_now;
        if (!memory_read(station + offsets[p], &list, sizeof(list)) ||
            !has_vtable(list, RVA_PLAYLIST_VTABLE) ||
            !memory_read(list + PLAYLIST_CURSOR_OFFSET, &position_now, 4) ||
            !memory_read(list + 0x40, &items, sizeof(items)) ||
            !memory_read(list + 0x48, &items_end, sizeof(items_end)) ||
            !memory_read(list + 8, &order, sizeof(order)) ||
            !memory_read(list + 0x10, &order_end, sizeof(order_end)) || items_end <= items ||
            position_now < 0) {
            continue;
        }
        int count = (int)((items_end - items) / 16);
        int ordered = order && order_end - order == count;
        // The playing item is at the cursor, or one before it when the station
        // already picked the next one.
        for (int back = 0; back < 2 && back < count; ++back) {
            int position = ((position_now - back) % count + count) % count, index = position;
            if (ordered && !memory_read(order + position, &index, 4)) {
                continue;
            }
            if (item_entry(items, count, index) == served_entry_address &&
                memory_is_writable(list + PLAYLIST_CURSOR_OFFSET, 4)) {
                int target = ((position - 2) % count + count) % count;
                memory_copy(list + PLAYLIST_CURSOR_OFFSET, &target, 4);
                log_append_value("CUSTOM RADIO: playlist cursor was ", position_now);
                log_append_value("CUSTOM RADIO: playlist cursor set to ", target);
                return 1;
            }
        }
    }
    return 0;
}

static int read_game_string(const void *value, char *output, size_t capacity);

// Entry index of a playlist entry: its SoundName (first field) is FH2MUSIC_<n>.
static int entry_index(const void *entry) {
    char key[32];
    unsigned index;
    if (!entry || !read_game_string(entry, key, sizeof(key)) || !parse_key_index(key, &index) ||
        index >= track_count) {
        return -1;
    }
    return (int)index;
}

// Playlist::GetNext(out, mode) for every playlist; only the custom station's
// track playlists are steered. Mode 0 picks items[cursor + 1], mode 1
// items[order[cursor + 1]], both wrapping; the cursor is set so that the pick
// is the wanted folder position.
static void *playlist_next_hook(void *list, GameShared *out, int mode) {
    unsigned char *station = served_valid && !single_entry ? custom_station() : NULL;
    unsigned char *mine[2] = {NULL, NULL}, *items, *items_end;
    int *order = NULL, *order_end = NULL;
    if (station) {
        memory_read(station + FREEROAM_PLAYLIST_OFFSET, &mine[0], sizeof(mine[0]));
        memory_read(station + EVENT_PLAYLIST_OFFSET, &mine[1], sizeof(mine[1]));
    }
    if (!station || (mode != 0 && mode != 1) || ((unsigned char *)list != mine[0] &&
                                                 (unsigned char *)list != mine[1]) ||
        !memory_read((unsigned char *)list + 0x40, &items, sizeof(items)) ||
        !memory_read((unsigned char *)list + 0x48, &items_end, sizeof(items_end)) ||
        items_end <= items || !memory_is_writable((unsigned char *)list + PLAYLIST_CURSOR_OFFSET, 4)) {
        return original_next(list, out, mode);
    }
    int count = (int)((items_end - items) / 16);
    int step = (int)InterlockedCompareExchange(&next_step, 0, 0);
    if (step == 0) {
        // A restart repeats the entry once; the serve that follows is the same
        // entry, so the pick itself ends the step.
        InterlockedCompareExchange(&next_step, 1, 0);
    }
    int current = (int)served_entry;
    int want = ((current + step) % count + count) % count;
    int position = -1;
    if (mode == 1) {
        if (memory_read((unsigned char *)list + 8, &order, sizeof(order)) &&
            memory_read((unsigned char *)list + 0x10, &order_end, sizeof(order_end)) && order &&
            order_end - order == count) {
            for (int p = 0; p < count && position < 0; ++p) {
                int index;
                if (memory_read(order + p, &index, 4) && index >= 0 && index < count &&
                    entry_index(item_entry(items, count, index)) == want) {
                    position = p;
                }
            }
        }
    } else if (entry_index(item_entry(items, count, want)) == want) {
        position = want;
    }
    if (position >= 0) {
        int target = position == 0 ? count - 1 : position - 1;
        memory_copy((unsigned char *)list + PLAYLIST_CURSOR_OFFSET, &target, 4);
        if (step < 0 && !wrap_logged) {
            log_append_value("CUSTOM RADIO: previous track: the radio is given entry ", want);
        }
        if (wrap_logged) {
            // already logged for this transition
        } else if (step > 0 && want == 0 && current == count - 1) {
            wrap_logged = 1;
            log_append("CUSTOM RADIO: playlist wrapped to first track.\r\n");
        } else if (step < 0 && want == count - 1 && current == 0) {
            wrap_logged = 1;
            log_append("CUSTOM RADIO: playlist wrapped to last track.\r\n");
        }
    } else {
        log_append("CUSTOM RADIO: next track not found in the playlist; the radio picks.\r\n");
    }
    return original_next(list, out, mode);
}

static void **history_find_hook(void **found, void *first, void *last, void *value) {
    unsigned char *station = served_valid && !single_entry ? custom_station() : NULL;
    void *head = NULL;
    original_find(found, first, last, value);
    if (station && memory_read(station + STATION_HISTORY_OFFSET, &head, sizeof(head)) &&
        head == last && *found != last) {
        *found = last;  // not "recently played": the folder order decides
    }
    return found;
}

// A key press from the game thread, if it is recent enough; clears it.
static int take_request(volatile LONG64 *request, uint64_t now) {
    LONG64 at = InterlockedExchange64(request, 0);
    return at && now >= (uint64_t)at && now - (uint64_t)at <= REQUEST_TTL_MS;
}

static void player_update_hook(void *player, float dt) {
    original_update(player, dt);
    if (!track_count) {
        return;
    }
    void *active = NULL;
    for (unsigned i = 0; i < PLAYER_SLOTS; ++i) {
        if (players[i].player == player && players[i].track < track_count) {
            active = player;
        }
    }
    void *instance = NULL;
    int state = 0, position = 0;
    if (active &&
        (!memory_read((unsigned char *)player + PLAYER_STATE_OFFSET, &state, 4) ||
         state != PLAYER_STATE_PLAYING ||
         !memory_read((unsigned char *)player + PLAYER_INSTANCE_OFFSET, &instance,
                      sizeof(instance)) ||
         !instance ||
         !memory_read((unsigned char *)player + PLAYER_POSITION_OFFSET, &position, 4))) {
        active = NULL;
    }
    if (active && served_valid) {
        static uint64_t last_touch;
        uint64_t moment = GetTickCount64();
        if (moment - last_touch > 2000) {
            last_touch = moment;
            cache_touch(served_track);
        }
    }
    if (active && served_valid && !served_done) {
        if (position > served_position) {
            served_position = position;
        }
        // Only a file that differs from the radio's entry can end before the
        // radio does; an unshifted file ends together with its own entry.
        if (served_moved &&
            position + (int)END_MARGIN_MS >= (int)tracks[served_track].duration) {
            cursor = (served_track + 1) % track_count;
            ++shift;
            finish_current(instance);
            log_append("CUSTOM RADIO: track finished; next from the folder.\r\n");
            return;
        }
    }
    // Only the player of the playing custom track takes the key presses (other
    // players of the radio update on the same thread and leave them).
    if (!active || !served_valid) {
        return;
    }
    uint64_t now = GetTickCount64();
    int next_pressed = take_request(&next_request, now);
    int back_pressed = take_request(&restart_request, now);
    if (!next_pressed && !back_pressed) {
        return;
    }
    LONG64 pending = InterlockedCompareExchange64(&transition_at, 0, 0);
    if (pending && (LONG64)now - pending < 4000) {
        log_append("CUSTOM RADIO: key ignored; the previous change is still in progress.\r\n");
        return;
    }
    if (shift_keys) {
        if (next_pressed) {
            cursor = (served_track + 1) % track_count;
            ++shift;
            finish_current(instance);
            log_append("CUSTOM RADIO: next track requested (shift).\r\n");
        } else if (position > (int)BACK_WINDOW_MS) {
            set_timeline(instance, 0);
            served_position = 0;
            log_append("CUSTOM RADIO: track restarted (shift).\r\n");
        } else {
            cursor = (served_track + track_count - 1) % track_count;
            --shift;
            finish_current(instance);
            log_append("CUSTOM RADIO: previous track requested (shift).\r\n");
        }
        return;
    }
    // Native route: move the station's own clock, so the radio itself picks the
    // next (or previous) entry at offset 0 and plays it as a normal transition.
    void *holder = NULL;
    unsigned char *station = custom_station();
    float *clock = NULL;
    if (station && has_vtable(player, RVA_PLAYER_VTABLE) &&
        memory_read((unsigned char *)player + PLAYER_HOLDER_OFFSET, &holder, sizeof(holder)) &&
        holder) {
        clock = station_clock(station, holder);
    }
    if (!clock) {
        log_append("CUSTOM RADIO: station clock not found; key ignored.\r\n");
        return;
    }
    // The F6 window counts from where this track started playing (a tuned-in
    // station joins mid-track). Restart: the radio resyncs a player to its
    // slot clock (live offset), so a seek alone does not hold; instead the
    // station moves on to the same entry (step 0), which plays from 0.
    int restart = back_pressed && position - played_from > (int)BACK_WINDOW_MS;
    if (restart && !original_next) {
        *clock = 0.0f;
        set_timeline(instance, 0);
        served_position = 0;
        log_append("CUSTOM RADIO: track restarted (station clock).\r\n");
        return;
    }
    if (restart) {
        InterlockedExchange(&next_step, 0);
        log_append_value("CUSTOM RADIO: track restart requested; it was at ms ", position);
    } else if (back_pressed && original_next) {
        InterlockedExchange(&next_step, -1);
    } else if (back_pressed && !playlist_back(station)) {
        log_append("CUSTOM RADIO: playlist position not found; F6 acts as next.\r\n");
    }
    served_done = 1;
    InterlockedExchange(&transition_serve, InterlockedCompareExchange(&serve_count, 0, 0));
    InterlockedExchange64(&transition_at, (LONG64)now ? (LONG64)now : 1);
    *clock = 1.0e9f;  // the station clamps it to SampleLength and moves on
    log_append(restart        ? "CUSTOM RADIO: the station plays the same entry again from 0.\r\n"
               : back_pressed ? "CUSTOM RADIO: previous track requested (station clock).\r\n"
                              : "CUSTOM RADIO: next track requested (station clock).\r\n");
}

// Reads a game std::string into `output` (NUL-terminated); 0 if unreadable.
static int read_game_string(const void *value, char *output, size_t capacity) {
    GameString text;
    if (!value || !memory_read(value, &text, sizeof(text)) || text.size >= capacity ||
        text.capacity < text.size) {
        return 0;
    }
    const char *source = text.capacity >= 16 ? text.heap_text : (const char *)value;
    if (!memory_read(source, output, text.size)) {
        return 0;
    }
    output[text.size] = 0;
    return 1;
}

// 0x668810 is effectively RadioModule::IsStationLocked(name): ChangeStation
// skips names it returns true for, and the HUD radio banner (0x78a070) is only
// shown when it returns false. UnlockAllStations makes it false for every name.
static unsigned char unlocked_hook(void *module, const void *name) {
    unsigned char locked = original_unlocked(module, name);
    char text[TEXT_CAPACITY];
    if (!locked) {
        return 0;
    }
    if (InterlockedCompareExchange(&unlock_all_live, 0, 0)) {
        return 0;
    }
    if (InterlockedCompareExchange(&station_number, 0, 0) && injected_name[0] &&
        read_game_string(name, text, sizeof(text)) && text_equal_ignore_case(text, injected_name)) {
        return 0;
    }
    return locked;
}

// --- RadioModule (game thread): station map, current name, D-pad cycle ----------

typedef struct {
    char names[MAX_MAP_STATIONS][64];
    unsigned char locked[MAX_MAP_STATIONS];
    unsigned count;
} StationMap;

// In-order walk of the RadioModule station map (MSVC tree, name at node+0x28).
static int read_station_map(unsigned char *module, StationMap *map) {
    unsigned char *head, *node, *child, *parent;
    unsigned char nil = 1;
    map->count = 0;
    if (!memory_read(module + MODULE_MAP_OFFSET, &head, sizeof(head)) || !head ||
        !memory_read(head, &node, sizeof(node))) {
        return 0;
    }
    for (unsigned guard = 0; node && node != head && guard < 256; ++guard) {
        if (!memory_read(node + 0x19, &nil, 1) || nil) {
            break;
        }
        if (map->count < MAX_MAP_STATIONS) {
            unsigned i = map->count;
            if (!read_game_string(node + MAP_NODE_NAME_OFFSET, map->names[i], sizeof(map->names[i]))) {
                map->names[i][0] = 0;
            }
            map->locked[i] = original_unlocked ? original_unlocked(module, node + MAP_NODE_NAME_OFFSET) : 0;
            ++map->count;
        }
        // successor
        if (!memory_read(node + 0x10, &child, sizeof(child)) || !child ||
            !memory_read(child + 0x19, &nil, 1)) {
            return 0;
        }
        if (!nil) {
            node = child;
            while (memory_read(node, &child, sizeof(child)) && child &&
                   memory_read(child + 0x19, &nil, 1) && !nil) {
                node = child;
            }
        } else {
            if (!memory_read(node + 8, &parent, sizeof(parent))) {
                return 0;
            }
            while (parent && memory_read(parent + 0x19, &nil, 1) && !nil) {
                if (!memory_read(parent + 0x10, &child, sizeof(child)) || child != node) {
                    break;
                }
                node = parent;
                if (!memory_read(node + 8, &parent, sizeof(parent))) {
                    return 0;
                }
            }
            node = parent;
        }
    }
    return 1;
}

static int current_station_name(unsigned char *module, char *output, size_t capacity) {
    unsigned char *wrapper;
    return memory_read(module + MODULE_WRAPPER_OFFSET, &wrapper, sizeof(wrapper)) && wrapper &&
           read_game_string(wrapper + WRAPPER_CURRENT_OFFSET, output, capacity);
}

static void select_station(const char *text);

static int map_index(const StationMap *map, const char *text) {
    for (unsigned i = 0; i < map->count; ++i) {
        if (text_equal_ignore_case(map->names[i], text)) {
            return (int)i;
        }
    }
    return -1;
}

// Wraps RadioModule::ChangeStation (D-pad). If the custom station is missing
// from the module's map, it is spliced into the cycle after the last and before
// the first selectable station; otherwise the game's own cycle is used as is.
static void change_station_hook(void *module, int forward) {
    static StationMap map;
    char current[64];
    if (!set_station || !InterlockedCompareExchange(&station_number, 0, 0) || !injected_name[0] ||
        !read_station_map(module, &map) || map_index(&map, injected_name) >= 0 ||
        !current_station_name(module, current, sizeof(current))) {
        original_change(module, forward);
        return;
    }
    int first = -1, last = -1;
    for (unsigned i = 0; i < map.count; ++i) {
        if (!map.names[i][0]) {
            first = -1;  // unreadable map: leave cycling to the game
            break;
        }
        if (!map.locked[i] || InterlockedCompareExchange(&unlock_all_live, 0, 0)) {
            if (first < 0) {
                first = (int)i;
            }
            last = (int)i;
        }
    }
    if (first < 0) {
        original_change(module, forward);
        return;
    }
    if (text_equal_ignore_case(current, injected_name)) {
        select_station(map.names[forward ? first : last]);
        log_append("CUSTOM RADIO: D-pad left the custom station.\r\n");
        return;
    }
    int at = map_index(&map, current);
    if ((forward && at == last) || (!forward && at == first)) {
        select_station(injected_name);
        log_append("CUSTOM RADIO: D-pad reached the custom station.\r\n");
        return;
    }
    original_change(module, forward);
}

// Logs what the radio sees, once after load and on every station change.
static void radio_diagnostics(void) {
    static int listed;
    static char last[64];
    static uint64_t last_poll;
    uint64_t now = GetTickCount64();
    if (!module_getter || !InterlockedCompareExchange(&station_number, 0, 0) ||
        now - last_poll < 500) {
        return;
    }
    last_poll = now;
    void *manager = NULL;
    GameShared shared = {0};
    if (!memory_read(image + RVA_MODULE_MANAGER_GLOBAL, &manager, sizeof(manager)) || !manager) {
        return;
    }
    module_getter(manager, &shared);
    unsigned char *module = shared.object;
    if (module) {
        char current[64];
        if (!listed) {
            static StationMap map;
            unsigned char *system, *begin, *end;
            listed = 1;
            if (memory_read(image + RVA_RADIO_SYSTEM_GLOBAL, &system, sizeof(system)) &&
                has_vtable(system, RVA_RADIO_SYSTEM_VTABLE) &&
                memory_read(system + 0x30, &begin, sizeof(begin)) &&
                memory_read(system + 0x38, &end, sizeof(end)) && end >= begin) {
                log_append_value("CUSTOM RADIO: radio system stations: ", (int)((end - begin) / 16));
                for (unsigned char *at = begin; at < end && at < begin + 32 * 16; at += 16) {
                    unsigned char *station;
                    int number = 0;
                    char name[64];
                    if (memory_read(at, &station, sizeof(station)) &&
                        has_vtable(station, RVA_STATION_VTABLE) &&
                        memory_read(station + STATION_NUMBER_OFFSET, &number, 4) &&
                        read_game_string(station + 0xe0, name, sizeof(name))) {
                        char label[96];
                        size_t length = text_length(name);
                        memory_copy(label, "CUSTOM RADIO:   station ", 24);
                        memory_copy(label + 24, name, length);
                        memory_copy(label + 24 + length, " number ", 9);
                        log_append_value(label, number);
                    }
                }
            }
            if (read_station_map(module, &map)) {
                log_append_value("CUSTOM RADIO: D-pad cycle (RadioModule map) stations: ", (int)map.count);
                for (unsigned i = 0; i < map.count; ++i) {
                    log_append_text("CUSTOM RADIO:   cycle ", map.names[i],
                                    map.locked[i] ? " (locked by progression)" : NULL);
                }
                log_append(map_index(&map, injected_name) >= 0
                               ? "CUSTOM RADIO: custom station is in the D-pad cycle.\r\n"
                               : "CUSTOM RADIO: custom station is NOT in the D-pad cycle; it is "
                                 "spliced in after the last station.\r\n");
            } else {
                log_append("CUSTOM RADIO: D-pad cycle map not readable.\r\n");
            }
        }
        if (current_station_name(module, current, sizeof(current)) &&
            !text_equal_ignore_case(current, last)) {
            memory_copy(last, current, sizeof(last));
            unsigned char *wrapper = NULL;
            int locked = original_unlocked &&
                         memory_read(module + MODULE_WRAPPER_OFFSET, &wrapper, sizeof(wrapper)) &&
                         wrapper && original_unlocked(module, wrapper + WRAPPER_CURRENT_OFFSET);
            log_append_parts("CUSTOM RADIO: current station is now '", current,
                             locked ? "' (game marks it locked)" : "' (not locked)", NULL);
        }
    }
    release_shared(&shared);
}

static void select_station(const char *text) {
    void *audio = NULL;
    if (!set_station || !memory_read(image + RVA_AUDIO_MANAGER, &audio, sizeof(audio)) || !audio) {
        return;
    }
    GameString name;
    size_t length = text_length(text);
    char *heap = NULL;
    memory_zero(&name, sizeof(name));
    if (length < 16) {
        memory_copy(name.inline_text, text, length + 1);
        name.capacity = 15;
    } else {
        heap = HeapAlloc(GetProcessHeap(), 0, length + 1);
        if (!heap) {
            return;
        }
        memory_copy(heap, text, length + 1);
        name.heap_text = heap;
        name.capacity = length;
    }
    name.size = length;
    // Same call the RadioSetStation state node makes; it copies the name.
    set_station(NULL, &name);
    if (heap) {
        HeapFree(GetProcessHeap(), 0, heap);
    }
}

// A change that produced no new serve within 3 s: tune the station again, which
// gives the radio a fresh player at the live position.
static void transition_watchdog(void) {
    LONG64 at = InterlockedCompareExchange64(&transition_at, 0, 0);
    if (!at || (LONG64)GetTickCount64() - at < 3000 ||
        InterlockedCompareExchange(&serve_count, 0, 0) !=
            InterlockedCompareExchange(&transition_serve, 0, 0)) {
        return;
    }
    InterlockedExchange64(&transition_at, 0);
    void *manager = NULL;
    GameShared shared = {0};
    char current[64];
    int ours = 0;
    if (module_getter && memory_read(image + RVA_MODULE_MANAGER_GLOBAL, &manager, sizeof(manager)) &&
        manager) {
        module_getter(manager, &shared);
        ours = shared.object && current_station_name(shared.object, current, sizeof(current)) &&
               text_equal_ignore_case(current, injected_name);
        release_shared(&shared);
    }
    if (ours) {
        select_station(injected_name);
        log_append("CUSTOM RADIO: watchdog: no track followed the change; station tuned again.\r\n");
    } else {
        log_append("CUSTOM RADIO: watchdog: change abandoned (another station is playing).\r\n");
    }
}

void radio_game_tick(unsigned pressed, int enabled) {
    if (!InterlockedCompareExchange(&installed, 0, 0) || !original_update) {
        return;  // the station is not installed (at most the lock hook is)
    }
    radio_diagnostics();
    transition_watchdog();
    uint64_t now = GetTickCount64();
    if (!enabled) {
        // Menus, loading or another window: a press from before does not count.
        InterlockedExchange64(&next_request, 0);
        InterlockedExchange64(&restart_request, 0);
    }
    if (pressed & (1u << KEY_NEXT)) {
        InterlockedExchange64(&next_request, (LONG64)now);
    }
    if (pressed & (1u << KEY_RESTART)) {
        InterlockedExchange64(&restart_request, (LONG64)now);
    }
    if (!(pressed & (1u << KEY_STATION)) || !set_station ||
        !InterlockedCompareExchange(&station_number, 0, 0) || !injected_name[0]) {
        return;
    }
    select_station(injected_name);
    log_append("CUSTOM RADIO: custom station selected (RadioStation key).\r\n");
}

// --- installation -------------------------------------------------------------------

// The three IsStationLocked calls, as one group calling one function.
static int install_lock_hook(void) {
    Hook hooks[3];
    Hook *group[3] = {&hooks[0], &hooks[1], &hooks[2]};
    int ok = hooks_prepare_call(&hooks[0], CALL_LOCKED_1) && hooks_prepare_call(&hooks[1], CALL_LOCKED_2) &&
             hooks_prepare_call(&hooks[2], CALL_LOCKED_3);
    if (ok && (hooks[1].current != hooks[0].current || hooks[2].current != hooks[0].current)) {
        log_append("CUSTOM RADIO: the station lock calls go to different code; not hooked.\r\n");
        ok = 0;
    }
    if (ok) {
        original_unlocked = (StationUnlockedFn)hooks[0].current;
        for (unsigned i = 0; i < 3; ++i) {
            hooks[i].replacement = (void *)unlocked_hook;
        }
        ok = hooks_commit(group, 3);
        if (!ok) {
            original_unlocked = NULL;
        }
    }
    log_append(!ok         ? "CUSTOM RADIO: lock check did not match; stations follow progression.\r\n"
               : unlock_all ? "CUSTOM RADIO: all radio stations unlocked (UnlockAllStations=1).\r\n"
                            : "CUSTOM RADIO: station lock hook ready (UnlockAllStations=0).\r\n");
    return ok;
}

static void install_unlock_and_select(void) {
    Hook hooks[2];
    Hook *group[2] = {&hooks[0], &hooks[1]};
    int change = hooks_prepare_call(&hooks[0], CALL_CHANGE_1) && hooks_prepare_call(&hooks[1], CALL_CHANGE_2);
    if (change && hooks[1].current != hooks[0].current) {
        log_append("CUSTOM RADIO: the D-pad station calls go to different code; not hooked.\r\n");
        change = 0;
    }
    if (hooks_function_ok(FN_MODULE_GETTER)) {
        module_getter = (ModuleGetterFn)hooks_function(FN_MODULE_GETTER);
    }
    if (change) {
        original_change = (ChangeStationFn)hooks[0].current;
        hooks[0].replacement = hooks[1].replacement = (void *)change_station_hook;
        change = hooks_commit(group, 2);
    }
    log_append(change ? "CUSTOM RADIO: D-pad cycle hook installed.\r\n"
                      : "CUSTOM RADIO: D-pad cycle hook did not match; native cycle only.\r\n");
    // push rbx; sub rsp,40h; mov rbx,[rip+audio manager]; test rbx,rbx
    unsigned char bytes[13];
    int32_t displacement = 0;
    if (hooks_function_ok(FN_SET_STATION) && memory_read(hooks_function(FN_SET_STATION), bytes, sizeof(bytes))) {
        memory_copy(&displacement, bytes + 9, 4);
    }
    if (displacement &&
        (unsigned char *)hooks_function(FN_SET_STATION) + 13 + displacement == image + RVA_AUDIO_MANAGER) {
        set_station = (SetStationFn)hooks_function(FN_SET_STATION);
        log_append("CUSTOM RADIO: the RadioStation key tunes straight to the custom station.\r\n");
    } else {
        log_append("CUSTOM RADIO: station select path did not match; the RadioStation key is disabled.\r\n");
    }
}

static void install_order(void) {
    Hook next, find;
    Hook *one[1];
    if (!single_entry && hooks_prepare_slot(&next, SLOT_PLAYLIST_NEXT)) {
        original_next = (PlaylistNextFn)next.current;
        next.replacement = (void *)playlist_next_hook;
        one[0] = &next;
        if (!hooks_commit(one, 1)) {
            original_next = NULL;
        }
    }
    if (original_next && hooks_prepare_call(&find, CALL_HISTORY_FIND)) {
        original_find = (HistoryFindFn)find.current;
        find.replacement = (void *)history_find_hook;
        one[0] = &find;
        if (!hooks_commit(one, 1)) {
            original_find = NULL;
        }
    }
    if (original_next && !original_find) {
        log_append("CUSTOM RADIO: recently-played check did not match; F6 may not step back.\r\n");
    }
    log_append(original_next ? "CUSTOM RADIO: folder order with wrap-around installed.\r\n"
                             : "CUSTOM RADIO: playlist next-pick did not match; the radio orders tracks.\r\n");
}

int radio_installed(void) {
    return InterlockedCompareExchange(&installed, 0, 0) != 0;
}

int radio_install(unsigned char *base, const RadioConfig *config) {
    image = base;
    hooks_bind(base);
    unlock_all = config->unlock_all;
    InterlockedExchange(&unlock_all_live, unlock_all);
    if (!config->enabled) {
        // The custom station is off: the station lock hook only for
        // UnlockAllStations=1; nothing at all otherwise.
        log_append("CUSTOM RADIO: the custom station is off (Enabled=0).\r\n");
        if (unlock_all && install_lock_hook()) {
            InterlockedExchange(&installed, 1);
        }
        return 1;
    }
    memory_copy(folder, config->folder, sizeof(folder));
    memory_copy(station_name, config->name, sizeof(station_name));
    gain = (float)config->volume / 100.0f;
    pcm_decode = config->pcm_decode;

    Hook core[4];
    Hook *group[4] = {&core[0], &core[1], &core[2], &core[3]};
    // Sound hooks first: they only answer FH2MUSIC_ keys, which exist only once the
    // XML hook has added the station.
    int ok = hooks_prepare_call(&core[0], CALL_SOUND_INFO) & hooks_prepare_call(&core[1], CALL_SUBSOUND) &
             hooks_prepare_slot(&core[2], SLOT_PLAYER_UPDATE) & hooks_prepare_slot(&core[3], SLOT_XML_READ);
    static const unsigned used[] = {FN_SET_TIMELINE, FN_GAME_ALLOC, FN_GAME_FREE};
    for (unsigned i = 0; i < ARRAY_COUNT(used); ++i) {
        if (!hooks_function_ok(used[i])) {
            log_append_parts("CUSTOM RADIO: ", hooks_function_name(used[i]),
                             ": the game function's first bytes are not as expected.", NULL);
            ok = 0;
        }
    }
    if (!ok) {
        log_append("CUSTOM RADIO: game code did not match; custom station disabled.\r\n");
        return 0;
    }
    build_silence();
    original_sound_info = (SoundInfoFn)core[0].current;
    original_subsound = (SubSoundFn)core[1].current;
    original_update = (PlayerUpdateFn)core[2].current;
    original_read = (XmlReadFn)core[3].current;
    set_timeline = (SetTimelineFn)hooks_function(FN_SET_TIMELINE);
    if (hooks_function_ok(FN_SET_VOLUME)) {
        set_volume = (SetVolumeFn)hooks_function(FN_SET_VOLUME);
    } else {
        log_append("CUSTOM RADIO: event volume call did not match; Volume setting ignored.\r\n");
    }
    game_alloc = (GameAllocFn)hooks_function(FN_GAME_ALLOC);
    game_free = (GameFreeFn)hooks_function(FN_GAME_FREE);
    core[0].replacement = (void *)sound_info_hook;
    core[1].replacement = (void *)subsound_hook;
    core[2].replacement = (void *)player_update_hook;
    core[3].replacement = (void *)xml_read_hook;
    if (!hooks_commit(group, 4)) {
        log_append("CUSTOM RADIO: custom station disabled.\r\n");
        original_update = NULL;
        return 0;
    }
    InterlockedExchange(&installed, 1);
    install_lock_hook();
    install_unlock_and_select();
    install_order();
    if (pcm_decode) {
        HANDLE worker = CreateThread(NULL, 0, prefetch_thread, NULL, 0, NULL);
        if (worker) {
            CloseHandle(worker);
        }
        log_append("CUSTOM RADIO: MP3s are decoded to PCM by Windows (PcmDecode=1).\r\n");
    }
    // The radio reads RadioInfo once; if that already happened, the station
    // cannot be added any more this session.
    unsigned char *system = NULL, *begin = NULL, *end = NULL;
    if (memory_read(image + RVA_RADIO_SYSTEM_GLOBAL, &system, sizeof(system)) &&
        has_vtable(system, RVA_RADIO_SYSTEM_VTABLE) && memory_read(system + 0x30, &begin, sizeof(begin)) &&
        memory_read(system + 0x38, &end, sizeof(end)) && end > begin) {
        log_append("CUSTOM RADIO: the radio's stations were already loaded when the plugin started; "
                   "the custom station is added the next time the game reads RadioInfo (at the "
                   "latest after a game restart).\r\n");
    }
    log_append_wide("CUSTOM RADIO: custom station hooks installed; folder ", folder);
    return 1;
}

#ifdef CUSTOM_RADIO_TEST
// Test build only: game calls that a fake image cannot run.
static void *test_alloc(size_t size) {
    return HeapAlloc(GetProcessHeap(), 0, size);
}
static void test_free(void *data) {
    if (data) {
        HeapFree(GetProcessHeap(), 0, data);
    }
}
__declspec(dllexport) void CustomRadioTestUseFakes(void *select) {
    game_alloc = test_alloc;
    game_free = test_free;
    module_getter = NULL;
    set_station = (SetStationFn)select;
}
__declspec(dllexport) int CustomRadioTestStation(char *name, unsigned capacity) {
    size_t length = text_length(injected_name);
    if (name && capacity > length) {
        memory_copy(name, injected_name, length + 1);
    }
    return (int)InterlockedCompareExchange(&station_number, 0, 0);
}
__declspec(dllexport) unsigned CustomRadioTestRequests(void) {
    return (InterlockedCompareExchange64(&next_request, 0, 0) ? 1u << KEY_NEXT : 0) |
           (InterlockedCompareExchange64(&restart_request, 0, 0) ? 1u << KEY_RESTART : 0);
}
__declspec(dllexport) int CustomRadioTestDescribe(const wchar_t *path, char *title, char *artist,
                                                  unsigned *duration) {
    Track track;
    const wchar_t *name = path;
    for (const wchar_t *at = path; *at; ++at) {
        if (*at == L'\\' || *at == L'/') {
            name = at + 1;
        }
    }
    memory_zero(&track, sizeof(track));
    memory_copy(track.name, name, (wide_length(name) + 1) * sizeof(wchar_t));
    mp3_describe(path, &track);
    memory_copy(title, track.title, TEXT_CAPACITY);
    memory_copy(artist, track.artist, TEXT_CAPACITY);
    *duration = track.duration;
    return 1;
}
// 1: decoded (44-byte WAV header copied, `size` the whole WAV); 0: failed;
// -1: no Windows MP3 codec here.
__declspec(dllexport) int CustomRadioTestDecode(const unsigned char *data, unsigned length,
                                                unsigned char *header, unsigned *size) {
    if (!mp3_decoder_available()) {
        return -1;
    }
    DWORD out_size = 0;
    unsigned char *wav = mp3_decode(data, length, &out_size);
    if (!wav) {
        return 0;
    }
    memory_copy(header, wav, 44);
    *size = out_size;
    HeapFree(GetProcessHeap(), 0, wav);
    return 1;
}
#endif
