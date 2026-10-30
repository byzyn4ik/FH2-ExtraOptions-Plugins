// Exit Game for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "pause_xaml.h"

#include <string.h>

static const char page_class[] = "x:Class=\"ForzaUI.A_PauseMenu\"";
static const char home_scope[] = "x:Name=\"PART_SCOPE_Home\"";
static const char online_name[] = "x:Name=\"PART_Button_HorizonOnline\"";
static const char solo_name[] = "x:Name=\"PART_Button_HorizonSolo\"";
static const char resources_close[] = "</Page.Resources>";
static const char dictionary_close[] = "</ResourceDictionary>";
static const char wide_style[] = "Style=\"{StaticResource A_Style_Button_Tile_2x1}\"";
static const char wide_look[] = "Template=\"{StaticResource A_ControlTemplate_HorizontalImageTile}\"";
static const char square_style[] = "Style=\"{StaticResource A_Style_Button_Tile_1x1}\"";
static const char square_look[] = "Template=\"{StaticResource EO_ControlTemplate_SquareImageTile}\"";
static const char column_one[] = "local:TileMenuNavigationManager.GridX=\"1\"";

// The game's tile look (as A_ControlTemplate_IconTile and the image tiles draw
// it), with the picture cropped: a wide 558x276 tile picture shows its middle
// 276x276.
static const char square_template[] =
    "<ControlTemplate x:Key=\"EO_ControlTemplate_SquareImageTile\" TargetType=\"{x:Type Button}\">"
    "<Grid Style=\"{StaticResource A_Style_FrameworkElement_DefaultTransition}\""
    " local:UIPageTransitionManager.TransitionIndex=\"{Binding "
    "(local:UIPageTransitionManager.TransitionIndex), RelativeSource={RelativeSource "
    "TemplatedParent}}\">"
    "<Grid Style=\"{StaticResource A_Style_Grid_HiddenWhenLocked}\">"
    "<local:FZMaterial Style=\"{StaticResource A_Style_FZMaterial_TileShadow}\"/></Grid>"
    "<Grid Style=\"{StaticResource A_Style_Grid_MotionBlur}\">"
    "<local:FZMaterial Style=\"{StaticResource A_Style_FZMaterial_TileBackground}\"/>"
    "<Grid ClipToBounds=\"True\"><local:FZMaterial"
    " Style=\"{StaticResource A_Style_FZMaterial_LockableImage}\""
    " ImageSource=\"{Binding DataContext.Icon, RelativeSource={RelativeSource TemplatedParent}}\""
    " Width=\"558\" Height=\"276\" HorizontalAlignment=\"Left\" VerticalAlignment=\"Top\""
    " Margin=\"-141,0,0,0\" StretchDirection=\"Both\"/></Grid>"
    "<Grid Margin=\"{StaticResource A_Thickness_TilePadding}\">"
    "<local:FZTextBlock x:Name=\"NameText\""
    " Text=\"{Binding DataContext.Heading, RelativeSource={RelativeSource TemplatedParent}}\""
    " Style=\"{StaticResource A_Style_FZTextBlock_TileTitle}\""
    " MaxWidth=\"{Binding Width, RelativeSource={RelativeSource TemplatedParent}}\"/>"
    "<local:FZMaterial Style=\"{StaticResource A_Style_Tile_LockIcon}\"/></Grid>"
    "<local:FZMaterial Style=\"{StaticResource A_Style_FZMaterial_TileSelectionBar}\"/>"
    "</Grid></Grid></ControlTemplate>";
static const char item_head[] = "<local:TileMenuDataItem x:Key=\"EO_MenuDataItem_ExitGame\" Heading=\"";
static const char item_tail[] = "\" Icon=\"/ForzaUI;component/Textures/Anthem/Icons/PauseMenu/Quit.png\"/>";
static const char exit_tile[] =
    "<Button Canvas.Left=\"{StaticResource A_Double_TileCoord_X_1}\""
    " Canvas.Top=\"{StaticResource A_Double_TileCoord_Y_1}\""
    " DataContext=\"{StaticResource EO_MenuDataItem_ExitGame}\""
    " Style=\"{StaticResource A_Style_Button_Tile_1x1}\""
    " Template=\"{StaticResource A_ControlTemplate_IconTile}\""
    " local:TileMenuNavigationManager.TileState=\"Enabled\""
    " local:TileMenuNavigationManager.GridX=\"1\" local:TileMenuNavigationManager.GridY=\"1\""
    " local:UIPageTransitionManager.TransitionIndex=\"2\" x:Name=\"" EXIT_GAME_TILE_NAME "\" />";

static const char *find_in(const char *begin, const char *end, const char *needle) {
    size_t n = strlen(needle);
    if (!begin || !end || end < begin || (size_t)(end - begin) < n) return NULL;
    for (const char *at = begin; at + n <= end; ++at)
        if (*at == *needle && !memcmp(at, needle, n)) return at;
    return NULL;
}

static int count_in(const char *begin, const char *end, const char *needle) {
    int count = 0;
    size_t n = strlen(needle);
    for (const char *at = find_in(begin, end, needle); at; at = find_in(at + n, end, needle)) ++count;
    return count;
}

static const char *find_unique(const char *begin, const char *end, const char *needle) {
    const char *at = find_in(begin, end, needle);
    return at && !find_in(at + strlen(needle), end, needle) ? at : NULL;
}

static int space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static int only_space(const char *begin, const char *end) {
    for (; begin < end; ++begin)
        if (!space(*begin)) return 0;
    return 1;
}

int pause_xaml_is_page(const char *text, size_t length) {
    return text && find_in(text, text + length, page_class) != NULL;
}

// The <Canvas ...> element holding `mark` (its attribute), nested Canvases
// included: [*begin, *end).
static int canvas_around(const char *text, const char *end, const char *mark, const char **begin,
                         const char **close) {
    const char *start = mark;
    while (start > text && *start != '<') --start;
    if (*start != '<' || (size_t)(end - start) < 8 || memcmp(start, "<Canvas", 7) || !space(start[7])) return 0;
    int depth = 0;
    for (const char *at = start; at < end; ++at) {
        if (*at != '<') continue;
        if ((size_t)(end - at) >= 8 && !memcmp(at, "<Canvas", 7) && (space(at[7]) || at[7] == '>' || at[7] == '/')) {
            const char *gt = find_in(at, end, ">");
            if (!gt) return 0;
            if (gt[-1] != '/') ++depth;
            at = gt;
        } else if ((size_t)(end - at) >= 9 && !memcmp(at, "</Canvas>", 9)) {
            if (--depth == 0) {
                *begin = start;
                *close = at + 9;
                return 1;
            }
        }
    }
    return 0;
}

typedef struct {
    const char *begin, *end;   // <Button ... />
    const char *style, *look;  // its wide style and template attributes
} WideTile;

// The self-closing <Button ... /> named `name` inside [scope, scope_end), with
// the wide tile style and template exactly once each.
static int wide_tile(const char *scope, const char *scope_end, const char *name, WideTile *tile) {
    const char *at = find_unique(scope, scope_end, name);
    if (!at) return 0;
    const char *start = at;
    while (start > scope && *start != '<') --start;
    const char *gt = find_in(at, scope_end, ">");
    if (*start != '<' || memcmp(start, "<Button", 7) || !space(start[7]) || !gt || gt[-1] != '/' ||
        find_in(start + 1, at, ">") || find_in(start + 1, at, "<"))
        return 0;
    tile->begin = start;
    tile->end = gt + 1;
    tile->style = find_unique(start, tile->end, wide_style);
    tile->look = find_unique(start, tile->end, wide_look);
    return tile->style && tile->look;
}

typedef struct {
    char *bytes;
    size_t length, capacity;
    int valid;
} Buffer;

static void put(Buffer *out, const char *text, size_t size) {
    if (!out->valid || size > out->capacity - out->length) {
        out->valid = 0;
        return;
    }
    memcpy(out->bytes + out->length, text, size);
    out->length += size;
}

static void put_text(Buffer *out, const char *text) { put(out, text, strlen(text)); }

// The page is parsed as single-byte text, so everything beyond printable ASCII
// (and the XML specials) is written as a character reference.
static void put_caption(Buffer *out, const wchar_t *caption) {
    static const char digits[] = "0123456789ABCDEF";
    for (; caption && *caption; ++caption) {
        unsigned code = (unsigned)*caption;
        if (code >= 0xd800 && code <= 0xdbff && caption[1] >= 0xdc00 && caption[1] <= 0xdfff) {
            code = 0x10000 + ((code - 0xd800) << 10) + ((unsigned)caption[1] - 0xdc00);
            ++caption;
        }
        if (code >= 0x20 && code < 0x7f && code != '"' && code != '&' && code != '<' && code != '>') {
            char c = (char)code;
            put(out, &c, 1);
            continue;
        }
        char reference[16] = "&#x";
        int n = 3, started = 0;
        for (int shift = 20; shift >= 0; shift -= 4) {
            unsigned digit = (code >> shift) & 15;
            if (digit || started || shift == 0) {
                reference[n++] = digits[digit];
                started = 1;
            }
        }
        reference[n++] = ';';
        put(out, reference, (size_t)n);
    }
}

typedef struct {
    const char *at;
    size_t skip;
    int what;  // 0 resources, 1 square style, 2 square look, 3 the tile
} Edit;

size_t pause_xaml_extend(const char *text, size_t length, char *output, size_t capacity,
                         const wchar_t *caption, const char **why) {
    const char *fail = NULL, *end = text ? text + length : NULL;
    const char *scope_begin = NULL, *scope_end = NULL, *resources = NULL;
    WideTile online = {0}, solo = {0};
    if (!text || !output || capacity < 2 || !pause_xaml_is_page(text, length)) {
        fail = "not the pause menu page";
    } else if (find_in(text, end, EXIT_GAME_TILE_NAME) || find_in(text, end, "EO_MenuDataItem_ExitGame")) {
        fail = "it already has the Exit Game tile";
    } else {
        const char *page_resources = find_unique(text, end, resources_close);
        const char *last = NULL;
        for (const char *at = find_in(text, page_resources, dictionary_close); at;
             at = find_in(at + 1, page_resources, dictionary_close))
            last = at;
        if (last && only_space(last + sizeof(dictionary_close) - 1, page_resources)) resources = last;
        const char *home = find_unique(text, end, home_scope);
        if (!resources)
            fail = "no </ResourceDictionary> closing <Page.Resources>";
        else if (!home || !canvas_around(text, end, home, &scope_begin, &scope_end))
            fail = "no Home tab (PART_SCOPE_Home canvas)";
        else if (find_in(text, end, square_look) || find_in(text, end, "x:Key=\"EO_ControlTemplate_SquareImageTile\""))
            fail = "the square tile template is already there";
        else if (!wide_tile(scope_begin, scope_end, online_name, &online) ||
                 !wide_tile(scope_begin, scope_end, solo_name, &solo))
            fail = "Online Road Trip / Horizon Solo are not the wide tiles on the Home tab";
        else if (count_in(scope_begin, scope_end, column_one))
            fail = "the Home tab already has a tile in column 1";
        else if (resources > scope_begin)
            fail = "the page resources come after the Home tab";
    }
    if (fail) {
        if (why) *why = fail;
        return 0;
    }
    const char *after = online.end > solo.end ? online.end : solo.end;
    Edit edits[] = {
        {resources, 0, 0},
        {online.style, sizeof(wide_style) - 1, 1},
        {online.look, sizeof(wide_look) - 1, 2},
        {solo.style, sizeof(wide_style) - 1, 1},
        {solo.look, sizeof(wide_look) - 1, 2},
        {after, 0, 3},
    };
    const unsigned count = sizeof(edits) / sizeof(edits[0]);
    for (unsigned i = 1; i < count; ++i)
        for (unsigned j = i; j > 0 && edits[j - 1].at > edits[j].at; --j) {
            Edit swap = edits[j];
            edits[j] = edits[j - 1];
            edits[j - 1] = swap;
        }
    Buffer out = {output, 0, capacity - 1, 1};
    const char *from = text;
    for (unsigned i = 0; i < count; ++i) {
        put(&out, from, (size_t)(edits[i].at - from));
        switch (edits[i].what) {
        case 0:
            put_text(&out, square_template);
            put_text(&out, item_head);
            put_caption(&out, caption && *caption ? caption : L"EXIT GAME");
            put_text(&out, item_tail);
            break;
        case 1: put_text(&out, square_style); break;
        case 2: put_text(&out, square_look); break;
        default: put_text(&out, exit_tile); break;
        }
        from = edits[i].at + edits[i].skip;
    }
    put(&out, from, (size_t)(end - from));
    if (!out.valid) {
        if (why) *why = "the page with the tile does not fit its buffer";
        return 0;
    }
    output[out.length] = 0;
    return out.length;
}
