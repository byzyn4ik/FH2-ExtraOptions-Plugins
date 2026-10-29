// Keyboard & Mouse for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include "kbm_menu.h"
#include "kbm_input.h"
#include "kbm_text.h"
#include "patch.h"

/* Inspected FH2 release: PauseOptionsMenu (the pause menu's Settings hub,
   A_PauseOptions.xaml) has its page init 0xdc8a60 in vtable slot 0x2e15940.
   It binds each tile with 0xd9fc10(page, "PART_Button_<tile>", &{handler,
   0}); the binder returns quietly when no element has that name. The Video
   tile's handler 0xdcfcd0 takes the page in rcx and starts the transition to
   the Video page, whose XAML settings_page.c then rebuilds as the
   KEYBOARD & MOUSE page. */
#define HUB_INIT_SLOT 0x2e15940u
#define HUB_INIT 0xdc8a60u
#define BIND_BUTTON 0xd9fc10u
#define VIDEO_TILE 0xdcfcd0u
/* The page must be read within this time after the tile was chosen. */
#define REQUEST_WINDOW_MS 5000u

typedef void (*PageMethod)(void *page);
typedef struct {
    void *method;
    int32_t adjust, unused;
} MemberHandler;
typedef void (*BindButton)(void *page, const char *name, const MemberHandler *handler);

static PageMethod open_video;
static void *previous_init;
static BindButton bind_button;
static volatile LONG64 requested_at;
static int installed;

static void keyboard_tile(void *page) {
    LONG64 now = (LONG64)GetTickCount64();
    InterlockedExchange64(&requested_at, now ? now : 1);
    open_video(page);
}

static void hub_initialize(void *page) {
    ((PageMethod)previous_init)(page);
    MemberHandler handler = {(void *)(uintptr_t)keyboard_tile, 0, 0};
    bind_button(page, "PART_Button_EOKeyboard", &handler);
}

int kbm_menu_take_request(void) {
    LONG64 at = InterlockedExchange64(&requested_at, 0);
    return at && (ULONGLONG)GetTickCount64() - (ULONGLONG)at < REQUEST_WINDOW_MS;
}

int kbm_menu_installed(void) {
    return installed;
}

/* The hub init slot holds the game's init or another module's hook (another
   plugin's tile): either is chained, so tiles install in any order. */
int kbm_menu_install(unsigned char *game_image) {
    static const unsigned char bind_code[] = {0x48, 0x8b, 0xc4, 0x57, 0x48, 0x81,
                                              0xec, 0x90, 0x00, 0x00, 0x00};
    static const unsigned char video_code[] = {0x40, 0x53, 0x48, 0x83, 0xec,
                                               0x30, 0x48, 0x8b, 0xd9};
    static const unsigned char init_code[] = {0x48, 0x8b, 0xc4};
    kbm_rebind_reload();
    if (!kbm_ini_path()[0] || !memory_equal(game_image + BIND_BUTTON, bind_code, sizeof(bind_code)) ||
        !memory_equal(game_image + VIDEO_TILE, video_code, sizeof(video_code)) ||
        !memory_equal(game_image + HUB_INIT, init_code, sizeof(init_code))) {
        log_append("KBM MENU: Settings hub not as expected; KEYBOARD & MOUSE tile left out "
                   "(KeyboardMouse.ini still sets every option).\r\n");
        return 0;
    }
    bind_button = (BindButton)(void *)(game_image + BIND_BUTTON);
    open_video = (PageMethod)(void *)(game_image + VIDEO_TILE);
    const char *problem = patch_slot((void **)(game_image + HUB_INIT_SLOT), game_image + HUB_INIT, game_image,
                                     GAME_IMAGE_SIZE, (void *)(uintptr_t)hub_initialize, &previous_init);
    if (problem) {
        log_line("KBM MENU: Settings hub init slot 0x%x not chained (%s); KEYBOARD & MOUSE tile left out.",
                 HUB_INIT_SLOT, problem);
        return 0;
    }
    installed = 1;
    char where[128] = "the game's init 0xdc8a60";
    if (previous_init != game_image + HUB_INIT) {
        char module[96];
        patch_describe(previous_init, module, sizeof(module));
        snprintf(where, sizeof(where), "a hook at %s", module);
    }
    log_line("KBM MENU: Settings hub init (slot 0x2e15940) chained onto %s; the KEYBOARD & MOUSE tile opens "
             "the keyboard & mouse page.", where);
    return 1;
}

/* ---------- option rows: KeyboardMouse.ini [KeyboardMouse] ---------- */

typedef struct {
    const char *property, *collection;
    const wchar_t *key;
    const wchar_t *const *values;
    unsigned count, fallback; /* fallback: kbm_config_defaults */
    const wchar_t *const *labels; /* NULL: Off/On, or the localized modes */
} OptionRow;

static const wchar_t *const modes[] = {L"Auto", L"KBM", L"PAD"};
static const wchar_t *const off_on[] = {L"0", L"1"};
static const wchar_t *const sensitivities[] = {L"40", L"60", L"80", L"100", L"130", L"160", L"200"};
static const wchar_t *const sensitivity_labels[] = {L"50%",  L"75%",  L"100%", L"125%",
                                                    L"160%", L"200%", L"250%"};
static const wchar_t *const steering[] = {L"50", L"75", L"100", L"125", L"150", L"200"};
static const wchar_t *const steering_labels[] = {L"50%",  L"75%",  L"100%",
                                                 L"125%", L"150%", L"200%"};
static const wchar_t *const cursor_sizes[] = {L"150", L"200", L"250", L"300", L"400"};
static const wchar_t *const cursor_labels[] = {L"150%", L"200%", L"250%", L"300%", L"400%"};
static const wchar_t *const map_speeds[] = {L"50",  L"100", L"150", L"200", L"300",
                                            L"400", L"500", L"600", L"800"};
static const wchar_t *const map_labels[] = {L"50%",  L"100%", L"150%", L"200%", L"300%",
                                            L"400%", L"500%", L"600%", L"800%"};

/* In the order of VideoOptionsText kbm_names. */
static const OptionRow options[KBM_MENU_OPTION_ROWS] = {
    {"KbmMode", "KbmModeChoices", L"Mode", modes, 3, 0, NULL},
    {"KbmMouseLook", "KbmMouseLookChoices", L"MouseLook", off_on, 2, 1, NULL},
    {"KbmMouseLookPitch", "KbmMouseLookPitchChoices", L"MouseLookPitch", off_on, 2, 0, NULL},
    {"KbmInvertMouseY", "KbmInvertMouseYChoices", L"InvertMouseY", off_on, 2, 0, NULL},
    {"KbmMouseSensitivity", "KbmMouseSensitivityChoices", L"MouseSensitivity", sensitivities, 7,
     2, sensitivity_labels},
    {"KbmSteeringSpeed", "KbmSteeringSpeedChoices", L"SteeringSpeed", steering, 6, 2,
     steering_labels},
    {"KbmMenuCursor", "KbmMenuCursorChoices", L"MenuCursor", off_on, 2, 1, NULL},
    {"KbmCursorSize", "KbmCursorSizeChoices", L"CursorSize", cursor_sizes, 5, 2, cursor_labels},
    {"KbmMapCursorSpeed", "KbmMapCursorSpeedChoices", L"MapCursorSpeed", map_speeds, 9, 5,
     map_labels},
};

/* The key binding rows, in kbm_rebind.c item order. */
#define KEY_ROW(name) {"KbmKey" #name, "KbmKey" #name "Choices"}
static const char *const key_rows[KBM_REBIND_ITEMS][2] = {
    KEY_ROW(Throttle),  KEY_ROW(Brake),        KEY_ROW(SteerLeft),       KEY_ROW(SteerRight),
    KEY_ROW(Handbrake), KEY_ROW(ShiftUp),      KEY_ROW(ShiftDown),       KEY_ROW(Clutch),
    KEY_ROW(Camera),    KEY_ROW(Rewind),       KEY_ROW(LookBack),        KEY_ROW(Activate),
    KEY_ROW(PhotoMode), KEY_ROW(RadioPrevious), KEY_ROW(RadioNext),      KEY_ROW(Map),
    KEY_ROW(Confirm),   KEY_ROW(Back),         KEY_ROW(Up),              KEY_ROW(Down),
    KEY_ROW(Left),      KEY_ROW(Right),        KEY_ROW(TabLeft),         KEY_ROW(TabRight),
    KEY_ROW(MenuX),     KEY_ROW(MenuY),        KEY_ROW(MenuLeftTrigger), KEY_ROW(MenuRightTrigger),
    KEY_ROW(DPadUp),    KEY_ROW(DPadDown),     KEY_ROW(DPadLeft),        KEY_ROW(DPadRight),
    KEY_ROW(Reset),     {"KbmKeyListIndex", NULL},
};
#undef KEY_ROW

const char *kbm_menu_property(unsigned row) {
    return row < KBM_MENU_OPTION_ROWS ? options[row].property
           : row < KBM_MENU_ROWS      ? key_rows[row - KBM_MENU_OPTION_ROWS][0]
                                      : NULL;
}

const char *kbm_menu_collection(unsigned row) {
    return row < KBM_MENU_OPTION_ROWS ? options[row].collection
           : row < KBM_MENU_ROWS      ? key_rows[row - KBM_MENU_OPTION_ROWS][1]
                                      : NULL;
}

/* A whole number, or -1. */
static int number(const wchar_t *text) {
    int value = 0;
    if (!*text) {
        return -1;
    }
    for (; *text; ++text) {
        if (*text < L'0' || *text > L'9' || value > 100000) {
            return -1;
        }
        value = value * 10 + (*text - L'0');
    }
    return value;
}

/* The INI value as the mod's parser reads it: an inline ; or # comment ends
   it. A number between two choices shows the nearest one. */
static int option_get(const OptionRow *r) {
    wchar_t value[32];
    const wchar_t *ini_path = kbm_ini_path();
    if (!ini_path[0] || !GetPrivateProfileStringW(L"KeyboardMouse", r->key, L"", value,
                                                  ARRAY_COUNT(value), ini_path)) {
        return (int)r->fallback;
    }
    unsigned end = 0;
    while (value[end] && value[end] != L';' && value[end] != L'#') {
        ++end;
    }
    while (end && (value[end - 1] == L' ' || value[end - 1] == L'\t')) {
        --end;
    }
    value[end] = 0;
    for (unsigned index = 0; index < r->count; ++index) {
        if (!lstrcmpiW(value, r->values[index])) {
            return (int)index;
        }
    }
    int wanted = number(value), best = (int)r->fallback, distance = -1;
    for (unsigned index = 0; wanted >= 0 && index < r->count; ++index) {
        int choice = number(r->values[index]);
        int gap = choice < 0 ? -1 : choice > wanted ? choice - wanted : wanted - choice;
        if (gap >= 0 && (distance < 0 || gap < distance)) {
            best = (int)index;
            distance = gap;
        }
    }
    return best;
}

int kbm_menu_get(unsigned row) {
    if (row >= KBM_MENU_OPTION_ROWS) {
        return row < KBM_MENU_ROWS ? kbm_rebind_get(row - KBM_MENU_OPTION_ROWS) : 0;
    }
    return option_get(&options[row]);
}

int kbm_menu_set(unsigned row, int value) {
    if (row >= KBM_MENU_OPTION_ROWS) {
        return row < KBM_MENU_ROWS && kbm_rebind_set(row - KBM_MENU_OPTION_ROWS, value);
    }
    const OptionRow *r = &options[row];
    const wchar_t *ini_path = kbm_ini_path();
    if (value < 0 || (unsigned)value >= r->count || !ini_path[0] || option_get(r) == value ||
        !WritePrivateProfileStringW(L"KeyboardMouse", r->key, r->values[value], ini_path)) {
        return 0;
    }
    kbm_menu_option(r->key, r->values[value]);
    return 1;
}

void kbm_menu_reset(void) {
    for (unsigned row = 0; row < KBM_MENU_OPTION_ROWS; ++row) {
        kbm_menu_set(row, (int)options[row].fallback);
    }
}

unsigned kbm_menu_choice_count(unsigned row) {
    return row < KBM_MENU_OPTION_ROWS ? options[row].count
           : row < KBM_MENU_ROWS      ? kbm_rebind_choice_count(row - KBM_MENU_OPTION_ROWS)
                                      : 0;
}

const wchar_t *kbm_menu_choice(unsigned char *game_image, unsigned row, unsigned index) {
    const KbmText *text = kbm_text(game_image);
    if (row >= KBM_MENU_OPTION_ROWS) {
        return row < KBM_MENU_ROWS ? kbm_rebind_choice(row - KBM_MENU_OPTION_ROWS, index, text)
                                   : NULL;
    }
    const OptionRow *r = &options[row];
    if (index >= r->count) {
        return NULL;
    }
    return r->labels          ? r->labels[index]
           : r->values == modes ? text->kbm_modes[index]
                                : index ? text->on : text->off;
}

const char *kbm_menu_name(unsigned char *game_image, unsigned row) {
    const KbmText *text = kbm_text(game_image);
    return row < KBM_MENU_OPTION_ROWS ? text->kbm_names[row]
           : row < KBM_MENU_ROWS      ? kbm_rebind_name(row - KBM_MENU_OPTION_ROWS, text)
                                      : NULL;
}

const char *kbm_menu_description(unsigned char *game_image, unsigned row) {
    const KbmText *text = kbm_text(game_image);
    return row < KBM_MENU_OPTION_ROWS ? text->kbm_descriptions[row]
           : row < KBM_MENU_ROWS      ? kbm_rebind_description(row - KBM_MENU_OPTION_ROWS, text)
                                      : NULL;
}

int kbm_menu_labels_stale(unsigned row) {
    return row >= KBM_MENU_OPTION_ROWS && row < KBM_MENU_ROWS &&
           kbm_rebind_labels_stale(row - KBM_MENU_OPTION_ROWS);
}

int kbm_menu_take_dirty(unsigned row) {
    return row >= KBM_MENU_OPTION_ROWS && row < KBM_MENU_ROWS &&
           kbm_rebind_take_dirty(row - KBM_MENU_OPTION_ROWS);
}

void kbm_menu_page_built(void) {
    kbm_rebind_page_built();
}

void kbm_menu_ui_tick(void) {
    if (installed) {
        kbm_rebind_ui_tick();
    }
}

/* ---------- the Settings hub tile ---------- */

int kbm_menu_is_hub_xaml(const char *text, size_t length) {
    return installed && length > 0 &&
           text_find(text, length, "x:Class=\"ForzaUI.A_PauseOptions\"") &&
           text_find(text, length, "x:Name=\"PART_Button_Controls\"");
}

/* The page's tile navigation grid is fixed at 5 x 2 cells; tiles outside it
   cannot take focus. Controls (2x1 at column 1 of the top row) is cut to its
   left cell and KEYBOARD & MOUSE takes the right one (column 2). Both show
   the right half of the Controls picture (2x1), where it is drawn; the
   picture is drawn at its own size, since an FZMaterial without a size
   collapses when aligned. */
static const char right_half_template[] =
    "<ControlTemplate x:Key=\"EO_ControlTemplate_RightHalfTile\" TargetType=\"{x:Type Button}\">"
    "<Grid Style=\"{StaticResource A_Style_FrameworkElement_DefaultTransition}\""
    " local:UIPageTransitionManager.TransitionIndex=\"{Binding "
    "(local:UIPageTransitionManager.TransitionIndex), RelativeSource={RelativeSource "
    "TemplatedParent}}\">"
    "<Grid Style=\"{StaticResource A_Style_Grid_HiddenWhenLocked}\">"
    "<local:FZMaterial Style=\"{StaticResource A_Style_FZMaterial_TileShadow}\"/></Grid>"
    "<Grid Style=\"{StaticResource A_Style_Grid_MotionBlur}\">"
    "<local:FZMaterial Style=\"{StaticResource A_Style_FZMaterial_TileBackground}\"/>"
    "<Grid ClipToBounds=\"True\">"
    "<local:FZMaterial Style=\"{StaticResource A_Style_FZMaterial_LockableImage}\""
    " ImageSource=\"{Binding DataContext.Icon, RelativeSource={RelativeSource TemplatedParent}}\""
    " Width=\"558\" Height=\"276\" HorizontalAlignment=\"Left\" VerticalAlignment=\"Top\""
    " Margin=\"-282,0,0,0\" StretchDirection=\"Both\"/></Grid>"
    "<Grid Margin=\"{StaticResource A_Thickness_TilePadding}\">"
    "<local:FZTextBlock x:Name=\"NameText\""
    " Text=\"{Binding DataContext.Heading, RelativeSource={RelativeSource TemplatedParent}}\""
    " Style=\"{StaticResource A_Style_FZTextBlock_TileTitle}\""
    " MaxWidth=\"{Binding Width, RelativeSource={RelativeSource TemplatedParent}}\"/>"
    "<local:FZMaterial Style=\"{StaticResource A_Style_Tile_LockIcon}\"/></Grid>"
    "<local:FZMaterial Style=\"{StaticResource A_Style_FZMaterial_TileSelectionBar}\"/>"
    "</Grid></Grid></ControlTemplate>"
    "<local:TileMenuDataItem x:Key=\"EO_MenuDataItem_Keyboard\" Heading=\"";
static const char keyboard_icon[] =
    "\" Icon=\"/ForzaUI;component/Textures/Anthem/Icons/TiledMenus/Controls2.png\"/>";
static const char keyboard_tile_xaml[] =
    "<Button x:Name=\"PART_Button_EOKeyboard\" local:TileMenuNavigationManager.TileState=\"Enabled\""
    " DataContext=\"{StaticResource EO_MenuDataItem_Keyboard}\""
    " local:TileMenuNavigationManager.GridX=\"2\" local:TileMenuNavigationManager.GridY=\"0\""
    " Style=\"{DynamicResource A_Style_Button_Tile_1x1}\""
    " Canvas.Left=\"{StaticResource A_Double_TileCoord_X_2}\""
    " Canvas.Top=\"{StaticResource A_Double_TileCoord_Y_0}\""
    " Template=\"{StaticResource EO_ControlTemplate_RightHalfTile}\""
    " local:UIPageTransitionManager.TransitionIndex=\"7\" local:BindingProxy.C=\"DisableMotionBlur\"/>";
static const char controls_wide[] = "Style=\"{DynamicResource A_Style_Button_Tile_2x1}\"";
static const char controls_narrow[] = "Style=\"{DynamicResource A_Style_Button_Tile_1x1}\"";
static const char controls_template[] =
    "Template=\"{StaticResource A_ControlTemplate_HorizontalImageTile}\"";
static const char controls_right_half[] =
    "Template=\"{StaticResource EO_ControlTemplate_RightHalfTile}\"";

typedef struct {
    const char *at;
    size_t skip;
    const char *insert;
    int caption; /* the tile caption and the data item's end follow */
} HubEdit;

typedef struct {
    char *bytes;
    size_t length, capacity;
    int valid;
} HubBuffer;

static void hub_append(HubBuffer *buffer, const char *text, size_t length) {
    if (!buffer->valid || length >= buffer->capacity - buffer->length) {
        buffer->valid = 0;
        return;
    }
    memory_copy(buffer->bytes + buffer->length, text, length);
    buffer->length += length;
}

static void hub_append_attribute(HubBuffer *buffer, const char *text) {
    for (; *text; ++text) {
        const char *escaped = *text == '&'   ? "&amp;"
                              : *text == '<' ? "&lt;"
                              : *text == '>' ? "&gt;"
                              : *text == '"' ? "&quot;"
                                             : NULL;
        hub_append(buffer, escaped ? escaped : text, escaped ? text_length(escaped) : 1);
    }
}

size_t kbm_menu_hub_xaml(unsigned char *game_image, const char *text, size_t length,
                         char *output, size_t capacity) {
    if (text_find(text, length, "PART_Button_EOKeyboard")) {
        return 0; /* already has the tile (read through another chain) */
    }
    const char *resources = text_find_unique(text, length, "</ResourceDictionary>");
    const char *tiles = text_find_unique(text, length, "</local:AutoSizeCanvas>");
    const char *controls = text_find_unique(text, length, "x:Name=\"PART_Button_Controls\"");
    const char *controls_end =
        controls ? text_find(controls, length - (size_t)(controls - text), "/>") : NULL;
    size_t span = controls_end ? (size_t)(controls_end - controls) : 0;
    const char *size = controls_end ? text_find(controls, span, controls_wide) : NULL;
    const char *look = controls_end ? text_find(controls, span, controls_template) : NULL;
    if (!capacity || !resources || !tiles || !size || !look || resources >= controls ||
        controls_end >= tiles) {
        static volatile LONG told;
        if (!InterlockedExchange(&told, 1))
            log_append("KBM MENU: Settings hub layout did not match; KEYBOARD & MOUSE tile left "
                   "out.\r\n");
        return 0;
    }
    HubEdit edits[] = {
        {resources, 0, right_half_template, 1},
        {size, sizeof(controls_wide) - 1, controls_narrow, 0},
        {look, sizeof(controls_template) - 1, controls_right_half, 0},
        {tiles, 0, keyboard_tile_xaml, 0},
    };
    for (unsigned i = 1; i < ARRAY_COUNT(edits); ++i) {
        for (unsigned j = i; j > 0 && edits[j - 1].at > edits[j].at; --j) {
            HubEdit swap = edits[j];
            edits[j] = edits[j - 1];
            edits[j - 1] = swap;
        }
    }
    const KbmText *localized = kbm_text(game_image);
    HubBuffer buffer = {output, 0, capacity, 1};
    // FH2's XML reader only enables UTF-8 decoding after seeing its byte-order mark.
    if (length < 3 || !memory_equal(text, "\xef\xbb\xbf", 3)) {
        hub_append(&buffer, "\xef\xbb\xbf", 3);
    }
    const char *from = text;
    for (unsigned i = 0; i < ARRAY_COUNT(edits); ++i) {
        hub_append(&buffer, from, (size_t)(edits[i].at - from));
        hub_append(&buffer, edits[i].insert, text_length(edits[i].insert));
        if (edits[i].caption) {
            hub_append_attribute(&buffer, localized->keyboard_mouse);
            hub_append(&buffer, keyboard_icon, sizeof(keyboard_icon) - 1);
        }
        from = edits[i].at + edits[i].skip;
    }
    hub_append(&buffer, from, length - (size_t)(from - text));
    if (!buffer.valid) {
        return 0;
    }
    output[buffer.length] = 0;
    return buffer.length;
}

#ifdef KBM_TEST
/* Offline: the hub copy and the rows without a hooked game. */
__declspec(dllexport) size_t KbmTestHubXaml(const char *text, size_t length, char *out, size_t capacity) {
    int was = installed;
    installed = 1;
    size_t result = kbm_menu_is_hub_xaml(text, length) ? kbm_menu_hub_xaml(NULL, text, length, out, capacity) : 0;
    installed = was;
    return result;
}

__declspec(dllexport) int KbmTestMenuInstalled(void) {
    return installed;
}

__declspec(dllexport) int KbmTestMenuGet(unsigned row) {
    return kbm_menu_get(row);
}

__declspec(dllexport) int KbmTestMenuSet(unsigned row, int value) {
    return kbm_menu_set(row, value);
}

__declspec(dllexport) const wchar_t *KbmTestMenuChoice(unsigned row, unsigned index) {
    return kbm_menu_choice(NULL, row, index);
}

/* Stand-ins for the tile binder and the Video tile's handler (the mapped
   game's code cannot run offline). */
__declspec(dllexport) void KbmTestMenuNatives(void *bind, void *video) {
    bind_button = (BindButton)bind;
    open_video = (PageMethod)video;
}

__declspec(dllexport) void *KbmTestHubHandlers(int which) {
    return which ? (void *)(uintptr_t)keyboard_tile : (void *)(uintptr_t)hub_initialize;
}
#endif
