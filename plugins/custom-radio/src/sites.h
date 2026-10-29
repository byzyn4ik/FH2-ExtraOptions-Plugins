// Custom Radio for Forza Horizon 2 Extra Options: the game code it uses.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// forza_durango_release_final.exe, SHA-256 3ec04c61...b7f0a3, image size
// 0x3a18000 (the same file under every Azimuth version). Every byte below was
// read from that file; tests/run_tests.py --game-exe checks them again and
// that none of them is relocated. The fake image in the tests is built from
// this table, so keep the X(...) lines one per line.
#ifndef CUSTOM_RADIO_SITES_H
#define CUSTOM_RADIO_SITES_H

#define GAME_IMAGE_SIZE 0x3a18000u

// Functions the plugin calls or hooks: their first bytes.
#define RADIO_FUNCTIONS(X)                                                                          \
    X(FN_XML_READ, 0x13de5a0, "\x40\x57\x48\x83\xec\x40\x48\xc7\x44\x24\x30\xfe\xff\xff\xff\x48")  \
    X(FN_PLAYER_UPDATE, 0x1025120, "\x40\x53\x48\x83\xec\x50\x48\x8b\xd9\x8b\x49\x20")             \
    X(FN_SOUND_INFO, 0x1a98920, "\x40\x53\x55\x56\x41\x56\x48\x81\xec\x48\x01\x00\x00")            \
    X(FN_SUBSOUND, 0x199f510, "\x40\x53\x55\x56\x57\x41\x56\x48\x81\xec\x60\x01\x00\x00")          \
    X(FN_SET_TIMELINE, 0x1a9b040, "\x48\x89\x5c\x24\x18\x48\x89\x74\x24\x20\x57\x48\x81\xec\x40\x01") \
    X(FN_SET_VOLUME, 0x1a9b2b0, "\x48\x89\x5c\x24\x18\x57\x48\x81\xec\x50\x01\x00\x00")            \
    X(FN_PLAYLIST_NEXT, 0x1007db0, "\x40\x53\x48\x83\xec\x30\x48\x8b\xda\x33\xd2\x89\x54\x24\x20") \
    X(FN_HISTORY_FIND, 0xfe7fd0, "\x4c\x89\x4c\x24\x20\x4c\x89\x44\x24\x18\x48\x89\x54\x24\x10\x53") \
    X(FN_SET_STATION, 0x65b310, "\x40\x53\x48\x83\xec\x40\x48\x8b\x1d\x7b\xef\xff\x02\x48\x85\xdb") \
    X(FN_STATION_LOCKED, 0x668810, "\x48\x8b\xc4\x48\x89\x58\x08\x48\x89\x68\x10\x48\x89\x70\x18\x57") \
    X(FN_CHANGE_STATION, 0x660e50, "\x48\x8b\xc4\x57\x48\x83\xec\x70\x48\xc7\x40\xa8\xfe\xff\xff\xff") \
    X(FN_MODULE_GETTER, 0x766c70, "\x40\x53\x48\x83\xec\x30\x48\x8b\x49\x08")                      \
    X(FN_GAME_ALLOC, 0x3229c0, "\x48\x89\x5c\x24\x08\x57\x48\x83\xec\x20")                         \
    X(FN_GAME_FREE, 0x31c7b0, "\x48\x85\xc9\x74\x1d\x53\x48\x83\xec\x20\x48\x8b\xd9")

// Virtual-table slots: slot RVA, the function it holds in the game.
#define RADIO_SLOTS(X)                                                                              \
    X(SLOT_XML_READ, 0x2ec4fd0, FN_XML_READ)                                                       \
    X(SLOT_PLAYER_UPDATE, 0x2e76248, FN_PLAYER_UPDATE)                                             \
    X(SLOT_PLAYLIST_NEXT, 0x2e76740, FN_PLAYLIST_NEXT)

// rel32 calls: the call's RVA, its target, the 8 bytes before and after it.
#define RADIO_CALLS(X)                                                                              \
    X(CALL_SOUND_INFO, 0x10189a0, FN_SOUND_INFO, "\x4c\x8d\x45\x80\x48\x8b\x4f\x18", "\x85\xc0\x0f\x85\xbc\x00\x00\x00") \
    X(CALL_SUBSOUND, 0x10221f2, FN_SUBSOUND, "\x48\x8b\x4f\x08\x4c\x8d\x47\x10", "\x8b\x53\x28\x48\x8b\x4b\x10\xe8") \
    X(CALL_HISTORY_FIND, 0x1009140, FN_HISTORY_FIND, "\x48\x8d\x8c\x24\x88\x00\x00\x00", "\x48\x39\x18\x0f\x95\xc0\x84\xc0") \
    X(CALL_LOCKED_1, 0x660fd1, FN_STATION_LOCKED, "\x18\x48\x8d\x53\x28\x48\x8b\xce", "\x84\xc0\x74\x09\x48\x3b\xdf\x0f") \
    X(CALL_LOCKED_2, 0x78a092, FN_STATION_LOCKED, "\x88\x00\x00\x00\x48\x8d\x53\x48", "\x84\xc0\x75\x39\xe8\x40\x8f\xc8") \
    X(CALL_LOCKED_3, 0x8fc8bf, FN_STATION_LOCKED, "\x48\x8b\xd0\x48\x8b\x4c\x24\x28", "\x48\x8b\xcb\x84\xc0\x48\x8d\x93") \
    X(CALL_CHANGE_1, 0x8fc659, FN_CHANGE_STATION, "\x60\x8b\x10\x48\x8b\x4c\x24\x28", "\x90\x48\x8b\x4c\x24\x30\x48\x85") \
    X(CALL_CHANGE_2, 0xc9fad5, FN_CHANGE_STATION, "\x0f\x94\xc2\x48\x8b\x4c\x24\x28", "\x90\x48\x8b\x4c\x24\x30\x48\x85")

// Data the plugin reads (never writes): globals and virtual tables.
#define RVA_RADIO_SYSTEM_GLOBAL 0x365d410u
#define RVA_AUDIO_MANAGER 0x365a298u
#define RVA_MODULE_MANAGER_GLOBAL 0x34c0f40u
#define RVA_RADIO_SYSTEM_VTABLE 0x2e76818u
#define RVA_STATION_VTABLE 0x2e76780u
#define RVA_PLAYLIST_VTABLE 0x2e76718u
#define RVA_PLAYER_VTABLE 0x2e76220u

#define SITE_ENUM(name, rva, ...) name,
enum { RADIO_FUNCTIONS(SITE_ENUM) FN_COUNT };
enum { RADIO_SLOTS(SITE_ENUM) SLOT_COUNT };
enum { RADIO_CALLS(SITE_ENUM) CALL_COUNT };
#undef SITE_ENUM
#endif
