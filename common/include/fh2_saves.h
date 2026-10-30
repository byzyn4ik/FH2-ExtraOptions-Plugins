// FH2 Extra Options plugins: reading Azimuth's saves (Photo Export, Saves in
// Documents). SPDX-License-Identifier: GPL-3.0-or-later
//
// Header only, C, no C runtime calls; links bcrypt (SHA-256), shell32 and
// ole32 (the Documents folder).
//
// Azimuth 1.0.1 to 1.0.4 keep Forza Horizon 2's saves in
//   <folder of the Azimuth executable>\data\titles\1A96545B\saves\
//     player-1\<container>\revisions\<revision: 16 hex digits>\
//       data\<blob> ...
//       commit
// (and a "machine" space beside player-1). Every save of a container is a new
// revision folder: Azimuth 1.0.4 creates it (CreateDirectoryW, unique), writes
// and hashes the blobs into data\, writes "pending" and renames it to "commit"
// (MoveFileExW at azimuth.exe+0x4e3f9f), then drops revisions older than the
// newest two. So a revision with a commit is complete and is never written
// again. The commit (format v2):
//   Azimuth complete save revision v2\n
//   blobs <n>\n
//   <sha256, 64 lower-case hex digits> <size> <blob name>\n   (n lines)
#ifndef FH2_SAVES_H
#define FH2_SAVES_H
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <stddef.h>
#include <stdint.h>

#define FH2_TITLE_ID L"1A96545B"
#define FH2_PATH_CAPACITY 1024u
#define FH2_COMMIT_CAPACITY 65536u  // Azimuth's own limit for a marker
#define FH2_MAX_BLOBS 128u
#define FH2_BLOB_NAME 256u

#ifndef BCRYPT_SHA256_ALG_HANDLE
#define BCRYPT_SHA256_ALG_HANDLE ((BCRYPT_ALG_HANDLE)0x00000041)
#endif

// ---------------------------------------------------------------- text

static inline size_t fh2_wlen(const wchar_t *text) {
    size_t length = 0;
    while (text && text[length]) ++length;
    return length;
}

static inline void fh2_wcopy(wchar_t *to, const wchar_t *from, size_t count) {
    for (size_t i = 0; i < count; ++i) to[i] = from[i];
}

static inline wchar_t fh2_wfold(wchar_t c) {
    return c >= L'a' && c <= L'z' ? (wchar_t)(c - 32) : c == L'/' ? L'\\' : c;
}

static inline int fh2_wequal_fold(const wchar_t *a, const wchar_t *b) {
    for (; *a && *b; ++a, ++b)
        if (fh2_wfold(*a) != fh2_wfold(*b)) return 0;
    return *a == *b;
}

// text starts with prefix (case-insensitive) at a path component boundary.
static inline int fh2_path_within(const wchar_t *text, const wchar_t *prefix) {
    size_t length = fh2_wlen(prefix);
    while (length > 3 && (prefix[length - 1] == L'\\' || prefix[length - 1] == L'/')) --length;
    for (size_t i = 0; i < length; ++i)
        if (fh2_wfold(text[i]) != fh2_wfold(prefix[i])) return 0;
    return !text[length] || text[length] == L'\\' || text[length] == L'/';
}

// path = text; its length, 0 when it does not fit.
static inline size_t fh2_path_set(wchar_t *path, const wchar_t *text) {
    size_t length = fh2_wlen(text);
    if (!length || length >= FH2_PATH_CAPACITY) return 0;
    fh2_wcopy(path, text, length + 1);
    return length;
}

// path[0..length) + "\" + name; the new length, 0 when length is 0 or it does not fit.
static inline size_t fh2_path_join(wchar_t *path, size_t length, const wchar_t *name) {
    size_t extra = fh2_wlen(name);
    if (!length || length + 1 + extra + 1 > FH2_PATH_CAPACITY) return 0;
    if (path[length - 1] != L'\\') path[length++] = L'\\';
    fh2_wcopy(path + length, name, extra + 1);
    return length + extra;
}

static inline size_t fh2_path_append(wchar_t *path, size_t length, const wchar_t *text) {
    size_t extra = fh2_wlen(text);
    if (!length || length + extra + 1 > FH2_PATH_CAPACITY) return 0;
    fh2_wcopy(path + length, text, extra + 1);
    return length + extra;
}

// ---------------------------------------------------------------- folders

static inline int fh2_dot_entry(const WIN32_FIND_DATAW *data) {
    return data->cFileName[0] == L'.' &&
           (!data->cFileName[1] || (data->cFileName[1] == L'.' && !data->cFileName[2]));
}

// A real folder (not a link, junction or other reparse point; not . or ..).
static inline int fh2_plain_folder(const WIN32_FIND_DATAW *data) {
    return (data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
           !(data->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && !fh2_dot_entry(data);
}

static inline int fh2_plain_file(const WIN32_FIND_DATAW *data) {
    return !(data->dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

// 1: path is a folder that is not a reparse point.
static inline int fh2_is_plain_folder(const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
           !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

static inline int fh2_exists(const wchar_t *path) {
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

// Creates every missing folder of an absolute path (X:\... or \\server\share\...).
// Folders it creates or finds must be real folders; a link anywhere at or below
// `trusted_length` characters (the part the caller owns) is refused. 1 = ok.
static inline int fh2_make_folders(const wchar_t *path, size_t trusted_length) {
    wchar_t work[FH2_PATH_CAPACITY];
    size_t length = fh2_path_set(work, path), start = 3;
    if (!length) return 0;
    if (work[0] == L'\\' && work[1] == L'\\') {  // \\server\share\: start after the share
        unsigned slashes = 0;
        for (start = 2; start < length && slashes < 2; ++start)
            if (work[start] == L'\\') ++slashes;
    }
    for (size_t i = start; i <= length; ++i) {
        if (i < length && work[i] != L'\\') continue;
        wchar_t saved = work[i];
        work[i] = 0;
        if (!CreateDirectoryW(work, NULL)) {
            DWORD attributes = GetFileAttributesW(work);
            if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
            if (i >= trusted_length && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return 0;
        }
        work[i] = saved;
    }
    return 1;
}

// ---------------------------------------------------------------- files

// Reads a whole small file (opened so that Azimuth can still write, rename or
// delete it). 1 = read, *size set; 0 = missing, unreadable or larger than capacity.
static inline int fh2_read_small(const wchar_t *path, char *buffer, size_t capacity, size_t *size) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER length;
    DWORD read = 0;
    int ok = GetFileSizeEx(file, &length) && length.QuadPart >= 0 && (unsigned long long)length.QuadPart < capacity &&
             ReadFile(file, buffer, (DWORD)length.QuadPart, &read, NULL) && read == (DWORD)length.QuadPart;
    CloseHandle(file);
    if (ok) {
        buffer[read] = 0;
        *size = read;
    }
    return ok;
}

// ---------------------------------------------------------------- SHA-256 (bcrypt)

typedef struct {
    BCRYPT_HASH_HANDLE handle;
} Fh2Sha256;

static inline int fh2_sha256_begin(Fh2Sha256 *hash) {
    hash->handle = NULL;
    return BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash->handle, NULL, 0, NULL, 0, 0) == 0;
}

static inline int fh2_sha256_add(Fh2Sha256 *hash, const void *data, size_t size) {
    while (size) {
        ULONG part = size > 0x40000000u ? 0x40000000u : (ULONG)size;
        if (BCryptHashData(hash->handle, (PUCHAR)data, part, 0) != 0) return 0;
        data = (const unsigned char *)data + part;
        size -= part;
    }
    return 1;
}

static inline int fh2_sha256_end(Fh2Sha256 *hash, unsigned char digest[32]) {
    int ok = hash->handle && BCryptFinishHash(hash->handle, digest, 32, 0) == 0;
    if (hash->handle) BCryptDestroyHash(hash->handle);
    hash->handle = NULL;
    return ok;
}

static inline int fh2_digest_equal(const unsigned char *a, const unsigned char *b) {
    unsigned char difference = 0;
    for (unsigned i = 0; i < 32; ++i) difference |= (unsigned char)(a[i] ^ b[i]);
    return !difference;
}

static inline void fh2_hex_digest(const unsigned char digest[32], char text[65]) {
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; ++i) {
        text[2 * i] = digits[digest[i] >> 4];
        text[2 * i + 1] = digits[digest[i] & 15];
    }
    text[64] = 0;
}

// ---------------------------------------------------------------- the layout

static inline int fh2_hex_value(unsigned c) {
    return c >= '0' && c <= '9' ? (int)(c - '0') : c >= 'a' && c <= 'f' ? (int)(c - 'a' + 10)
                                               : c >= 'A' && c <= 'F' ? (int)(c - 'A' + 10) : -1;
}

// A revision folder's name: 1 to 16 hex digits. Its value, or 0 when it is not one
// (*valid tells 0 from "0").
static inline unsigned long long fh2_revision_number(const wchar_t *name, int *valid) {
    unsigned long long value = 0;
    size_t digits = 0;
    *valid = 0;
    for (; name[digits]; ++digits) {
        int nibble = name[digits] < 128 ? fh2_hex_value((unsigned)name[digits]) : -1;
        if (nibble < 0 || digits >= 16) return 0;
        value = value << 4 | (unsigned)nibble;
    }
    *valid = digits > 0;
    return value;
}

// A space folder of the saves: player-<...> or machine.
static inline int fh2_space_name(const wchar_t *name) {
    static const wchar_t player[] = L"player-";
    for (unsigned i = 0; i < 7; ++i)
        if (fh2_wfold(name[i]) != fh2_wfold(player[i])) return fh2_wequal_fold(name, L"machine");
    return name[7] != 0;
}

typedef struct {
    unsigned char hash[32];
    unsigned long long size;
    wchar_t name[FH2_BLOB_NAME];
} Fh2Blob;

typedef struct {
    unsigned count;
    unsigned long long bytes;
    Fh2Blob blobs[FH2_MAX_BLOBS];
} Fh2Commit;

// A blob name Azimuth accepts: printable ASCII without separators, not . or ..
static inline int fh2_blob_name(const char *text, size_t length, wchar_t output[FH2_BLOB_NAME]) {
    if (!length || length >= FH2_BLOB_NAME || (length == 1 && text[0] == '.') ||
        (length == 2 && text[0] == '.' && text[1] == '.'))
        return 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c <= ' ' || c >= 127 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|')
            return 0;
        output[i] = (wchar_t)c;
    }
    output[length] = 0;
    return 1;
}

// Parses a v2 commit exactly as Azimuth writes it (each line ends in \n; a \r
// before it is accepted). NULL when it is one, else what is wrong.
static inline const char *fh2_parse_commit(const char *text, size_t size, Fh2Commit *commit) {
    static const char header[] = "Azimuth complete save revision v";
    size_t at = 0;
    commit->count = 0;
    commit->bytes = 0;
    for (unsigned i = 0; i < sizeof(header) - 1; ++i)
        if (at >= size || text[at++] != header[i]) return "it is not an Azimuth commit marker";
    if (at + 1 >= size || text[at] != '2' || (text[at + 1] != '\n' && text[at + 1] != '\r'))
        return "its marker is of a format other than v2";
    ++at;
    // Line by line.
    unsigned expected = 0;
    int have_count = 0;
    while (at < size) {
        if (text[at] == '\r') ++at;
        if (at >= size || text[at] != '\n') return "a line does not end in a line break";
        ++at;
        if (at >= size) break;
        size_t end = at;
        while (end < size && text[end] != '\n' && text[end] != '\r') ++end;
        const char *line = text + at;
        size_t length = end - at;
        if (!have_count) {
            if (length < 7 || line[0] != 'b' || line[1] != 'l' || line[2] != 'o' || line[3] != 'b' || line[4] != 's' ||
                line[5] != ' ')
                return "it has no blob count";
            for (size_t i = 6; i < length; ++i) {
                if (line[i] < '0' || line[i] > '9' || expected > FH2_MAX_BLOBS) return "its blob count is invalid";
                expected = expected * 10 + (unsigned)(line[i] - '0');
            }
            if (!expected || expected > FH2_MAX_BLOBS) return "its blob count is out of range";
            have_count = 1;
        } else {
            if (commit->count >= expected) return "it has text after its blobs";
            Fh2Blob *blob = &commit->blobs[commit->count];
            if (length < 64 + 1 + 1 + 1 + 1 || line[64] != ' ') return "a blob line is invalid";
            for (unsigned i = 0; i < 32; ++i) {
                int high = fh2_hex_value((unsigned char)line[2 * i]), low = fh2_hex_value((unsigned char)line[2 * i + 1]);
                if (high < 0 || low < 0) return "a blob hash is invalid";
                blob->hash[i] = (unsigned char)(high << 4 | low);
            }
            size_t i = 65;
            unsigned long long value = 0;
            if (line[i] < '0' || line[i] > '9') return "a blob size is invalid";
            for (; i < length && line[i] >= '0' && line[i] <= '9'; ++i) {
                if (value > (1ull << 40)) return "a blob size is out of range";
                value = value * 10 + (unsigned)(line[i] - '0');
            }
            if (i >= length || line[i] != ' ') return "a blob line is invalid";
            ++i;
            if (!fh2_blob_name(line + i, length - i, blob->name)) return "a blob name is invalid";
            for (unsigned j = 0; j < commit->count; ++j)
                if (fh2_wequal_fold(commit->blobs[j].name, blob->name)) return "it lists a blob twice";
            blob->size = value;
            commit->bytes += value;
            ++commit->count;
        }
        at = end;
    }
    if (!have_count) return "it has no blob count";
    if (commit->count != expected) return "it lists fewer blobs than its count";
    return NULL;
}

// The blob of that name, or NULL.
static inline const Fh2Blob *fh2_commit_blob(const Fh2Commit *commit, const wchar_t *name) {
    for (unsigned i = 0; i < commit->count; ++i)
        if (fh2_wequal_fold(commit->blobs[i].name, name)) return &commit->blobs[i];
    return NULL;
}

// ---------------------------------------------------------------- the host

// Azimuth builds whose save layout is the one above (Extra Options 0.4.4's
// adapters/azimuth_host.c: TimeDateStamp and SizeOfImage of the executable).
// "1.0.4", ... or NULL for any other executable.
static inline const char *fh2_azimuth_build(const void *host) {
    static const struct {
        uint32_t stamp, size;
        const char *name;
    } builds[] = {{0x6adff280u, 0x9a6000u, "1.0.1"},
                  {0x6ae05dd1u, 0x9af000u, "1.0.2"},
                  {0x6ae0f17au, 0x9d8000u, "1.0.3"},
                  {0x6ae393ebu, 0x8a2000u, "1.0.4"}};
    const unsigned char *base = (const unsigned char *)host;
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    SIZE_T done = 0;
    if (!base || !ReadProcessMemory(GetCurrentProcess(), base, &dos, sizeof(dos), &done) || done != sizeof(dos) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x1000 ||
        !ReadProcessMemory(GetCurrentProcess(), base + dos.e_lfanew, &nt, sizeof(nt), &done) || done != sizeof(nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return NULL;
    for (unsigned i = 0; i < sizeof(builds) / sizeof(builds[0]); ++i)
        if (nt.FileHeader.TimeDateStamp == builds[i].stamp && nt.OptionalHeader.SizeOfImage == builds[i].size)
            return builds[i].name;
    return NULL;
}

// <folder of the process executable>\data\titles\1A96545B\saves; its length, 0 on failure.
static inline size_t fh2_azimuth_saves(wchar_t *output) {
    DWORD count = GetModuleFileNameW(NULL, output, FH2_PATH_CAPACITY);
    if (!count || count >= FH2_PATH_CAPACITY) return 0;
    while (count && output[count - 1] != L'\\' && output[count - 1] != L'/') --count;
    if (!count) return 0;
    output[count] = 0;
    return fh2_path_append(output, count, L"data\\titles\\" FH2_TITLE_ID L"\\saves");
}

// The user's Documents folder (the known folder, wherever it was moved); its
// length, 0 when Windows does not give one.
static inline size_t fh2_documents(wchar_t *output) {
    static const GUID documents = {0xfdd39ad0, 0x238f, 0x46af, {0xad, 0xb4, 0x6c, 0x85, 0x48, 0x03, 0x69, 0xc7}};
    PWSTR found = NULL;
    size_t length = 0;
    if (SHGetKnownFolderPath(&documents, 0, NULL, &found) == S_OK && found) length = fh2_path_set(output, found);
    if (found) CoTaskMemFree(found);
    while (length > 3 && output[length - 1] == L'\\') output[--length] = 0;
    return length;
}

// A folder setting: environment variables expanded, an absolute path
// (X:\... or \\server\share\...), no trailing backslash. Its length, 0 when not usable.
static inline size_t fh2_folder_setting(const wchar_t *text, wchar_t *output) {
    DWORD count = ExpandEnvironmentStringsW(text, output, FH2_PATH_CAPACITY);
    if (!count || count > FH2_PATH_CAPACITY) return 0;
    size_t length = fh2_wlen(output);
    while (length && (output[length - 1] == L' ' || output[length - 1] == L'\t')) output[--length] = 0;
    size_t start = 0;
    while (output[start] == L' ' || output[start] == L'\t') ++start;
    if (start) {
        fh2_wcopy(output, output + start, length - start + 1);
        length -= start;
    }
    for (size_t i = 0; i < length; ++i)
        if (output[i] == L'/') output[i] = L'\\';
    while (length > 3 && output[length - 1] == L'\\') output[--length] = 0;
    int drive = length >= 3 && ((output[0] | 0x20) >= L'a' && (output[0] | 0x20) <= L'z') && output[1] == L':' &&
                output[2] == L'\\';
    int share = length >= 5 && output[0] == L'\\' && output[1] == L'\\' && output[2] != L'?' && output[2] != L'.';
    for (size_t i = 0; i + 1 < length; ++i)  // no . or .. components
        if (output[i] == L'\\' && output[i + 1] == L'.' &&
            (!output[i + 2] || output[i + 2] == L'\\' || (output[i + 2] == L'.' && (!output[i + 3] || output[i + 3] == L'\\'))))
            return 0;
    return drive || share ? length : 0;
}

// UTF-8 of a path into a log buffer; its length.
static inline size_t fh2_utf8(const wchar_t *text, char *output, size_t capacity) {
    if (!capacity) return 0;
    int written = text && *text ? WideCharToMultiByte(CP_UTF8, 0, text, (int)fh2_wlen(text), output, (int)capacity - 1, NULL, NULL)
                                : 0;
    if (written < 0) written = 0;
    output[written] = 0;
    return (size_t)written;
}

#endif
