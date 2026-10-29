// Custom Radio for Forza Horizon 2 Extra Options: CustomRadio.ini and its keys.
// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef CUSTOM_RADIO_CONFIG_H
#define CUSTOM_RADIO_CONFIG_H
#include "support.h"

enum { KEY_STATION, KEY_NEXT, KEY_RESTART, KEY_COUNT };
enum { FOLDER_CAPACITY = 1024, NAME_CAPACITY = 256 };

typedef struct {
    int enabled;            // [CustomRadio] Enabled (1)
    int unlock_all;         // UnlockAllStations (0)
    int pcm_decode;         // PcmDecode (1)
    int volume;             // Volume, percent 0-400 (100)
    wchar_t folder[FOLDER_CAPACITY];  // Folder, resolved (Music beside the launcher)
    char name[NAME_CAPACITY];         // Name, UTF-8; empty: MY MUSIC / MOJA MUZYKA
    unsigned keyboard[KEY_COUNT];     // [Keys] <action>Keyboard: virtual-key code, 0 = none
    unsigned xbox[KEY_COUNT];         // [Keys] <action>Xbox: XInput button bits, 0 = none
} RadioConfig;

// Reads `ini_path`; a relative Folder is taken from `base_directory`. Missing
// keys get their defaults; a bad value is logged and its default used.
void config_load(const wchar_t *ini_path, const wchar_t *base_directory, RadioConfig *config);
void config_defaults(RadioConfig *config);
const char *config_action_name(unsigned action);

// Key names as in Extra Options' [Bindings]: F1-F24, A-Z, 0-9, Escape, Enter,
// Space, Tab, Backspace, Insert, Delete, Home, End, PageUp, PageDown, arrows,
// Shift, Ctrl, Alt, 0xNN or a decimal virtual-key code; None. 0 on failure.
int config_parse_keyboard(const wchar_t *text, unsigned *key);
// XInput buttons, combined with '+': A, B, X, Y, LB, RB, DPadUp/Down/Left/Right,
// View (Back), Menu (Start), LeftStick (LS), RightStick (RS); None.
int config_parse_xbox(const wchar_t *text, unsigned *buttons);

// Edge detection for the three actions. `down` has bit n set while action n's
// key or button combination is held. An action fires once per press, only
// while `enabled`; a key held while disabled must be released before it fires.
typedef struct {
    unsigned held;
} KeyEdges;
unsigned keys_pressed(KeyEdges *edges, unsigned down, int enabled);
typedef SHORT(WINAPI *KeyStateFn)(int key);
unsigned keys_down(const RadioConfig *config, KeyStateFn key_state, unsigned buttons);
#endif
