// Xtra Features for Forza Horizon 2 (Extra Options plugin): garage thumbnails.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from the Extra Options fork (features/car_previews.c and
// features/car_preview_rebuild.c).
//
// 1. Size. Garage thumbnails (GarageThumbnail_<car> in the save: bigThumb.png
//    and smallThumb.png) are rendered by CUI4PresentationRenderer (vtable slot
//    20) into an 816x459 target, or 384x216 in its second mode, and stored at
//    the sizes of a two-rectangle table in .rdata (0x1dfde70, copied by its
//    one reader 0x274121 each time): 816x459 and 192x108. Both are multiplied
//    by CarPreviewScale (2x is 1:1 in the My Cars panel at 2160p), and the
//    second mode's target is the small picture's size. The renderer's size
//    block (0xaa6a81) is rewritten in place (patch.c write_code, the other
//    threads suspended):
//        mov esi, big width / mov r13d, big height / lea r15, [rdi+0x50]
//        cmp r12d, 1 / jne end
//        mov esi, small width / mov r13d, small height
//        add r15, 0x30                                  ; was lea r15, [rdi+0x80]
//        nop / nop
//    end:
//    The original derives the second width from the first height (lea esi,
//    [r13-0x4b]), so it is set directly. Applies to thumbnails the game makes
//    after it started (a car bought, painted or upgraded) and to F8.
//
// 2. F8 re-renders the garage thumbnail of every car with a saved one, at
//    the current size, through the game's own save path. For each car it
//    builds the list Car::BuildSaveTasks (0x2f3520) makes for a saved car
//    (0x2d9820: VerifySpace, GarageThumbnail and GarageFinalize tasks) and
//    hands it to a new CBackgroundSaveTask, as the game does for its
//    background saves (0xa30310): the background task's save queue (0x351d50,
//    ticked by the game every frame) runs each task, writes the files it
//    produced and then lets it record the new thumbnail on the car. The cars
//    are the Career_Garage rows whose thumbnail is a saved one ("CS:..."; stock
//    cars show packaged pictures), read from the game's own database. F8 is
//    read and the cars advanced from UserInterface's vtable slot 8
//    (0x2ecfbc0, game 0x140ba10), which the game calls once per frame on its
//    main thread in menus and while driving; the slot is chained.
#include <stdio.h>

#include "xtra.h"

#define RENDER_SIZE 0xaa6a81u
#define STORED_SIZE 0x1dfde70u

#define UI_VTABLE 0x2ecfb80u
#define UI_TICK_SLOT (UI_VTABLE + 8 * 8)
#define UI_TICK 0x140ba10u
#define PLAYER_MANAGER 0x327270u
#define CAR_BY_ID 0x2dadd0u
#define BUILD_LIVERY_TASKS 0x2d9820u
#define GAME_ALLOCATE 0x3229c0u
#define GAME_FREE 0x31c7b0u
#define BACKGROUND_TASK_VTABLE 0x2d3bbb8u
#define BACKGROUND_TASK_SIZE 0x98u
#define SAVE_QUEUE_INIT 0x33b660u
#define BACKGROUND_TASK_ADD 0xa46ac0u
#define BACKGROUND_SCHEDULER 0x367a6d0u
#define BACKGROUND_SUBMIT 0x126c170u
#define THUMBNAIL_STATE 0x34af4b8u
#define THUMBNAIL_RENDER_FLAG 0xa0
#define CAPTURE_OWNER 0x34aba60u
#define CAPTURE_IDLE 5
#define CAPTURE_START 1
#define CAPTURE_FINISH 0xe1f6d0u
#define TASK_REFERENCES 0x70
#define PHASE_WAIT_MS 30000u
#define SAVE_WAIT_MS 120000u
#define GAME_DATABASE 0x34b3640u
#define MAX_CARS 256

static const unsigned char render_original[38] = {
    0xbe, 0x30, 0x03, 0x00, 0x00, 0x41, 0xbd, 0xcb, 0x01, 0x00, 0x00, 0x4c, 0x8d, 0x7f, 0x50, 0x41, 0x83, 0xfc, 0x01,
    0x75, 0x11, 0x41, 0x8d, 0x75, 0xb5, 0x41, 0xbd, 0xd8, 0x00, 0x00, 0x00, 0x4c, 0x8d, 0xbf, 0x80, 0x00, 0x00, 0x00};
static const unsigned char render_patched[38] = {
    0xbe, 0x60, 0x06, 0x00, 0x00, 0x41, 0xbd, 0x96, 0x03, 0x00, 0x00, 0x4c, 0x8d, 0x7f, 0x50, 0x41, 0x83, 0xfc, 0x01,
    0x75, 0x0f, 0xbe, 0x00, 0x03, 0x00, 0x00, 0x41, 0xbd, 0xb0, 0x01, 0x00, 0x00, 0x49, 0x83, 0xc7, 0x30, 0x90, 0x90};
// Offsets of the four immediates in render_patched.
enum { BIG_WIDTH = 1, BIG_HEIGHT = 7, SMALL_WIDTH = 22, SMALL_HEIGHT = 28 };
static const int32_t stored_original[8] = {0, 0, 816, 459, 0, 0, 192, 108};

static const Code expected[] = {
    // the table's one reader: vmovups xmm0,[0x1dfde70]; ...; vmovups xmm1,[0x1dfde80]
    CODE(0x274121, "the thumbnail size table's reader",
         0xc5, 0xf8, 0x10, 0x05, 0x47, 0x9d, 0xb8, 0x01, 0xc5, 0xf8, 0x29, 0x45, 0xc0, 0xc5, 0xf8, 0x10, 0x0d, 0x4a,
         0x9d, 0xb8, 0x01),
    CODE(PLAYER_MANAGER, "the player manager getter", 0x48, 0x8b, 0x05, 0xa1, 0xcb, 0x18, 0x03, 0xc3),
    CODE(CAR_BY_ID, "the garage's car by Id",
         0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48),
    CODE(BUILD_LIVERY_TASKS, "the saved car's task builder",
         0x48, 0x8b, 0xc4, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08, 0x53, 0x55, 0x56, 0x57, 0x41),
    CODE(GAME_ALLOCATE, "allocate",
         0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x8b, 0x15, 0x08, 0xc2, 0x22, 0x03),
    CODE(GAME_FREE, "free", 0x48, 0x85, 0xc9, 0x74, 0x1d, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9),
    CODE(SAVE_QUEUE_INIT, "the save queue init",
         0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x33, 0xff, 0x48, 0x8b, 0xd9, 0x48),
    CODE(BACKGROUND_TASK_ADD, "the background save's add task",
         0x48, 0x89, 0x54, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe),
    CODE(BACKGROUND_SUBMIT, "the background scheduler's submit",
         0x48, 0x83, 0xec, 0x28, 0x48, 0x89, 0x54, 0x24, 0x38, 0x48, 0x85, 0xd2, 0x74, 0x11, 0x48, 0x8d),
    CODE(CAPTURE_FINISH, "the thumbnail capture finish",
         0x40, 0x57, 0x48, 0x83, 0xec, 0x40, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff, 0x48),
};

static const Slot expected_slots[] = {
    {BACKGROUND_TASK_VTABLE, 0x9f3e40u, "CBackgroundSaveTask's vtable"},
    {UI_VTABLE, 0x141a040u, "UserInterface's vtable"},
};

typedef void *(*Getter)(void *);
typedef void **(*CarById)(void *garage, void **out, int id, int flag);
typedef void *(*BuildTasks)(void *list, void **car);
typedef void *(*Allocate)(size_t);
typedef void (*Initialize)(void *);
typedef void (*AddTask)(void *background, void **task);
typedef void (*Submit)(void *scheduler, void *background);
typedef void (*Release)(void *);
typedef void (*Free)(void *);

typedef struct {
    void **begin, **end, **capacity;
} RefVector;

static unsigned char *image, *stub;
enum { IDLE, NEXT_CAR, CAPTURE_READY, SAVING, SETTLE };

static int phase, key_down, rendered;
static void *saving;
static int car_ids[MAX_CARS];
static unsigned car_count, next_car;
static ULONGLONG deadline;

// ------------------------------------------------------------ size

// The render block and the size table at `scale` (1: the game's own).
static void patched_sizes(unsigned scale, unsigned char render[38], int32_t stored[8]) {
    if (scale <= 1) {
        fh2_copy_bytes(render, render_original, 38);
        fh2_copy_bytes(stored, stored_original, 32);
        return;
    }
    const int32_t sizes[4] = {816 * (int32_t)scale, 459 * (int32_t)scale, 192 * (int32_t)scale, 108 * (int32_t)scale};
    const int32_t table[8] = {0, 0, sizes[0], sizes[1], 0, 0, sizes[2], sizes[3]};
    fh2_copy_bytes(render, render_patched, 38);
    fh2_copy_bytes(render + BIG_WIDTH, &sizes[0], 4);
    fh2_copy_bytes(render + BIG_HEIGHT, &sizes[1], 4);
    fh2_copy_bytes(render + SMALL_WIDTH, &sizes[2], 4);
    fh2_copy_bytes(render + SMALL_HEIGHT, &sizes[3], 4);
    fh2_copy_bytes(stored, table, 32);
}

static int install_size(unsigned scale) {
    unsigned char render[38];
    int32_t stored[8];
    if (scale <= 1) {
        say("GarageThumbnails: CarPreviewScale=1, thumbnails at the console's size (F8 still re-renders them).");
        return 1;
    }
    patched_sizes(scale, render, stored);
    const char *problem = write_code(image + RENDER_SIZE, render_original, render, sizeof(render));
    if (problem) {
        say("GarageThumbnails: the renderer's size block 0x%x could not be written (%s); thumbnails stay at the "
            "console's size.", RENDER_SIZE, problem);
        return 0;
    }
    problem = write_code(image + STORED_SIZE, (const unsigned char *)stored_original, (const unsigned char *)stored,
                         sizeof(stored));
    if (problem) {
        // Put the renderer back: the two go together.
        write_code(image + RENDER_SIZE, render, render_original, sizeof(render));
        say("GarageThumbnails: the stored size table 0x%x could not be written (%s); thumbnails stay at the "
            "console's size.", STORED_SIZE, problem);
        return 0;
    }
    say("GarageThumbnails: new garage thumbnails at %ux the console's size (%dx%d and %dx%d).", scale, 816 * scale,
        459 * scale, 192 * scale, 108 * scale);
    return 1;
}

// ------------------------------------------------------------ rebuild

static void release(void *object) {
    if (object) ((Release)(*(void ***)object)[3])(object);
}

static void add_reference(void *object) { ((Release)(*(void ***)object)[2])(object); }

static void clear(RefVector *vector) {
    for (void **item = vector->begin; item && item < vector->end; ++item) release(*item);
    if (vector->begin) ((Free)(image + GAME_FREE))(vector->begin);
    vector->begin = vector->end = vector->capacity = NULL;
}

static void *garage(void) {
    void *manager = ((void *(*)(void))(image + PLAYER_MANAGER))();
    if (!manager) return NULL;
    void *player = ((Getter)(*(void ***)manager)[0x48 / 8])(manager);
    return player ? ((Getter)(*(void ***)player)[0xd8 / 8])(player) : NULL;
}

typedef struct {
    char *pointer_or_text[2];  // MSVC std::string: inline text up to 15 bytes
    size_t size, capacity;
} GameString;

typedef void (*Query)(void *database, void **result, const char *sql);
typedef int (*RowCount)(void *result);
typedef int (*IntegerAt)(void *result, int row, int column, int unused);

static const char *string_text(const GameString *text) {
    return text->capacity >= 16 ? text->pointer_or_text[0] : (const char *)text->pointer_or_text;
}

// Reads the garage Ids with a saved thumbnail, as the game queries its garage
// table (0x2e0210: table name in the garage object's string at +0x10; the SQL
// is a plain C string; database [GAME_DATABASE], query vtable +0x40, row
// count +0x148, integer cell +0x98).
static void read_ids(void *owner) {
    car_count = next_car = 0;
    void *database = read_pointer(image + GAME_DATABASE);
    if (!database) return;
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT Id FROM %s WHERE Thumbnail LIKE 'CS:%%' ORDER BY Id",
             string_text((const GameString *)((unsigned char *)owner + 0x10)));
    void *result = NULL;
    ((Query)((*(void ***)database)[0x40 / 8]))(database, &result, sql);
    if (result) {
        int rows = ((RowCount)((*(void ***)result)[0x148 / 8]))(result);
        for (int row = 0; row < rows && car_count < MAX_CARS; ++row) {
            int id = ((IntegerAt)((*(void ***)result)[0x98 / 8]))(result, row, 0, 0);
            if (id > 0) car_ids[car_count++] = id;
        }
        release(result);
    }
}

// The capture state machine that renders garage thumbnails, at
// [[CAPTURE_OWNER]+0x50]+0x24; null when it does not exist yet.
static volatile int32_t *capture_state(void) {
    unsigned char *owner = read_pointer(image + CAPTURE_OWNER);
    unsigned char *capture = owner ? read_pointer(owner + 0x50) : NULL;
    return capture ? (volatile int32_t *)(capture + 0x24) : NULL;
}

// Starts a thumbnail capture as PopupSaveDataWithThumbnail does before its
// saves (0xe10cd9): once the capture state is idle (5), set the render flag
// and move it to 1. Without this the thumbnail task re-saves the existing
// pictures (flag clear) or waits forever for a render (capture idle).
static int start_capture(void) {
    volatile int32_t *state = capture_state();
    unsigned char *thumbnails = read_pointer(image + THUMBNAIL_STATE);
    if (!state || *state != CAPTURE_IDLE || !thumbnails) return 0;
    thumbnails[THUMBNAIL_RENDER_FLAG] = 1;
    *state = CAPTURE_START;
    return 1;
}

// Hands one car's save tasks to a new background save. Returns the last task
// with an extra reference, to see when the save is done, or null if nothing
// was queued.
static void *queue_car(void *owner, int car_id) {
    void *car = NULL;
    ((CarById)(image + CAR_BY_ID))(owner, &car, car_id, 0);
    if (!car) {
        say("GarageThumbnails: no car for garage Id %d.", car_id);
        return NULL;
    }
    RefVector tasks = {0};
    // The builder takes over one reference to the car.
    add_reference(car);
    void *reference = car;
    ((BuildTasks)(image + BUILD_LIVERY_TASKS))(&tasks, &reference);
    release(car);
    void **background = ((Allocate)(image + GAME_ALLOCATE))(BACKGROUND_TASK_SIZE);
    if (!background || tasks.begin == tasks.end) {
        clear(&tasks);
        say("GarageThumbnails: no save tasks for garage Id %d.", car_id);
        return NULL;
    }
    background[0] = image + BACKGROUND_TASK_VTABLE;
    ((Initialize)(image + SAVE_QUEUE_INIT))(background + 1);
    for (void **task = tasks.begin; task < tasks.end; ++task) {
        // The queue takes over this reference.
        add_reference(*task);
        void *task_reference = *task;
        ((AddTask)(image + BACKGROUND_TASK_ADD))(background, &task_reference);
    }
    void *last = tasks.end[-1];
    add_reference(last);
    clear(&tasks);
    void *scheduler = read_pointer(image + BACKGROUND_SCHEDULER);
    ((Submit)(image + BACKGROUND_SUBMIT))(scheduler, background);
    return last;
}

static int capture_idle(void) {
    volatile int32_t *state = capture_state();
    return state && *state == CAPTURE_IDLE;
}

static void stop(const char *why) {
    volatile int32_t *state = capture_state();
    say("GarageThumbnails: F8 re-render stopped: %s (capture state %d).", why, state ? (int)*state : -1);
    if (saving) {
        release(saving);
        saving = NULL;
    }
    phase = IDLE;
}

// One car at a time, in PopupSaveDataWithThumbnail's order (0xe10c20): start
// a capture once the capture state is idle, wait for it to be ready (idle
// again), run the saves, let the presentation and thumbnail renderer settle
// with 0xe1f6d0 (it uses only globals), and wait for idle again. Queuing saves
// before the capture is ready renders the first car of a session but leaves
// the following ones blank.
static void advance(void) {
    ULONGLONG now = GetTickCount64();
    switch (phase) {
    case NEXT_CAR:
        if (next_car >= car_count) {
            say("GarageThumbnails: F8 re-render finished; %d car(s) rendered.", rendered);
            phase = IDLE;
        } else if (start_capture()) {
            phase = CAPTURE_READY;
            deadline = now + PHASE_WAIT_MS;
        } else if (now > deadline) {
            stop("the capture was never idle");
        }
        break;
    case CAPTURE_READY:
        if (capture_idle()) {
            void *owner = garage();
            if (!owner) {
                stop("the garage is unavailable");
                break;
            }
            saving = queue_car(owner, car_ids[next_car++]);
            phase = saving ? SAVING : SETTLE;
            deadline = now + SAVE_WAIT_MS;
        } else if (now > deadline) {
            stop("the capture was not ready");
        }
        break;
    case SAVING: {
        // Done once the save queue has released the task (ours is the last reference).
        LONG references = 0;
        fh2_safe_read((unsigned char *)saving + TASK_REFERENCES, &references, sizeof(references));
        if (references <= 1) {
            release(saving);
            saving = NULL;
            ++rendered;
            ((void (*)(void *))(image + CAPTURE_FINISH))(NULL);
            phase = SETTLE;
            deadline = now + PHASE_WAIT_MS;
        } else if (now > deadline) {
            stop("the save did not finish");
        }
        break;
    }
    case SETTLE:
        if (capture_idle()) {
            phase = NEXT_CAR;
            deadline = now + PHASE_WAIT_MS;
        } else if (now > deadline) {
            stop("the capture did not settle");
        }
        break;
    }
}

// F8 while the game's window (a window of this process) is in front.
static int f8_pressed(void) {
    HWND window = GetForegroundWindow();
    DWORD process = 0;
    if (!window || !GetWindowThreadProcessId(window, &process) || process != GetCurrentProcessId()) return 0;
    return (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
}

// Once per frame on the game's main thread (UserInterface slot 8).
static void tick(void) {
    int pressed = f8_pressed();
    int start = pressed && !key_down;
    key_down = pressed;
    if (start && phase == IDLE) {
        void *owner = garage();
        if (owner) read_ids(owner);
        rendered = 0;
        phase = car_count ? NEXT_CAR : IDLE;
        deadline = GetTickCount64() + PHASE_WAIT_MS;
        say("GarageThumbnails: F8 re-render started; %u car(s) with a saved thumbnail.", car_count);
    }
    if (phase != IDLE) advance();
}

// A stub for the slot that keeps the slot's arguments (rcx, rdx, r8, r9,
// xmm0-3) whatever its signature: it calls tick and then jumps to what the
// slot held (the game's function or another module's hook).
static unsigned char *make_stub(void *previous) {
    // push rcx/rdx/r8/r9; sub rsp,0x68; movdqu [rsp+0x20..0x50],xmm0-3; mov rax,tick
    static const unsigned char head[] = {0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x48, 0x83, 0xec, 0x68, 0xf3, 0x0f,
                                         0x7f, 0x44, 0x24, 0x20, 0xf3, 0x0f, 0x7f, 0x4c, 0x24, 0x30, 0xf3, 0x0f,
                                         0x7f, 0x54, 0x24, 0x40, 0xf3, 0x0f, 0x7f, 0x5c, 0x24, 0x50, 0x48, 0xb8};
    // call rax; restore; add rsp,0x68; pop r9/r8/rdx/rcx; jmp [rip+0]
    static const unsigned char tail[] = {0xff, 0xd0, 0xf3, 0x0f, 0x6f, 0x44, 0x24, 0x20, 0xf3, 0x0f, 0x6f, 0x4c, 0x24,
                                         0x30, 0xf3, 0x0f, 0x6f, 0x54, 0x24, 0x40, 0xf3, 0x0f, 0x6f, 0x5c, 0x24, 0x50,
                                         0x48, 0x83, 0xc4, 0x68, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59, 0xff, 0x25, 0,
                                         0,    0,    0};
    unsigned char *code = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!code) return NULL;
    unsigned char *p = code;
    void *callback = (void *)(uintptr_t)tick;
    fh2_copy_bytes(p, head, sizeof(head));
    p += sizeof(head);
    fh2_copy_bytes(p, &callback, 8);
    p += 8;
    fh2_copy_bytes(p, tail, sizeof(tail));
    p += sizeof(tail);
    fh2_copy_bytes(p, &previous, 8);
    DWORD ignored;
    if (!VirtualProtect(code, 0x1000, PAGE_EXECUTE_READ, &ignored)) {
        VirtualFree(code, 0, MEM_RELEASE);
        return NULL;
    }
    FlushInstructionCache(GetCurrentProcess(), code, 0x1000);
    return code;
}

static int install_rebuild(void) {
    void **slot = (void **)(image + UI_TICK_SLOT);
    void *before = read_pointer(slot);
    // The stub jumps to what the slot holds now; chain_slot checks it is the
    // game's function or another module's code before swapping.
    stub = before ? make_stub(before) : NULL;
    if (!stub) {
        say("GarageThumbnails: no memory for the F8 hook; F8 does nothing.");
        return 0;
    }
    void *previous = NULL;
    const char *problem = chain_slot(slot, image + UI_TICK, image, stub, &previous);
    if (problem || previous != before) {
        VirtualFree(stub, 0, MEM_RELEASE);
        stub = NULL;
        say("GarageThumbnails: UserInterface slot 0x%x could not be chained (%s); F8 does nothing.", UI_TICK_SLOT,
            problem ? problem : "another module changed it at the same time");
        return 0;
    }
    char where[160];
    if (before == image + UI_TICK)
        snprintf(where, sizeof(where), "the game's 0x%x", UI_TICK);
    else
        describe(before, where, sizeof(where));
    say("GarageThumbnails: UserInterface slot 0x%x chained onto %s; F8 re-renders the thumbnails of every car "
        "with a saved one.", UI_TICK_SLOT, where);
    return 1;
}

const char *thumbnails_check(const unsigned char *game, char *why, size_t size) {
    const char *problem = check_code(game, expected, ARRAY_COUNT(expected), why, size);
    if (!problem) problem = check_slots(game, expected_slots, ARRAY_COUNT(expected_slots), why, size);
    if (problem) return problem;
    if (!fh2_bytes_equal(game + RENDER_SIZE, render_original, sizeof(render_original))) {
        snprintf(why, size, "the renderer's size block at 0x%x is not as inspected", RENDER_SIZE);
        return why;
    }
    if (!fh2_bytes_equal(game + STORED_SIZE, stored_original, sizeof(stored_original))) {
        snprintf(why, size, "the stored size table at 0x%x is not as inspected", STORED_SIZE);
        return why;
    }
    const void *tick_now = read_pointer(game + UI_TICK_SLOT);
    if (tick_now != game + UI_TICK && (!tick_now || in_game(game, tick_now) || !fh2_is_code(tick_now))) {
        snprintf(why, size, "UserInterface slot 0x%x holds neither 0x%x nor another module's code", UI_TICK_SLOT,
                 UI_TICK);
        return why;
    }
    return NULL;
}

int thumbnails_install(unsigned char *game, const XtraSettings *settings) {
    image = game;
    int sized = install_size((unsigned)settings->scale);
    int rebuild = install_rebuild();
    return sized || rebuild;
}

#ifdef XTRA_TEST
// The render block (38 bytes) and size table (32) for a scale.
__declspec(dllexport) void XtraTestThumbnailSizes(unsigned scale, unsigned char *render, int32_t *stored) {
    patched_sizes(scale, render, stored);
}
#endif
