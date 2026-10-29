// Custom Radio for Forza Horizon 2 Extra Options: MP3 files.
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CUSTOM_RADIO_MP3_H
#define CUSTOM_RADIO_MP3_H
#include "support.h"

enum { TEXT_CAPACITY = 256 };

typedef struct {
    wchar_t name[MAX_PATH];
    char title[TEXT_CAPACITY], artist[TEXT_CAPACITY];
    unsigned duration;  // ms; 0 when unknown
} Track;

// Title and artist (ID3v2 TIT2/TPE1, else ID3v1, else the file name) and the
// length (Xing/Info or VBRI frame count, else the first frame's bit rate).
// `track->name` must hold the file name. Not reentrant (one static buffer).
void mp3_describe(const wchar_t *path, Track *track);

// Decodes a whole MP3 file with Windows' MP3 codec (msacm32) to a 16-bit PCM
// WAV image (44-byte header + samples), from HeapAlloc. NULL if it cannot.
void *mp3_decode(const unsigned char *data, DWORD size, DWORD *output_size);
// 1 when msacm32 and its stream functions are there.
int mp3_decoder_available(void);
#endif
