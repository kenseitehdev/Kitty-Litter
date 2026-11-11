#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <objbase.h>
#include <wchar.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define ID_TABS 100
#define ID_LIST 101
#define ID_SCAN 102
#define ID_QUARANTINE 103
#define ID_RESTORE 104
#define ID_RELEASE 105
#define ID_STATUS 106
#define ICON_APP 201
#define MAX_CANDIDATES 5000
#define MAX_VISITED 100000
#define CANDIDATE 0
#define QUARANTINE 1

typedef struct { wchar_t *path; wchar_t *category; ULONGLONG bytes; } Candidate;
typedef struct { wchar_t *original; wchar_t *payload; wchar_t *category; ULONGLONG bytes; } Record;

static HINSTANCE instance;
static HWND main_window, tabs, list_view, status_text;
static wchar_t data_dir[MAX_PATH], quarantine_dir[MAX_PATH], index_file[MAX_PATH];
static Candidate candidates[MAX_CANDIDATES];
static size_t candidate_count;
static Record records[MAX_CANDIDATES];
static size_t record_count;
static unsigned visited_count;
static int active_tab;

static wchar_t *wide_duplicate(const wchar_t *text) {
    size_t n = wcslen(text) + 1;
    wchar_t *copy = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (copy) memcpy(copy, text, n * sizeof(wchar_t));
    return copy;
}

static void set_status(const wchar_t *text) { SetWindowTextW(status_text, text); }

static void free_candidates(void) {
    for (size_t i = 0; i < candidate_count; i++) {
        free(candidates[i].path); free(candidates[i].category);
    }
    candidate_count = 0;
}

static void free_records(void) {
    for (size_t i = 0; i < record_count; i++) {
        free(records[i].original); free(records[i].payload); free(records[i].category);
    }
    record_count = 0;
}

static wchar_t *long_path(const wchar_t *path) {
    if (wcsncmp(path, L"\\\\?\\", 4) == 0) return wide_duplicate(path);
    size_t n = wcslen(path);
    if (n >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        wchar_t *result = (wchar_t *)malloc((n + 7) * sizeof(wchar_t));
        if (result) swprintf(result, n + 7, L"\\\\?\\UNC\\%ls", path + 2);
        return result;
    }
    wchar_t *result = (wchar_t *)malloc((n + 5) * sizeof(wchar_t));
    if (result) swprintf(result, n + 5, L"\\\\?\\%ls", path);
    return result;
}

static int utf8_to_wide(const char *src, wchar_t **dst) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, NULL, 0);
    if (!n) return 0;
    *dst = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    return *dst && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, *dst, n) != 0;
}

static char *wide_to_utf8(const wchar_t *src) {
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, -1, NULL, 0, NULL, NULL);
    if (!n) return NULL;
    char *dst = (char *)malloc((size_t)n);
    if (!dst || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, src, -1, dst, n, NULL, NULL)) {
        free(dst); return NULL;
    }
    return dst;
}

static void add_candidate(const wchar_t *path, const wchar_t *category, ULONGLONG bytes) {
    if (candidate_count >= MAX_CANDIDATES) return;
    candidates[candidate_count].path = wide_duplicate(path);
    candidates[candidate_count].category = wide_duplicate(category);
    candidates[candidate_count].bytes = bytes;
    if (candidates[candidate_count].path && candidates[candidate_count].category) candidate_count++;
}

static int installer_extension(const wchar_t *path) {
    const wchar_t *dot = wcsrchr(path, L'.');
    if (!dot) return 0;
    return !_wcsicmp(dot, L".exe") || !_wcsicmp(dot, L".msi") ||
           !_wcsicmp(dot, L".msix") || !_wcsicmp(dot, L".zip") ||
           !_wcsicmp(dot, L".7z") || !_wcsicmp(dot, L".iso");
}

static void scan_directory(const wchar_t *root, int downloads, FILETIME cutoff, unsigned depth) {
    if (candidate_count >= MAX_CANDIDATES || visited_count >= MAX_VISITED || depth > 48) return;
    wchar_t *root_long = long_path(root);
    if (!root_long) return;
    DWORD root_attributes = GetFileAttributesW(root_long);
    if (root_attributes == INVALID_FILE_ATTRIBUTES ||
        !(root_attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (root_attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        free(root_long);
        return;
    }
    size_t n = wcslen(root_long);
    wchar_t *pattern = (wchar_t *)malloc((n + 3) * sizeof(wchar_t));
    if (!pattern) { free(root_long); return; }
    swprintf(pattern, n + 3, L"%ls%ls*", root_long,
             n && root_long[n-1] == L'\\' ? L"" : L"\\");
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(pattern, &data);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) { free(root_long); return; }
    do {
        if (visited_count++ >= MAX_VISITED) break;
        if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..") ||
            (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT))) continue;
        size_t plen = wcslen(root_long), flen = wcslen(data.cFileName);
        wchar_t *child = (wchar_t *)malloc((plen + flen + 2) * sizeof(wchar_t));
        if (!child) continue;
        swprintf(child, plen + flen + 2, L"%ls%ls%ls", root_long,
                 plen && root_long[plen-1] == L'\\' ? L"" : L"\\", data.cFileName);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            scan_directory(child, downloads, cutoff, depth + 1);
        } else {
            ULONGLONG bytes = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
            FILETIME modified = data.ftLastWriteTime;
            if (CompareFileTime(&modified, &cutoff) < 0 &&
                ((!downloads && bytes >= 1024ULL*1024) ||
                 (downloads && bytes >= 50ULL*1024*1024 && installer_extension(child))))
                add_candidate(child, downloads ? L"Old installer/download" : L"Cache or log", bytes);
        }
        free(child);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    free(root_long);
}

static void insert_list_row(int row, const wchar_t *path, const wchar_t *category,
                             ULONGLONG bytes, LPARAM data) {
    wchar_t size_text[48], name[MAX_PATH];
    swprintf(size_text, 48, L"%llu bytes", (unsigned long long)bytes);
    const wchar_t *base = wcsrchr(path, L'\\');
    wcsncpy(name, base ? base + 1 : path, MAX_PATH - 1); name[MAX_PATH - 1] = 0;
    LVITEMW item = {0}; item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = row; item.pszText = name; item.lParam = data;
    ListView_InsertItem(list_view, &item);
    ListView_SetItemText(list_view, row, 1, size_text);
    ListView_SetItemText(list_view, row, 2, (LPWSTR)category);
    ListView_SetItemText(list_view, row, 3, (LPWSTR)path);
    ListView_SetCheckState(list_view, row, FALSE);
}

static LPARAM list_row_data(int row) {
    LVITEMW item = {0}; item.mask = LVIF_PARAM; item.iItem = row;
    return ListView_GetItem(list_view, &item) ? item.lParam : (LPARAM)-1;
}

static void refresh_list(void) {
    ListView_DeleteAllItems(list_view);
    if (active_tab == CANDIDATE) {
        for (size_t i = 0; i < candidate_count; i++)
            insert_list_row((int)i, candidates[i].path, candidates[i].category,
                            candidates[i].bytes, (LPARAM)i);
    } else {
        free_records();
        FILE *file = _wfopen(index_file, L"rb");
        if (file) {
            char line[160000];
            while (record_count < MAX_CANDIDATES && fgets(line, sizeof(line), file)) {
                char *fields[4] = {0}; char *p = line; int count = 0;
                while (count < 4) { fields[count++] = p; char *tab = strchr(p, '\t'); if (!tab) break; *tab = 0; p = tab + 1; }
                if (count < 4) continue;
                char *newline = strpbrk(fields[3], "\r\n"); if (newline) *newline = 0;
                Record *r = &records[record_count];
                if (!utf8_to_wide(fields[0], &r->original) || !utf8_to_wide(fields[1], &r->payload) ||
                    !utf8_to_wide(fields[3], &r->category)) {
                    free(r->original); free(r->payload); free(r->category); memset(r, 0, sizeof(*r)); continue;
                }
                r->bytes = _strtoui64(fields[2], NULL, 10);
                wchar_t *long_payload = long_path(r->payload);
                if (long_payload && GetFileAttributesW(long_payload) != INVALID_FILE_ATTRIBUTES) {
                    free(r->payload); r->payload = long_payload;
                    insert_list_row((int)record_count, r->original, r->category,
                                    r->bytes, (LPARAM)record_count);
                    record_count++;
                } else {
                    free(long_payload);
                    free(r->original); free(r->payload); free(r->category); memset(r, 0, sizeof(*r));
                }
            }
            fclose(file);
        }
    }
    ShowWindow(GetDlgItem(main_window, ID_QUARANTINE), active_tab == CANDIDATE ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(main_window, ID_RESTORE), active_tab == QUARANTINE ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(main_window, ID_RELEASE), active_tab == QUARANTINE ? SW_SHOW : SW_HIDE);
}

static int append_record(const Record *record) {
    char *original = wide_to_utf8(record->original), *payload = wide_to_utf8(record->payload);
    char *category = wide_to_utf8(record->category);
    if (!original || !payload || !category) { free(original); free(payload); free(category); return 0; }
    FILE *file = _wfopen(index_file, L"ab");
    int ok = file && fprintf(file, "%s\t%s\t%llu\t%s\n", original, payload,
        (unsigned long long)record->bytes, category) > 0 && fflush(file) == 0;
    if (file) fclose(file);
    free(original); free(payload); free(category);
    return ok;
}

static int rewrite_index(const wchar_t *remove_payload) {
    wchar_t temp_path[MAX_PATH]; swprintf(temp_path, MAX_PATH, L"%ls.tmp", index_file);
    FILE *out = _wfopen(temp_path, L"wb");
    if (!out) return 0;
    int ok = 1;
    for (size_t i = 0; i < record_count; i++) {
        Record *r = &records[i];
        if (!r->payload || !r->payload[0]) continue;
        if (!_wcsicmp(r->payload, remove_payload)) continue;
        char *original = wide_to_utf8(r->original), *payload = wide_to_utf8(r->payload), *category = wide_to_utf8(r->category);
        if (!original || !payload || !category || fprintf(out, "%s\t%s\t%llu\t%s\n", original, payload,
            (unsigned long long)r->bytes, category) < 0) ok = 0;
        free(original); free(payload); free(category);
    }
    if (fclose(out) != 0) ok = 0;
    if (ok) ok = MoveFileExW(temp_path, index_file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(temp_path);
    return ok;
}

static void do_scan(void) {
    free_candidates(); ListView_DeleteAllItems(list_view);
    wchar_t local[MAX_PATH], profile[MAX_PATH];
    DWORD a = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    DWORD b = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
    if (!a || a >= MAX_PATH || !b || b >= MAX_PATH) { set_status(L"Could not locate this user's data folders."); return; }
    FILETIME now; GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER cutoff; cutoff.LowPart = now.dwLowDateTime; cutoff.HighPart = now.dwHighDateTime;
    cutoff.QuadPart -= (ULONGLONG)30 * 24 * 60 * 60 * 10000000ULL;
    FILETIME cache_cutoff = {cutoff.LowPart, cutoff.HighPart};
    wchar_t downloads[MAX_PATH], temp[MAX_PATH], logs[MAX_PATH];
    swprintf(downloads, MAX_PATH, L"%ls\\Downloads", profile);
    swprintf(temp, MAX_PATH, L"%ls\\Temp", local);
    swprintf(logs, MAX_PATH, L"%ls\\Logs", local);
    scan_directory(temp, 0, cache_cutoff, 0);
    scan_directory(logs, 0, cache_cutoff, 0);
    cutoff.QuadPart = (((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime) -
        (ULONGLONG)60 * 24 * 60 * 60 * 10000000ULL;
    FILETIME download_cutoff = {cutoff.LowPart, cutoff.HighPart};
    scan_directory(downloads, 1, download_cutoff, 0);
    refresh_list();
    wchar_t msg[128]; swprintf(msg, 128, L"Found %zu reviewable item(s). Nothing has been deleted.", candidate_count);
    set_status(msg);
}

static void do_quarantine(void) {
    unsigned moved = 0;
    for (int row = ListView_GetItemCount(list_view) - 1; row >= 0; row--) {
        if (!ListView_GetCheckState(list_view, row)) continue;
        int index = (int)list_row_data(row);
        if (index < 0 || (size_t)index >= candidate_count) continue;
        Candidate *c = &candidates[index];
        wchar_t *source = long_path(c->path);
        if (!source || GetFileAttributesW(source) == INVALID_FILE_ATTRIBUTES) { free(source); continue; }
        GUID guid; if (CoCreateGuid(&guid) != S_OK) { free(source); continue; }
        wchar_t id[40], payload[MAX_PATH]; StringFromGUID2(&guid, id, 40);
        swprintf(payload, MAX_PATH, L"%ls\\%ls", quarantine_dir, id);
        wchar_t *payload_long = long_path(payload);
        WIN32_FILE_ATTRIBUTE_DATA attrs;
        if (!payload_long || !GetFileAttributesExW(source, GetFileExInfoStandard, &attrs) ||
            (attrs.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
            free(source); free(payload_long); continue;
        }
        ULONGLONG current_size = ((ULONGLONG)attrs.nFileSizeHigh << 32) | attrs.nFileSizeLow;
        if (current_size != c->bytes || !MoveFileExW(source, payload_long, MOVEFILE_WRITE_THROUGH)) {
            free(source); free(payload_long); continue;
        }
        Record record = {c->path, payload_long, c->category, c->bytes};
        if (append_record(&record)) moved++;
        else MoveFileExW(payload_long, source, MOVEFILE_WRITE_THROUGH);
        free(source); free(payload_long);
    }
    free_candidates(); refresh_list();
    wchar_t msg[128]; swprintf(msg, 128, L"Moved %u item(s) to quarantine. Nothing was permanently deleted.", moved);
    set_status(msg);
}

static void do_quarantine_action(int release) {
    int selected = 0;
    for (int i = 0; i < ListView_GetItemCount(list_view); i++) if (ListView_GetCheckState(list_view, i)) selected++;
    if (!selected) { set_status(L"Select quarantined items first."); return; }
    if (release) {
        wchar_t prompt[160]; swprintf(prompt, 160, L"Permanently delete %d selected quarantined item(s)? This cannot be undone.", selected);
        if (MessageBoxW(main_window, prompt, L"Confirm permanent deletion", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
    }
    unsigned changed = 0;
    int index_error = 0;
    for (int row = ListView_GetItemCount(list_view) - 1; row >= 0; row--) {
        if (!ListView_GetCheckState(list_view, row)) continue;
        int index = (int)list_row_data(row);
        if (index < 0 || (size_t)index >= record_count) continue;
        Record *r = &records[index];
        wchar_t *payload = long_path(r->payload), *original = long_path(r->original);
        int ok = 0;
        if (release) ok = payload && DeleteFileW(payload);
        else if (original && GetFileAttributesW(original) == INVALID_FILE_ATTRIBUTES) {
            wchar_t *parent = wide_duplicate(original), *slash = parent ? wcsrchr(parent, L'\\') : NULL;
            if (slash) { *slash = 0; SHCreateDirectoryExW(NULL, parent, NULL); }
            ok = MoveFileExW(payload, original, MOVEFILE_WRITE_THROUGH) != 0;
            free(parent);
        }
        if (ok) {
            if (rewrite_index(r->payload)) {
                r->payload[0] = 0;
                changed++;
            } else if (!release) {
                if (!MoveFileExW(original, payload, MOVEFILE_WRITE_THROUGH)) {
                    changed++;
                    index_error = 1;
                }
            } else {
                changed++;
                index_error = 1;
            }
        }
        free(payload); free(original);
    }
    refresh_list();
    wchar_t message[128];
    swprintf(message, 128, index_error ? L"Completed %u action(s), but the quarantine index needs repair." :
             release ? L"Permanently deleted %u quarantined item(s)." :
             L"Restored %u item(s); existing original paths were skipped.", changed);
    set_status(message);
}

static void layout_controls(HWND hwnd) {
    RECT r; GetClientRect(hwnd, &r);
    int width = r.right - r.left, height = r.bottom - r.top;
    MoveWindow(tabs, 10, 10, width - 20, 32, TRUE);
    MoveWindow(list_view, 10, 48, width - 20, height - 112, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_SCAN), 10, height - 54, 90, 28, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_QUARANTINE), 108, height - 54, 200, 28, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_RESTORE), 108, height - 54, 140, 28, TRUE);
    MoveWindow(GetDlgItem(hwnd, ID_RELEASE), 258, height - 54, 180, 28, TRUE);
    MoveWindow(status_text, 10, height - 22, width - 20, 18, TRUE);
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        tabs = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                               0, 0, 0, 0, hwnd, (HMENU)ID_TABS, instance, NULL);
        TCITEMW ti = {0}; ti.mask = TCIF_TEXT; ti.pszText = L"Candidates"; TabCtrl_InsertItem(tabs, 0, &ti);
        ti.pszText = L"Quarantine"; TabCtrl_InsertItem(tabs, 1, &ti);
        list_view = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
            0, 0, 0, 0, hwnd, (HMENU)ID_LIST, instance, NULL);
        ListView_SetExtendedListViewStyle(list_view, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t *titles[] = {L"Name", L"Size", L"Category", L"Path"};
        int widths[] = {180, 110, 180, 520};
        for (int i=0;i<4;i++){LVCOLUMNW col={0};col.mask=LVCF_TEXT|LVCF_WIDTH;col.pszText=(LPWSTR)titles[i];col.cx=widths[i];ListView_InsertColumn(list_view,i,&col);}
        CreateWindowExW(0, L"BUTTON", L"Scan Now", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 0,0,0,0,hwnd,(HMENU)ID_SCAN,instance,NULL);
        CreateWindowExW(0, L"BUTTON", L"Move Selected to Quarantine", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON, 0,0,0,0,hwnd,(HMENU)ID_QUARANTINE,instance,NULL);
        CreateWindowExW(0, L"BUTTON", L"Restore Selected", WS_CHILD|BS_PUSHBUTTON, 0,0,0,0,hwnd,(HMENU)ID_RESTORE,instance,NULL);
        CreateWindowExW(0, L"BUTTON", L"Release Permanently…", WS_CHILD|BS_PUSHBUTTON, 0,0,0,0,hwnd,(HMENU)ID_RELEASE,instance,NULL);
        status_text = CreateWindowExW(0, L"STATIC", L"Review before cleanup. Nothing is deleted until separately confirmed.", WS_CHILD|WS_VISIBLE, 0,0,0,0,hwnd,(HMENU)ID_STATUS,instance,NULL);
        layout_controls(hwnd); refresh_list(); return 0;
    }
    case WM_SIZE: layout_controls(hwnd); return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case ID_SCAN: do_scan(); return 0;
        case ID_QUARANTINE: do_quarantine(); return 0;
        case ID_RESTORE: do_quarantine_action(0); return 0;
        case ID_RELEASE: do_quarantine_action(1); return 0;
        }
        break;
    case WM_NOTIFY:
        if (((LPNMHDR)lparam)->idFrom == ID_TABS && ((LPNMHDR)lparam)->code == TCN_SELCHANGE) {
            active_tab = TabCtrl_GetCurSel(tabs); refresh_list(); return 0;
        }
        break;
    case WM_DESTROY:
        free_candidates(); free_records(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE previous, PWSTR command_line, int show) {
    (void)previous; (void)command_line; (void)show; instance = hinst;
    PWSTR local = NULL;
    if (SHGetKnownFolderPath(&FOLDERID_LocalAppData, KF_FLAG_CREATE, NULL, &local) != S_OK) return 1;
    swprintf(data_dir, MAX_PATH, L"%ls\\Neo-Bell Laboratories\\Kitty Litter", local);
    CoTaskMemFree(local);
    swprintf(quarantine_dir, MAX_PATH, L"%ls\\Quarantine", data_dir);
    swprintf(index_file, MAX_PATH, L"%ls\\index.tsv", data_dir);
    int directory_result = SHCreateDirectoryExW(NULL, quarantine_dir, NULL);
    if (directory_result != ERROR_SUCCESS && directory_result != ERROR_ALREADY_EXISTS) return 1;
    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSW wc = {0}; wc.lpfnWndProc = window_proc; wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(ICON_APP)); wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.lpszClassName = L"KittyLitterWindow";
    if (!RegisterClassW(&wc)) return 1;
    main_window = CreateWindowExW(0, wc.lpszClassName, L"Kitty Litter · Review before cleanup",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 620, NULL, NULL, instance, NULL);
    if (!main_window) return 1;
    ShowWindow(main_window, SW_SHOW); UpdateWindow(main_window);
    MSG msg; while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return (int)msg.wParam;
}
