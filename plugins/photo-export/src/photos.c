// Photo Export: Azimuth's photo saves as JPG files.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A photo is a save container player-N\Photo_NNNN_<yyyyMMddHHmmss, UTC> whose
// revisions hold data\image (a JFIF JPEG), data\thumb, data\header and
// data\metadata. Only the newest revision with a commit is read; its image is
// exported when its size and SHA-256 match the commit and it starts with a
// JPEG marker. The JPG is written beside its final name and renamed to it
// without replacing anything: a file of that name is never overwritten (an
// identical one counts as exported, a different one gets the next name). The
// saves are only read (shared so that Azimuth can write, rename and delete
// while they are open). What was exported is listed in PhotoExport.list (a
// hidden file in the JPG folder), so a JPG deleted on purpose is not exported again.
#include <stdio.h>
#include <string.h>

#include "photo_export.h"

#define PHOTO_MAX_BYTES (256u << 20)
#define PHOTO_TRIES 5u
#define PHOTO_ERROR_LINES 20u
#define PHOTO_NAME_TRIES 10u
#define INDEX_MAX_BYTES (16u << 20)
#define TEMP_SUFFIX L".photo-export.tmp"

typedef struct {
    unsigned long long key;
    FILETIME revisions;
    unsigned char state, tries, counted;
} Entry;

enum { PHOTO_PENDING, PHOTO_DONE, PHOTO_FAILED };
enum { RESULT_EXPORTED, RESULT_PRESENT, RESULT_WAIT, RESULT_ERROR };

static wchar_t saves_root[FH2_PATH_CAPACITY], photos_root[FH2_PATH_CAPACITY], index_path[FH2_PATH_CAPACITY];
static wchar_t player_path[FH2_PATH_CAPACITY], container_path[FH2_PATH_CAPACITY], work_path[FH2_PATH_CAPACITY];
static wchar_t dest_path[FH2_PATH_CAPACITY], temp_path[FH2_PATH_CAPACITY];
static char commit_text[FH2_COMMIT_CAPACITY];
static Fh2Commit commit;
static Entry *entries;
static size_t entry_count, entry_capacity;
static unsigned long long *exported_keys;
static size_t exported_count_keys, exported_capacity;
static unsigned error_lines;
static int photos_ready, index_usable, newer_pending;
static unsigned long long found_count, exported_count, present_count, listed_count;
static const char *failure;

// ---------------------------------------------------------------- keys

static unsigned long long fnv(unsigned long long hash, const wchar_t *text) {
    for (; *text; ++text) {
        hash ^= (unsigned long long)fh2_wfold(*text);
        hash *= 1099511628211ull;
    }
    return hash;
}

#define FNV_START 14695981039346656037ull

static void *grow(void *items, size_t *capacity, size_t count, size_t item) {
    if (count < *capacity) return items;
    size_t more = *capacity ? *capacity * 2 : 256;
    void *grown = HeapAlloc(GetProcessHeap(), 0, more * item);
    if (!grown) return NULL;
    if (items) {
        memcpy(grown, items, count * item);
        HeapFree(GetProcessHeap(), 0, items);
    }
    *capacity = more;
    return grown;
}

static Entry *find_entry(unsigned long long key) {
    for (size_t i = 0; i < entry_count; ++i)
        if (entries[i].key == key) return &entries[i];
    Entry *grown = (Entry *)grow(entries, &entry_capacity, entry_count, sizeof(Entry));
    if (!grown) return NULL;
    entries = grown;
    Entry *entry = &entries[entry_count++];
    entry->key = key;
    entry->revisions.dwLowDateTime = entry->revisions.dwHighDateTime = 0;
    entry->state = PHOTO_PENDING;
    entry->tries = 0;
    entry->counted = 0;
    return entry;
}

static int ensure_photos_folder(void);

// ---------------------------------------------------------------- the list of exported photos

// "<player>\<container> <revision> <sha256>": the line of the list, and its key.
static size_t index_line(const wchar_t *player, const wchar_t *container, const wchar_t *revision,
                         const unsigned char digest[32], wchar_t *line, size_t capacity) {
    char hex[65];
    fh2_hex_digest(digest, hex);
    int length = _snwprintf(line, capacity, L"%ls\\%ls %ls %hs", player, container, revision, hex);
    return length > 0 && (size_t)length < capacity ? (size_t)length : 0;
}

static int index_has(unsigned long long key) {
    for (size_t i = 0; i < exported_count_keys; ++i)
        if (exported_keys[i] == key) return 1;
    return 0;
}

static void index_remember(unsigned long long key) {
    if (index_has(key)) return;
    unsigned long long *grown =
        (unsigned long long *)grow(exported_keys, &exported_capacity, exported_count_keys, sizeof(*exported_keys));
    if (!grown) return;
    exported_keys = grown;
    exported_keys[exported_count_keys++] = key;
}

static void index_load(void) {
    exported_count_keys = 0;
    index_usable = index_path[0] != 0;
    if (!index_usable) return;
    HANDLE file = CreateFileW(index_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            say("the list of exported photos cannot be read (error %lu); a JPG already in the folder still counts",
                GetLastError());
            index_usable = 0;
        }
        return;
    }
    LARGE_INTEGER size;
    char *text = NULL;
    DWORD read = 0;
    if (GetFileSizeEx(file, &size) && size.QuadPart < INDEX_MAX_BYTES &&
        (text = (char *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)size.QuadPart + 1)) &&
        ReadFile(file, text, (DWORD)size.QuadPart, &read, NULL) && read == (DWORD)size.QuadPart) {
        wchar_t line[FH2_PATH_CAPACITY];
        for (DWORD at = 0; at < read;) {
            DWORD end = at;
            while (end < read && text[end] != '\n' && text[end] != '\r') ++end;
            int length = end > at ? MultiByteToWideChar(CP_UTF8, 0, text + at, (int)(end - at), line,
                                                        FH2_PATH_CAPACITY - 1)
                                  : 0;
            if (length > 0) {
                line[length] = 0;
                index_remember(fnv(FNV_START, line));
            }
            at = end + 1;
        }
    } else {
        say("the list of exported photos cannot be read; a JPG already in the folder still counts");
        index_usable = 0;
    }
    if (text) HeapFree(GetProcessHeap(), 0, text);
    CloseHandle(file);
}

static void index_add(const wchar_t *line) {
    index_remember(fnv(FNV_START, line));
    if (!index_usable || !ensure_photos_folder()) return;
    char text[FH2_PATH_CAPACITY * 3 + 2];
    size_t length = fh2_utf8(line, text, sizeof(text) - 2);
    text[length++] = '\r';
    text[length++] = '\n';
    HANDLE file = CreateFileW(index_path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN,
                              NULL);
    DWORD written = 0;
    int ok = file != INVALID_HANDLE_VALUE && WriteFile(file, text, (DWORD)length, &written, NULL) &&
             written == length;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!ok) {
        say("the list of exported photos cannot be written (error %lu); a JPG already in the folder still counts",
            GetLastError());
        index_usable = 0;
    }
}

// ---------------------------------------------------------------- one container

// The newest revision folder with a commit file; its name goes to best.
// newer_pending: a newer revision without a commit (being written) is there.
static int newest_committed(wchar_t best[20]) {
    WIN32_FIND_DATAW data;
    unsigned long long best_value = 0, newest_value = 0;
    int found = 0, any = 0;
    newer_pending = 0;
    size_t revisions = fh2_path_join(work_path, fh2_path_set(work_path, container_path), L"revisions\\*");
    HANDLE find = revisions ? FindFirstFileW(work_path, &data) : INVALID_HANDLE_VALUE;
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        int valid;
        unsigned long long value = fh2_revision_number(data.cFileName, &valid);
        if (!fh2_plain_folder(&data) || !valid) continue;
        if (!any || value > newest_value) newest_value = value;
        any = 1;
        if (found && value <= best_value) continue;
        size_t length = fh2_path_join(work_path, fh2_path_set(work_path, container_path), L"revisions");
        length = fh2_path_join(work_path, length, data.cFileName);
        length = fh2_path_join(work_path, length, L"commit");
        DWORD attributes = length ? GetFileAttributesW(work_path) : INVALID_FILE_ATTRIBUTES;
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
            continue;
        fh2_wcopy(best, data.cFileName, fh2_wlen(data.cFileName) + 1);
        best_value = value;
        found = 1;
    } while (FindNextFileW(find, &data));
    FindClose(find);
    newer_pending = any && (!found || newest_value > best_value);
    return found;
}

static void put_digits(wchar_t *at, unsigned value, unsigned width) {
    for (unsigned d = width; d; --d) {
        at[d - 1] = (wchar_t)(L'0' + value % 10);
        value /= 10;
    }
}

// "FH2 yyyy-MM-dd HH-mm-ss" in local time from the container name
// Photo_NNNN_yyyyMMddHHmmss (UTC), else from the image's write time;
// number receives NNNN.
static void photo_name(const wchar_t *container, FILETIME written, wchar_t name[32], wchar_t number[12]) {
    SYSTEMTIME utc, local;
    int parsed = 0;
    memset(&utc, 0, sizeof(utc));
    const wchar_t *cursor = container + 6;  // after "Photo_"
    size_t digits = 0;
    while (digits < 10 && cursor[digits] >= L'0' && cursor[digits] <= L'9') {
        number[digits] = cursor[digits];
        ++digits;
    }
    number[digits] = 0;
    if (digits && cursor[digits] == L'_') {
        const wchar_t *stamp = cursor + digits + 1;
        unsigned values[14], i = 0;
        for (; i < 14 && stamp[i] >= L'0' && stamp[i] <= L'9'; ++i) values[i] = stamp[i] - L'0';
        if (i == 14 && !stamp[14]) {
            utc.wYear = (WORD)(values[0] * 1000 + values[1] * 100 + values[2] * 10 + values[3]);
            utc.wMonth = (WORD)(values[4] * 10 + values[5]);
            utc.wDay = (WORD)(values[6] * 10 + values[7]);
            utc.wHour = (WORD)(values[8] * 10 + values[9]);
            utc.wMinute = (WORD)(values[10] * 10 + values[11]);
            utc.wSecond = (WORD)(values[12] * 10 + values[13]);
            FILETIME check;
            parsed = utc.wYear >= 2000 && SystemTimeToFileTime(&utc, &check);
        }
    }
    if (!parsed) parsed = FileTimeToSystemTime(&written, &utc);
    if (!parsed || !SystemTimeToTzSpecificLocalTime(NULL, &utc, &local)) local = utc;
    static const wchar_t pattern[] = L"FH2 0000-00-00 00-00-00";
    memcpy(name, pattern, sizeof(pattern));
    put_digits(name + 4, local.wYear, 4);
    put_digits(name + 9, local.wMonth, 2);
    put_digits(name + 12, local.wDay, 2);
    put_digits(name + 15, local.wHour, 2);
    put_digits(name + 18, local.wMinute, 2);
    put_digits(name + 21, local.wSecond, 2);
}

static int ensure_photos_folder(void) {
    if (!photos_ready) photos_ready = fh2_make_folders(photos_root, fh2_wlen(photos_root) + 1);
    return photos_ready;
}

// 1: path is a file holding exactly image[0..size).
static int same_file(const wchar_t *path, const unsigned char *image, DWORD size) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER length;
    int same = GetFileSizeEx(file, &length) && length.QuadPart == size;
    unsigned char chunk[65536];
    for (DWORD at = 0; same && at < size;) {
        DWORD want = size - at < sizeof(chunk) ? size - at : (DWORD)sizeof(chunk), read = 0;
        same = ReadFile(file, chunk, want, &read, NULL) && read == want && !memcmp(chunk, image + at, want);
        at += want;
    }
    CloseHandle(file);
    return same;
}

// Writes the image as dest_path through a temporary file beside it; never replaces.
static int write_photo(const unsigned char *image, DWORD size, FILETIME written, DWORD *error) {
    size_t length = fh2_path_append(temp_path, fh2_path_set(temp_path, dest_path), TEMP_SUFFIX);
    *error = ERROR_BUFFER_OVERFLOW;
    if (!length) return 0;
    HANDLE file = CreateFileW(temp_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        return 0;
    }
    DWORD done = 0;
    int ok = WriteFile(file, image, size, &done, NULL) && done == size && FlushFileBuffers(file);
    if (ok) SetFileTime(file, &written, NULL, &written);
    *error = ok ? 0 : GetLastError();
    CloseHandle(file);
    // The written copy is read back before it gets its name.
    if (ok && !same_file(temp_path, image, size)) {
        ok = 0;
        *error = ERROR_CRC;
    }
    if (ok && !MoveFileExW(temp_path, dest_path, MOVEFILE_WRITE_THROUGH)) {  // no REPLACE_EXISTING
        *error = GetLastError();
        ok = 0;
    }
    if (!ok) DeleteFileW(temp_path);
    return ok;
}

static int export_container(const wchar_t *player, const wchar_t *container) {
    wchar_t revision[20], name[32], number[12], suffix[24], line[FH2_PATH_CAPACITY];
    unsigned char digest[32];
    size_t commit_size = 0;
    if (!newest_committed(revision)) return RESULT_WAIT;
    size_t base = fh2_path_join(work_path, fh2_path_set(work_path, container_path), L"revisions");
    base = fh2_path_join(work_path, base, revision);
    if (!fh2_path_join(work_path, base, L"commit") ||
        !fh2_read_small(work_path, commit_text, sizeof(commit_text), &commit_size)) {
        failure = "its commit file cannot be read";
        return RESULT_ERROR;
    }
    const char *invalid = fh2_parse_commit(commit_text, commit_size, &commit);
    if (invalid) {
        failure = invalid;
        return RESULT_ERROR;
    }
    const Fh2Blob *blob = fh2_commit_blob(&commit, L"image");
    if (!blob) {
        failure = "its commit lists no image";
        return RESULT_ERROR;
    }
    if (!blob->size || blob->size > PHOTO_MAX_BYTES) {
        failure = "the image size is out of range";
        return RESULT_ERROR;
    }
    if (!index_line(player, container, revision, blob->hash, line, FH2_PATH_CAPACITY)) {
        failure = "its name is too long";
        return RESULT_ERROR;
    }
    if (index_has(fnv(FNV_START, line))) return RESULT_PRESENT;  // exported before (listed)
    DWORD size = (DWORD)blob->size;
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!fh2_path_join(work_path, base, L"data\\image") ||
        !GetFileAttributesExW(work_path, GetFileExInfoStandard, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) || info.nFileSizeHigh ||
        info.nFileSizeLow != size) {
        failure = "data\\image is missing or its size differs from the commit";
        return RESULT_ERROR;
    }
    unsigned char *image = (unsigned char *)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!image) {
        failure = "out of memory";
        return RESULT_ERROR;
    }
    int result = RESULT_ERROR;
    HANDLE file = CreateFileW(work_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    DWORD read = 0;
    int ok = file != INVALID_HANDLE_VALUE && ReadFile(file, image, size, &read, NULL) && read == size;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    Fh2Sha256 hash;
    if (!ok) {
        failure = "data\\image cannot be read";
        goto done;
    }
    if (size < 4 || image[0] != 0xff || image[1] != 0xd8 || image[2] != 0xff) {
        failure = "data\\image is not a JPEG";
        goto done;
    }
    if (!fh2_sha256_begin(&hash) || !fh2_sha256_add(&hash, image, size) || !fh2_sha256_end(&hash, digest)) {
        failure = "SHA-256 is not available";
        goto done;
    }
    if (!fh2_digest_equal(digest, blob->hash)) {
        failure = "data\\image does not match the SHA-256 in its commit";
        goto done;
    }
    if (!ensure_photos_folder()) {
        failure = "the JPG folder cannot be created";
        goto done;
    }
    photo_name(container, info.ftLastWriteTime, name, number);
    for (unsigned attempt = 0; attempt < PHOTO_NAME_TRIES; ++attempt) {
        // "", " (NNNN)", " (NNNN-2)", ...
        suffix[0] = 0;
        if (attempt == 1) _snwprintf(suffix, 24, L" (%ls)", number[0] ? number : L"0");
        if (attempt > 1) _snwprintf(suffix, 24, L" (%ls-%u)", number[0] ? number : L"0", attempt);
        suffix[23] = 0;
        size_t length = fh2_path_join(dest_path, fh2_path_set(dest_path, photos_root), name);
        length = fh2_path_append(dest_path, length, suffix);
        length = fh2_path_append(dest_path, length, L".jpg");
        if (!length) {
            failure = "the JPG's path is too long";
            goto done;
        }
        if (fh2_exists(dest_path)) {
            if (same_file(dest_path, image, size)) {  // exported before: left alone
                index_add(line);
                result = RESULT_PRESENT;
                goto done;
            }
            continue;  // a different file: never overwritten
        }
        DWORD error;
        if (write_photo(image, size, info.ftLastWriteTime, &error)) {
            char from[FH2_PATH_CAPACITY * 3], to[FH2_PATH_CAPACITY * 3];
            fh2_utf8(line, from, sizeof(from));
            fh2_utf8(dest_path, to, sizeof(to));
            *strchr(from, ' ') = 0;  // "<player>\<container>"
            index_add(line);
            say("exported %s\\revisions\\%ls -> %s (%lu bytes, SHA-256 verified)", from, revision, to, size);
            result = RESULT_EXPORTED;
            goto done;
        }
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) {
            failure = "the JPG cannot be written";
            goto done;
        }
    }
    failure = "every file name for it is taken by a different file";
done:
    VirtualFree(image, 0, MEM_RELEASE);
    return result;
}

static int check_container(const wchar_t *player, const wchar_t *container) {
    WIN32_FIND_DATAW data;
    size_t length = fh2_path_join(work_path, fh2_path_set(work_path, container_path), L"revisions");
    HANDLE find = length ? FindFirstFileW(work_path, &data) : INVALID_HANDLE_VALUE;
    if (find == INVALID_HANDLE_VALUE) return 0;
    FindClose(find);
    if (!fh2_plain_folder(&data)) return 0;
    Entry *entry = find_entry(fnv(fnv(FNV_START, player), container));
    if (!entry) return 0;
    // A new revision changes the revisions folder's write time.
    int changed = CompareFileTime(&entry->revisions, &data.ftLastWriteTime) != 0;
    if (!changed && entry->state != PHOTO_PENDING) return 0;
    if (changed) {
        entry->revisions = data.ftLastWriteTime;
        entry->state = PHOTO_PENDING;
        entry->tries = 0;
    }
    failure = NULL;
    int result = export_container(player, container);
    if (result == RESULT_WAIT) return 0;
    if (result == RESULT_ERROR) {
        if (++entry->tries < PHOTO_TRIES) return 0;
        entry->state = PHOTO_FAILED;
        if (error_lines < PHOTO_ERROR_LINES) {
            char path[FH2_PATH_CAPACITY * 3];
            fh2_utf8(container_path, path, sizeof(path));
            say("ERROR: %s not exported: %s (checked %u times; tried again when the photo changes)", path,
                failure ? failure : "unknown error", PHOTO_TRIES);
            if (++error_lines == PHOTO_ERROR_LINES) say("further errors are not logged this session");
        }
        return 0;
    }
    // Done, unless a newer revision is still being written: then it is looked
    // at again on the next pass (its commit does not change the folder's time).
    entry->state = newer_pending ? PHOTO_PENDING : PHOTO_DONE;
    if (!entry->counted) ++found_count;
    if (result == RESULT_PRESENT) {
        if (!entry->counted) ++present_count;
        entry->counted = 1;
        return 0;
    }
    entry->counted = 1;
    ++exported_count;
    return 1;
}

int photos_scan(void) {
    WIN32_FIND_DATAW players, photos;
    int exported = 0;
    size_t root = fh2_path_set(player_path, saves_root);
    HANDLE find = fh2_path_join(player_path, root, L"player-*") ? FindFirstFileW(player_path, &players)
                                                               : INVALID_HANDLE_VALUE;
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!fh2_plain_folder(&players)) continue;
        size_t player = fh2_path_join(player_path, root, players.cFileName);
        size_t pattern = fh2_path_join(container_path, fh2_path_set(container_path, player_path), L"Photo_*");
        HANDLE inner = player && pattern ? FindFirstFileW(container_path, &photos) : INVALID_HANDLE_VALUE;
        if (inner == INVALID_HANDLE_VALUE) continue;
        do {
            if (!fh2_plain_folder(&photos)) continue;
            if (!fh2_path_join(container_path, fh2_path_set(container_path, player_path), photos.cFileName)) continue;
            exported += check_container(players.cFileName, photos.cFileName);
        } while (FindNextFileW(inner, &photos));
        FindClose(inner);
    } while (FindNextFileW(find, &players));
    FindClose(find);
    return exported;
}

void photos_prepare(void) {
    // Temporary files of an export cut short (the game closed mid-write): only
    // this plugin's own *.jpg.photo-export.tmp in the JPG folder.
    WIN32_FIND_DATAW data;
    size_t root = fh2_path_set(work_path, photos_root);
    HANDLE find = fh2_path_join(work_path, root, L"*.jpg" TEMP_SUFFIX) ? FindFirstFileW(work_path, &data)
                                                                      : INVALID_HANDLE_VALUE;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (!fh2_plain_file(&data)) continue;
            if (fh2_path_join(temp_path, fh2_path_set(temp_path, photos_root), data.cFileName)) DeleteFileW(temp_path);
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    index_load();
    listed_count = exported_count_keys;
}

void photos_summary(void) {
    say("first scan: %llu photos, %llu exported now, %llu exported before (%llu listed in PhotoExport.list)",
        found_count, exported_count, present_count, listed_count);
}

int photos_configure(const wchar_t *saves, const wchar_t *folder, const wchar_t *index) {
    if (!fh2_path_set(saves_root, saves) || !fh2_path_set(photos_root, folder)) return 0;
    if (!index || !*index || !fh2_path_set(index_path, index)) index_path[0] = 0;
    entry_count = 0;
    exported_count_keys = 0;
    error_lines = 0;
    photos_ready = 0;
    found_count = exported_count = present_count = listed_count = 0;
    return 1;
}
