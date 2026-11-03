// catch_error.dll bridge for the Steam Workshop package (Forager 4.1.9, Windows x86). The bootstrap in
// Main.gml writes it to the Forager profile folder (%LOCALAPPDATA%\Forager), where GameMaker finds it before
// the game's own catch_error.dll. It forwards the extension's 23 exports to the original, loaded by absolute
// path from the game folder, and loads the co-op DLL named in ForagerCoop-current.txt.
// Based on the Workshop integration bridge for v0.1.2 (MIT).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static HMODULE self;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
FARPROC targets[23];
static const char *names[] = {
    "catch_error_clear", "catch_error_dequeue", "catch_error_fatal_force_raw", "catch_error_get_dump_path",
    "catch_error_get_exec_params", "catch_error_get_exec_path", "catch_error_get_fatal", "catch_error_get_newer",
    "catch_error_get_normal", "catch_error_get_prompt_flags", "catch_error_get_prompt_kind",
    "catch_error_get_prompt_text", "catch_error_get_prompt_title", "catch_error_init_raw", "catch_error_is_loaded",
    "catch_error_is_ready", "catch_error_set_dump_path_raw", "catch_error_set_exec_raw", "catch_error_set_fatal",
    "catch_error_set_newer_raw", "catch_error_set_normal", "catch_error_set_prompt_raw", "catch_error_size"};

#define PATH_CHARS 1200
// "ForagerCoop-" + 16 hex digits + ".dll", as written by Main.gml.
#define PAYLOAD_PREFIX "ForagerCoop-"
#define PAYLOAD_CHARS 32

static void append(WCHAR *p, const WCHAR *s) {
    while (*p) ++p;
    while ((*p++ = *s++)) {}
}

// The directory of a module (NULL: Forager.exe) with a trailing backslash.
static BOOL directory(HMODULE m, WCHAR *p) {
    DWORD n = GetModuleFileNameW(m, p, 1024);
    if (!n || n >= 1024) return FALSE;
    while (n && p[n - 1] != L'\\' && p[n - 1] != L'/') --n;
    p[n] = 0;
    return n != 0;
}

static BOOL exists(const WCHAR *path) { return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }

static BOOL CALLBACK resolve_once(PINIT_ONCE o, PVOID p, PVOID *c) {
    WCHAR path[PATH_CHARS] = {0};
    if (!directory(NULL, path)) return FALSE;
    append(path, L"catch_error.dll");
    HMODULE original = LoadLibraryW(path);
    if (!original || original == self) return FALSE;
    for (int i = 0; i < 23; ++i) {
        targets[i] = GetProcAddress(original, names[i]);
        if (!targets[i]) return FALSE;
    }
    return TRUE;
}

void resolve(void) {
    if (!InitOnceExecuteOnce(&once, resolve_once, NULL, NULL)) ExitProcess(127);
}

// The package's marker in the local mods folder or in a Workshop item of this Steam library, so that
// unsubscribing stops the co-op even though this bridge stays in the profile folder.
static BOOL source_present(void) {
    WCHAR path[PATH_CHARS] = {0};
    if (directory(self, path)) {
        append(path, L"mods\\ForagerCoop\\forager-coop.marker");
        if (exists(path)) return TRUE;
    }
    path[0] = 0;
    if (!directory(NULL, path)) return FALSE;
    append(path, L"..\\..\\workshop\\content\\751780\\*");
    WIN32_FIND_DATAW data;
    HANDLE search = FindFirstFileW(path, &data);
    if (search == INVALID_HANDLE_VALUE) return FALSE;
    BOOL found = FALSE;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || data.cFileName[0] == L'.') continue;
        path[0] = 0;
        if (!directory(NULL, path)) break;
        append(path, L"..\\..\\workshop\\content\\751780\\");
        append(path, data.cFileName);
        append(path, L"\\forager-coop.marker");
        if (exists(path)) {
            found = TRUE;
            break;
        }
    } while (FindNextFileW(search, &data));
    FindClose(search);
    return found;
}

// Reads ForagerCoop-current.txt into name; only a name of the expected form is accepted.
static BOOL current_payload(WCHAR *name) {
    WCHAR path[PATH_CHARS] = {0};
    if (!directory(self, path)) return FALSE;
    append(path, L"ForagerCoop-current.txt");
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    char text[64];
    DWORD size = 0;
    BOOL read = ReadFile(file, text, sizeof text, &size, NULL);
    CloseHandle(file);
    if (!read) return FALSE;
    while (size && (text[size - 1] == '\r' || text[size - 1] == '\n' || text[size - 1] == ' ')) --size;
    if (size != PAYLOAD_CHARS) return FALSE;
    for (DWORD i = 0; i < size; ++i) {
        const char ch = text[i];
        if (i < sizeof PAYLOAD_PREFIX - 1) {
            if (ch != PAYLOAD_PREFIX[i]) return FALSE;
        } else if (i < size - 4) {
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return FALSE;
        } else if (ch != ".dll"[i - (size - 4)]) {
            return FALSE;
        }
        name[i] = (WCHAR)ch;
    }
    name[size] = 0;
    return TRUE;
}

static DWORD WINAPI start(LPVOID p) {
    if (!source_present()) return 0;
    WCHAR path[PATH_CHARS] = {0};
    // A manual install (version.dll next to Forager.exe) already runs the co-op; a second copy would hook twice.
    if (!directory(NULL, path)) return 0;
    append(path, L"version.dll");
    if (exists(path)) return 0;
    path[0] = 0;
    if (!directory(self, path)) return 0;
    append(path, L"ForagerCoop-disabled.txt");
    if (exists(path)) return 0;
    WCHAR name[PAYLOAD_CHARS + 1];
    if (!current_payload(name)) return 0;
    path[0] = 0;
    if (!directory(self, path)) return 0;
    append(path, name);
    LoadLibraryW(path);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        self = instance;
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(NULL, 0, start, NULL, 0, NULL);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}

// Each export resolves the original once, restores every register and jumps to the original's export.
#define PROXY(n)                                                                                                    \
    __declspec(naked) void proxy_##n(void) {                                                                     \
        __asm pushfd __asm pushad __asm call resolve __asm popad __asm popfd __asm jmp dword ptr[targets + 4 * n] \
    }
PROXY(0) PROXY(1) PROXY(2) PROXY(3) PROXY(4) PROXY(5) PROXY(6) PROXY(7) PROXY(8) PROXY(9) PROXY(10) PROXY(11)
PROXY(12) PROXY(13) PROXY(14) PROXY(15) PROXY(16) PROXY(17) PROXY(18) PROXY(19) PROXY(20) PROXY(21) PROXY(22)
