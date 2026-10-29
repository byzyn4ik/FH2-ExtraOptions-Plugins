// Custom Radio for Forza Horizon 2 Extra Options: MP3 files.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp3.h"
#include <mmsystem.h>
#include <mmreg.h>
#include <msacm.h>

enum { TAG_BUFFER_SIZE = 65536, DECODE_CHUNK = 16384 };

static unsigned char tag_buffer[TAG_BUFFER_SIZE];

static unsigned big_endian(const unsigned char *bytes, unsigned count, int syncsafe) {
    unsigned value = 0;
    for (unsigned index = 0; index < count; ++index) {
        value = syncsafe ? value << 7 | (bytes[index] & 0x7f) : value << 8 | bytes[index];
    }
    return value;
}

// Decodes the first value of an ID3 text frame (encoding byte + payload) as UTF-8.
static void decode_text(const unsigned char *data, unsigned size, char *output) {
    wchar_t wide[TEXT_CAPACITY];
    unsigned count = 0;
    output[0] = 0;
    if (!size) {
        return;
    }
    unsigned encoding = data[0];
    ++data;
    --size;
    if (encoding == 3) {
        unsigned length = 0;
        while (length < size && data[length] && length < TEXT_CAPACITY - 1) {
            ++length;
        }
        memory_copy(output, data, length);
        output[length] = 0;
    } else {
        if (encoding == 1 || encoding == 2) {
            int big = encoding == 2;
            if (size >= 2 && ((data[0] == 0xfe && data[1] == 0xff) ||
                              (data[0] == 0xff && data[1] == 0xfe))) {
                big = data[0] == 0xfe;
                data += 2;
                size -= 2;
            }
            for (unsigned index = 0; index + 1 < size && count + 1 < TEXT_CAPACITY / 3;
                 index += 2) {
                wchar_t character = big ? (wchar_t)(data[index] << 8 | data[index + 1])
                                        : (wchar_t)(data[index] | data[index + 1] << 8);
                if (!character) {
                    break;
                }
                wide[count++] = character;
            }
        } else {
            for (unsigned index = 0; index < size && data[index] && count + 1 < TEXT_CAPACITY / 3;
                 ++index) {
                wide[count++] = data[index];
            }
        }
        wide[count] = 0;
        if (!WideCharToMultiByte(CP_UTF8, 0, wide, -1, output, TEXT_CAPACITY, NULL, NULL)) {
            output[0] = 0;
        }
    }
    size_t length = text_length(output);
    while (length && (output[length - 1] == ' ' || output[length - 1] == '\t')) {
        output[--length] = 0;
    }
}

static void read_id3v2(unsigned size, Track *track) {
    unsigned version = tag_buffer[3], flags = tag_buffer[5];
    unsigned end = 10 + big_endian(tag_buffer + 6, 4, 1), cursor = 10;
    if (version < 2 || version > 4 || flags & 0x80) {
        return;
    }
    if (end > size) {
        end = size;
    }
    if (version >= 3 && flags & 0x40 && end >= 14) {
        unsigned extended = big_endian(tag_buffer + 10, 4, version == 4);
        cursor += version == 4 ? extended : extended + 4;
    }
    unsigned header = version == 2 ? 6 : 10, id_size = version == 2 ? 3 : 4;
    while (cursor < end && end - cursor >= header && tag_buffer[cursor]) {
        const unsigned char *frame = tag_buffer + cursor;
        unsigned frame_size = version == 2   ? big_endian(frame + 3, 3, 0)
                              : version == 4 ? big_endian(frame + 4, 4, 1)
                                             : big_endian(frame + 4, 4, 0);
        if (frame_size > end - cursor - header) {
            break;
        }
        if (memory_equal(frame, version == 2 ? "TT2" : "TIT2", id_size)) {
            decode_text(frame + header, frame_size, track->title);
        } else if (memory_equal(frame, version == 2 ? "TP1" : "TPE1", id_size)) {
            decode_text(frame + header, frame_size, track->artist);
        }
        cursor += header + frame_size;
    }
}

static int read_at(HANDLE file, uint64_t offset, void *output, DWORD size, DWORD *read) {
    LARGE_INTEGER position;
    position.QuadPart = (LONGLONG)offset;
    *read = 0;
    return SetFilePointerEx(file, position, NULL, FILE_BEGIN) && ReadFile(file, output, size, read, NULL);
}

static void read_id3v1(HANDLE file, uint64_t size, Track *track, int *present) {
    unsigned char tag[128], field[31];
    DWORD read;
    *present = 0;
    if (size < 128 || !read_at(file, size - 128, tag, sizeof(tag), &read) ||
        read != sizeof(tag) || !memory_equal(tag, "TAG", 3)) {
        return;
    }
    *present = 1;
    field[0] = 0;
    if (!track->title[0]) {
        memory_copy(field + 1, tag + 3, 30);
        decode_text(field, sizeof(field), track->title);
    }
    if (!track->artist[0]) {
        memory_copy(field + 1, tag + 33, 30);
        decode_text(field, sizeof(field), track->artist);
    }
}

static unsigned mp3_duration(HANDLE file, uint64_t start, uint64_t size, int id3v1) {
    static const unsigned short rates1[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160,
                                            192, 224, 256, 320};
    static const unsigned short rates2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96,
                                            112, 128, 144, 160};
    static const unsigned frequencies[3][3] = {
        {44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
    DWORD read;
    if (start >= size || !read_at(file, start, tag_buffer, 8192, &read) || read < 64) {
        return 0;
    }
    for (DWORD at = 0; at + 64 <= read; ++at) {
        const unsigned char *frame = tag_buffer + at;
        if (frame[0] != 0xff || (frame[1] & 0xe0) != 0xe0) {
            continue;
        }
        unsigned version = (frame[1] >> 3) & 3, layer = (frame[1] >> 1) & 3;
        unsigned bitrate_index = frame[2] >> 4, rate_index = (frame[2] >> 2) & 3;
        if (version == 1 || layer != 1 || !bitrate_index || bitrate_index == 15 ||
            rate_index == 3) {
            continue;
        }
        int mpeg1 = version == 3, mono = (frame[3] >> 6) == 3;
        unsigned frequency = frequencies[mpeg1 ? 0 : version == 2 ? 1 : 2][rate_index];
        unsigned samples = mpeg1 ? 1152 : 576;
        unsigned side = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
        const unsigned char *xing = frame + 4 + side;
        if (at + 4 + side + 16 <= read &&
            (memory_equal(xing, "Xing", 4) || memory_equal(xing, "Info", 4)) &&
            (big_endian(xing + 4, 4, 0) & 1)) {
            uint64_t frames = big_endian(xing + 8, 4, 0);
            return (unsigned)(frames * samples * 1000 / frequency);
        }
        const unsigned char *vbri = frame + 36;
        if (at + 36 + 18 <= read && memory_equal(vbri, "VBRI", 4)) {
            uint64_t frames = big_endian(vbri + 14, 4, 0);
            return (unsigned)(frames * samples * 1000 / frequency);
        }
        unsigned kbps = (mpeg1 ? rates1 : rates2)[bitrate_index];
        uint64_t bytes = size - start - at - (id3v1 ? 128 : 0);
        return (unsigned)(bytes * 8 / kbps);
    }
    return 0;
}

void mp3_describe(const wchar_t *path, Track *track) {
    track->title[0] = track->artist[0] = 0;
    track->duration = 0;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    LARGE_INTEGER size;
    DWORD read = 0;
    uint64_t audio = 0;
    int id3v1 = 0;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
        read_at(file, 0, tag_buffer, sizeof(tag_buffer), &read)) {
        if (read >= 10 && memory_equal(tag_buffer, "ID3", 3)) {
            audio = 10 + (uint64_t)big_endian(tag_buffer + 6, 4, 1) + (tag_buffer[5] & 0x10 ? 10 : 0);
            read_id3v2(read, track);
        }
        read_id3v1(file, (uint64_t)size.QuadPart, track, &id3v1);
        track->duration = mp3_duration(file, audio, (uint64_t)size.QuadPart, id3v1);
    }
    CloseHandle(file);
    if (!track->title[0]) {
        WideCharToMultiByte(CP_UTF8, 0, track->name, -1, track->title, TEXT_CAPACITY, NULL, NULL);
        size_t length = text_length(track->title);
        if (length > 4 && track->title[length - 4] == '.') {
            track->title[length - 4] = 0;
        }
    }
}

// --- MP3 -> PCM -------------------------------------------------------------------
// The game's FMOD MP3 decoder mis-handles joint (mid/side) stereo frames, which
// most MP3s use: the result sounds hollow, with the centre (mids, vocals) lost.
// Windows' own MP3 ACM codec decodes the file to 16-bit PCM instead; FMOD then
// plays a plain stereo WAV from memory. PcmDecode=0 serves the MP3 as before.

typedef MMRESULT(ACMAPI *AcmOpenFn)(LPHACMSTREAM, HACMDRIVER, LPWAVEFORMATEX, LPWAVEFORMATEX,
                                    LPWAVEFILTER, DWORD_PTR, DWORD_PTR, DWORD);
typedef MMRESULT(ACMAPI *AcmSizeFn)(HACMSTREAM, DWORD, LPDWORD, DWORD);
typedef MMRESULT(ACMAPI *AcmHeaderFn)(HACMSTREAM, LPACMSTREAMHEADER, DWORD);
typedef MMRESULT(ACMAPI *AcmCloseFn)(HACMSTREAM, DWORD);

static AcmOpenFn acm_open;
static AcmSizeFn acm_size;
static AcmHeaderFn acm_prepare, acm_convert, acm_unprepare;
static AcmCloseFn acm_close;
static INIT_ONCE acm_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK load_acm(INIT_ONCE *once, void *parameter, void **context) {
    (void)once;
    (void)parameter;
    (void)context;
    HMODULE acm = LoadLibraryExW(L"msacm32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (acm) {
        acm_open = (AcmOpenFn)(uintptr_t)GetProcAddress(acm, "acmStreamOpen");
        acm_size = (AcmSizeFn)(uintptr_t)GetProcAddress(acm, "acmStreamSize");
        acm_prepare = (AcmHeaderFn)(uintptr_t)GetProcAddress(acm, "acmStreamPrepareHeader");
        acm_convert = (AcmHeaderFn)(uintptr_t)GetProcAddress(acm, "acmStreamConvert");
        acm_unprepare = (AcmHeaderFn)(uintptr_t)GetProcAddress(acm, "acmStreamUnprepareHeader");
        acm_close = (AcmCloseFn)(uintptr_t)GetProcAddress(acm, "acmStreamClose");
    }
    if (!acm_open || !acm_size || !acm_prepare || !acm_convert || !acm_unprepare || !acm_close) {
        acm_open = NULL;
        log_append("CUSTOM RADIO: Windows MP3 decoder (msacm32) unavailable; MP3 served as is.\r\n");
    }
    return TRUE;
}

int mp3_decoder_available(void) {
    InitOnceExecuteOnce(&acm_once, load_acm, NULL, NULL);
    return acm_open != NULL;
}

// First MPEG-1/2/2.5 Layer III frame at or after `at`.
static int mp3_frame(const unsigned char *data, DWORD end, DWORD *at, unsigned *rate,
                     unsigned *channels, unsigned *kbps, int *mpeg1) {
    static const unsigned short rates1[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160,
                                            192, 224, 256, 320};
    static const unsigned short rates2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96,
                                            112, 128, 144, 160};
    static const unsigned frequencies[3][3] = {
        {44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
    for (DWORD i = *at; i + 4 <= end && i < *at + 65536; ++i) {
        const unsigned char *h = data + i;
        unsigned version = (h[1] >> 3) & 3, layer = (h[1] >> 1) & 3, bitrate = h[2] >> 4,
                 sampling = (h[2] >> 2) & 3;
        if (h[0] != 0xff || (h[1] & 0xe0) != 0xe0 || version == 1 || layer != 1 || !bitrate ||
            bitrate == 15 || sampling == 3) {
            continue;
        }
        *mpeg1 = version == 3;
        *rate = frequencies[*mpeg1 ? 0 : version == 2 ? 1 : 2][sampling];
        *channels = (h[3] >> 6) == 3 ? 1 : 2;
        *kbps = (*mpeg1 ? rates1 : rates2)[bitrate];
        *at = i;
        return 1;
    }
    return 0;
}

static void *grow(void *data, size_t used, size_t *capacity, size_t need) {
    if (need <= *capacity) {
        return data;
    }
    size_t bigger = *capacity * 2 > need ? *capacity * 2 : need;
    void *next = HeapAlloc(GetProcessHeap(), 0, bigger);
    if (next) {
        memory_copy(next, data, used);
        *capacity = bigger;
    }
    HeapFree(GetProcessHeap(), 0, data);
    return next;
}

void *mp3_decode(const unsigned char *data, DWORD size, DWORD *output_size) {
    DWORD start = 0, end = size;
    unsigned rate, channels, kbps;
    int mpeg1;
    if (size >= 10 && memory_equal(data, "ID3", 3)) {
        start = 10 + big_endian(data + 6, 4, 1) + (data[5] & 0x10 ? 10 : 0);
    }
    if (size >= 128 && memory_equal(data + size - 128, "TAG", 3)) {
        end = size - 128;
    }
    if (start >= end || !mp3_decoder_available() ||
        !mp3_frame(data, end, &start, &rate, &channels, &kbps, &mpeg1)) {
        return NULL;
    }
    MPEGLAYER3WAVEFORMAT mp3;
    WAVEFORMATEX pcm;
    memory_zero(&mp3, sizeof(mp3));
    mp3.wfx.wFormatTag = WAVE_FORMAT_MPEGLAYER3;
    mp3.wfx.nChannels = (WORD)channels;
    mp3.wfx.nSamplesPerSec = rate;
    mp3.wfx.nAvgBytesPerSec = kbps * 125;
    mp3.wfx.nBlockAlign = 1;
    mp3.wfx.cbSize = MPEGLAYER3_WFX_EXTRA_BYTES;
    mp3.wID = MPEGLAYER3_ID_MPEG;
    mp3.fdwFlags = MPEGLAYER3_FLAG_PADDING_OFF;
    mp3.nBlockSize = (WORD)((mpeg1 ? 144 : 72) * kbps * 1000 / rate);
    mp3.nFramesPerBlock = 1;
    mp3.nCodecDelay = 1393;
    pcm.wFormatTag = WAVE_FORMAT_PCM;
    pcm.nChannels = (WORD)channels;
    pcm.nSamplesPerSec = rate;
    pcm.nBlockAlign = (WORD)(channels * 2);
    pcm.nAvgBytesPerSec = rate * pcm.nBlockAlign;
    pcm.wBitsPerSample = 16;
    pcm.cbSize = 0;
    HACMSTREAM stream = NULL;
    if (acm_open(&stream, NULL, &mp3.wfx, &pcm, NULL, 0, 0, ACM_STREAMOPENF_NONREALTIME) &&
        acm_open(&stream, NULL, &mp3.wfx, &pcm, NULL, 0, 0, 0)) {
        log_append("CUSTOM RADIO: MP3 decoder refused the format; MP3 served as is.\r\n");
        return NULL;
    }
    DWORD chunk_out = 0;
    unsigned char *source = NULL, *target = NULL, *out = NULL;
    size_t used = 44, capacity = 0;
    int ok = 0;
    if (!acm_size(stream, DECODE_CHUNK, &chunk_out, ACM_STREAMSIZEF_SOURCE) && chunk_out) {
        source = HeapAlloc(GetProcessHeap(), 0, DECODE_CHUNK);
        target = HeapAlloc(GetProcessHeap(), 0, chunk_out);
        capacity = (size_t)((uint64_t)(end - start) * pcm.nAvgBytesPerSec / (kbps * 125)) +
                   (1 << 20);
        out = HeapAlloc(GetProcessHeap(), 0, capacity);
    }
    if (source && target && out) {
        ACMSTREAMHEADER header;
        memory_zero(&header, sizeof(header));
        header.cbStruct = sizeof(header);
        header.pbSrc = source;
        header.cbSrcLength = DECODE_CHUNK;
        header.pbDst = target;
        header.cbDstLength = chunk_out;
        if (!acm_prepare(stream, &header, 0)) {
            DWORD at = start, carry = 0;
            ok = 1;
            for (int last = 0; ok && !last;) {
                DWORD take = end - at < DECODE_CHUNK - carry ? end - at : DECODE_CHUNK - carry;
                memory_copy(source + carry, data + at, take);
                at += take;
                last = at >= end;
                header.cbSrcLength = carry + take;
                header.cbSrcLengthUsed = header.cbDstLengthUsed = 0;
                if (acm_convert(stream, &header,
                                last ? ACM_STREAMCONVERTF_END : ACM_STREAMCONVERTF_BLOCKALIGN)) {
                    ok = 0;
                    break;
                }
                out = grow(out, used, &capacity, used + header.cbDstLengthUsed);
                if (!out) {
                    ok = 0;
                    break;
                }
                memory_copy(out + used, target, header.cbDstLengthUsed);
                used += header.cbDstLengthUsed;
                carry = header.cbSrcLength - header.cbSrcLengthUsed;
                if (carry && header.cbSrcLengthUsed) {
                    for (DWORD i = 0; i < carry; ++i) {
                        source[i] = source[header.cbSrcLengthUsed + i];
                    }
                } else if (carry >= DECODE_CHUNK) {
                    ok = 0;  // the decoder made no progress
                }
            }
            header.cbSrcLength = DECODE_CHUNK;
            header.cbDstLength = chunk_out;
            acm_unprepare(stream, &header, 0);
        }
    }
    acm_close(stream, 0);
    if (source) {
        HeapFree(GetProcessHeap(), 0, source);
    }
    if (target) {
        HeapFree(GetProcessHeap(), 0, target);
    }
    if (!ok || used <= 44 || used > 0xffffffffu) {
        if (out) {
            HeapFree(GetProcessHeap(), 0, out);
        }
        log_append("CUSTOM RADIO: MP3 decode failed; MP3 served as is.\r\n");
        return NULL;
    }
    static const unsigned char header[44] = {
        'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
        16, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16, 0,
        'd', 'a', 't', 'a', 0, 0, 0, 0};
    uint32_t bytes = (uint32_t)(used - 44), riff = bytes + 36, sample_rate = rate,
             byte_rate = pcm.nAvgBytesPerSec;
    uint16_t channel_count = (uint16_t)channels, align = pcm.nBlockAlign;
    memory_copy(out, header, sizeof(header));
    memory_copy(out + 4, &riff, 4);
    memory_copy(out + 22, &channel_count, 2);
    memory_copy(out + 24, &sample_rate, 4);
    memory_copy(out + 28, &byte_rate, 4);
    memory_copy(out + 32, &align, 2);
    memory_copy(out + 40, &bytes, 4);
    *output_size = (DWORD)used;
    return out;
}
