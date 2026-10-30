// Saves in Documents (mirror): a verified copy of Azimuth's saves.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Azimuth keeps the saves in <Azimuth folder>\data\titles\1A96545B\saves and
// this plugin leaves them there (nothing is redirected, linked or written in
// that folder). It copies each committed revision, once, into the same layout
// below the mirror folder (default Documents\My Games\Forza Horizon 2\Saves\1A96545B):
//
//   - Only revisions with a commit are read: Azimuth renames "pending" to
//     "commit" after its blobs are written, and never writes that revision
//     again (a save is a new revision). A revision without one is being
//     written and is left alone. The saves are opened shared (read, write,
//     delete), so Azimuth can still write, rename or delete while a copy runs.
//   - A copy is made in <mirror>.staging\<name>, every blob checked against
//     the size and SHA-256 its commit lists while it is copied and again when
//     the copy is read back, the commit written last, then the folder is
//     renamed into place. A revision already in the mirror is never written:
//     the same commit means it is there; a different one (or a damaged copy)
//     is kept as it is and this revision goes to <mirror>.conflicts.
//   - Nothing in the mirror is deleted (KeepRevisions=0, the default). With
//     KeepRevisions=n (n >= 2), revisions of a container older than its n
//     newest verified ones are removed from the mirror only, file by listed file.
//   - A copy cut short (the game closed during it) stays in .staging; at the
//     next start it is renamed into place when every blob verifies, else
//     removed (only .staging is ever cleaned up).
#include <stdio.h>
#include <string.h>

#include "documents_saves.h"

#define CHUNK (1u << 20)
#define MAX_REVISIONS 256u
#define TRIES 5u
#define ERROR_LINES 30u
#define WARNING_LINES 10u
#define SPACE_MARGIN (64ull << 20)

typedef struct {
    unsigned long long key;
    unsigned char state, tries;
} Entry;

enum { REVISION_NEW, REVISION_DONE, REVISION_FAILED };
enum { COPY_OK, COPY_KILLED, COPY_GONE, COPY_ERROR };

static wchar_t source_root[FH2_PATH_CAPACITY], mirror_root[FH2_PATH_CAPACITY];
static wchar_t staging_root[FH2_PATH_CAPACITY], conflict_root[FH2_PATH_CAPACITY];
static size_t mirror_length;
static unsigned keep_revisions;
static char source_commit[FH2_COMMIT_CAPACITY], other_commit[FH2_COMMIT_CAPACITY];
static Fh2Commit commit, other;
static unsigned char *chunk_buffer;
static Entry *entries;
static size_t entry_count, entry_capacity;
static unsigned error_lines, warning_lines, unique;
static unsigned long long seen_count, copied_count, present_count, conflict_count, failed_count, pruned_count;
static const char *failure;
static unsigned chunk_size = CHUNK;
#ifdef DOCUMENTS_SAVES_TEST
static MirrorFault fault_hook;
static unsigned long long fake_free_space;
#define FAULT(step, path) (fault_hook && fault_hook((step), (path)))
#else
#define FAULT(step, path) 0
#endif

// ---------------------------------------------------------------- small helpers

static unsigned long long fnv(unsigned long long hash, const wchar_t *text) {
    for (; *text; ++text) {
        hash ^= (unsigned long long)fh2_wfold(*text);
        hash *= 1099511628211ull;
    }
    return hash;
}

static Entry *entry_for(const wchar_t *space, const wchar_t *container, const wchar_t *revision) {
    unsigned long long key = fnv(fnv(fnv(14695981039346656037ull, space), container), revision);
    for (size_t i = 0; i < entry_count; ++i)
        if (entries[i].key == key) return &entries[i];
    if (entry_count == entry_capacity) {
        size_t more = entry_capacity ? entry_capacity * 2 : 512;
        Entry *grown = (Entry *)HeapAlloc(GetProcessHeap(), 0, more * sizeof(Entry));
        if (!grown) return NULL;
        if (entries) {
            memcpy(grown, entries, entry_count * sizeof(Entry));
            HeapFree(GetProcessHeap(), 0, entries);
        }
        entries = grown;
        entry_capacity = more;
    }
    Entry *entry = &entries[entry_count++];
    entry->key = key;
    entry->state = REVISION_NEW;
    entry->tries = 0;
    return entry;
}

static void say_path(const char *before, const wchar_t *path, const char *after) {
    char text[FH2_PATH_CAPACITY * 3];
    fh2_utf8(path, text, sizeof(text));
    say("%s%s%s", before, text, after);
}

static int error_allowed(void) {
    if (error_lines >= ERROR_LINES) return 0;
    if (++error_lines == ERROR_LINES) say("further errors are not logged this session");
    return error_lines < ERROR_LINES;
}

// "<space>\<container>\revisions\<revision>" into text.
static void relative_name(char *text, size_t capacity, const wchar_t *space, const wchar_t *container,
                          const wchar_t *revision) {
    snprintf(text, capacity, "%ls\\%ls\\revisions\\%ls", space, container, revision);
    text[capacity - 1] = 0;
}

static size_t revision_path(wchar_t *path, const wchar_t *root, const wchar_t *space, const wchar_t *container,
                            const wchar_t *revision) {
    size_t length = fh2_path_join(path, fh2_path_set(path, root), space);
    length = fh2_path_join(path, length, container);
    length = fh2_path_join(path, length, L"revisions");
    return revision ? fh2_path_join(path, length, revision) : length;
}

// Removes a folder this plugin made below .staging, and everything in it.
// Links inside are removed as links, never followed.
static void remove_tree(wchar_t *path, size_t length) {
    WIN32_FIND_DATAW data;
    size_t pattern = fh2_path_join(path, length, L"*");
    HANDLE find = pattern ? FindFirstFileW(path, &data) : INVALID_HANDLE_VALUE;
    path[length] = 0;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (fh2_dot_entry(&data)) continue;
            size_t child = fh2_path_join(path, length, data.cFileName);
            if (!child) continue;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) RemoveDirectoryW(path);
                else remove_tree(path, child);
            } else {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                    SetFileAttributesW(path, data.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
                DeleteFileW(path);
            }
            path[length] = 0;
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    RemoveDirectoryW(path);
}

// ---------------------------------------------------------------- copying one blob

// SHA-256 of a whole file; 1 = read, its size in *size.
static int hash_file(const wchar_t *path, unsigned char digest[32], unsigned long long *size) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    Fh2Sha256 hash;
    int ok = fh2_sha256_begin(&hash);
    *size = 0;
    while (ok) {
        DWORD read = 0;
        ok = ReadFile(file, chunk_buffer, chunk_size, &read, NULL) != 0;
        if (!ok || !read) break;
        ok = fh2_sha256_add(&hash, chunk_buffer, read);
        *size += read;
    }
    ok = fh2_sha256_end(&hash, digest) && ok;
    CloseHandle(file);
    return ok;
}

static int same_size(const wchar_t *path, unsigned long long size) {
    WIN32_FILE_ATTRIBUTE_DATA info;
    return GetFileAttributesExW(path, GetFileExInfoStandard, &info) &&
           !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
           (((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow) == size;
}

// Copies from (a blob in the game folder) to `to` (new), checking it against
// the commit; then reads the copy back and checks it again.
static int copy_blob(const wchar_t *from, const wchar_t *to, const Fh2Blob *blob) {
    HANDLE input = CreateFileW(from, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (input == INVALID_HANDLE_VALUE) {
        failure = "a blob cannot be opened";
        return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND ? COPY_GONE : COPY_ERROR;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(input, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        (((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow) != blob->size) {
        CloseHandle(input);
        failure = "a blob's size differs from its commit";
        return COPY_ERROR;
    }
    HANDLE output = CreateFileW(to, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE) {
        CloseHandle(input);
        failure = "a copy of a blob cannot be created";
        return COPY_ERROR;
    }
    Fh2Sha256 hash;
    unsigned char digest[32];
    unsigned long long total = 0;
    int result = fh2_sha256_begin(&hash) ? COPY_OK : COPY_ERROR;
    if (result != COPY_OK) failure = "SHA-256 is not available";
    while (result == COPY_OK) {
        DWORD read = 0, written = 0;
        if (!ReadFile(input, chunk_buffer, chunk_size, &read, NULL)) {
            failure = "a blob cannot be read";
            result = COPY_ERROR;
            break;
        }
        if (!read) break;
        total += read;
        if (total > blob->size || !fh2_sha256_add(&hash, chunk_buffer, read) ||
            !WriteFile(output, chunk_buffer, read, &written, NULL) || written != read) {
            failure = total > blob->size ? "a blob grew while it was copied" : "a copy of a blob cannot be written";
            result = COPY_ERROR;
            break;
        }
        if (FAULT(0, to)) {
            fh2_sha256_end(&hash, digest);
            CloseHandle(output);
            CloseHandle(input);
            return COPY_KILLED;
        }
    }
    int hashed = fh2_sha256_end(&hash, digest);
    if (result == COPY_OK && (!hashed || total != blob->size || !fh2_digest_equal(digest, blob->hash))) {
        failure = "a blob does not match the size and SHA-256 its commit lists";
        result = COPY_ERROR;
    }
    if (result == COPY_OK && !FlushFileBuffers(output)) {
        failure = "a copy of a blob cannot be flushed";
        result = COPY_ERROR;
    }
    if (result == COPY_OK) SetFileTime(output, &info.ftCreationTime, NULL, &info.ftLastWriteTime);
    CloseHandle(output);
    CloseHandle(input);
    if (result != COPY_OK) return result;
    unsigned long long size;
    if (!hash_file(to, digest, &size) || size != blob->size || !fh2_digest_equal(digest, blob->hash)) {
        failure = "a copied blob does not read back as written";
        return COPY_ERROR;
    }
    return COPY_OK;
}

// Writes a whole file (new) and reads it back.
static int write_new(const wchar_t *path, const char *text, size_t size) {
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    DWORD written = 0;
    int ok = WriteFile(file, text, (DWORD)size, &written, NULL) && written == size && FlushFileBuffers(file);
    CloseHandle(file);
    size_t read = 0;
    return ok && fh2_read_small(path, other_commit, sizeof(other_commit), &read) && read == size &&
           !memcmp(other_commit, text, size);
}

// ---------------------------------------------------------------- copying one revision

static unsigned long long free_space(const wchar_t *path) {
#ifdef DOCUMENTS_SAVES_TEST
    if (fake_free_space) return fake_free_space;
#endif
    wchar_t probe[FH2_PATH_CAPACITY];
    size_t length = fh2_path_set(probe, path);
    ULARGE_INTEGER available;
    while (length > 3) {
        if (GetDiskFreeSpaceExW(probe, &available, NULL, NULL)) return available.QuadPart;
        while (length > 3 && probe[length - 1] != L'\\') --length;
        if (length > 3) probe[--length] = 0;
    }
    return GetDiskFreeSpaceExW(probe, &available, NULL, NULL) ? available.QuadPart : ~0ull;
}

// A staging folder name: <kind>~<space>~<container>~<revision>~<unique>
// (kind m: into the mirror, c: into the conflicts); names hold no '~'.
static int staging_name(wchar_t *name, size_t capacity, wchar_t kind, const wchar_t *space, const wchar_t *container,
                        const wchar_t *revision) {
    for (const wchar_t *text = space; *text; ++text)
        if (*text == L'~') return 0;
    for (const wchar_t *text = container; *text; ++text)
        if (*text == L'~') return 0;
    int length = _snwprintf(name, capacity, L"%lc~%ls~%ls~%ls~%08lx%04x", kind, space, container, revision,
                            GetCurrentProcessId() ^ GetTickCount(), ++unique & 0xffff);
    return length > 0 && (size_t)length < capacity;
}

// Copies the revision `from` (its commit already read and parsed into source_commit /
// commit) into a new staging folder and renames it to `to`.
static int copy_revision(const wchar_t *from, const wchar_t *to, wchar_t kind, const wchar_t *space,
                         const wchar_t *container, const wchar_t *revision, size_t commit_size) {
    wchar_t stage[FH2_PATH_CAPACITY], source[FH2_PATH_CAPACITY], target[FH2_PATH_CAPACITY], name[300];
    if (free_space(mirror_root) < commit.bytes + SPACE_MARGIN) {
        failure = "the disk of the mirror folder has too little free space";
        return COPY_ERROR;
    }
    if (!fh2_make_folders(staging_root, mirror_length) ||
        !staging_name(name, sizeof(name) / sizeof(name[0]), kind, space, container, revision)) {
        failure = "the staging folder cannot be made";
        return COPY_ERROR;
    }
    size_t stage_length = fh2_path_join(stage, fh2_path_set(stage, staging_root), name);
    if (!stage_length || !CreateDirectoryW(stage, NULL)) {
        failure = "the staging folder cannot be made";
        return COPY_ERROR;
    }
    size_t data_length = fh2_path_join(stage, stage_length, L"data");
    int result = data_length && CreateDirectoryW(stage, NULL) ? COPY_OK : COPY_ERROR;
    if (result != COPY_OK) failure = "the staging folder cannot be made";
    size_t source_length = fh2_path_join(source, fh2_path_set(source, from), L"data");
    for (unsigned i = 0; result == COPY_OK && i < commit.count; ++i) {
        if (!fh2_path_join(source, source_length, commit.blobs[i].name) ||
            !fh2_path_join(stage, data_length, commit.blobs[i].name)) {
            failure = "a path is too long";
            result = COPY_ERROR;
            break;
        }
        result = copy_blob(source, stage, &commit.blobs[i]);
    }
    if (result == COPY_KILLED) return result;
    if (result == COPY_OK) {
        if (!fh2_path_join(stage, stage_length, L"commit") || !write_new(stage, source_commit, commit_size)) {
            failure = "the commit cannot be written";
            result = COPY_ERROR;
        }
    }
    stage[stage_length] = 0;
    if (result == COPY_OK && FAULT(1, stage)) return COPY_KILLED;
    // The revision's parent folders in the mirror; a link among them is refused.
    size_t parent = fh2_path_set(target, to);
    while (parent && target[parent - 1] != L'\\') --parent;
    if (result == COPY_OK && parent) {
        target[parent - 1] = 0;
        if (!fh2_make_folders(target, mirror_length)) {
            failure = "the mirror's folders cannot be made (or one is a link)";
            result = COPY_ERROR;
        }
    }
    if (result == COPY_OK && FAULT(2, stage)) return COPY_KILLED;
    if (result == COPY_OK && !MoveFileExW(stage, to, MOVEFILE_WRITE_THROUGH)) {  // never replaces
        failure = fh2_exists(to) ? "the revision appeared in the mirror meanwhile" : "the copy cannot be renamed into place";
        result = COPY_ERROR;
    }
    if (result != COPY_OK) {
        stage[stage_length] = 0;
        remove_tree(stage, stage_length);
    }
    RemoveDirectoryW(staging_root);  // only when empty
    if (result != COPY_OK) {
        // Gone from the game folder (Azimuth dropped an old revision): no error.
        wchar_t check[FH2_PATH_CAPACITY];
        if (fh2_path_join(check, fh2_path_set(check, from), L"commit") && !fh2_exists(check)) return COPY_GONE;
    }
    return result;
}

// ---------------------------------------------------------------- one revision

// 1: the mirror's revision folder `path` holds exactly the commit text and
// blobs of the right sizes (its blobs were hashed when copied).
static int mirror_matches(const wchar_t *path, size_t commit_size) {
    wchar_t work[FH2_PATH_CAPACITY];
    size_t read = 0, length = fh2_path_set(work, path);
    if (!fh2_is_plain_folder(path) || !fh2_path_join(work, length, L"commit") ||
        !fh2_read_small(work, other_commit, sizeof(other_commit), &read) || read != commit_size ||
        memcmp(other_commit, source_commit, read))
        return 0;
    length = fh2_path_join(work, length, L"data");
    for (unsigned i = 0; i < commit.count; ++i)
        if (!fh2_path_join(work, length, commit.blobs[i].name) || !same_size(work, commit.blobs[i].size)) return 0;
    return 1;
}

static void prune(const wchar_t *space, const wchar_t *container);
static unsigned newer_in_mirror(const wchar_t *space, const wchar_t *container, const wchar_t *revision);

// A revision reaches its end: counted once.
static void finish(Entry *entry, unsigned char state, unsigned long long *counter) {
    entry->state = state;
    ++seen_count;
    if (counter) ++*counter;
}

// A copy that did not finish: tried again on the next passes, given up (logged) after TRIES.
static void retry(Entry *entry, int result, const char *name, const char *where) {
    if (result == COPY_KILLED) return;
    if (result == COPY_GONE) {  // Azimuth dropped that revision meanwhile (it keeps the newest two)
        entry->state = REVISION_DONE;
        return;
    }
    if (++entry->tries < TRIES) return;
    finish(entry, REVISION_FAILED, &failed_count);
    if (error_allowed()) say("ERROR: %s not copied%s: %s (tried %u times)", name, where, failure, TRIES);
}

static int process_revision(const wchar_t *space, const wchar_t *container, const wchar_t *revision) {
    wchar_t from[FH2_PATH_CAPACITY], to[FH2_PATH_CAPACITY], work[FH2_PATH_CAPACITY];
    char name[FH2_PATH_CAPACITY];
    Entry *entry = entry_for(space, container, revision);
    if (!entry || entry->state != REVISION_NEW) return 0;
    relative_name(name, sizeof(name), space, container, revision);
    size_t commit_size = 0;
    if (!revision_path(from, source_root, space, container, revision) ||
        !revision_path(to, mirror_root, space, container, revision) ||
        !fh2_path_join(work, fh2_path_set(work, from), L"commit")) {
        finish(entry, REVISION_FAILED, &failed_count);
        if (error_allowed()) say("ERROR: %s not copied: its path is too long", name);
        return 0;
    }
    if (!fh2_read_small(work, source_commit, sizeof(source_commit), &commit_size)) return 0;  // gone meanwhile
    const char *invalid = fh2_parse_commit(source_commit, commit_size, &commit);
    if (invalid) {
        finish(entry, REVISION_FAILED, &failed_count);
        if (error_allowed()) say("ERROR: %s not copied: its commit is not one Azimuth 1.0.4 writes (%s)", name, invalid);
        return 0;
    }
    if (!fh2_exists(to)) {
        // KeepRevisions=n: a revision older than the n newest already in the mirror would only be removed again.
        if (keep_revisions && newer_in_mirror(space, container, revision) >= keep_revisions) {
            finish(entry, REVISION_DONE, &present_count);
            return 0;
        }
        int result = copy_revision(from, to, L'm', space, container, revision, commit_size);
        if (result != COPY_OK) {
            retry(entry, result, name, "");
            return 0;
        }
        finish(entry, REVISION_DONE, &copied_count);
        say("copied %s (%u blobs, %llu bytes, SHA-256 verified)", name, commit.count, commit.bytes);
        if (keep_revisions) prune(space, container);
        return 1;
    }
    if (mirror_matches(to, commit_size)) {
        finish(entry, REVISION_DONE, &present_count);
        return 0;
    }
    // That revision number is already in the mirror with other contents (or a
    // damaged copy): it is kept as it is, and this one goes beside it, to
    // <mirror>.conflicts\...\revisions\<revision>-<first 12 hex digits of its commit's SHA-256>.
    unsigned char digest[32];
    char hex[65];
    wchar_t tag[64];
    Fh2Sha256 hash;
    if (!fh2_sha256_begin(&hash) || !fh2_sha256_add(&hash, source_commit, commit_size) || !fh2_sha256_end(&hash, digest))
        return 0;
    fh2_hex_digest(digest, hex);
    _snwprintf(tag, 64, L"%ls-%.12hs", revision, hex);
    tag[63] = 0;
    if (!revision_path(to, conflict_root, space, container, tag)) {
        finish(entry, REVISION_FAILED, &failed_count);
        return 0;
    }
    if (mirror_matches(to, commit_size)) {
        finish(entry, REVISION_DONE, &conflict_count);
        return 0;
    }
    int result = copy_revision(from, to, L'c', space, container, tag, commit_size);
    if (result != COPY_OK) {
        retry(entry, result, name, " beside the mirror");
        return 0;
    }
    finish(entry, REVISION_DONE, &conflict_count);
    say_path("WARNING: a revision is already in the mirror with other contents; that copy is kept as it is, and the "
             "game folder's (SHA-256 verified) went to ",
             to, "");
    return 1;
}

// ---------------------------------------------------------------- revisions of a container

typedef struct {
    unsigned long long value;
    wchar_t name[20];
} Revision;

static int newest_first(const void *a, const void *b) {
    unsigned long long x = ((const Revision *)a)->value, y = ((const Revision *)b)->value;
    return x < y ? 1 : x > y ? -1 : 0;
}

// The revision folders (hex names) of root\space\container\revisions, newest
// first; committed: only those with a commit file. Their count.
static unsigned list_revisions(const wchar_t *root, const wchar_t *space, const wchar_t *container, int committed,
                               Revision *list) {
    wchar_t path[FH2_PATH_CAPACITY], probe[FH2_PATH_CAPACITY];
    WIN32_FIND_DATAW data;
    unsigned count = 0;
    size_t length = revision_path(path, root, space, container, NULL);
    if (!length || !fh2_is_plain_folder(path)) return 0;
    size_t pattern = fh2_path_join(path, length, L"*");
    HANDLE find = pattern ? FindFirstFileW(path, &data) : INVALID_HANDLE_VALUE;
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        int valid;
        unsigned long long value = fh2_revision_number(data.cFileName, &valid);
        if (!fh2_plain_folder(&data) || !valid || count >= MAX_REVISIONS) continue;
        if (committed) {
            fh2_wcopy(probe, path, length);  // ...\revisions
            probe[length] = 0;
            size_t child = fh2_path_join(probe, length, data.cFileName);
            child = fh2_path_join(probe, child, L"commit");
            DWORD attributes = child ? GetFileAttributesW(probe) : INVALID_FILE_ATTRIBUTES;
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
                continue;
        }
        list[count].value = value;
        fh2_wcopy(list[count].name, data.cFileName, fh2_wlen(data.cFileName) + 1);
        ++count;
    } while (FindNextFileW(find, &data));
    FindClose(find);
    qsort(list, count, sizeof(Revision), newest_first);
    return count;
}

static Revision source_list[MAX_REVISIONS], mirror_list[MAX_REVISIONS];

// How many committed revisions of the container the mirror holds that are newer than `revision`.
static unsigned newer_in_mirror(const wchar_t *space, const wchar_t *container, const wchar_t *revision) {
    int valid;
    unsigned long long value = fh2_revision_number(revision, &valid);
    unsigned count = list_revisions(mirror_root, space, container, 1, mirror_list), newer = 0;
    for (unsigned i = 0; valid && i < count; ++i)
        if (mirror_list[i].value > value) ++newer;
    return newer;
}

// KeepRevisions=n: the container's mirrored revisions older than its n newest
// verified ones are removed, each only when it holds exactly what its commit
// lists (else it is left and logged).
static void prune(const wchar_t *space, const wchar_t *container) {
    wchar_t path[FH2_PATH_CAPACITY], work[FH2_PATH_CAPACITY];
    char name[FH2_PATH_CAPACITY];
    unsigned count = list_revisions(mirror_root, space, container, 1, mirror_list), kept = 0;
    for (unsigned i = 0; i < count; ++i) {
        size_t length = revision_path(path, mirror_root, space, container, mirror_list[i].name), read = 0;
        if (!length || !fh2_path_join(work, fh2_path_set(work, path), L"commit") ||
            !fh2_read_small(work, other_commit, sizeof(other_commit), &read) ||
            fh2_parse_commit(other_commit, read, &other))
            continue;  // not one this plugin wrote: never removed
        if (kept < keep_revisions) {
            ++kept;
            continue;
        }
        // Exactly the listed blobs in data\, and commit and data in the folder.
        WIN32_FIND_DATAW data;
        int exact = 1;
        unsigned files = 0;
        size_t data_length = fh2_path_join(work, fh2_path_set(work, path), L"data");
        HANDLE find = fh2_path_join(work, data_length, L"*") ? FindFirstFileW(work, &data) : INVALID_HANDLE_VALUE;
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (fh2_dot_entry(&data)) continue;
                const Fh2Blob *blob = fh2_plain_file(&data) ? fh2_commit_blob(&other, data.cFileName) : NULL;
                if (!blob || (((unsigned long long)data.nFileSizeHigh << 32) | data.nFileSizeLow) != blob->size) exact = 0;
                ++files;
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
        find = fh2_path_join(work, fh2_path_set(work, path), L"*") ? FindFirstFileW(work, &data) : INVALID_HANDLE_VALUE;
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (fh2_dot_entry(&data)) continue;
                if (!(fh2_wequal_fold(data.cFileName, L"commit") && fh2_plain_file(&data)) &&
                    !(fh2_wequal_fold(data.cFileName, L"data") && fh2_plain_folder(&data)))
                    exact = 0;
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
        relative_name(name, sizeof(name), space, container, mirror_list[i].name);
        if (!exact || files != other.count) {
            if (error_allowed()) say("KeepRevisions: %s in the mirror holds other files than its commit lists; left as it is", name);
            continue;
        }
        data_length = fh2_path_join(work, fh2_path_set(work, path), L"data");
        for (unsigned j = 0; data_length && j < other.count; ++j)
            if (fh2_path_join(work, data_length, other.blobs[j].name)) DeleteFileW(work);
        work[data_length] = 0;
        RemoveDirectoryW(work);
        if (fh2_path_join(work, fh2_path_set(work, path), L"commit")) DeleteFileW(work);
        if (RemoveDirectoryW(path)) {
            ++pruned_count;
            say("KeepRevisions=%u: removed %s from the mirror (%u newer verified revisions are kept)", keep_revisions,
                name, keep_revisions);
        }
    }
}

static int process_container(const wchar_t *space, const wchar_t *container) {
    int copied = 0;
    unsigned count = list_revisions(source_root, space, container, 1, source_list);
    // Newest first: an older one may be dropped by Azimuth while it is copied.
    for (unsigned i = 0; i < count; ++i) copied += process_revision(space, container, source_list[i].name);
    return copied;
}

int mirror_pass(void) {
    wchar_t spaces[FH2_PATH_CAPACITY], containers[FH2_PATH_CAPACITY];
    WIN32_FIND_DATAW space, container;
    int copied = 0;
    size_t root = fh2_path_set(spaces, source_root);
    HANDLE find = fh2_path_join(spaces, root, L"*") ? FindFirstFileW(spaces, &space) : INVALID_HANDLE_VALUE;
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!fh2_plain_folder(&space) || !fh2_space_name(space.cFileName)) continue;
        size_t length = fh2_path_join(containers, fh2_path_set(containers, source_root), space.cFileName);
        HANDLE inner = length && fh2_path_join(containers, length, L"*") ? FindFirstFileW(containers, &container)
                                                                       : INVALID_HANDLE_VALUE;
        if (inner == INVALID_HANDLE_VALUE) continue;
        do {
            if (fh2_plain_folder(&container)) copied += process_container(space.cFileName, container.cFileName);
        } while (FindNextFileW(inner, &container));
        FindClose(inner);
    } while (FindNextFileW(find, &space));
    FindClose(find);
    return copied;
}

void mirror_summary(void) {
    say("first pass: %llu committed revisions in the game folder's saves; %llu copied now, %llu already in the mirror, "
        "%llu kept beside it (conflicts), %llu not copied",
        seen_count, copied_count, present_count, conflict_count, failed_count);
}

// ---------------------------------------------------------------- start

// Splits a staging name; 0 when it is not one of this plugin's.
static int parse_staging(const wchar_t *name, wchar_t *kind, wchar_t space[64], wchar_t container[128],
                         wchar_t revision[64]) {
    wchar_t *parts[3] = {space, container, revision};
    size_t limits[3] = {64, 128, 64};
    if ((name[0] != L'm' && name[0] != L'c') || name[1] != L'~') return 0;
    *kind = name[0];
    const wchar_t *at = name + 2;
    for (unsigned p = 0; p < 3; ++p) {
        size_t n = 0;
        while (*at && *at != L'~' && n + 1 < limits[p]) parts[p][n++] = *at++;
        parts[p][n] = 0;
        if (!n || *at != L'~') return 0;
        ++at;
    }
    return *at != 0;
}

// Every blob of the staged revision verifies against its commit.
static int staged_complete(const wchar_t *stage) {
    wchar_t work[FH2_PATH_CAPACITY];
    unsigned char digest[32];
    unsigned long long size;
    size_t read = 0, length = fh2_path_set(work, stage);
    if (!fh2_path_join(work, length, L"commit") || !fh2_read_small(work, source_commit, sizeof(source_commit), &read) ||
        fh2_parse_commit(source_commit, read, &commit))
        return 0;
    length = fh2_path_join(work, length, L"data");
    for (unsigned i = 0; i < commit.count; ++i)
        if (!fh2_path_join(work, length, commit.blobs[i].name) || !hash_file(work, digest, &size) ||
            size != commit.blobs[i].size || !fh2_digest_equal(digest, commit.blobs[i].hash))
            return 0;
    return 1;
}

static void recover_staging(void) {
    wchar_t stage[FH2_PATH_CAPACITY], to[FH2_PATH_CAPACITY];
    wchar_t kind, space[64], container[128], revision[64];
    WIN32_FIND_DATAW data;
    size_t root = fh2_path_set(stage, staging_root);
    if (!root || !fh2_is_plain_folder(staging_root)) return;
    HANDLE find = fh2_path_join(stage, root, L"*") ? FindFirstFileW(stage, &data) : INVALID_HANDLE_VALUE;
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (fh2_dot_entry(&data)) continue;
        size_t length = fh2_path_join(stage, root, data.cFileName);
        if (!length) continue;
        if (!fh2_plain_folder(&data) || !parse_staging(data.cFileName, &kind, space, container, revision)) {
            say_path("left as it is in the staging folder (not one of this plugin's copies): ", stage, "");
            continue;
        }
        char name[FH2_PATH_CAPACITY];
        relative_name(name, sizeof(name), space, container, revision);
        int placed = 0;
        if (staged_complete(stage) && revision_path(to, kind == L'm' ? mirror_root : conflict_root, space, container, revision) &&
            !fh2_exists(to)) {
            wchar_t parent[FH2_PATH_CAPACITY];
            size_t up = fh2_path_set(parent, to);
            while (up && parent[up - 1] != L'\\') --up;
            if (up) parent[up - 1] = 0;
            placed = up && fh2_make_folders(parent, mirror_length) && MoveFileExW(stage, to, MOVEFILE_WRITE_THROUGH);
        }
        if (placed) {
            say("a copy cut short last time was complete (every blob verified): %s renamed into place", name);
        } else {
            stage[length] = 0;
            remove_tree(stage, length);
            say("an unfinished copy of %s from last time was removed from the staging folder (it is copied again "
                "if the game folder still has it)",
                name);
        }
        stage[root] = 0;
    } while (FindNextFileW(find, &data));
    FindClose(find);
    RemoveDirectoryW(staging_root);  // only when empty
}

// Documents holding newer revisions than the game folder (saves kept there by
// an earlier build, or restored by hand): said once at the start, never acted on.
static void compare_newest(void) {
    wchar_t spaces[FH2_PATH_CAPACITY], containers[FH2_PATH_CAPACITY];
    WIN32_FIND_DATAW space, container;
    unsigned long long mirror_containers = 0, source_containers = 0, newer = 0;
    for (int pass = 0; pass < 2; ++pass) {
        const wchar_t *root_path = pass ? source_root : mirror_root;
        size_t root = fh2_path_set(spaces, root_path);
        HANDLE find = fh2_path_join(spaces, root, L"*") ? FindFirstFileW(spaces, &space) : INVALID_HANDLE_VALUE;
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            if (!fh2_plain_folder(&space) || !fh2_space_name(space.cFileName)) continue;
            size_t length = fh2_path_join(containers, fh2_path_set(containers, root_path), space.cFileName);
            HANDLE inner = length && fh2_path_join(containers, length, L"*") ? FindFirstFileW(containers, &container)
                                                                           : INVALID_HANDLE_VALUE;
            if (inner == INVALID_HANDLE_VALUE) continue;
            do {
                if (!fh2_plain_folder(&container)) continue;
                if (pass) {
                    ++source_containers;
                    continue;
                }
                unsigned mine = list_revisions(mirror_root, space.cFileName, container.cFileName, 1, mirror_list);
                if (!mine) continue;
                ++mirror_containers;
                unsigned theirs = list_revisions(source_root, space.cFileName, container.cFileName, 1, source_list);
                if (theirs && mirror_list[0].value <= source_list[0].value) continue;
                if (!theirs) continue;  // a container the game deleted (or never had here)
                ++newer;
                if (warning_lines < WARNING_LINES) {
                    ++warning_lines;
                    say("WARNING: the mirror holds a newer revision of %ls\\%ls (%ls) than the game folder (%ls); the "
                        "game uses the game folder's. Both are kept",
                        space.cFileName, container.cFileName, mirror_list[0].name, source_list[0].name);
                }
            } while (FindNextFileW(inner, &container));
            FindClose(inner);
        } while (FindNextFileW(find, &space));
        FindClose(find);
    }
    if (newer)
        say("WARNING: %llu containers are newer in the mirror than in the game folder (saves kept in Documents by an "
            "earlier build?). To play those, close the game and restore them as the plugin's README says",
            newer);
    if (!source_containers && mirror_containers)
        say("the game folder has no saves, the mirror has %llu containers: the game starts without them and nothing "
            "is copied back. To restore them, close the game and see the plugin's README",
            mirror_containers);
}

void mirror_prepare(void) {
    recover_staging();
    compare_newest();
}

int mirror_configure(const wchar_t *source, const wchar_t *mirror, unsigned keep) {
    mirror_length = fh2_path_set(mirror_root, mirror);
    if (!fh2_path_set(source_root, source) || !mirror_length ||
        !fh2_path_append(staging_root, fh2_path_set(staging_root, mirror_root), L".staging") ||
        !fh2_path_append(conflict_root, fh2_path_set(conflict_root, mirror_root), L".conflicts"))
        return 0;
    if (!chunk_buffer) chunk_buffer = (unsigned char *)VirtualAlloc(NULL, CHUNK, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!chunk_buffer) return 0;
    keep_revisions = keep ? (keep < 2 ? 2 : keep) : 0;
    entry_count = 0;
    error_lines = warning_lines = 0;
    seen_count = copied_count = present_count = conflict_count = failed_count = pruned_count = 0;
    return 1;
}


#ifdef DOCUMENTS_SAVES_TEST
void mirror_test_hooks(MirrorFault fault, unsigned chunk, unsigned long long space) {
    fault_hook = fault;
    chunk_size = chunk && chunk <= CHUNK ? chunk : CHUNK;
    fake_free_space = space;
}
#endif
