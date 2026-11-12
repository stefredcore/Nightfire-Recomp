/* Nightfire launcher: picks the mode, starts the game, edits nightfire.ini and
 * mkb.ini and updates the checkout.
 *
 * It holds no game knowledge beyond the names of mkb.ini's controls. The game
 * is build\Release\nightfire.exe, started with the mode on its command line;
 * it sets everything else itself (src/main.c). An update runs the build:
 * the submodule, scripts\recompile.ps1 (cheap when nothing it
 * reads changed) and scripts\build.ps1. Everything is found from the folder
 * above the exe that holds scripts\build.ps1.
 *
 * nightfire.ini and mkb.ini are tracked but hold the player's settings, so an
 * update takes them out of git's way, pulls, and puts them back (update_job).
 * The exe cannot be overwritten while it runs, and Windows does allow renaming
 * it: the build then writes a new one next to it.
 *
 * A folder installed by Nightfire-Setup (installer\) starts as no git
 * checkout: its version, repository and branch are in install-info.txt, a
 * check asks GitHub with git ls-remote, and the first update makes the folder
 * a checkout (make_checkout); the default key layout is mkb.default.ini until
 * then. Updates need git on the PC. */
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define WM_JOB_LINE (WM_APP + 1)        /* lParam: wchar_t *, freed by the window */
#define WM_JOB_STEP (WM_APP + 2)        /* wParam: 0..100 */
#define WM_JOB_DONE (WM_APP + 3)        /* wParam: job, lParam: result */
#define WM_KEYS_WAIT (WM_APP + 4)

enum { ID_VANILLA = 100, ID_ENHANCED, ID_PLAY, ID_SETTINGS, ID_UPDATE, ID_TIMER,
       ID_S_RES, ID_S_ANISO, ID_S_WIDE, ID_S_FLICKER, ID_S_VSYNC, ID_S_FULL, ID_S_DRVAA, ID_S_HZ60, ID_S_KEYS, ID_S_SENS, ID_S_SENSV,
       ID_K_FOOT, ID_K_DRIVE, ID_K_LIST, ID_K_BIND, ID_K_CLEAR, ID_K_DEFAULT };
enum { JOB_VERSION, JOB_CHECK, JOB_UPDATE };

#define HEADER_H 110
#define WIN_W    420
#define KEYS_W   540
#define KEYS_LIST_H 392

static HINSTANCE s_inst;
static HWND s_win, s_vanilla, s_enhanced, s_play, s_settings, s_update, s_text, s_bar, s_status;
static HWND s_dlg, s_kdlg;
static HFONT s_font, s_big, s_small;
static wchar_t s_root[MAX_PATH], s_ini[MAX_PATH], s_mkb[MAX_PATH], s_log[MAX_PATH];
static wchar_t s_branch[64] = L"main", s_version[64] = L"unknown";
static int s_dpi = 96;
static int s_git;                       /* a git checkout; else installed by Nightfire-Setup */
static volatile LONG s_busy;
static int s_running, s_missing;
static long s_pending;                  /* commits to fetch, from the last check */
static HANDLE s_logfile = INVALID_HANDLE_VALUE;
static DWORD s_launching;               /* tick of the start, until the game is up */
#define LAUNCH_WAIT_MS 15000

static int px(int v)
{
    return MulDiv(v, s_dpi, 96);
}

static int exists(const wchar_t *path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static void join(wchar_t *out, const wchar_t *file)
{
    swprintf(out, MAX_PATH, L"%s\\%s", s_root, file);
}

static int find_root(void)
{
    wchar_t p[MAX_PATH], t[MAX_PATH];
    int i;

    GetModuleFileNameW(NULL, p, MAX_PATH);
    for (i = 0; i < 5; i++) {
        wchar_t *s = wcsrchr(p, L'\\');
        if (!s)
            return 0;
        *s = 0;
        swprintf(t, MAX_PATH, L"%s\\scripts\\build.ps1", p);
        if (exists(t)) {
            wcscpy(s_root, p);
            join(s_ini, L"nightfire.ini");
            join(s_mkb, L"mkb.ini");
            join(s_log, L"logs\\update.log");
            join(t, L".git");
            s_git = exists(t);
            return 1;
        }
    }
    return 0;
}

static int enhanced(void)
{
    return SendMessageW(s_enhanced, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static int game_running(void)
{
    PROCESSENTRY32W e;
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    int found = 0;

    if (h == INVALID_HANDLE_VALUE)
        return 0;
    e.dwSize = sizeof e;
    if (Process32FirstW(h, &e))
        do {
            if (!_wcsicmp(e.szExeFile, L"nightfire.exe") || !_wcsicmp(e.szExeFile, L"nightfire_driving.exe"))
                found = 1;
        } while (!found && Process32NextW(h, &e));
    CloseHandle(h);
    return found;
}

/* What can be pressed follows from what is going on. */
static void update_ui(void)
{
    wchar_t t[160], exe[MAX_PATH], xbe[MAX_PATH];
    int busy = s_busy != 0 || s_launching;

    join(exe, L"build\\Release\\nightfire.exe");
    join(xbe, L"game\\default.xbe");
    s_missing = !exists(exe) ? 1 : !exists(xbe) ? 2 : 0;
    EnableWindow(s_play, !busy && !s_running && !s_missing);
    EnableWindow(s_settings, !busy && enhanced() && !s_dlg);
    EnableWindow(s_update, !busy && !s_running);
    EnableWindow(s_vanilla, !busy && !s_running);
    EnableWindow(s_enhanced, !busy && !s_running);
    SendMessageW(s_play, BCM_SETNOTE, 0, (LPARAM)(enhanced() ? L"Start the Enhanced Edition" : L"Start the Vanilla Edition"));
    SendMessageW(s_settings, BCM_SETNOTE, 0,
                 (LPARAM)(enhanced() ? L"Change resolution and controls" : L"Enhanced only"));
    if (s_pending > 0) {
        SetWindowTextW(s_update, L"Install the update");
        if (s_git)
            swprintf(t, 160, L"%ld new change%s, then a rebuild", s_pending, s_pending == 1 ? L"" : L"s");
        else
            wcscpy(t, L"A newer version, then a rebuild");
    } else {
        SetWindowTextW(s_update, L"Check for updates");
        swprintf(t, 160, L"Installed version: %s", s_version);
    }
    SendMessageW(s_update, BCM_SETNOTE, 0, (LPARAM)t);
    SendMessageW(s_status, SB_SETTEXTW, 0, (LPARAM)(s_launching ? L"Starting ..." : busy ? L"Working ..." : s_running ? L"Game running"
                 : s_missing == 1 ? L"Not built yet" : s_missing == 2 ? L"game\\default.xbe missing" : L"Ready"));
    SendMessageW(s_status, SB_SETTEXTW, 1, (LPARAM)(enhanced() ? L"Enhanced" : L"Vanilla"));
}

static void say(const wchar_t *text)
{
    SetWindowTextW(s_text, text);
}

/* ---- Running tools --------------------------------------------------- */

static void job_line(const char *line)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, line, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc(n * sizeof *w);

    if (!w)
        return;
    MultiByteToWideChar(CP_UTF8, 0, line, -1, w, n);
    PostMessageW(s_win, WM_JOB_LINE, 0, (LPARAM)w);
}

/* Runs cmd in the project folder with its output in the log. Each line goes
 * to the window when show is set; the first line of output to first, if
 * asked. Returns the exit code, or -1 if it could not start. */
static int run(const wchar_t *cmd, int show, char *first, size_t cap)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    HANDLE rd, wr;
    wchar_t buf[1024];
    char chunk[4096], line[512];
    size_t ll = 0;
    DWORD got, code = (DWORD)-1;
    int have_first = 0;

    if (!CreatePipe(&rd, &wr, &sa, 0))
        return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = wr;
    wcsncpy(buf, cmd, 1023);
    buf[1023] = 0;
    if (first)
        first[0] = 0;
    if (!CreateProcessW(NULL, buf, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, s_root, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        return -1;
    }
    CloseHandle(wr);
    while (ReadFile(rd, chunk, sizeof chunk, &got, NULL) && got) {
        DWORD i, w;
        if (s_logfile != INVALID_HANDLE_VALUE)
            WriteFile(s_logfile, chunk, got, &w, NULL);
        for (i = 0; i < got; i++) {
            char c = chunk[i];
            if (c == '\r' || c == '\n') {
                if (ll) {
                    line[ll] = 0;
                    if (first && !have_first) {
                        strncpy(first, line, cap - 1);
                        first[cap - 1] = 0;
                        have_first = 1;
                    }
                    if (show)
                        job_line(line);
                    ll = 0;
                }
            } else if (ll + 1 < sizeof line) {
                line[ll++] = c;
            }
        }
    }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

static void step(int pct, const char *text)
{
    PostMessageW(s_win, WM_JOB_STEP, (WPARAM)pct, 0);
    job_line(text);
}

/* ---- Jobs ------------------------------------------------------------- */

/* install-info.txt, written by the installer: the commit it was made from
 * (project), the repository and branch its updates come from (repo, branch). */
static void info_get(const wchar_t *key, wchar_t *out, DWORD cap)
{
    wchar_t info[MAX_PATH];
    join(info, L"install-info.txt");
    GetPrivateProfileStringW(L"install", key, L"", out, cap, info);
}

static void read_version(void)
{
    char a[64], b[64];

    /* An install: the commit the installer was made from. */
    if (!s_git) {
        info_get(L"project", s_version, 64);
        if (!s_version[0])
            wcscpy(s_version, L"unknown");
        return;
    }
    if (!run(L"git rev-parse --abbrev-ref HEAD", 0, a, sizeof a) && a[0]) {
        MultiByteToWideChar(CP_UTF8, 0, a, -1, s_branch, 64);
    }
    if (!run(L"git rev-list --count HEAD", 0, a, sizeof a)
            && !run(L"git rev-parse --short HEAD", 0, b, sizeof b))
        swprintf(s_version, 64, L"%S (%S)", a, b);
}

/* Results of the update jobs besides a count: */
#define UPD_FAILED   -1     /* see logs\update.log */
#define UPD_CHANGED  -2     /* the folder has changes of its own */
#define UPD_NO_REPO  -3     /* an install that does not say where it came from */
#define UPD_NO_GIT   -4     /* git is not on this PC */

static long check_job(void)
{
    wchar_t c[600];
    char n[128];

    step(30, "Looking for updates ...");
    /* An install: the newest commit on GitHub against the installed one,
     * without a checkout. Its first update makes the folder one. */
    if (!s_git) {
        wchar_t repo[400], branch[64], have[64];
        char a[64];
        info_get(L"repo", repo, 400);
        info_get(L"branch", branch, 64);
        info_get(L"project", have, 64);
        if (!repo[0] || !branch[0] || !have[0])
            return UPD_NO_REPO;
        if (run(L"git --version", 0, NULL, 0))
            return UPD_NO_GIT;
        swprintf(c, 600, L"git ls-remote \"%s\" refs/heads/%s", repo, branch);
        if (run(c, 0, n, sizeof n) || !n[0])
            return UPD_FAILED;
        WideCharToMultiByte(CP_UTF8, 0, have, -1, a, sizeof a, NULL, NULL);
        return _strnicmp(n, a, strlen(a)) ? 1 : 0;
    }
    if (run(L"git fetch origin", 0, NULL, 0))
        return -1;
    swprintf(c, 160, L"git rev-list --count HEAD..origin/%s", s_branch);
    if (run(c, 0, n, sizeof n))
        return -1;
    return atol(n);
}

/* The player's values: nothing else in the file is theirs to keep. Each with
 * the name it had before the file was in English, read where the new one is
 * missing and dropped once the new one is written. */
static const struct { const wchar_t *sec, *key, *old_sec, *old_key; } s_keys[] = {
    { L"Graphics", L"Resolution", L"Grafik", L"Aufloesung" },
    { L"Graphics", L"Widescreen", L"Grafik", L"Breitbild" },
    { L"Graphics", L"Anisotropy", L"Grafik", L"Anisotropie" },
    { L"Graphics", L"FlickerFilter", L"Grafik", L"Flimmerfilter" },
    { L"Graphics", L"VSync", L"Grafik", L"VSync" },
    { L"Graphics", L"Fullscreen", L"Graphics", L"Fullscreen" },
    { L"Graphics", L"DrivingAA", L"Graphics", L"DrivingAA" },
    { L"Graphics", L"RefreshRate", L"Graphics", L"RefreshRate" },
    { L"Launcher", L"Mode", L"Launcher", L"Modus" },
};
#define NKEYS (sizeof s_keys / sizeof s_keys[0])

static void ini_get(size_t i, wchar_t *out, DWORD cap)
{
    GetPrivateProfileStringW(s_keys[i].sec, s_keys[i].key, L"", out, cap, s_ini);
    if (!out[0])
        GetPrivateProfileStringW(s_keys[i].old_sec, s_keys[i].old_key, L"", out, cap, s_ini);
}

static void ini_put(size_t i, const wchar_t *v)
{
    WritePrivateProfileStringW(s_keys[i].sec, s_keys[i].key, v, s_ini);
    if (_wcsicmp(s_keys[i].sec, s_keys[i].old_sec) || _wcsicmp(s_keys[i].key, s_keys[i].old_key))
        WritePrivateProfileStringW(s_keys[i].old_sec, s_keys[i].old_key, NULL, s_ini);
}

static size_t ini_index(const wchar_t *key)
{
    size_t i;
    for (i = 0; i < NKEYS && _wcsicmp(s_keys[i].key, key); i++)
        ;
    return i;
}

/* run(), with the output in a file of its own. */
static int run_to_file(const wchar_t *cmd, const wchar_t *path)
{
    HANDLE keep = s_logfile;
    int r;

    s_logfile = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    r = run(cmd, 0, NULL, 0);
    if (s_logfile != INVALID_HANDLE_VALUE)
        CloseHandle(s_logfile);
    s_logfile = keep;
    return r;
}

/* Anything changed besides the two settings files stops the update, and a
 * developer's work with it. */
static int tree_clean(void)
{
    char buf[256];
    wchar_t tmp[MAX_PATH];
    FILE *f;
    int clean = 1;

    join(tmp, L"logs\\update_status.tmp");
    run_to_file(L"git status --porcelain --untracked-files=no", tmp);
    f = _wfopen(tmp, L"r");
    if (f) {
        while (fgets(buf, sizeof buf, f))
            if (!strstr(buf, "nightfire.ini") && !strstr(buf, "mkb.ini"))
                clean = 0;
        fclose(f);
    }
    DeleteFileW(tmp);
    return clean;
}

/* Deletes a folder inside the project folder with everything in it. */
static int remove_tree(const wchar_t *path)
{
    wchar_t from[MAX_PATH + 2];
    SHFILEOPSTRUCTW op;
    size_t n = wcslen(s_root);

    if (_wcsnicmp(path, s_root, n) || path[n] != L'\\' || !exists(path))
        return !exists(path);
    memset(from, 0, sizeof from);
    wcsncpy(from, path, MAX_PATH);
    memset(&op, 0, sizeof op);
    op.wFunc = FO_DELETE;
    op.pFrom = from;                      /* double-NUL ended */
    op.fFlags = FOF_NO_UI;
    return SHFileOperationW(&op) == 0 && !exists(path);
}

/* An install by Nightfire-Setup becomes a checkout of the repository it came
 * from: git init, fetch, check out (the tracked files get the newest version,
 * the game, the build and the logs are ignored and stay), and the toolkit as
 * its submodule instead of the installer's copy. From then on it updates like
 * any checkout. */
static long make_checkout(void)
{
    wchar_t repo[400], branch[64], c[600], tk[MAX_PATH], dotgit[MAX_PATH];

    info_get(L"repo", repo, 400);
    info_get(L"branch", branch, 64);
    if (!repo[0] || !branch[0])
        return UPD_NO_REPO;
    if (run(L"git --version", 0, NULL, 0))
        return UPD_NO_GIT;
    step(10, "Fetching the newest version from GitHub ...");
    join(dotgit, L".git");
    swprintf(c, 600, L"git remote add origin \"%s\"", repo);
    if (run(L"git init -q", 1, NULL, 0) || run(c, 1, NULL, 0)
            || (swprintf(c, 600, L"git fetch origin %s", branch), run(c, 1, NULL, 0))
            || (swprintf(c, 600, L"git checkout -q -f -B %s --track origin/%s", branch, branch), run(c, 1, NULL, 0))) {
        /* Still an install, as before: no half checkout left behind. */
        remove_tree(dotgit);
        return UPD_FAILED;
    }
    s_git = 1;
    wcsncpy(s_branch, branch, 63);
    join(tk, L"external\\xboxrecomp");
    join(dotgit, L"external\\xboxrecomp\\.git");
    if (!exists(dotgit) && !remove_tree(tk))
        return UPD_FAILED;
    return 0;
}

static long update_job(void)
{
    wchar_t vals[NKEYS][32], bak[MAX_PATH], self[MAX_PATH], old[MAX_PATH];
    size_t i;
    int r;

    step(5, "Checking the folder ...");
    if (s_git && !tree_clean()) {
        job_line("Update stopped: the folder has changes of its own.");
        return UPD_CHANGED;
    }
    for (i = 0; i < NKEYS; i++)
        ini_get(i, vals[i], 32);
    join(bak, L"nightfire.ini.bak");
    CopyFileW(s_ini, bak, FALSE);
    join(bak, L"mkb.ini.bak");
    CopyFileW(s_mkb, bak, FALSE);
    if (s_git) {
        run(L"git checkout -- nightfire.ini mkb.ini", 1, NULL, 0);
        step(10, "Fetching the changes from GitHub ...");
        r = run(L"git pull --ff-only", 1, NULL, 0) ? UPD_FAILED : 0;
    } else {
        r = (int)make_checkout();
    }
    for (i = 0; i < NKEYS; i++)
        if (vals[i][0])
            ini_put(i, vals[i]);
    CopyFileW(bak, s_mkb, FALSE);
    if (r)
        return r;

    step(15, "Updating the toolkit ...");
    if (run(L"git submodule update --init --recursive", 1, NULL, 0))
        return -1;

    step(20, "Translating the game code (15 to 30 minutes the first time) ...");
    if (run(L"powershell -NoProfile -ExecutionPolicy RemoteSigned -File scripts\\recompile.ps1", 1, NULL, 0))
        return -1;

    step(60, "Building the game ...");
    GetModuleFileNameW(NULL, self, MAX_PATH);
    swprintf(old, MAX_PATH, L"%s.old", self);
    DeleteFileW(old);
    MoveFileW(self, old);
    r = run(L"powershell -NoProfile -ExecutionPolicy RemoteSigned -File scripts\\build.ps1", 1, NULL, 0);
    if (!exists(self))
        MoveFileW(old, self);
    if (r)
        return -1;
    step(100, "Done.");
    return 0;
}

static DWORD WINAPI job_thread(void *arg)
{
    int job = (int)(INT_PTR)arg;
    long r = 0;
    wchar_t dir[MAX_PATH];

    join(dir, L"logs");
    CreateDirectoryW(dir, NULL);
    s_logfile = CreateFileW(s_log, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                            job == JOB_UPDATE ? CREATE_ALWAYS : OPEN_ALWAYS, 0, NULL);
    if (s_logfile != INVALID_HANDLE_VALUE)
        SetFilePointer(s_logfile, 0, NULL, FILE_END);
    if (job == JOB_CHECK)
        r = check_job();
    else if (job == JOB_UPDATE)
        r = update_job();
    read_version();
    if (s_logfile != INVALID_HANDLE_VALUE)
        CloseHandle(s_logfile);
    s_logfile = INVALID_HANDLE_VALUE;
    PostMessageW(s_win, WM_JOB_DONE, (WPARAM)job, (LPARAM)r);
    return 0;
}

static void start_job(int job)
{
    HANDLE h;

    if (InterlockedCompareExchange(&s_busy, 1, 0))
        return;
    update_ui();
    SendMessageW(s_bar, PBM_SETPOS, 0, 0);
    h = CreateThread(NULL, 0, job_thread, (void *)(INT_PTR)job, 0, NULL);
    if (h)
        CloseHandle(h);
    else
        InterlockedExchange(&s_busy, 0);
}

static void job_done(int job, long r)
{
    wchar_t t[200];

    InterlockedExchange(&s_busy, 0);
    if (r == UPD_NO_GIT) {
        say(L"Updates need Git: install it from git-scm.com, then try again.");
    } else if (r == UPD_NO_REPO) {
        say(L"This install does not say where its updates come from. Use a newer Nightfire-Setup.");
    } else if (job == JOB_CHECK) {
        s_pending = r > 0 ? r : 0;
        SendMessageW(s_bar, PBM_SETPOS, 100, 0);
        if (r < 0)
            say(L"No connection to GitHub. Details in logs\\update.log.");
        else if (r == 0)
            say(L"The game is up to date.");
        else
            say(L"An update is ready.");
    } else if (job == JOB_UPDATE) {
        s_pending = 0;
        if (r == 0) {
            SendMessageW(s_bar, PBM_SETPOS, 100, 0);
            swprintf(t, 200, L"Update done. New version: %s", s_version);
            say(t);
        } else if (r == UPD_CHANGED) {
            say(L"Update stopped: the folder has changes of its own.");
        } else {
            say(L"Update failed. Details in logs\\update.log.");
        }
    }
    update_ui();
}

/* ---- Actions ---------------------------------------------------------- */

static void play(void)
{
    wchar_t exe[MAX_PATH], cmd[MAX_PATH + 32];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    join(exe, L"build\\Release\\nightfire.exe");
    swprintf(cmd, MAX_PATH + 32, L"\"%s\" %s", exe, enhanced() ? L"enhanced" : L"original");
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, s_root, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        s_launching = GetTickCount();
        say(L"Starting the game ...");
        update_ui();
    } else {
        say(L"The game could not be started.");
    }
}

static void save_mode(void)
{
    ini_put(ini_index(L"Mode"), enhanced() ? L"enhanced" : L"original");
}

/* ---- Settings --------------------------------------------------------- */

static int ini_int(const wchar_t *key, int def)
{
    wchar_t t[32];
    ini_get(ini_index(key), t, 32);
    return t[0] ? _wtoi(t) : def;
}

static void ini_set(const wchar_t *key, int v)
{
    wchar_t t[16];
    swprintf(t, 16, L"%d", v);
    ini_put(ini_index(key), t);
}

static HWND add(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h),
                             parent, (HMENU)(INT_PTR)id, s_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    return c;
}

static void dlg_close(void)
{
    EnableWindow(s_win, TRUE);
    DestroyWindow(s_dlg);
    s_dlg = NULL;
    SetForegroundWindow(s_win);
    update_ui();
}

static const int s_aniso[] = { 1, 2, 4, 8, 16 };

/* Mouse sensitivity on the slider in tenths, 0.5 to 10 (the PC version's
 * default is 2), and where it stood when the dialog opened: a value set by
 * hand outside the slider's range stays unless the slider is moved. */
#define SENS_MIN 5
#define SENS_MAX 100
static int s_sens_open;

static void open_keys(void);
static void mkb_get(const char *name, char *out, size_t cap);
static int mkb_put(const char *name, const char *val);

static void sens_show(HWND h)
{
    wchar_t t[16];
    int v = (int)SendDlgItemMessageW(h, ID_S_SENS, TBM_GETPOS, 0, 0);
    swprintf(t, 16, L"%d.%d", v / 10, v % 10);
    SetDlgItemTextW(h, ID_S_SENSV, t);
}

/* The resolution list: the usual monitor sizes in the picture's shape --
 * 16:9 with widescreen (an enhancement: the Xbox game itself is 4:3 only),
 * else 4:3 -- and those Windows offers for the monitor in that shape, up to
 * 3840x2160 and 1920x1440. Saved as Resolution = <w>x<h>, the size the game
 * draws its picture at (src/main.c, RECOMP_GPUBE_SIZE). A file with an older
 * step n (1-8, n times 640x480) shows the size nearest to its height. */
typedef struct { int w, h; } Res;
static const Res s_res_wide[] = { {1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080},
                                  {2560, 1440}, {3200, 1800}, {3840, 2160} };
static const Res s_res_43[] = { {640, 480}, {800, 600}, {1024, 768}, {1152, 864}, {1280, 960},
                                {1400, 1050}, {1600, 1200}, {1920, 1440} };
static const Res s_res_uw[] = { {2560, 1080}, {3440, 1440}, {3840, 1600} };
#define RES_MAX 48
static Res s_res[RES_MAX];
static int s_nres;

/* shape 0 4:3 (exactly), 1 16:9 (within 1 %: 1366x768 is 16:9 to the pixel
 * row), 2 ultrawide (2.3 to 2.45: 2560x1080, 3440x1440, 3840x1600 ...) */
static int res_shape(int w, int h, int shape)
{
    if (shape == 2)
        return w * 100 >= h * 230 && w * 100 <= h * 245;
    return shape ? (w * 900 >= h * 1584 && w * 900 <= h * 1616) : w * 3 == h * 4;
}

static void res_add(int w, int h)
{
    int i, k;
    for (i = 0; i < s_nres; i++)
        if (s_res[i].w == w && s_res[i].h == h)
            return;
    if (s_nres == RES_MAX)
        return;
    for (i = 0; i < s_nres && (s_res[i].h < h || (s_res[i].h == h && s_res[i].w < w)); i++) {}
    for (k = s_nres; k > i; k--)
        s_res[k] = s_res[k - 1];
    s_res[i].w = w;
    s_res[i].h = h;
    s_nres++;
}

/* Fills the list for the shape and chooses the entry with height h (the
 * nearest there is). */
static void res_fill(HWND c, int shape, int h)
{
    DEVMODEW dm;
    DWORD i;
    int k, best = 0, mw = 0, mh = 0;
    wchar_t t[64];

    s_nres = 0;
    if (shape == 2)
        for (k = 0; k < (int)(sizeof s_res_uw / sizeof s_res_uw[0]); k++)
            res_add(s_res_uw[k].w, s_res_uw[k].h);
    else if (shape)
        for (k = 0; k < (int)(sizeof s_res_wide / sizeof s_res_wide[0]); k++)
            res_add(s_res_wide[k].w, s_res_wide[k].h);
    else
        for (k = 0; k < (int)(sizeof s_res_43 / sizeof s_res_43[0]); k++)
            res_add(s_res_43[k].w, s_res_43[k].h);
    memset(&dm, 0, sizeof dm);
    dm.dmSize = sizeof dm;
    for (i = 0; EnumDisplaySettingsW(NULL, i, &dm); i++)
        if (res_shape((int)dm.dmPelsWidth, (int)dm.dmPelsHeight, shape) && dm.dmPelsHeight >= 480
                && dm.dmPelsWidth <= (DWORD)(shape == 2 ? 5120 : shape ? 3840 : 1920))
            res_add((int)dm.dmPelsWidth, (int)dm.dmPelsHeight);
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        mw = (int)dm.dmPelsWidth;
        mh = (int)dm.dmPelsHeight;
    }
    SendMessageW(c, CB_RESETCONTENT, 0, 0);
    for (k = 0; k < s_nres; k++) {
        swprintf(t, 64, L"%d x %d%s", s_res[k].w, s_res[k].h,
                 s_res[k].w == 640 && s_res[k].h == 480 ? L" (as the console)"
                 : s_res[k].w == mw && s_res[k].h == mh ? L" (this monitor)" : L"");
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)t);
        if (abs(s_res[k].h - h) < abs(s_res[best].h - h))
            best = k;
    }
    SendMessageW(c, CB_SETCURSEL, best, 0);
}

/* The height the saved Resolution draws: <w>x<h>, or a step n of 480 * n. */
static int res_saved_height(void)
{
    wchar_t t[32];
    int w = 0, h = 0;
    ini_get(ini_index(L"Resolution"), t, 32);
    if (swscanf(t, L"%d x %d", &w, &h) == 2 && h >= 240)
        return h;
    w = _wtoi(t);
    return 480 * (w >= 1 && w <= 8 ? w : 1);
}

static LRESULT CALLBACK dlg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        HWND c;
        int i, y = 16, cur;
        wchar_t t[64];

        /* Widescreen = 0 4:3, 1 16:9, 2 ultrawide (21:9) -- enhancements: the
         * Xbox game is 4:3 only */
        add(h, L"STATIC", L"Picture", 0, 16, y + 3, 110, 20, 0);
        c = add(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 130, y, 214, 200, ID_S_WIDE);
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"4:3 (as the console)");
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"16:9 widescreen");
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"21:9 ultrawide");
        cur = ini_int(L"Widescreen", 0);
        SendMessageW(c, CB_SETCURSEL, cur < 0 || cur > 2 ? 0 : cur, 0);
        y += 32;
        add(h, L"STATIC", L"Resolution", 0, 16, y + 3, 110, 20, 0);
        c = add(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 130, y, 214, 240, ID_S_RES);
        res_fill(c, cur < 0 || cur > 2 ? 0 : cur, res_saved_height());
        y += 32;
        add(h, L"STATIC", L"Anisotropy", 0, 16, y + 3, 110, 20, 0);
        c = add(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 130, y, 214, 200, ID_S_ANISO);
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"Off (as the console)");
        for (i = 1; i < 5; i++) {
            swprintf(t, 64, L"%dx", s_aniso[i]);
            SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)t);
        }
        cur = ini_int(L"Anisotropy", 1);
        SendMessageW(c, CB_SETCURSEL, 0, 0);
        for (i = 0; i < 5; i++)
            if (s_aniso[i] == cur)
                SendMessageW(c, CB_SETCURSEL, i, 0);
        y += 36;
        c = add(h, L"BUTTON", L"Flicker filter (softer Image)", BS_AUTOCHECKBOX | WS_TABSTOP, 16, y, 328, 22, ID_S_FLICKER);
        SendMessageW(c, BM_SETCHECK, ini_int(L"FlickerFilter", 0) ? BST_CHECKED : BST_UNCHECKED, 0);
        y += 26;
        c = add(h, L"BUTTON", L"VSync", BS_AUTOCHECKBOX | WS_TABSTOP, 16, y, 328, 22, ID_S_VSYNC);
        SendMessageW(c, BM_SETCHECK, ini_int(L"VSync", 1) ? BST_CHECKED : BST_UNCHECKED, 0);
        y += 26;
        c = add(h, L"BUTTON", L"Fullscreen (F11 switches ingame)", BS_AUTOCHECKBOX | WS_TABSTOP, 16, y, 328, 22, ID_S_FULL);
        SendMessageW(c, BM_SETCHECK, ini_int(L"Fullscreen", 0) ? BST_CHECKED : BST_UNCHECKED, 0);
        y += 26;
        c = add(h, L"BUTTON", L"Original Console AA for Driving Missions", BS_AUTOCHECKBOX | WS_TABSTOP, 16, y, 328, 22, ID_S_DRVAA);
        SendMessageW(c, BM_SETCHECK, ini_int(L"DrivingAA", 0) ? BST_CHECKED : BST_UNCHECKED, 0);
        y += 26;
        c = add(h, L"BUTTON", L"60 Hz (European disc, as the US version)", BS_AUTOCHECKBOX | WS_TABSTOP, 16, y, 328, 22, ID_S_HZ60);
        SendMessageW(c, BM_SETCHECK, ini_int(L"RefreshRate", 50) == 60 ? BST_CHECKED : BST_UNCHECKED, 0);
        y += 32;
        {
            char s[32];
            mkb_get("sensitivity", s, sizeof s);
            cur = (int)(atof(s[0] ? s : "2") * 10.0 + 0.5);
        }
        add(h, L"STATIC", L"Mouse sensitivity", 0, 16, y + 3, 110, 20, 0);
        c = add(h, L"msctls_trackbar32", L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 124, y, 180, 26, ID_S_SENS);
        SendMessageW(c, TBM_SETRANGE, FALSE, MAKELPARAM(SENS_MIN, SENS_MAX));
        SendMessageW(c, TBM_SETPAGESIZE, 0, 5);
        s_sens_open = cur < SENS_MIN ? SENS_MIN : cur > SENS_MAX ? SENS_MAX : cur;
        SendMessageW(c, TBM_SETPOS, TRUE, s_sens_open);
        add(h, L"STATIC", L"", SS_RIGHT, 306, y + 3, 38, 20, ID_S_SENSV);
        sens_show(h);
        y += 36;
        add(h, L"BUTTON", L"Edit controls ...", WS_TABSTOP, 16, y, 170, 28, ID_S_KEYS);
        add(h, L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, 192, y, 76, 28, IDOK);
        add(h, L"BUTTON", L"Cancel", WS_TABSTOP, 272, y, 72, 28, IDCANCEL);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDOK:
            {
                int k = (int)SendDlgItemMessageW(h, ID_S_RES, CB_GETCURSEL, 0, 0);
                wchar_t t[32];
                if (k >= 0 && k < s_nres) {
                    swprintf(t, 32, L"%dx%d", s_res[k].w, s_res[k].h);
                    ini_put(ini_index(L"Resolution"), t);
                }
            }
            ini_set(L"Anisotropy", s_aniso[SendDlgItemMessageW(h, ID_S_ANISO, CB_GETCURSEL, 0, 0)]);
            ini_set(L"Widescreen", (int)SendDlgItemMessageW(h, ID_S_WIDE, CB_GETCURSEL, 0, 0));
            ini_set(L"FlickerFilter", SendDlgItemMessageW(h, ID_S_FLICKER, BM_GETCHECK, 0, 0) == BST_CHECKED);
            ini_set(L"VSync", SendDlgItemMessageW(h, ID_S_VSYNC, BM_GETCHECK, 0, 0) == BST_CHECKED);
            ini_set(L"Fullscreen", SendDlgItemMessageW(h, ID_S_FULL, BM_GETCHECK, 0, 0) == BST_CHECKED);
            ini_set(L"DrivingAA", SendDlgItemMessageW(h, ID_S_DRVAA, BM_GETCHECK, 0, 0) == BST_CHECKED);
            ini_set(L"RefreshRate", SendDlgItemMessageW(h, ID_S_HZ60, BM_GETCHECK, 0, 0) == BST_CHECKED ? 60 : 50);
            {
                int v = (int)SendDlgItemMessageW(h, ID_S_SENS, TBM_GETPOS, 0, 0);
                char s[16];
                snprintf(s, sizeof s, "%d.%d", v / 10, v % 10);
                if (v != s_sens_open && !mkb_put("sensitivity", s)) {
                    say(L"Could not write mkb.ini.");
                    return 0;
                }
            }
            say(L"Settings saved.");
            dlg_close();
            return 0;
        case IDCANCEL:
            dlg_close();
            return 0;
        case ID_S_KEYS:
            open_keys();
            return 0;
        case ID_S_WIDE:
            if (HIWORD(w) == CBN_SELCHANGE) {
                /* the other shape's list, at the height chosen in this one */
                int k = (int)SendDlgItemMessageW(h, ID_S_RES, CB_GETCURSEL, 0, 0);
                res_fill(GetDlgItem(h, ID_S_RES),
                         (int)SendDlgItemMessageW(h, ID_S_WIDE, CB_GETCURSEL, 0, 0),
                         k >= 0 && k < s_nres ? s_res[k].h : 480);
            }
            return 0;
        }
        break;
    case WM_HSCROLL:
        if ((HWND)l == GetDlgItem(h, ID_S_SENS))
            sens_show(h);
        return 0;
    case WM_CLOSE:
        dlg_close();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void open_settings(void)
{
    RECT r, wr = { 0, 0, 0, 0 };
    DWORD st = WS_POPUP | WS_CAPTION | WS_SYSMENU;

    wr.right = px(360);
    wr.bottom = px(322);
    AdjustWindowRect(&wr, st, FALSE);
    GetWindowRect(s_win, &r);
    s_dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"NfSettings", L"Settings", st,
                            r.left + px(20), r.top + px(100), wr.right - wr.left, wr.bottom - wr.top,
                            s_win, NULL, s_inst, NULL);
    if (s_dlg) {
        EnableWindow(s_win, FALSE);
        ShowWindow(s_dlg, SW_SHOW);
    }
    update_ui();
}

/* ---- Main window ------------------------------------------------------ */

static void paint_header(HDC dc, int w, const wchar_t *sub)
{
    TRIVERTEX v[2];
    GRADIENT_RECT g = { 0, 1 };
    RECT r;
    HFONT old;
    int i, y = px(10), h = px(HEADER_H - 16), x = px(12), cw = w - px(24);

    v[0].x = x; v[0].y = y; v[0].Red = 0x0800; v[0].Green = 0x0900; v[0].Blue = 0x0D00; v[0].Alpha = 0;
    v[1].x = x + cw; v[1].y = y + h; v[1].Red = 0x5800; v[1].Green = 0x1400; v[1].Blue = 0x0A00; v[1].Alpha = 0;
    GradientFill(dc, v, 2, &g, 1, GRADIENT_FILL_RECT_H);
    /* Fine scan lines, for the feel of the TV the game was made for. */
    for (i = y; i < y + h; i += px(3)) {
        RECT l = { x, i, x + cw, i + 1 };
        HBRUSH b = CreateSolidBrush(RGB(8, 9, 13));
        FillRect(dc, &l, b);
        DeleteObject(b);
    }
    SetBkMode(dc, TRANSPARENT);
    old = (HFONT)SelectObject(dc, s_big);
    SetTextColor(dc, RGB(245, 245, 245));
    SetRect(&r, x, y + px(16), x + cw, y + px(66));
    DrawTextW(dc, L"NIGHTFIRE", -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, s_small);
    SetTextColor(dc, RGB(226, 82, 58));
    SetRect(&r, x, y + px(66), x + cw, y + px(84));
    DrawTextW(dc, sub, -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, old);
}

/* ---- Controls --------------------------------------------------------- */

/* mkb.ini's controls by what they do in the game. The lines keep their place
 * and comments, only the keys after the '=' change. A row with key2 moves two
 * controls with one set of keys: in the driving missions W and S work the
 * pedals and the menus at once, and the D-pad's up and down are free there
 * (drivecfg.def in race.viv), where the stick's also turns the camera. */
typedef struct { const wchar_t *label; const char *key, *key2; } KeyRow;

static const KeyRow s_foot[] = {
    { L"Forward", "+forward" }, { L"Back", "+back" },
    { L"Strafe left", "+moveleft" }, { L"Strafe right", "+moveright" },
    { L"Fire", "+attack" }, { L"Alternate fire / zoom", "+attack2" },
    { L"Jump", "+jump" }, { L"Crouch", "+duck" },
    { L"Use / reload", "+use" }, { L"Vision mode", "+vision" },
    { L"Next weapon", "+nextweapon" }, { L"Previous weapon", "+prevweapon" },
    { L"Next gadget", "+nextgadget" }, { L"Previous gadget", "+prevgadget" },
    { L"Zoom in (while aiming)", "+zoomin" }, { L"Zoom out (while aiming)", "+zoomout" },
    { L"Pause", "+pause" },
    { L"Menu: up", "+menu_up" }, { L"Menu: down", "+menu_down" },
    { L"Menu: left", "+menu_left" }, { L"Menu: right", "+menu_right" },
    { L"Menu: accept", "+menu_accept" }, { L"Menu: back", "+menu_back" },
    { L"Menu: Y button", "+menu_y" }, { L"Menu: X button", "+menu_x" },
};
static const KeyRow s_drive[] = {
    { L"Gas / menu up", "right_trigger", "dpad_up" },
    { L"Brake / menu down", "left_trigger", "dpad_down" },
    { L"Steer left / menu left", "left_stick_left" },
    { L"Steer right / menu right", "left_stick_right" },
    { L"Fire / accept", "a" }, { L"Fire on the gun", "+attack" },
    { L"Handbrake", "b" }, { L"Use gadget", "x" }, { L"Camera view", "y" },
    { L"Previous weapon", "white" }, { L"Next weapon", "black" },
    { L"Look back", "right_thumb" }, { L"Pause", "start" },
};
#define KEY_SLOTS 4     /* mkb_input: bindings per control */
#define KEY_ROWS  26    /* the longer page */
#define KEY_NAME  24

static const struct { const char *sec; const KeyRow *rows; int n; } s_pages[2] = {
    { "nightfire", s_foot, (int)(sizeof s_foot / sizeof s_foot[0]) },
    { "nightfire_driving", s_drive, (int)(sizeof s_drive / sizeof s_drive[0]) },
};

/* The toolkit's key names (input_map.c). No label: the keyboard's own name for
 * the key, which depends on the layout. */
static const struct { const char *name; UINT vk; const wchar_t *label; } s_names[] = {
    { "mouse1", VK_LBUTTON, L"Mouse L" }, { "mouse2", VK_RBUTTON, L"Mouse R" },
    { "mouse3", VK_MBUTTON, L"Mouse M" }, { "mouse4", VK_XBUTTON1, L"Mouse 4" },
    { "mouse5", VK_XBUTTON2, L"Mouse 5" }, { "wheelup", 0, L"Wheel ↑" }, { "wheeldown", 0, L"Wheel ↓" },
    { "aim+wheelup", 0, L"Wheel ↑" }, { "aim+wheeldown", 0, L"Wheel ↓" },
    { "space", VK_SPACE, L"Space" }, { "enter", VK_RETURN, L"Enter" }, { "escape", VK_ESCAPE, L"Esc" },
    { "tab", VK_TAB, L"Tab" }, { "backspace", VK_BACK, L"Backspace" }, { "capslock", VK_CAPITAL, L"Caps Lock" },
    { "up", VK_UP, L"Up" }, { "down", VK_DOWN, L"Down" },
    { "left", VK_LEFT, L"Left" }, { "right", VK_RIGHT, L"Right" },
    { "lshift", VK_LSHIFT, L"Left Shift" }, { "rshift", VK_RSHIFT, L"Right Shift" },
    { "lctrl", VK_LCONTROL, L"Left Ctrl" }, { "rctrl", VK_RCONTROL, L"Right Ctrl" },
    { "lalt", VK_LMENU, L"Alt" }, { "ralt", VK_RMENU, L"Alt Gr" },
    { "insert", VK_INSERT, L"Insert" }, { "delete", VK_DELETE, L"Delete" }, { "home", VK_HOME, L"Home" },
    { "end", VK_END, L"End" }, { "pageup", VK_PRIOR, L"Page Up" }, { "pagedown", VK_NEXT, L"Page Down" },
    { "pause", VK_PAUSE, L"Pause" }, { "numpadmultiply", VK_MULTIPLY, L"Num *" }, { "numpadadd", VK_ADD, L"Num +" },
    { "numpadsubtract", VK_SUBTRACT, L"Num -" }, { "numpaddecimal", VK_DECIMAL, L"Num ." },
    { "numpaddivide", VK_DIVIDE, L"Num /" }, { "semicolon", VK_OEM_1 }, { "equals", VK_OEM_PLUS },
    { "comma", VK_OEM_COMMA }, { "minus", VK_OEM_MINUS }, { "period", VK_OEM_PERIOD }, { "slash", VK_OEM_2 },
    { "grave", VK_OEM_3 }, { "leftbracket", VK_OEM_4 }, { "backslash", VK_OEM_5 },
    { "rightbracket", VK_OEM_6 }, { "quote", VK_OEM_7 },
};
#define NNAMES (sizeof s_names / sizeof s_names[0])

static char s_bound[2][KEY_ROWS][KEY_SLOTS][KEY_NAME];
static HWND s_klist, s_ktext, s_kcancel, s_kfoot;
static int s_kpage, s_krow, s_kcol, s_kwait;

static const wchar_t *s_khint =
    L"Double-click a field, then press a key. F9 and F11 belong to the game.";

static char *read_all(const wchar_t *path)
{
    FILE *f = _wfopen(path, L"rb");
    long n;
    char *b = NULL;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n >= 0 && (b = (char *)malloc((size_t)n + 1)) != NULL) {
        n = (long)fread(b, 1, (size_t)n, f);
        b[n] = 0;
    }
    fclose(f);
    return b;
}

static const char *line_end(const char *p)
{
    while (*p && *p != '\n')
        p++;
    return p;
}

/* The page of a "[section]" line, -1 for any other section, -2 if it is none. */
static int section_of(const char *p, const char *e)
{
    int i;

    while (p < e && (*p == ' ' || *p == '\t'))
        p++;
    if (p == e || *p != '[')
        return -2;
    p++;
    for (i = 0; i < 2; i++) {
        size_t n = strlen(s_pages[i].sec);
        if ((size_t)(e - p) > n && !strncmp(p, s_pages[i].sec, n) && p[n] == ']')
            return i;
    }
    return -1;
}

/* A "key = value" line: its key, and where its value begins and ends. */
static int line_key(const char *p, const char *e, char *key, size_t cap, const char **val, const char **end)
{
    const char *eq, *k;
    size_t n;

    while (p < e && (*p == ' ' || *p == '\t'))
        p++;
    for (eq = p; eq < e && *eq != '=' && *eq != '#' && *eq != ';'; eq++)
        ;
    if (eq == e || *eq != '=' || eq == p)
        return 0;
    for (k = eq; k > p && (k[-1] == ' ' || k[-1] == '\t'); k--)
        ;
    n = (size_t)(k - p);
    if (!n || n >= cap)
        return 0;
    memcpy(key, p, n);
    key[n] = 0;
    for (p = eq + 1; p < e && (*p == ' ' || *p == '\t'); p++)
        ;
    *val = p;
    while (p < e && *p != '#' && *p != ';' && *p != '\r' && *p != '\n')
        p++;
    while (p > *val && (p[-1] == ' ' || p[-1] == '\t'))
        p--;
    *end = p;
    return 1;
}

/* The bindings of one page out of mkb.ini's text, all of them for page -1. */
static void keys_read(const char *txt, int only)
{
    const char *p = txt, *e;
    int pg = -1;

    if (only < 0)
        memset(s_bound, 0, sizeof s_bound);
    else
        memset(s_bound[only], 0, sizeof s_bound[only]);
    for (; *p; p = *e ? e + 1 : e) {
        const char *v, *ve;
        char key[32];
        int h, r, slot = 0;

        e = line_end(p);
        if ((h = section_of(p, e)) != -2) {
            pg = h;
            continue;
        }
        if (pg < 0 || (only >= 0 && pg != only) || !line_key(p, e, key, sizeof key, &v, &ve))
            continue;
        for (r = 0; r < s_pages[pg].n && strcmp(key, s_pages[pg].rows[r].key); r++)
            ;
        if (r == s_pages[pg].n)
            continue;
        while (v < ve && slot < KEY_SLOTS) {
            const char *c = v;
            size_t n;
            while (c < ve && *c != ',')
                c++;
            for (n = (size_t)(c - v); n && (v[n - 1] == ' ' || v[n - 1] == '\t'); n--)
                ;
            if (n && n < KEY_NAME) {
                memcpy(s_bound[pg][r][slot], v, n);
                s_bound[pg][r][slot][n] = 0;
                slot++;
            }
            for (v = c < ve ? c + 1 : c; v < ve && (*v == ' ' || *v == '\t'); v++)
                ;
        }
    }
}

static void keys_value(int pg, int r, char *out, size_t cap)
{
    int s;

    out[0] = 0;
    for (s = 0; s < KEY_SLOTS && s_bound[pg][r][s][0]; s++) {
        strncat(out, s ? ", " : "", cap - strlen(out) - 1);
        strncat(out, s_bound[pg][r][s], cap - strlen(out) - 1);
    }
}

/* Lines of a page's controls that the file does not have yet, placed after
 * the section's last line that is not blank. */
static size_t keys_missing(char *out, size_t n, int pg, char seen[2][KEY_ROWS][2], const char *eol)
{
    char text[1024];
    size_t at = n, tn;
    int r, k;

    text[0] = 0;
    for (r = 0; r < s_pages[pg].n; r++)
        for (k = 0; k < 2; k++) {
            const char *key = k ? s_pages[pg].rows[r].key2 : s_pages[pg].rows[r].key;
            char val[128];
            if (!key || seen[pg][r][k])
                continue;
            keys_value(pg, r, val, sizeof val);
            snprintf(text + strlen(text), sizeof text - strlen(text), "%s = %s%s", key, val, eol);
        }
    if (!text[0])
        return n;
    while (at && (out[at - 1] == '\n' || out[at - 1] == '\r'))
        at--;
    while (at < n && out[at] != '\n')
        at++;
    if (at < n)
        at++;
    tn = strlen(text);
    memmove(out + at + tn, out + at, n - at);
    memcpy(out + at, text, tn);
    return n + tn;
}

/* mkb.ini's text with the bindings in place. An unchanged line stays as it is
 * to the byte; a changed one keeps its comment in its column. */
static char *keys_merge(const char *in)
{
    size_t cap = strlen(in) + 16384, n = 0;
    char *out = (char *)malloc(cap);
    char seen[2][KEY_ROWS][2];
    const char *eol = strstr(in, "\r\n") ? "\r\n" : "\n", *p = in, *e;
    int pg = -1, had[2] = { 0, 0 }, i;

    if (!out)
        return NULL;
    memset(seen, 0, sizeof seen);
    for (; *p; p = e) {
        const char *v, *ve;
        char key[32], val[128];
        int h, r, k, found = 0;

        e = line_end(p);
        if (*e)
            e++;
        if ((h = section_of(p, e)) != -2) {
            if (pg >= 0)
                n = keys_missing(out, n, pg, seen, eol);
            pg = h;
            if (h >= 0)
                had[h] = 1;
        } else if (pg >= 0 && line_key(p, e, key, sizeof key, &v, &ve)) {
            for (r = 0; r < s_pages[pg].n && !found; r++) {
                const KeyRow *row = &s_pages[pg].rows[r];
                k = row->key2 && !strcmp(key, row->key2);
                if (k || !strcmp(key, row->key)) {
                    found = 1;
                    seen[pg][r][k] = 1;
                    keys_value(pg, r, val, sizeof val);
                }
            }
            if (found && (strlen(val) != (size_t)(ve - v) || memcmp(val, v, (size_t)(ve - v)))) {
                const char *tail = ve;
                size_t len = strlen(val), head = (size_t)(v - p);
                while (tail < e && (*tail == ' ' || *tail == '\t'))
                    tail++;
                memcpy(out + n, p, head);
                n += head;
                memcpy(out + n, val, len);
                n += len;
                if (*tail == '#' || *tail == ';') {
                    size_t gap = (size_t)(tail - v) > len ? (size_t)(tail - v) - len : 1;
                    memset(out + n, ' ', gap);
                    n += gap;
                }
                memcpy(out + n, tail, (size_t)(e - tail));
                n += (size_t)(e - tail);
                continue;
            }
        }
        memcpy(out + n, p, (size_t)(e - p));
        n += (size_t)(e - p);
    }
    if (pg >= 0)
        n = keys_missing(out, n, pg, seen, eol);
    if (n && out[n - 1] != '\n') {
        memcpy(out + n, eol, strlen(eol));
        n += strlen(eol);
    }
    for (i = 0; i < 2; i++)
        if (!had[i]) {
            n += (size_t)sprintf(out + n, "%s[%s]%s", eol, s_pages[i].sec, eol);
            n = keys_missing(out, n, i, seen, eol);
        }
    out[n] = 0;
    return out;
}

/* Whether the line p..e is the "[*]" heading, the section every title reads. */
static int section_all(const char *p, const char *e)
{
    while (p < e && (*p == ' ' || *p == '\t'))
        p++;
    return e - p >= 3 && !strncmp(p, "[*]", 3);
}

/* A value of mkb.ini's [*] section (the mouse's settings), "" if missing. */
static void mkb_get(const char *name, char *out, size_t cap)
{
    char *txt = read_all(s_mkb), key[32];
    const char *p, *e, *v, *ve;
    int in = 0;

    out[0] = 0;
    for (p = txt; p && *p; p = *e ? e + 1 : e) {
        e = line_end(p);
        if (section_of(p, e) != -2)
            in = section_all(p, e);
        else if (in && line_key(p, e, key, sizeof key, &v, &ve) && !_stricmp(key, name)
                && (size_t)(ve - v) < cap) {
            memcpy(out, v, (size_t)(ve - v));
            out[ve - v] = 0;
        }
    }
    free(txt);
}

/* Sets a value of mkb.ini's [*] section: the line keeps its place and its
 * comment, a missing one goes right under the heading. */
static int mkb_put(const char *name, const char *val)
{
    char *in = read_all(s_mkb), *out, key[32];
    const char *eol, *p, *e, *v, *ve, *at = NULL;
    size_t n = 0, vl = strlen(val);
    wchar_t tmp[MAX_PATH];
    int sec = 0, done = 0, ok = 0;
    FILE *f;

    if (!in)
        return 0;
    eol = strstr(in, "\r\n") ? "\r\n" : "\n";
    if ((out = (char *)malloc(strlen(in) + strlen(name) + vl + 16)) == NULL) {
        free(in);
        return 0;
    }
    for (p = in; *p; p = e) {
        e = line_end(p);
        if (*e)
            e++;
        if (section_of(p, e) != -2) {
            sec = section_all(p, e);
            if (sec && !at)
                at = e;
        } else if (sec && !done && line_key(p, e, key, sizeof key, &v, &ve) && !_stricmp(key, name)) {
            memcpy(out + n, p, (size_t)(v - p));
            n += (size_t)(v - p);
            memcpy(out + n, val, vl);
            n += vl;
            memcpy(out + n, ve, (size_t)(e - ve));
            n += (size_t)(e - ve);
            done = 1;
            continue;
        }
        memcpy(out + n, p, (size_t)(e - p));
        n += (size_t)(e - p);
    }
    out[n] = 0;
    if (!done) {
        /* Under the heading, or a new [*] at the top. */
        size_t head = at ? (size_t)(at - in) : 0;
        char line[160];
        int ln = snprintf(line, sizeof line, "%s%s%s = %s%s", at ? "" : "[*]", at ? "" : eol,
                          name, val, eol);
        memmove(out + head + ln, out + head, n - head + 1);
        memcpy(out + head, line, (size_t)ln);
        n += (size_t)ln;
    }
    swprintf(tmp, MAX_PATH, L"%s.tmp", s_mkb);
    if ((f = _wfopen(tmp, L"wb")) != NULL) {
        ok = fwrite(out, 1, n, f) == n;
        ok = !fclose(f) && ok;
        ok = ok && MoveFileExW(tmp, s_mkb, MOVEFILE_REPLACE_EXISTING);
        if (!ok)
            DeleteFileW(tmp);
    }
    free(in);
    free(out);
    return ok;
}

static int keys_save(void)
{
    wchar_t tmp[MAX_PATH];
    char *in = read_all(s_mkb), *out = in ? keys_merge(in) : NULL;
    FILE *f;
    int ok = 0;

    swprintf(tmp, MAX_PATH, L"%s.tmp", s_mkb);
    if (out && (f = _wfopen(tmp, L"wb")) != NULL) {
        ok = fwrite(out, 1, strlen(out), f) == strlen(out);
        ok = !fclose(f) && ok;
        ok = ok && MoveFileExW(tmp, s_mkb, MOVEFILE_REPLACE_EXISTING);
        if (!ok)
            DeleteFileW(tmp);
    }
    free(in);
    free(out);
    return ok;
}

static void key_label(const char *tok, wchar_t *out, int cap)
{
    size_t i;
    UINT vk = 0;

    out[0] = 0;
    if (!tok[0])
        return;
    for (i = 0; i < NNAMES; i++)
        if (!_stricmp(tok, s_names[i].name)) {
            if (s_names[i].label) {
                wcsncpy(out, s_names[i].label, cap);
                return;
            }
            vk = s_names[i].vk;
        }
    if (!vk || !GetKeyNameTextW((LONG)(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16), out, cap)) {
        MultiByteToWideChar(CP_ACP, 0, tok, -1, out, cap);
        _wcsupr(out);
    }
}

static const char *vk_token(UINT vk)
{
    static char b[12];
    size_t i;

    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        b[0] = (char)(vk >= 'A' ? vk + 32 : vk);
        b[1] = 0;
        return b;
    }
    if (vk >= VK_F1 && vk <= VK_F12) {
        snprintf(b, sizeof b, "f%u", vk - VK_F1 + 1);
        return b;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        snprintf(b, sizeof b, "numpad%u", vk - VK_NUMPAD0);
        return b;
    }
    for (i = 0; i < NNAMES; i++)
        if (s_names[i].vk == vk)
            return s_names[i].name;
    return NULL;
}

static void keys_fill(void)
{
    int r, s;
    const KeyRow *rows = s_pages[s_kpage].rows;

    SendMessageW(s_klist, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(s_klist);
    for (r = 0; r < s_pages[s_kpage].n; r++) {
        LVITEMW it;
        memset(&it, 0, sizeof it);
        it.mask = LVIF_TEXT;
        it.iItem = r;
        it.pszText = (LPWSTR)rows[r].label;
        SendMessageW(s_klist, LVM_INSERTITEMW, 0, (LPARAM)&it);
        for (s = 0; s < KEY_SLOTS; s++) {
            wchar_t t[32];
            key_label(s_bound[s_kpage][r][s], t, 32);
            ListView_SetItemText(s_klist, r, s + 1, t);
        }
    }
    ListView_SetItemState(s_klist, s_krow, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    SendMessageW(s_klist, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(s_klist, NULL, TRUE);
}

/* The field the next key lands on: never past the row's first empty one. */
static int keys_slot(void)
{
    int n = 0;

    while (n < KEY_SLOTS && s_bound[s_kpage][s_krow][n][0])
        n++;
    return s_kcol > n ? n : s_kcol;
}

static void keys_wait_end(const wchar_t *text)
{
    s_kwait = 0;
    ReleaseCapture();
    SetWindowTextW(s_kcancel, L"Cancel");
    SetWindowTextW(s_ktext, text);
    keys_fill();
    SetFocus(s_klist);
}

static void keys_wait(void)
{
    wchar_t t[120];

    s_kcol = keys_slot();
    s_kwait = 1;
    ListView_SetItemText(s_klist, s_krow, s_kcol + 1, (LPWSTR)L"...");
    swprintf(t, 120, L"Key, mouse button or wheel for \"%s\" ...", s_pages[s_kpage].rows[s_krow].label);
    SetWindowTextW(s_ktext, t);
    SetWindowTextW(s_kcancel, L"Cancel input");
    SetFocus(s_kdlg);
    SetCapture(s_kdlg);
}

static void keys_take(const char *tok)
{
    char (*row)[KEY_NAME] = s_bound[s_kpage][s_krow];
    wchar_t t[160], name[32];
    int s, j, r, col = s_kcol, n = 0;

    if (!tok) {
        keys_wait_end(L"This key cannot be bound.");
        return;
    }
    if (!strcmp(tok, "f9") || !strcmp(tok, "f11")) {
        keys_wait_end(L"F9 switches mouse and keyboard, F11 full screen: both are taken.");
        return;
    }
    /* The zoom acts while aiming: its wheel is the aim+ one, which leaves the
     * plain wheel to the weapons. */
    if (!strncmp(tok, "wheel", 5) && strstr(s_pages[s_kpage].rows[s_krow].key, "zoom")) {
        static char aimed[KEY_NAME];
        snprintf(aimed, sizeof aimed, "aim+%s", tok);
        tok = aimed;
    }
    while (n < KEY_SLOTS && row[n][0])
        n++;
    for (j = 0; j < n; j++)
        if (j != col && !strcmp(row[j], tok)) {
            for (s = j; s + 1 < KEY_SLOTS; s++)
                strcpy(row[s], row[s + 1]);
            row[KEY_SLOTS - 1][0] = 0;
            if (j < col)
                col--;
            break;
        }
    strcpy(row[col], tok);
    key_label(tok, name, 32);
    swprintf(t, 160, L"%s bound.", name);
    for (r = 0; r < s_pages[s_kpage].n; r++)
        for (s = 0; s < KEY_SLOTS; s++)
            if (r != s_krow && !strcmp(s_bound[s_kpage][r][s], tok))
                swprintf(t, 160, L"%s is also on \"%s\".", name, s_pages[s_kpage].rows[r].label);
    s_kcol = col;
    keys_wait_end(t);
}

static void keys_clear(void)
{
    char (*row)[KEY_NAME] = s_bound[s_kpage][s_krow];
    int s;

    for (s = s_kcol; s + 1 < KEY_SLOTS; s++)
        strcpy(row[s], row[s + 1]);
    row[KEY_SLOTS - 1][0] = 0;
    keys_fill();
}

static void keys_default(void)
{
    wchar_t tmp[MAX_PATH];
    char *txt;

    if (s_git) {
        join(tmp, L"logs\\mkb_default.tmp");
        txt = !run_to_file(L"git show HEAD:mkb.ini", tmp) ? read_all(tmp) : NULL;
        DeleteFileW(tmp);
    } else {
        join(tmp, L"mkb.default.ini");          /* written by the installer */
        txt = read_all(tmp);
    }
    if (!txt || !txt[0]) {
        SetWindowTextW(s_ktext, L"The default layout could not be read.");
    } else {
        keys_read(txt, s_kpage);
        keys_fill();
        SetWindowTextW(s_ktext, L"Default layout of this page loaded. Save to keep it.");
    }
    free(txt);
}

/* Keys the dialog manager must not see: Enter on the list would save and
 * close, and while waiting every key is the answer. So is the wheel, which
 * Windows hands to the window under the pointer: the list would scroll. */
static int keys_pretranslate(const MSG *m)
{
    if (s_kdlg && s_kwait && m->message == WM_MOUSEWHEEL) {
        keys_take(GET_WHEEL_DELTA_WPARAM(m->wParam) > 0 ? "wheelup" : "wheeldown");
        return 1;
    }
    if (!s_kdlg || (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN))
        return 0;
    if (s_kwait) {
        UINT vk = (UINT)m->wParam, scan = (m->lParam >> 16) & 0xFF;
        int ext = (m->lParam >> 24) & 1;
        if (m->lParam & (1 << 30))
            return 1;       /* the key that opened the field, still held */
        if (vk == VK_SHIFT)
            vk = MapVirtualKeyW(scan, MAPVK_VSC_TO_VK_EX);
        else if (vk == VK_CONTROL)
            vk = ext ? VK_RCONTROL : VK_LCONTROL;
        else if (vk == VK_MENU)
            vk = ext ? VK_RMENU : VK_LMENU;
        keys_take(vk_token(vk));
        return 1;
    }
    if (m->wParam == VK_RETURN && GetFocus() == s_klist) {
        keys_wait();
        return 1;
    }
    return 0;
}

static LRESULT keys_draw(NMLVCUSTOMDRAW *d)
{
    switch (d->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        return CDRF_NOTIFYSUBITEMDRAW;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        int mine = (int)d->nmcd.dwItemSpec == s_krow, cell = mine && d->iSubItem == s_kcol + 1;

        d->nmcd.uItemState &= ~(UINT)(CDIS_SELECTED | CDIS_FOCUS);
        d->clrText = cell ? RGB(255, 255, 255) : GetSysColor(COLOR_WINDOWTEXT);
        d->clrTextBk = cell ? (s_kwait ? RGB(226, 82, 58) : RGB(88, 20, 10))
                            : mine ? RGB(246, 236, 233) : GetSysColor(COLOR_WINDOW);
        return CDRF_NEWFONT;
    }
    }
    return CDRF_DODEFAULT;
}

static void keys_close(void)
{
    if (s_kwait) {
        s_kwait = 0;
        ReleaseCapture();
    }
    EnableWindow(s_dlg, TRUE);
    DestroyWindow(s_kdlg);
    s_kdlg = NULL;
    SetForegroundWindow(s_dlg);
}

/* While waiting, any button is the answer. Only a left click on the field's
 * own cancel button is not. */
static void keys_mouse(const char *tok, LPARAM l)
{
    if (!strcmp(tok, "mouse1")) {
        POINT p = { (short)LOWORD(l), (short)HIWORD(l) };
        RECT r;
        GetWindowRect(s_kcancel, &r);
        MapWindowPoints(NULL, s_kdlg, (POINT *)&r, 2);
        if (PtInRect(&r, p)) {
            keys_wait_end(s_khint);
            return;
        }
    }
    keys_take(tok);
}

static LRESULT CALLBACK keys_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        static const wchar_t *const head[] = { L"Action", L"Key 1", L"Key 2", L"Key 3", L"Key 4" };
        LVCOLUMNW c;
        int i, y = HEADER_H + 2;

        add(h, L"STATIC", L"PART", SS_RIGHT, 96, y + 6, 104, 18, 0);
        s_kfoot = add(h, L"BUTTON", L"On foot", BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP, 210, y, 96, 28, ID_K_FOOT);
        add(h, L"BUTTON", L"Driving", BS_AUTORADIOBUTTON | BS_PUSHLIKE, 306, y, 96, 28, ID_K_DRIVE);
        SendMessageW(s_kfoot, BM_SETCHECK, BST_CHECKED, 0);
        y += 38;
        s_klist = add(h, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER
                      | WS_BORDER | WS_TABSTOP, 12, y, KEYS_W - 24, KEYS_LIST_H, ID_K_LIST);
        ListView_SetExtendedListViewStyle(s_klist, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        memset(&c, 0, sizeof c);
        c.mask = LVCF_TEXT | LVCF_WIDTH;
        for (i = 0; i < 5; i++) {
            c.pszText = (LPWSTR)head[i];
            c.cx = px(i ? 76 : 188);
            SendMessageW(s_klist, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
        }
        y += KEYS_LIST_H + 6;
        s_ktext = add(h, L"STATIC", s_khint, SS_LEFT | SS_ENDELLIPSIS, 14, y, KEYS_W - 28, 18, 0);
        y += 24;
        add(h, L"BUTTON", L"Bind", WS_TABSTOP, 12, y, 82, 28, ID_K_BIND);
        add(h, L"BUTTON", L"Clear", WS_TABSTOP, 98, y, 82, 28, ID_K_CLEAR);
        add(h, L"BUTTON", L"Default", WS_TABSTOP, 184, y, 82, 28, ID_K_DEFAULT);
        add(h, L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, KEYS_W - 216, y, 84, 28, IDOK);
        s_kcancel = add(h, L"BUTTON", L"Cancel", WS_TABSTOP, KEYS_W - 128, y, 116, 28, IDCANCEL);
        s_kpage = s_krow = s_kcol = 0;
        keys_fill();
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(h, &r);
        FillRect((HDC)w, &r, GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT r;
        HDC dc = BeginPaint(h, &ps);
        GetClientRect(h, &r);
        paint_header(dc, r.right, L"C O N T R O L S");
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)w, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_LBUTTONDOWN:
        if (s_kwait)
            keys_mouse("mouse1", l);
        return 0;
    case WM_RBUTTONDOWN:
        if (s_kwait)
            keys_mouse("mouse2", l);
        return 0;
    case WM_MBUTTONDOWN:
        if (s_kwait)
            keys_mouse("mouse3", l);
        return 0;
    case WM_XBUTTONDOWN:
        if (s_kwait)
            keys_mouse(HIWORD(w) == XBUTTON1 ? "mouse4" : "mouse5", l);
        return TRUE;
    case WM_MOUSEWHEEL:
        if (s_kwait)
            keys_take(GET_WHEEL_DELTA_WPARAM(w) > 0 ? "wheelup" : "wheeldown");
        return 0;
    case WM_CAPTURECHANGED:
        if (s_kwait && (HWND)l != h)
            keys_wait_end(s_khint);
        return 0;
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)l;
        if (n->hwndFrom != s_klist)
            break;
        switch (n->code) {
        case NM_CUSTOMDRAW:
            return keys_draw((NMLVCUSTOMDRAW *)l);
        case NM_CLICK:
        case NM_DBLCLK: {
            NMITEMACTIVATE *a = (NMITEMACTIVATE *)l;
            if (a->iItem >= 0) {
                s_krow = a->iItem;
                if (a->iSubItem > 0)
                    s_kcol = a->iSubItem - 1;
                s_kcol = keys_slot();
                if (n->code == NM_DBLCLK && a->iSubItem > 0)
                    PostMessageW(h, WM_KEYS_WAIT, 0, 0);
                InvalidateRect(s_klist, NULL, FALSE);
            }
            return 0;
        }
        case LVN_ITEMCHANGED: {
            NMLISTVIEW *v = (NMLISTVIEW *)l;
            if ((v->uNewState & LVIS_FOCUSED) && v->iItem >= 0 && v->iItem != s_krow) {
                s_krow = v->iItem;
                s_kcol = keys_slot();
                InvalidateRect(s_klist, NULL, FALSE);
            }
            return 0;
        }
        case LVN_KEYDOWN: {
            NMLVKEYDOWN *k = (NMLVKEYDOWN *)l;
            if (k->wVKey == VK_LEFT && s_kcol > 0)
                s_kcol--;
            else if (k->wVKey == VK_RIGHT && s_kcol + 1 < KEY_SLOTS)
                s_kcol++;
            else if (k->wVKey == VK_DELETE)
                keys_clear();
            s_kcol = keys_slot();
            InvalidateRect(s_klist, NULL, FALSE);
            return 0;
        }
        }
        break;
    }
    case WM_KEYS_WAIT:
        if (!s_kwait)
            keys_wait();
        return 0;
    case WM_COMMAND:
        if (s_kwait)
            return 0;
        switch (LOWORD(w)) {
        case ID_K_FOOT:
        case ID_K_DRIVE:
            s_kpage = LOWORD(w) == ID_K_DRIVE;
            s_krow = s_kcol = 0;
            keys_fill();
            return 0;
        case ID_K_BIND:
            SetFocus(s_klist);
            keys_wait();
            return 0;
        case ID_K_CLEAR:
            keys_clear();
            SetFocus(s_klist);
            return 0;
        case ID_K_DEFAULT:
            keys_default();
            return 0;
        case IDOK:
            if (keys_save()) {
                say(L"Controls saved. They apply from the next start of the game.");
                keys_close();
            } else {
                SetWindowTextW(s_ktext, L"mkb.ini could not be written.");
            }
            return 0;
        case IDCANCEL:
            keys_close();
            return 0;
        }
        break;
    case WM_CLOSE:
        keys_close();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void open_keys(void)
{
    RECT r, wr = { 0, 0, 0, 0 };
    MONITORINFO mi;
    DWORD st = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    char *txt = read_all(s_mkb);
    int x, y, ww, wh;

    if (!txt) {
        MessageBoxW(s_dlg, L"mkb.ini was not found.", L"Controls", MB_OK | MB_ICONERROR);
        return;
    }
    keys_read(txt, -1);
    free(txt);
    wr.right = px(KEYS_W);
    wr.bottom = px(HEADER_H + 2 + 38 + KEYS_LIST_H + 6 + 24 + 28 + 12);
    AdjustWindowRect(&wr, st, FALSE);
    ww = wr.right - wr.left;
    wh = wr.bottom - wr.top;
    GetWindowRect(s_win, &r);
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(s_win, MONITOR_DEFAULTTONEAREST), &mi);
    x = r.left + ((r.right - r.left) - ww) / 2;
    y = r.top;
    if (x + ww > mi.rcWork.right) x = mi.rcWork.right - ww;
    if (y + wh > mi.rcWork.bottom) y = mi.rcWork.bottom - wh;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;
    s_kdlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"NfKeys", L"Controls", st, x, y, ww, wh, s_dlg, NULL, s_inst, NULL);
    if (s_kdlg) {
        EnableWindow(s_dlg, FALSE);
        ShowWindow(s_kdlg, SW_SHOW);
    }
}

static LRESULT CALLBACK win_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE: {
        int y = HEADER_H + 2, pads[2];
        HWND c;

        s_win = h;
        add(h, L"STATIC", L"EDITION", SS_RIGHT, 36, y + 6, 104, 18, 0);
        s_vanilla = add(h, L"BUTTON", L"Vanilla", BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP, 150, y, 96, 28, ID_VANILLA);
        s_enhanced = add(h, L"BUTTON", L"Enhanced", BS_AUTORADIOBUTTON | BS_PUSHLIKE, 246, y, 96, 28, ID_ENHANCED);
        y += 38;
        s_play = add(h, L"BUTTON", L"Play Nightfire", BS_COMMANDLINK | WS_TABSTOP, 12, y, WIN_W - 24, 58, ID_PLAY);
        y += 62;
        s_settings = add(h, L"BUTTON", L"Settings", BS_COMMANDLINK | WS_TABSTOP, 12, y, WIN_W - 24, 58, ID_SETTINGS);
        y += 62;
        s_update = add(h, L"BUTTON", L"Check for updates", BS_COMMANDLINK | WS_TABSTOP, 12, y, WIN_W - 24, 58, ID_UPDATE);
        y += 68;
        s_text = add(h, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 14, y, WIN_W - 28, 18, 0);
        y += 22;
        s_bar = CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
                                px(14), px(y), px(WIN_W - 28), px(16), h, NULL, s_inst, NULL);
        SendMessageW(s_bar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        s_status = c = CreateWindowExW(0, STATUSCLASSNAMEW, NULL, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, h, NULL, s_inst, NULL);
        SendMessageW(c, WM_SETFONT, (WPARAM)s_font, TRUE);
        pads[0] = px(WIN_W - 100);
        pads[1] = -1;
        SendMessageW(c, SB_SETPARTS, 2, (LPARAM)pads);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT r;
        GetClientRect(h, &r);
        FillRect((HDC)w, &r, GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT r;
        HDC dc = BeginPaint(h, &ps);
        GetClientRect(h, &r);
        paint_header(dc, r.right, L"S T A T I C   R E C O M P");
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)w, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_VANILLA:
        case ID_ENHANCED:
            save_mode();
            update_ui();
            return 0;
        case ID_PLAY:
            play();
            return 0;
        case ID_SETTINGS:
            open_settings();
            return 0;
        case ID_UPDATE:
            if (s_pending > 0) {
                if (MessageBoxW(h, L"Update and rebuild now? The game is not available until it is done, "
                                   L"and it can take a while.", L"Update", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
                    start_job(JOB_UPDATE);
            } else {
                start_job(JOB_CHECK);
            }
            return 0;
        }
        break;
    case WM_TIMER: {
        int r = game_running();
        /* The game is up: the launcher's work is done. If it never comes up,
         * the launcher stays to say so. */
        if (s_launching) {
            if (r) {
                DestroyWindow(h);
            } else if (GetTickCount() - s_launching > LAUNCH_WAIT_MS) {
                s_launching = 0;
                say(L"The game did not start. Details in logs\\stderr_last.txt.");
                update_ui();
            }
            return 0;
        }
        /* nightfire.exe ends at the switch to the driving image, the game does not. */
        if (r != s_running) {
            s_running = r;
            if (!r)
                say(L"Game closed.");
            update_ui();
        }
        return 0;
    }
    case WM_JOB_LINE:
        say((const wchar_t *)l);
        free((void *)l);
        return 0;
    case WM_JOB_STEP:
        SendMessageW(s_bar, PBM_SETPOS, w, 0);
        return 0;
    case WM_JOB_DONE:
        job_done((int)w, (long)l);
        return 0;
    case WM_CLOSE:
        if (s_busy && MessageBoxW(h, L"The update is still running. Close anyway?", L"Update",
                                  MB_YESNO | MB_ICONWARNING) != IDYES)
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static HFONT font(int size, int weight, const wchar_t *face)
{
    return CreateFontW(-px(size), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_PROGRESS_CLASS | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES
                                           | ICC_LISTVIEW_CLASSES };
    RECT wr = { 0, 0, 0, 0 };
    DWORD st = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    wchar_t mode[16], old[MAX_PATH], self[MAX_PATH];
    HANDLE one;
    MSG msg;
    HDC dc;

    (void)prev; (void)cmd;
    s_inst = inst;
    one = CreateMutexW(NULL, FALSE, L"NightfireLauncher");
    if (one && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"NfLauncher", NULL);
        if (other)
            SetForegroundWindow(other);
        return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    InitCommonControlsEx(&ic);
    if (!find_root()) {
        MessageBoxW(NULL, L"The project folder (with scripts\\build.ps1) was not found.", L"Nightfire", MB_OK | MB_ICONERROR);
        return 1;
    }
    GetModuleFileNameW(NULL, self, MAX_PATH);
    swprintf(old, MAX_PATH, L"%s.old", self);
    DeleteFileW(old);

    dc = GetDC(NULL);
    s_dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    s_font = font(12, FW_NORMAL, L"Segoe UI");
    s_big = font(34, FW_BOLD, L"Segoe UI");
    s_small = font(11, FW_SEMIBOLD, L"Segoe UI");

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = win_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));     /* assets/nightfire.ico */
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"NfLauncher";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = dlg_proc;
    wc.lpszClassName = L"NfSettings";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = keys_proc;
    wc.lpszClassName = L"NfKeys";
    RegisterClassExW(&wc);

    wr.right = px(WIN_W);
    wr.bottom = px(HEADER_H + 2 + 38 + 62 + 62 + 68 + 22 + 16 + 34);
    AdjustWindowRect(&wr, st, FALSE);
    CreateWindowExW(0, L"NfLauncher", L"Nightfire Launcher", st, CW_USEDEFAULT, CW_USEDEFAULT,
                    wr.right - wr.left, wr.bottom - wr.top, NULL, NULL, inst, NULL);
    if (!s_win)
        return 1;

    ini_get(ini_index(L"Mode"), mode, 16);
    if (!mode[0])
        wcscpy(mode, L"original");
    SendMessageW(_wcsicmp(mode, L"enhanced") ? s_vanilla : s_enhanced, BM_SETCHECK, BST_CHECKED, 0);
    s_running = game_running();
    update_ui();
    ShowWindow(s_win, show);
    SetTimer(s_win, ID_TIMER, 1000, NULL);
    start_job(JOB_VERSION);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (keys_pretranslate(&msg))
            continue;
        if (s_kdlg ? IsDialogMessageW(s_kdlg, &msg) : s_dlg && IsDialogMessageW(s_dlg, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
