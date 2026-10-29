// Exit Game for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two hooks, both chained onto whatever is there when the plugin starts
// (Extra Options 0.4.4 has its own hooks on both sites; other plugins may
// chain them too). Each handler calls the previous target first.
//
// 1. The pause page's XAML. The game reads every XAML resource through one
//    call on its UIRenderer LoadComponent path, "call 0xaabec0" at
//    game+0x16ebb37. Extra Options points that call at its thunk in padding
//    (game+0x13fe354: mov rax, <its reader>; jmp rax), whose reader edits the
//    pause page (CarsNextToHome) and others. The call's 4-byte displacement is
//    swapped, atomically, for a jump to read_resource (below), which calls
//    the previous target and then gives the pause page (A_PauseMenu) its
//    Exit Game tile (pause_xaml.c) in a new resource buffer, as Extra Options
//    does for its own edits.
//
// 2. The pause page's init. PauseMenuTiled's vtable slot game+0x2e14310 holds
//    its page init (game 0xdc6e30; Extra Options' Return to Garage and the
//    Weather Control plugin chain it). The init binds each tile:
//    `lea rax,<handler>` then 0xd9f9f0(page, "PART_Button_<tile>",
//    &{handler, 0}), as for Car Packs at 0xdc8155. The slot is swapped
//    (compare-exchange) for pause_init, which runs the previous init and then
//    binds PART_Button_EOExitGame the same way to choose_exit_game().
//
// Every native function and site is checked byte for byte first; on any
// difference nothing is changed.
#include "exit_game.h"

#include <stdio.h>
#include <string.h>

#include "pause_xaml.h"

enum {
    IMAGE_SIZE = 0x3a18000,
    PAUSE_INIT_SLOT = 0x2e14310,    // PauseMenuTiled vtable: page init
    PAUSE_UPDATE_SLOT = 0x2e14350,  // ... and page update (identifies the vtable)
    PAUSE_INIT = 0xdc6e30,
    PAUSE_UPDATE = 0xdce660,
    CAR_PACKS_BINDING = 0xdc8155,   // lea rax,<Car Packs handler> ... call BIND_BUTTON
    BIND_BUTTON = 0xd9f9f0,
    RESOURCE_CALL = 0x16ebb37,
    GET_RESOURCE = 0xaabec0,
    EXTRA_OPTIONS_THUNK = 0x13fe354,
    NATIVE_ALLOCATE = 0x3229c0,
    NATIVE_FREE = 0x31c7b0,
    OBJECT_INIT = 0x143eb80,
    OBJECT_CLEANUP = 0x1442050,
    DELETE_OBJECT = 0x257730,
    MAX_XAML = 1 << 20,
    EXTRA_ROOM = 0x4000,
};

typedef struct {
    uint32_t rva;
    const char *name;
    unsigned length;
    const unsigned char *bytes;
} Code;

#define CODE(rva, name, ...) {rva, name, sizeof((const unsigned char[]){__VA_ARGS__}), (const unsigned char[]){__VA_ARGS__}}
static const Code expected_code[] = {
    CODE(PAUSE_INIT, "pause page init",
         0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac),
    CODE(PAUSE_UPDATE, "pause page update",
         0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xf9, 0xe8, 0x8e, 0xe2),
    CODE(BIND_BUTTON, "tile binder",
         0x48, 0x8b, 0xc4, 0x57, 0x48, 0x81, 0xec, 0x90, 0x00, 0x00, 0x00, 0x48, 0xc7, 0x40, 0xa0, 0xfe),
    // lea rax,[handler]; mov [rsp+20],rax; mov [rsp+28],r12d; vmovaps xmm0,[rsp+20];
    // vmovdqa [rbp+4e0],xmm0; lea r8,[rbp+4e0]; lea rdx,["PART_Button_CarPacks"];
    // mov rcx,rdi; call 0xd9f9f0
    CODE(CAR_PACKS_BINDING, "Car Packs tile binding",
         0x48, 0x8d, 0x05, 0xc4, 0x55, 0xff, 0xff, 0x48, 0x89, 0x44, 0x24, 0x20, 0x44, 0x89, 0x64, 0x24,
         0x28, 0xc5, 0xf8, 0x28, 0x44, 0x24, 0x20, 0xc5, 0xf9, 0x7f, 0x85, 0xe0, 0x04, 0x00, 0x00, 0x4c,
         0x8d, 0x85, 0xe0, 0x04, 0x00, 0x00, 0x48, 0x8d, 0x15, 0x86, 0x7c, 0x19, 0x01, 0x48, 0x8b, 0xcf,
         0xe8, 0x66, 0x78, 0xfd, 0xff),
    // ... mov [rdi],r15; mov rcx,[rax]; mov [rdi+8],rcx after the resource call.
    CODE(RESOURCE_CALL + 5, "code after the XAML resource call",
         0x4c, 0x89, 0x3f, 0x48, 0x8b, 0x08, 0x48, 0x89, 0x4f, 0x08),
    CODE(NATIVE_ALLOCATE, "allocate",
         0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x8b, 0x15),
    CODE(NATIVE_FREE, "free",
         0x48, 0x85, 0xc9, 0x74, 0x1d, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b),
    CODE(OBJECT_INIT, "object init",
         0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8d),
    CODE(OBJECT_CLEANUP, "object cleanup",
         0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8d, 0x05, 0xb3, 0xd0, 0x9f),
    CODE(DELETE_OBJECT, "delete object",
         0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8, 0xb2, 0xa2),
};
static const unsigned char reader_prologue[] = {0x48, 0x83, 0xec, 0x18, 0x48, 0x8b, 0x41, 0x38, 0xc7, 0x04, 0x24, 0x00};
static const char car_packs_name[] = "PART_Button_CarPacks";

typedef struct {
    void **vtable;
    unsigned char header[0x20];
    char *bytes;
    int length;
    int capacity;
} NativeResource;
_Static_assert(offsetof(NativeResource, bytes) == 0x28, "native resource buffer");

typedef struct {
    void *method;
    int32_t adjust, unused;
} MemberHandler;

typedef void (*PageFunction)(void *page);
typedef void **(*ReadResource)(void *reader, void **output);
typedef struct {
    void *(*allocate)(size_t);
    void (*free_memory)(void *);
    void (*object_init)(void *);
    void (*object_cleanup)(void *);
    void (*delete_object)(void *);
    void (*bind_button)(void *page, const char *name, const MemberHandler *handler);
} Natives;

static unsigned char *image;
static Natives natives;
static PageFunction previous_init;
static ReadResource previous_reader;
static void *resource_vtable[7];
static volatile LONG tile_on_page, told_added, told_layout, told_memory;
#ifdef EXIT_GAME_TEST
static Natives test_natives;
static int use_test_natives;
#endif

static int in_game(const unsigned char *game, const void *address) {
    return (const unsigned char *)address >= game && (const unsigned char *)address < game + IMAGE_SIZE;
}

// "winmm.dll+0x1234" for code in a module, else the address.
static void describe(const void *address, char *out, size_t size) {
    HMODULE module = NULL;
    wchar_t path[MAX_PATH];
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)address, &module) &&
        GetModuleFileNameW(module, path, MAX_PATH)) {
        const wchar_t *name = wcsrchr(path, L'\\');
        snprintf(out, size, "%ls+0x%llx", name ? name + 1 : path,
                 (unsigned long long)((const unsigned char *)address - (const unsigned char *)module));
    } else {
        snprintf(out, size, "0x%llx", (unsigned long long)(uintptr_t)address);
    }
}

// A slot holds the game's function or another module's hook (executable code
// outside the game image).
static int slot_ok(const unsigned char *game, uint32_t slot, uint32_t native, void **target) {
    void *value = NULL;
    if (!fh2_safe_read(game + slot, &value, sizeof(value))) return 0;
    *target = value;
    return value == game + native || (!in_game(game, value) && fh2_is_code(value));
}

const char *hooks_check(const unsigned char *game, uint32_t size, const char **init_target,
                        const char **resource_target) {
    static char problem[200], init_text[160], resource_text[200];
    char where[120];
    if (init_target) *init_target = NULL;
    if (resource_target) *resource_target = NULL;
    if (!game || size != IMAGE_SIZE) return "the game image is not the supported size (0x3a18000)";
    for (unsigned i = 0; i < sizeof(expected_code) / sizeof(expected_code[0]); ++i) {
        const Code *code = &expected_code[i];
        if (!fh2_bytes_equal(game + code->rva, code->bytes, code->length)) {
            snprintf(problem, sizeof(problem), "the game's %s at 0x%x is not as expected", code->name, code->rva);
            return problem;
        }
    }
    // The Car Packs binding's name operand: lea rdx at +0x26, disp32 at +0x29.
    int32_t displacement;
    memcpy(&displacement, game + CAR_PACKS_BINDING + 0x29, 4);
    if (!fh2_bytes_equal(game + CAR_PACKS_BINDING + 0x2d + displacement, car_packs_name, sizeof(car_packs_name)))
        return "the Car Packs tile binding does not name PART_Button_CarPacks";

    void *init = NULL, *update = NULL;
    if (!slot_ok(game, PAUSE_UPDATE_SLOT, PAUSE_UPDATE, &update))
        return "the pause page vtable (update slot 0x2e14350) is not as expected";
    if (!slot_ok(game, PAUSE_INIT_SLOT, PAUSE_INIT, &init))
        return "the pause page init slot 0x2e14310 holds neither the game's init nor executable code";
    if (init == game + PAUSE_INIT) {
        snprintf(init_text, sizeof(init_text), "the game's pause page init 0x%x", PAUSE_INIT);
    } else {
        describe(init, where, sizeof(where));
        snprintf(init_text, sizeof(init_text), "a hook at %s (Extra Options' or another plugin's)", where);
    }

    const unsigned char *called = fh2_call_target(game + RESOURCE_CALL);
    if (!called) return "the XAML resource site 0x16ebb37 is not a call";
    if (called == game + GET_RESOURCE) {
        if (!fh2_bytes_equal(called, reader_prologue, sizeof(reader_prologue)))
            return "the game's resource reader at 0xaabec0 is not as expected";
        snprintf(resource_text, sizeof(resource_text), "the game's resource reader 0x%x", GET_RESOURCE);
    } else if (called == game + EXTRA_OPTIONS_THUNK) {
        unsigned char thunk[12];
        void *hook;
        if (!fh2_safe_read(called, thunk, sizeof(thunk)) || thunk[0] != 0x48 || thunk[1] != 0xb8 || thunk[10] != 0xff ||
            thunk[11] != 0xe0)
            return "the XAML resource call goes to 0x13fe354, which is not Extra Options' thunk";
        memcpy(&hook, thunk + 2, sizeof(hook));
        if (in_game(game, hook) || !fh2_is_code(hook)) return "Extra Options' resource thunk does not lead to its code";
        describe(hook, where, sizeof(where));
        snprintf(resource_text, sizeof(resource_text), "Extra Options' resource thunk 0x%x -> %s", EXTRA_OPTIONS_THUNK,
                 where);
    } else if (!in_game(game, called) && fh2_is_code(called)) {
        describe(called, where, sizeof(where));
        snprintf(resource_text, sizeof(resource_text), "another module's hook at %s", where);
    } else {
        return "the XAML resource call goes somewhere unexpected";
    }
    if (((uintptr_t)(game + RESOURCE_CALL + 1) & 3) != 0) return "the XAML resource call is not 4-byte aligned";
    if (init_target) *init_target = init_text;
    if (resource_target) *resource_target = resource_text;
    return NULL;
}

// ------------------------------------------------------------- chaining

// The shared chaining (common/include/fh2_guard.h, also used by Azimuth
// Graphics Options on the same resource call), with this plugin's wording.
static const char *chain_problem(Fh2ChainResult result) {
    switch (result) {
    case FH2_CHAINED: return NULL;
    case FH2_NOT_CALL:
    case FH2_TARGET_NOT_CODE: return "its target is not executable code";
    case FH2_UNALIGNED: return "it is not 8-byte aligned";  // only the slot can be (hooks_check)
    case FH2_NO_NEAR_MEMORY: return "no free memory within 2 GB of it";
    case FH2_JUMP_FAILED: return "the jump could not be made executable";
    case FH2_WRITE_FAILED: return "it could not be written";
    case FH2_RACED: return "another module changed it at the same time";
    }
    return "?";
}

// --------------------------------------------------------------- handlers

static void release_object(void *object) {
    if (object && InterlockedDecrement((volatile LONG *)((unsigned char *)object + 0x10)) == 0)
        natives.delete_object(object);
}

static void *delete_resource(NativeResource *resource, unsigned flags) {
    natives.object_cleanup(resource);
    if (flags & 1) natives.free_memory(resource);
    return resource;
}

static void tell_once(volatile LONG *flag, const char *text) {
    if (!InterlockedExchange(flag, 1)) say(text);
}

static void **read_resource(void *reader, void **output) {
    previous_reader(reader, output);
    NativeResource *original = output ? (NativeResource *)*output : NULL;
    if (!original || !original->bytes || original->length <= 0 || original->length > MAX_XAML ||
        !pause_xaml_is_page(original->bytes, (size_t)original->length))
        return output;
    size_t capacity = (size_t)original->length + EXTRA_ROOM;
    NativeResource *replacement = natives.allocate(sizeof(NativeResource) + capacity);
    if (!replacement) {
        InterlockedExchange(&tile_on_page, 0);
        tell_once(&told_memory, "no memory for the pause menu with the tile; the menu is left as it is");
        return output;
    }
    char *bytes = (char *)(replacement + 1);
    const char *why = NULL;
    size_t length = pause_xaml_extend(original->bytes, (size_t)original->length, bytes, capacity,
                                      exit_caption(image), &why);
    if (!length) {
        natives.free_memory(replacement);
        // A page that already has the tile (loaded through this hook twice) keeps it.
        if (!why || strcmp(why, "it already has the Exit Game tile")) {
            InterlockedExchange(&tile_on_page, 0);
            if (!InterlockedExchange(&told_layout, 1)) {
                char line[200];
                snprintf(line, sizeof(line), "the pause menu is not as expected (%s); no Exit Game tile",
                         why ? why : "?");
                say(line);
            }
        }
        return output;
    }
    // As Extra Options does: the game's resource object with this buffer,
    // owned by the parser through the native reference count.
    natives.object_init(replacement);
    memcpy(resource_vtable, original->vtable - 1, sizeof(resource_vtable));
    resource_vtable[1] = (void *)delete_resource;  // vtable slot 0, after the type information
    replacement->vtable = resource_vtable + 1;
    replacement->bytes = bytes;
    replacement->length = (int)length;
    replacement->capacity = (int)capacity;
    *output = replacement;
    release_object(original);
    InterlockedExchange(&tile_on_page, 1);
    tell_once(&told_added, "Exit Game tile added to the pause menu's Home tab");
    return output;
}

static void pause_init(void *page) {
    previous_init(page);
    if (page && tile_on_page) {
        MemberHandler handler = {(void *)(uintptr_t)choose_exit_game, 0, 0};
        natives.bind_button(page, EXIT_GAME_TILE_NAME, &handler);
    }
}

int hooks_install(unsigned char *game) {
    image = game;
    natives.allocate = (void *(*)(size_t))(void *)(game + NATIVE_ALLOCATE);
    natives.free_memory = (void (*)(void *))(void *)(game + NATIVE_FREE);
    natives.object_init = (void (*)(void *))(void *)(game + OBJECT_INIT);
    natives.object_cleanup = (void (*)(void *))(void *)(game + OBJECT_CLEANUP);
    natives.delete_object = (void (*)(void *))(void *)(game + DELETE_OBJECT);
    natives.bind_button = (void (*)(void *, const char *, const MemberHandler *))(void *)(game + BIND_BUTTON);
#ifdef EXIT_GAME_TEST
    if (use_test_natives) natives = test_natives;
#endif
    char line[200];
    // The init first: alone it binds nothing until the page has the tile.
    void **slot = (void **)(game + PAUSE_INIT_SLOT);
    void *init = *slot;
    previous_init = (PageFunction)init;
    const char *problem = chain_problem(fh2_chain_slot(slot, init, (void *)(uintptr_t)pause_init));
    if (problem) {
        snprintf(line, sizeof(line), "the pause page init slot 0x%x could not be chained (%s); nothing is installed",
                 PAUSE_INIT_SLOT, problem);
        say(line);
        return 0;
    }
    // previous_reader is set inside chain_call before the call is swapped.
    problem = chain_problem(
        fh2_chain_call(game + RESOURCE_CALL, (void *)(uintptr_t)read_resource, (void **)&previous_reader));
    if (problem) {
        // The init hook stays (another module may have chained it since); it binds nothing.
        snprintf(line, sizeof(line), "the XAML resource call 0x%x could not be chained (%s); no Exit Game tile",
                 RESOURCE_CALL, problem);
        say(line);
        return 0;
    }
    return 1;
}

#ifdef EXIT_GAME_TEST
// Test build only: stand-ins for the game's functions (the tests run on a
// mapped copy of the game executable, whose code cannot be run).
__declspec(dllexport) void ExitGameTestNatives(void *allocate, void *free_memory, void *object_init,
                                               void *object_cleanup, void *delete_object, void *bind_button) {
    test_natives.allocate = (void *(*)(size_t))allocate;
    test_natives.free_memory = (void (*)(void *))free_memory;
    test_natives.object_init = (void (*)(void *))object_init;
    test_natives.object_cleanup = (void (*)(void *))object_cleanup;
    test_natives.delete_object = (void (*)(void *))delete_object;
    test_natives.bind_button = (void (*)(void *, const char *, const MemberHandler *))bind_button;
    use_test_natives = 1;
}

__declspec(dllexport) const char *ExitGameTestCheck(const unsigned char *game, uint32_t size, int which) {
    const char *init = NULL, *resource = NULL;
    const char *problem = hooks_check(game, size, &init, &resource);
    return problem ? problem : which ? resource : init;
}

__declspec(dllexport) void *ExitGameTestHandlers(int which) {
    return which == 0 ? (void *)(uintptr_t)pause_init
         : which == 1 ? (void *)(uintptr_t)read_resource
                      : (void *)(uintptr_t)choose_exit_game;
}
#endif
