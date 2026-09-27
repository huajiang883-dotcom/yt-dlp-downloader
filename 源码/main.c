/* ============================================================================
 * yt-dlp 视频下载器 v2  (Win32 GUI, C)
 *
 * 功能：
 *   - 打开程序 → 粘贴视频链接 → 选文件夹 → 一键下载（强制最高画质）
 *   - 窗口置顶（钉在屏幕上）、窗口可自由拉伸/最大化
 *   - 全局快捷键 Ctrl+Shift+Y：任何程序里按下 → 自动抓剪贴板链接并开始下载
 *   - 一键检查更新 yt-dlp.exe / ffmpeg.exe（可设启动时自动检查）
 *   - 无 ffmpeg 时自动引导下载安装（最高画质必需），绝不降级画质
 *   - 便携模式：优先使用程序同目录的 yt-dlp.exe / ffmpeg.exe
 *   - 自动查找 yt-dlp（配置 → 程序同目录 → PATH → D:\ → Python Scripts → 手动指定）
 *   - 配置保存在 %APPDATA%\YTDownloader\config.ini
 *
 * 交叉编译 (Linux, llvm-mingw)：
 *   x86_64-w64-mingw32-windres app.rc -O coff -o app.res
 *   x86_64-w64-mingw32-clang -O2 -municode -mwindows main.c app.res \
 *       -o yt-dlp视频下载器.exe -luser32 -lgdi32 -lshell32 -lole32 -luuid \
 *       -lcomctl32 -lwininet
 * ========================================================================== */
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <wininet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ziputil.h"

#define IDC_LBL_URL   1001
#define IDC_URL       1002
#define IDC_LBL_DIR   1003
#define IDC_DIR       1004
#define IDC_BROWSE    1005
#define IDC_CK_PL     1006
#define IDC_DL        1007
#define IDC_STOP      1008
#define IDC_OPEN      1009
#define IDC_LBL_LOG   1010
#define IDC_LOG       1011
#define IDC_CK_TOP    1012
#define IDC_CK_UPD    1013
#define IDC_UPDATE    1014

#define HOTKEY_ID           1      /* Ctrl+Shift+Y */
#define UPDATE_URL_YTDLP_API L"https://api.github.com/repos/yt-dlp/yt-dlp/releases/latest"
#define UPDATE_URL_YTDLP_PYPI L"https://pypi.org/pypi/yt-dlp/json"
#define UPDATE_URL_FFMPEG_ZIP L"https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip"

#define WM_APP_LOG      (WM_APP + 1)   /* wParam = malloc 的 WCHAR 块 */
#define WM_APP_DONE     (WM_APP + 2)   /* wParam = 下载子进程退出代码 */
#define WM_APP_ASKYT    (WM_APP + 3)   /* 启动后提示查找/选择 yt-dlp */
#define WM_APP_UPDONE   (WM_APP + 4)   /* 更新检查结束 */
#define WM_APP_FFRESULT (WM_APP + 5)   /* wParam=1: ffmpeg 安装成功 */
#define WM_APP_AUTOCHK  (WM_APP + 6)   /* 启动时自动检查更新 */

#define APP_TITLE_U8    "yt-dlp 视频下载器"

static HINSTANCE g_hInst;
static HFONT     g_font;
static float     g_scale = 1.0f;
static WNDPROC   g_oldUrlProc;

#define SX(x) ((int)((x) * g_scale + 0.5f))

typedef struct {
    HWND hwnd;
    HWND hUrl, hDir, hBrowse, hCkPl, hCkTop, hCkUpd, hDl, hStop, hOpen, hUpdate, hLog;
    WCHAR ytdlp[MAX_PATH + 64];
    WCHAR ini[MAX_PATH + 64];
    WCHAR startupUrl[4096];
    HANDLE hProcess;
    volatile LONG running;
    volatile LONG stopped;
    volatile LONG updating;
} AppState;

static AppState g;

/* ------------------------------------------------------------------ */
/* 文件/路径工具                                                       */
/* ------------------------------------------------------------------ */
/* 非空文件（空文件视为无效，避免残留 0 字节文件被当作可执行程序） */
static BOOL FileNonEmpty(const WCHAR *p)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &d))
        return FALSE;
    if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return FALSE;
    return d.nFileSizeHigh != 0 || d.nFileSizeLow != 0;
}

static void PathJoin(WCHAR *dst, size_t cap, const WCHAR *dir, const WCHAR *name)
{
    _snwprintf(dst, cap, L"%s\\%s", dir, name);
}

static void GetProgDir(WCHAR *out, DWORD cap)
{
    GetModuleFileNameW(NULL, out, cap);
    WCHAR *s = wcsrchr(out, L'\\');
    if (s) *s = 0;
}

/* ------------------------------------------------------------------ */
/* 日志：UI 线程 Log()，工作线程 LogT()（PostMessage 投递）            */
/* ------------------------------------------------------------------ */
static void Log(const WCHAR *fmt, ...)
{
    WCHAR msg[1200];
    WCHAR line[1280];
    va_list va;
    SYSTEMTIME st;
    int n;

    va_start(va, fmt);
    n = _vsnwprintf(msg, 1199, fmt, va);
    va_end(va);
    if (n < 0) n = 1199;
    msg[1199] = 0;
    while (n > 0 && (msg[n - 1] == L'\r' || msg[n - 1] == L'\n'))
        msg[--n] = 0;

    GetLocalTime(&st);
    _snwprintf(line, 1279, L"[%02d:%02d:%02d] %s\r\n", st.wHour, st.wMinute, st.wSecond, msg);
    line[1279] = 0;

    if (!g.hLog)
        return;
    {
        int cur = GetWindowTextLengthW(g.hLog);
        if (cur > 400000)
            SetWindowTextW(g.hLog, L"");
        cur = GetWindowTextLengthW(g.hLog);
        SendMessageW(g.hLog, EM_SETSEL, (WPARAM)cur, (LPARAM)cur);
        SendMessageW(g.hLog, EM_REPLACESEL, FALSE, (LPARAM)line);
        SendMessageW(g.hLog, EM_SCROLLCARET, 0, 0);
    }
}

static void PostBlock(const WCHAR *src, size_t n)
{
    WCHAR *block;
    if (!n)
        return;
    block = (WCHAR *)malloc((n + 1) * sizeof(WCHAR));
    if (!block)
        return;
    memcpy(block, src, n * sizeof(WCHAR));
    block[n] = 0;
    if (!PostMessageW(g.hwnd, WM_APP_LOG, (WPARAM)block, 0))
        free(block);
}

static void LogT(const WCHAR *fmt, ...)
{
    WCHAR msg[1200];
    WCHAR line[1280];
    va_list va;
    SYSTEMTIME st;
    int n;

    va_start(va, fmt);
    n = _vsnwprintf(msg, 1199, fmt, va);
    va_end(va);
    if (n < 0) n = 1199;
    msg[1199] = 0;
    while (n > 0 && (msg[n - 1] == L'\r' || msg[n - 1] == L'\n'))
        msg[--n] = 0;

    GetLocalTime(&st);
    _snwprintf(line, 1279, L"[%02d:%02d:%02d] %s\r\n", st.wHour, st.wMinute, st.wSecond, msg);
    line[1279] = 0;
    PostBlock(line, wcslen(line));
}

/* ------------------------------------------------------------------ */
/* 配置读写：%APPDATA%\YTDownloader\config.ini                        */
/* ------------------------------------------------------------------ */
static void ConfigInit(void)
{
    WCHAR appdata[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (appdata[0]) {
        WCHAR dir[MAX_PATH + 64];
        _snwprintf(dir, MAX_PATH + 64, L"%s\\YTDownloader", appdata);
        SHCreateDirectoryExW(NULL, dir, NULL);
        _snwprintf(g.ini, MAX_PATH + 64, L"%s\\config.ini", dir);
    } else {
        WCHAR exe[MAX_PATH];
        GetProgDir(exe, MAX_PATH);
        _snwprintf(g.ini, MAX_PATH + 64, L"%s\\YTDownloader-config.ini", exe);
    }
}

static void ConfigLoad(void)
{
    WCHAR buf[MAX_PATH + 64];
    int top, upd, pl;

    if (!g.ini[0])
        return;
    if (GetPrivateProfileStringW(L"Settings", L"OutDir", L"", buf, MAX_PATH + 64, g.ini) && buf[0]) {
        SetWindowTextW(g.hDir, buf);
    } else {
        WCHAR prof[MAX_PATH];
        if (GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH)) {
            PathJoin(buf, MAX_PATH + 64, prof, L"Downloads");
            SetWindowTextW(g.hDir, buf);
        }
    }
    GetPrivateProfileStringW(L"Settings", L"YtDlp", L"", g.ytdlp, MAX_PATH + 64, g.ini);
    pl = GetPrivateProfileIntW(L"Settings", L"Playlist", 0, g.ini);
    top = GetPrivateProfileIntW(L"Settings", L"Topmost", 0, g.ini);
    upd = GetPrivateProfileIntW(L"Settings", L"AutoUpdate", 0, g.ini);
    SendMessageW(g.hCkPl, BM_SETCHECK, pl ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g.hCkTop, BM_SETCHECK, top ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g.hCkUpd, BM_SETCHECK, upd ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void ConfigSave(void)
{
    WCHAR buf[MAX_PATH + 64];
    WCHAR num[4];
    if (!g.ini[0])
        return;
    GetWindowTextW(g.hDir, buf, MAX_PATH + 64);
    WritePrivateProfileStringW(L"Settings", L"OutDir", buf, g.ini);
    WritePrivateProfileStringW(L"Settings", L"YtDlp", g.ytdlp[0] ? g.ytdlp : NULL, g.ini);
    wsprintfW(num, L"%d", SendMessageW(g.hCkPl, BM_GETCHECK, 0, 0) == BST_CHECKED);
    WritePrivateProfileStringW(L"Settings", L"Playlist", num, g.ini);
    wsprintfW(num, L"%d", SendMessageW(g.hCkTop, BM_GETCHECK, 0, 0) == BST_CHECKED);
    WritePrivateProfileStringW(L"Settings", L"Topmost", num, g.ini);
    wsprintfW(num, L"%d", SendMessageW(g.hCkUpd, BM_GETCHECK, 0, 0) == BST_CHECKED);
    WritePrivateProfileStringW(L"Settings", L"AutoUpdate", num, g.ini);
}

/* ------------------------------------------------------------------ */
/* 查找 yt-dlp.exe / ffmpeg.exe                                        */
/* ------------------------------------------------------------------ */
static BOOL TryYtdlp(const WCHAR *p)
{
    if (p && p[0] && FileNonEmpty(p)) {
        wcsncpy(g.ytdlp, p, MAX_PATH + 63);
        g.ytdlp[MAX_PATH + 63] = 0;
        return TRUE;
    }
    return FALSE;
}

static BOOL ResolveYtdlp(void)
{
    WCHAR tmp[MAX_PATH + 64];
    WCHAR exe[MAX_PATH];

    if (g.ytdlp[0] && FileNonEmpty(g.ytdlp))
        return TRUE;
    g.ytdlp[0] = 0;

    /* 1. 程序同目录（便携模式优先） */
    GetProgDir(exe, MAX_PATH);
    PathJoin(tmp, MAX_PATH + 64, exe, L"yt-dlp.exe");
    if (TryYtdlp(tmp))
        return TRUE;

    /* 2. 系统 PATH（含 C:\Windows\System32） */
    if (SearchPathW(NULL, L"yt-dlp.exe", NULL, MAX_PATH + 64, tmp, NULL) &&
        TryYtdlp(tmp))
        return TRUE;

    /* 3. D 盘根目录 */
    if (TryYtdlp(L"D:\\yt-dlp.exe"))
        return TRUE;

    /* 4. Python Scripts 目录（pip 安装方式） */
    {
        static const WCHAR *specs[][2] = {
            { L"LOCALAPPDATA", L"%s\\Programs\\Python\\Python*" },
            { L"APPDATA",       L"%s\\Python\\Python*" },
        };
        int i;
        for (i = 0; i < 2; i++) {
            WCHAR base[MAX_PATH], pattern[MAX_PATH + 64];
            WIN32_FIND_DATAW fd;
            HANDLE hf;
            DWORD bl = GetEnvironmentVariableW(specs[i][0], base, MAX_PATH);
            if (!bl || bl >= MAX_PATH)
                continue;
            _snwprintf(pattern, MAX_PATH + 64, specs[i][1], base);
            hf = FindFirstFileW(pattern, &fd);
            if (hf == INVALID_HANDLE_VALUE)
                continue;
            do {
                WCHAR cand[MAX_PATH + 96], root[MAX_PATH + 64];
                WCHAR *star;
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    continue;
                if (wcsncmp(fd.cFileName, L"Python", 6) != 0)
                    continue;
                _snwprintf(root, MAX_PATH + 64, specs[i][1], base);
                star = wcsrchr(root, L'*');
                if (star) *star = 0;
                _snwprintf(cand, MAX_PATH + 96, L"%s%s\\Scripts\\yt-dlp.exe", root, fd.cFileName);
                if (TryYtdlp(cand)) {
                    FindClose(hf);
                    return TRUE;
                }
            } while (FindNextFileW(hf, &fd));
            FindClose(hf);
        }
    }
    return FALSE;
}

/* 手动选择 yt-dlp.exe */
static BOOL AskLocateYtdlp(HWND owner)
{
    IFileDialog *pfd = NULL;
    HRESULT hr;

    MessageBoxW(owner,
        L"没有找到 yt-dlp.exe。\n\n"
        L"点击确定后，请手动选择 yt-dlp.exe 所在位置。\n\n"
        L"（如果还没安装 yt-dlp，可以点「检查更新」按钮自动下载，\n"
        L"或到 https://github.com/yt-dlp/yt-dlp/releases 手动下载）",
        L"提示", MB_OK | MB_ICONINFORMATION);

    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IFileDialog, (void **)&pfd);
    if (FAILED(hr))
        return FALSE;

    {
        COMDLG_FILTERSPEC fs[1] = { { L"yt-dlp 程序", L"yt-dlp.exe" } };
        pfd->lpVtbl->SetFileTypes(pfd, 1, fs);
        pfd->lpVtbl->SetFileTypeIndex(pfd, 1);
        pfd->lpVtbl->SetOptions(pfd, FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
        pfd->lpVtbl->SetTitle(pfd, L"选择 yt-dlp.exe");
    }
    if (SUCCEEDED(pfd->lpVtbl->Show(pfd, owner))) {
        IShellItem *psi = NULL;
        if (SUCCEEDED(pfd->lpVtbl->GetResult(pfd, &psi))) {
            PWSTR path = NULL;
            if (SUCCEEDED(psi->lpVtbl->GetDisplayName(psi, SIGDN_FILESYSPATH, &path))) {
                wcsncpy(g.ytdlp, path, MAX_PATH + 63);
                g.ytdlp[MAX_PATH + 63] = 0;
                CoTaskMemFree(path);
            }
            psi->lpVtbl->Release(psi);
        }
    }
    pfd->lpVtbl->Release(pfd);

    if (g.ytdlp[0] && FileNonEmpty(g.ytdlp)) {
        ConfigSave();
        Log(L"[信息] 已设置 yt-dlp: %s", g.ytdlp);
        return TRUE;
    }
    return FALSE;
}

/* 查找 ffmpeg（只做存在性检测，不运行它）：程序同目录 → PATH → yt-dlp 同目录 → D:\ */
static BOOL FindFfmpeg(WCHAR *out, size_t cap)
{
    WCHAR tmp[MAX_PATH + 64];
    WCHAR exe[MAX_PATH];

    GetProgDir(exe, MAX_PATH);
    PathJoin(tmp, MAX_PATH + 64, exe, L"ffmpeg.exe");
    if (FileNonEmpty(tmp)) {
        wcsncpy(out, tmp, cap - 1);
        out[cap - 1] = 0;
        return TRUE;
    }
    if (SearchPathW(NULL, L"ffmpeg.exe", NULL, MAX_PATH + 64, tmp, NULL) &&
        FileNonEmpty(tmp)) {
        wcsncpy(out, tmp, cap - 1);
        out[cap - 1] = 0;
        return TRUE;
    }
    if (g.ytdlp[0]) {
        WCHAR *s;
        wcsncpy(exe, g.ytdlp, MAX_PATH - 1);
        exe[MAX_PATH - 1] = 0;
        s = wcsrchr(exe, L'\\');
        if (s) *s = 0;
        PathJoin(tmp, MAX_PATH + 64, exe, L"ffmpeg.exe");
        if (FileNonEmpty(tmp)) {
            wcsncpy(out, tmp, cap - 1);
            out[cap - 1] = 0;
            return TRUE;
        }
    }
    if (FileNonEmpty(L"D:\\ffmpeg.exe")) {
        wcsncpy(out, L"D:\\ffmpeg.exe", cap - 1);
        out[cap - 1] = 0;
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* 布局（窗口可拉伸）                                                  */
/* ------------------------------------------------------------------ */
static void LayoutControls(int cw, int ch)
{
    int logH;
    if (!g.hUrl)
        return;
    MoveWindow(g.hUrl, SX(12), SX(30), cw - SX(24), SX(56), TRUE);
    MoveWindow(g.hDir, SX(12), SX(116), cw - SX(24) - SX(100), SX(24), TRUE);
    MoveWindow(g.hBrowse, cw - SX(104), SX(114), SX(92), SX(28), TRUE);
    logH = ch - SX(252);
    if (logH < SX(60))
        logH = SX(60);
    MoveWindow(g.hLog, SX(12), SX(240), cw - SX(24), logH, TRUE);
}

/* ------------------------------------------------------------------ */
/* 窗口置顶                                                            */
/* ------------------------------------------------------------------ */
static void ApplyTopmost(void)
{
    BOOL on = SendMessageW(g.hCkTop, BM_GETCHECK, 0, 0) == BST_CHECKED;
    SetWindowPos(g.hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

/* ------------------------------------------------------------------ */
/* 全局快捷键：剪贴板                                                  */
/* ------------------------------------------------------------------ */
static BOOL GetClipboardUrl(WCHAR *out, DWORD cap)
{
    BOOL ok = FALSE;
    if (!OpenClipboard(NULL))
        return FALSE;
    {
        HANDLE h = GetClipboardData(CF_UNICODETEXT);
        if (h) {
            const WCHAR *s = (const WCHAR *)GlobalLock(h);
            if (s) {
                const WCHAR *p = s;
                WCHAR *e;
                BOOL hasScheme = FALSE;
                while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n' ||
                       *p == L'"' || *p == L'\'')
                    p++;
                if (wcsncmp(p, L"http://", 7) == 0 || wcsncmp(p, L"https://", 8) == 0)
                    hasScheme = TRUE;
                if ((hasScheme || wcsncmp(p, L"www.", 4) == 0) && wcslen(p) < cap - 8) {
                    if (hasScheme)
                        wcsncpy(out, p, cap - 1);
                    else
                        _snwprintf(out, cap, L"https://%s", p);
                    out[cap - 1] = 0;
                    e = out;
                    while (*e && *e != L'\r' && *e != L'\n')
                        e++;
                    while (e > out && (e[-1] == L' ' || e[-1] == L'\t'))
                        e--;
                    *e = 0;
                    ok = TRUE;
                }
                GlobalUnlock(h);
            }
        }
    }
    CloseClipboard();
    return ok;
}

/* ------------------------------------------------------------------ */
/* 文件夹选择 / 打开文件夹                                             */
/* ------------------------------------------------------------------ */
static void BrowseFolder(void)
{
    IFileDialog *pfd = NULL;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IFileDialog, (void **)&pfd);
    if (SUCCEEDED(hr)) {
        pfd->lpVtbl->SetOptions(pfd, FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        pfd->lpVtbl->SetTitle(pfd, L"选择保存文件夹");
        if (SUCCEEDED(pfd->lpVtbl->Show(pfd, g.hwnd))) {
            IShellItem *psi = NULL;
            if (SUCCEEDED(pfd->lpVtbl->GetResult(pfd, &psi))) {
                PWSTR path = NULL;
                if (SUCCEEDED(psi->lpVtbl->GetDisplayName(psi, SIGDN_FILESYSPATH, &path))) {
                    SetWindowTextW(g.hDir, path);
                    CoTaskMemFree(path);
                    ConfigSave();
                }
                psi->lpVtbl->Release(psi);
            }
        }
        pfd->lpVtbl->Release(pfd);
    } else {
        BROWSEINFOW bi;
        WCHAR path[MAX_PATH] = L"";
        LPITEMIDLIST pidl;
        ZeroMemory(&bi, sizeof(bi));
        bi.hwndOwner = g.hwnd;
        bi.pszDisplayName = path;
        bi.lpszTitle = L"选择保存文件夹";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        pidl = SHBrowseForFolderW(&bi);
        if (pidl) {
            if (SHGetPathFromIDListW(pidl, path)) {
                SetWindowTextW(g.hDir, path);
                ConfigSave();
            }
            CoTaskMemFree(pidl);
        }
    }
}

static void OpenFolder(void)
{
    WCHAR dir[MAX_PATH + 64];
    if (!GetWindowTextW(g.hDir, dir, MAX_PATH + 64) || !dir[0]) {
        MessageBoxW(g.hwnd, L"还没有设置保存位置，请先点「选择文件夹…」", L"提示",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    SHCreateDirectoryExW(NULL, dir, NULL);
    if ((INT_PTR)ShellExecuteW(g.hwnd, L"open", dir, NULL, NULL, SW_SHOWNORMAL) <= 32)
        MessageBoxW(g.hwnd, L"打开文件夹失败", L"提示", MB_OK | MB_ICONINFORMATION);
}

/* ------------------------------------------------------------------ */
/* 进程运行工具（同步、捕获输出；用于 --version / PowerShell）         */
/* ------------------------------------------------------------------ */
static BOOL RunCapture(const WCHAR *exe, const WCHAR *args, WCHAR *out, DWORD outCap,
                       DWORD *pExitCode, const WCHAR *cwd)
{
    WCHAR *cmd;
    size_t cap;
    SECURITY_ATTRIBUTES sa;
    HANDLE hRead = NULL, hWrite = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    BYTE buf[8192];
    DWORD n, code = 0;
    size_t outLen = 0;
    BOOL ok = FALSE;

    cap = wcslen(exe) + wcslen(args) + 16;
    cmd = (WCHAR *)malloc(cap * sizeof(WCHAR));
    if (!cmd)
        return FALSE;
    _snwprintf(cmd, cap, L"\"%s\" %s", exe, args);

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        free(cmd);
        return FALSE;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;

    ZeroMemory(&pi, sizeof(pi));
    if (CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, cwd, &si, &pi)) {
        CloseHandle(hWrite);
        hWrite = NULL;
        if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_TIMEOUT)
            TerminateProcess(pi.hProcess, 1);
        while (ReadFile(hRead, buf, sizeof(buf), &n, NULL) && n > 0) {
            int r = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (LPCCH)buf, n, NULL, 0);
            UINT cp = CP_UTF8;
            if (r <= 0) {
                cp = CP_ACP;
                r = MultiByteToWideChar(CP_ACP, 0, (LPCCH)buf, n, NULL, 0);
            }
            if (r > 0 && out && outLen + (size_t)r + 1 < outCap) {
                MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0,
                                    (LPCCH)buf, n, out + outLen, r);
                outLen += r;
            }
        }
        GetExitCodeProcess(pi.hProcess, &code);
        if (pExitCode)
            *pExitCode = code;
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        ok = TRUE;
    }
    if (hWrite)
        CloseHandle(hWrite);
    CloseHandle(hRead);
    if (out && outLen < outCap)
        out[outLen] = 0;
    free(cmd);
    return ok;
}

/* ------------------------------------------------------------------ */
/* 网络工具（WinINet）：GET 文本 / 响应头 / 下载文件                   */
/* ------------------------------------------------------------------ */
static HINTERNET NetOpen(DWORD *errOut)
{
    HINTERNET hi = InternetOpenW(L"ytdlp-updater/2.0", INTERNET_OPEN_TYPE_PRECONFIG,
                                 NULL, NULL, 0);
    if (!hi && errOut)
        *errOut = GetLastError();
    if (hi) {
        DWORD t = 20000;
        InternetSetOptionW(hi, INTERNET_OPTION_CONNECT_TIMEOUT, &t, sizeof(t));
        t = 60000;
        InternetSetOptionW(hi, INTERNET_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));
    }
    return hi;
}

static char *HttpGetBody(const WCHAR *url, DWORD *lenOut)
{
    HINTERNET hi, hu;
    char *buf = NULL;
    size_t cap = 65536, len = 0;
    BYTE tmp[8192];
    DWORD n;

    hi = NetOpen(NULL);
    if (!hi)
        return NULL;
    hu = InternetOpenUrlW(hi, url, NULL, 0,
                          INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hu) {
        InternetCloseHandle(hi);
        return NULL;
    }
    buf = (char *)malloc(cap);
    while (buf && InternetReadFile(hu, tmp, sizeof(tmp), &n) && n > 0) {
        if (len + n + 1 > cap) {
            cap = (len + n) * 2;
            buf = (char *)realloc(buf, cap);
            if (!buf)
                break;
        }
        memcpy(buf + len, tmp, n);
        len += n;
        if (len > 8 * 1024 * 1024)
            break;
    }
    if (buf)
        buf[len] = 0;
    if (lenOut)
        *lenOut = (DWORD)len;
    InternetCloseHandle(hu);
    InternetCloseHandle(hi);
    return buf;
}

static BOOL HttpGetHeader(const WCHAR *url, DWORD dwHeader, WCHAR *buf, DWORD cap)
{
    HINTERNET hi, hu;
    BOOL ok = FALSE;
    DWORD len = cap;

    hi = NetOpen(NULL);
    if (!hi)
        return FALSE;
    hu = InternetOpenUrlW(hi, url, NULL, 0,
                          INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (hu) {
        if (HttpQueryInfoW(hu, dwHeader, buf, &len, NULL))
            ok = TRUE;
        InternetCloseHandle(hu);
    }
    InternetCloseHandle(hi);
    return ok;
}

static BOOL DownloadToFile(const WCHAR *url, const WCHAR *dest)
{
    HINTERNET hi, hu;
    HANDLE f;
    BYTE tmp[65536];
    DWORD n, total = 0, lastLog = 0, expect = 0, clen = sizeof(expect);
    BOOL ok = TRUE;

    hi = NetOpen(NULL);
    if (!hi)
        return FALSE;
    hu = InternetOpenUrlW(hi, url, NULL, 0,
                          INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hu) {
        InternetCloseHandle(hi);
        return FALSE;
    }
    HttpQueryInfoW(hu, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER,
                   &expect, &clen, NULL);
    f = CreateFileW(dest, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        InternetCloseHandle(hu);
        InternetCloseHandle(hi);
        return FALSE;
    }
    while (InternetReadFile(hu, tmp, sizeof(tmp), &n) && n > 0) {
        DWORD w = 0;
        if (!WriteFile(f, tmp, n, &w, NULL) || w != n) {
            ok = FALSE;
            break;
        }
        total += n;
        if (total / (4 * 1024 * 1024) != lastLog / (4 * 1024 * 1024)) {
            LogT(L"[更新] 已下载 %lu MB", (unsigned long)(total / (1024 * 1024)));
            lastLog = total;
        }
    }
    CloseHandle(f);
    InternetCloseHandle(hu);
    InternetCloseHandle(hi);
    if (!ok || total == 0 || (expect && total != expect)) {
        DeleteFileW(dest);
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* JSON 小工具（避免引入依赖）                                         */
/* ------------------------------------------------------------------ */
static BOOL JsonGetTag(const char *json, WCHAR *out, DWORD cap)
{
    const char *p = strstr(json, "\"tag_name\"");
    const char *e;
    int r;
    if (!p)
        return FALSE;
    p += 10;                     /* 跳过 "tag_name" */
    while (*p && (*p == ':' || *p == ' ' || *p == '\t'))
        p++;
    if (*p != '"')
        return FALSE;
    p++;
    e = strchr(p, '"');
    if (!e)
        return FALSE;
    r = MultiByteToWideChar(CP_UTF8, 0, p, (int)(e - p), out, cap);
    if (r <= 0)
        return FALSE;
    out[r] = 0;
    return TRUE;
}

static BOOL JsonGetAsset(const char *json, const WCHAR *nameEnd, WCHAR *out, DWORD cap)
{
    /* 找所有 "browser_download_url":"..."，返回以 nameEnd 结尾的那个 */
    const char *p = json;
    while ((p = strstr(p, "\"browser_download_url\"")) != NULL) {
        const char *u, *e;
        WCHAR tmp[1200];
        int r;
        p += 23;                 /* 跳过 "browser_download_url" */
        while (*p && (*p == ':' || *p == ' ' || *p == '\t'))
            p++;
        if (*p != '"')
            continue;
        u = ++p;
        e = strchr(u, '"');
        if (!e)
            return FALSE;
        if ((size_t)(e - u) < 1199) {
            r = MultiByteToWideChar(CP_UTF8, 0, u, (int)(e - u), tmp, 1199);
            if (r > 0) {
                tmp[r] = 0;
                if (r >= (int)wcslen(nameEnd) &&
                    _wcsicmp(tmp + r - wcslen(nameEnd), nameEnd) == 0) {
                    wcsncpy(out, tmp, cap - 1);
                    out[cap - 1] = 0;
                    return TRUE;
                }
            }
        }
        p = e;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* 更新逻辑（在工作线程中运行）                                        */
/* ------------------------------------------------------------------ */
/* 版本号逐段数字比较（忽略前导零：2026.08.19 == 2026.8.19） */
static int VersionCmp(const WCHAR *a, const WCHAR *b)
{
    for (;;) {
        DWORD va = 0, vb = 0;
        while (*a >= L'0' && *a <= L'9')
            va = va * 10 + (DWORD)(*a++ - L'0');
        while (*b >= L'0' && *b <= L'9')
            vb = vb * 10 + (DWORD)(*b++ - L'0');
        if (va != vb)
            return va < vb ? -1 : 1;
        if (!*a && !*b)
            return 0;
        if (*a == L'.')
            a++;
        if (*b == L'.')
            b++;
    }
}

/* 解析 {"version":"..."} 字段（PyPI JSON 用） */
static BOOL JsonGetVersion(const char *json, WCHAR *out, DWORD cap)
{
    const char *p = strstr(json, "\"version\"");
    const char *e;
    int r;
    if (!p)
        return FALSE;
    p += 9;                      /* 跳过 "version" */
    while (*p && (*p == ':' || *p == ' ' || *p == '\t'))
        p++;
    if (*p != '"')
        return FALSE;
    p++;
    e = strchr(p, '"');
    if (!e)
        return FALSE;
    r = MultiByteToWideChar(CP_UTF8, 0, p, (int)(e - p), out, cap);
    if (r <= 0)
        return FALSE;
    out[r] = 0;
    return TRUE;
}

static void UpdateYtdlp(void)
{
    WCHAR progDir[MAX_PATH], target[MAX_PATH + 64], tmpTarget[MAX_PATH + 64];
    WCHAR localVer[256] = L"", remoteVer[256] = L"", assetUrl[1200] = L"";
    WCHAR apiUrl[1200];
    WCHAR out[512];
    DWORD code = 0;
    char *json;
    BOOL haveLocal;

    GetProgDir(progDir, MAX_PATH);
    PathJoin(target, MAX_PATH + 64, progDir, L"yt-dlp.exe");
    GetPrivateProfileStringW(L"Settings", L"YtDlpApiUrl", UPDATE_URL_YTDLP_API,
                             apiUrl, 1200, g.ini);

    haveLocal = g.ytdlp[0] && FileNonEmpty(g.ytdlp) &&
                RunCapture(g.ytdlp, L"--version", out, 512, &code, NULL);
    if (haveLocal) {
        WCHAR *p = out;
        while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n')
            p++;
        wcsncpy(localVer, p, 255);
        localVer[255] = 0;
        {
            WCHAR *e = localVer;
            while (*e && *e != L'\r' && *e != L'\n' && *e != L' ')
                e++;
            *e = 0;
        }
    }

    json = HttpGetBody(apiUrl, NULL);
    if (!json) {
        /* GitHub 不可达 → PyPI 兜底（仅查版本；国内网络 PyPI 通常可达） */
        json = HttpGetBody(UPDATE_URL_YTDLP_PYPI, NULL);
        if (!json) {
            LogT(L"[更新] yt-dlp 版本查询失败（GitHub 与 PyPI 均不可达，请检查网络）");
            return;
        }
        if (!JsonGetVersion(json, remoteVer, 256)) {
            free(json);
            LogT(L"[更新] 无法解析 yt-dlp 版本信息");
            return;
        }
        free(json);
        assetUrl[0] = 0;
        LogT(L"[更新] GitHub 不可达，已通过 PyPI 查询最新版本：%s", remoteVer);
    } else {
        if (!JsonGetTag(json, remoteVer, 256) ||
            !JsonGetAsset(json, L"yt-dlp.exe", assetUrl, 1200)) {
            free(json);
            LogT(L"[更新] 无法解析 yt-dlp 版本信息");
            return;
        }
        free(json);
    }

    if (haveLocal && VersionCmp(localVer, remoteVer) == 0 && FileNonEmpty(target)) {
        LogT(L"[更新] yt-dlp 已是最新（%s）", localVer);
        return;
    }
    if (!assetUrl[0]) {
        LogT(L"[更新] yt-dlp 有新版本 %s（当前 %s），但当前网络无法访问 GitHub 下载。"
             L"可稍后换网络重试，或手动到 yt-dlp 官网下载后替换本文件夹中的 yt-dlp.exe",
             remoteVer, haveLocal ? localVer : L"未安装");
        return;
    }
    LogT(L"[更新] yt-dlp 新版本 %s（当前 %s），开始下载…",
         remoteVer, haveLocal ? localVer : L"未安装");
    _snwprintf(tmpTarget, MAX_PATH + 64, L"%s.new", target);
    DeleteFileW(tmpTarget);
    if (!DownloadToFile(assetUrl, tmpTarget)) {
        DeleteFileW(tmpTarget);
        LogT(L"[更新] yt-dlp.exe 下载失败（GitHub 不可达或网络中断）");
        return;
    }
    if (!MoveFileExW(tmpTarget, target, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmpTarget);
        LogT(L"[更新] 替换 yt-dlp.exe 失败（文件可能被占用，请关闭其他正在使用它的程序）");
        return;
    }
    wcsncpy(g.ytdlp, target, MAX_PATH + 63);
    g.ytdlp[MAX_PATH + 63] = 0;
    ConfigSave();
    LogT(L"[更新] yt-dlp 已更新：%s", target);
}

static void UpdateFfmpeg(void)
{
    WCHAR progDir[MAX_PATH], target[MAX_PATH + 64], tmpTarget[MAX_PATH + 64];
    WCHAR zipTmp[MAX_PATH + 64], tempBase[MAX_PATH];
    WCHAR zipUrl[1200];
    WCHAR remoteVer[256] = L"", localVer[256] = L"";
    BOOL need;

    GetProgDir(progDir, MAX_PATH);
    PathJoin(target, MAX_PATH + 64, progDir, L"ffmpeg.exe");
    GetPrivateProfileStringW(L"Settings", L"FfmpegZipUrl", UPDATE_URL_FFMPEG_ZIP,
                             zipUrl, 1200, g.ini);
    GetPrivateProfileStringW(L"Settings", L"FfmpegVer", L"", localVer, 256, g.ini);

    if (!HttpGetHeader(zipUrl, HTTP_QUERY_LAST_MODIFIED, remoteVer, 256)) {
        LogT(L"[更新] ffmpeg 版本查询失败（网络问题）");
        return;
    }
    /* ffmpeg 已存在但没有版本记录：视为当前版本（便携包自带），记录后跳过 */
    if (FileNonEmpty(target) && !localVer[0]) {
        WritePrivateProfileStringW(L"Settings", L"FfmpegVer", remoteVer, g.ini);
        LogT(L"[更新] ffmpeg 已存在（%s），记录版本信息，无需下载", target);
        return;
    }
    need = !FileNonEmpty(target) || wcscmp(localVer, remoteVer) != 0;
    if (!need) {
        LogT(L"[更新] ffmpeg 已是最新");
        return;
    }
    LogT(L"[更新] ffmpeg 有更新（或未安装），开始下载（约 80MB）…");

    GetTempPathW(MAX_PATH, tempBase);
    _snwprintf(zipTmp, MAX_PATH + 64, L"%s\\ytdlp-update-ffmpeg.zip", tempBase);
    DeleteFileW(zipTmp);

    if (!DownloadToFile(zipUrl, zipTmp)) {
        LogT(L"[更新] ffmpeg 下载失败（网络问题）");
        return;
    }
    LogT(L"[更新] 正在解压 ffmpeg…");
    _snwprintf(tmpTarget, MAX_PATH + 64, L"%s.new", target);
    DeleteFileW(tmpTarget);
    if (ZipExtractEntry(zipTmp, L"ffmpeg.exe", tmpTarget) != 0) {
        DeleteFileW(tmpTarget);
        DeleteFileW(zipTmp);
        LogT(L"[更新] 解压 ffmpeg 失败（压缩包可能损坏，请重试）");
        return;
    }
    if (!MoveFileExW(tmpTarget, target, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmpTarget);
        DeleteFileW(zipTmp);
        LogT(L"[更新] 替换 ffmpeg.exe 失败（文件可能被占用）");
        return;
    }
    WritePrivateProfileStringW(L"Settings", L"FfmpegVer", remoteVer, g.ini);
    DeleteFileW(zipTmp);
    LogT(L"[更新] ffmpeg 已更新：%s", target);
}

static DWORD WINAPI UpdateThread(LPVOID p)
{
    (void)p;
    UpdateYtdlp();
    UpdateFfmpeg();
    LogT(L"[更新] 检查完成");
    PostMessageW(g.hwnd, WM_APP_UPDONE, 0, 0);
    return 0;
}

static DWORD WINAPI FfInstallThread(LPVOID p)
{
    (void)p;
    LogT(L"[ffmpeg] 开始下载安装（约 80MB），完成后自动继续下载视频…");
    UpdateFfmpeg();
    {
        WCHAR progDir[MAX_PATH], target[MAX_PATH + 64];
        GetProgDir(progDir, MAX_PATH);
        PathJoin(target, MAX_PATH + 64, progDir, L"ffmpeg.exe");
        PostMessageW(g.hwnd, WM_APP_FFRESULT, FileNonEmpty(target), 0);
    }
    return 0;
}

static void StartCheckUpdates(void)
{
    HANDLE hThread;
    if (g.updating)
        return;
    if (g.running) {
        Log(L"[提示] 正在下载视频，请完成后再检查更新");
        return;
    }
    if (!(g.ytdlp[0] && FileNonEmpty(g.ytdlp)))
        ResolveYtdlp();
    g.updating = 1;
    EnableWindow(g.hUpdate, FALSE);
    hThread = CreateThread(NULL, 0, UpdateThread, NULL, 0, NULL);
    if (hThread)
        CloseHandle(hThread);
    else
        UpdateThread(NULL);
}

/* ------------------------------------------------------------------ */
/* 下载                                                                */
/* ------------------------------------------------------------------ */
static void SetBusy(BOOL busy)
{
    EnableWindow(g.hDl, !busy);
    EnableWindow(g.hStop, busy);
}

static void StartDownload(void);

static DWORD WINAPI ReaderThread(LPVOID param)
{
    HANDLE hRead = (HANDLE)param;
    BYTE  raw[8192];
    BYTE *buf = NULL;
    WCHAR *acc = NULL;
    size_t rawLen = 0, rawCap = 0;
    size_t accLen = 0, accCap = 0;
    DWORD lastFlush = GetTickCount();
    DWORD code = 0;

    rawCap = 16384;
    accCap = 16384;
    buf = (BYTE *)malloc(rawCap);
    acc = (WCHAR *)malloc(accCap * sizeof(WCHAR));
    if (!buf || !acc)
        goto out;

    for (;;) {
        DWORD n = 0;
        size_t i = 0;
        if (!ReadFile(hRead, raw, sizeof(raw), &n, NULL) || n == 0)
            break;
        if (rawLen + n > rawCap) {
            rawCap = rawLen + n + 8192;
            buf = (BYTE *)realloc(buf, rawCap);
            if (!buf)
                goto out;
        }
        memcpy(buf + rawLen, raw, n);
        rawLen += n;

        while (i < rawLen) {
            size_t nl = i, lineLen;
            int r;
            UINT cp = CP_UTF8;
            while (nl < rawLen && buf[nl] != '\n')
                nl++;
            if (nl >= rawLen)
                break;
            lineLen = nl - i;
            if (lineLen && buf[nl - 1] == '\r')
                lineLen--;
            r = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    (LPCCH)(buf + i), (int)lineLen, NULL, 0);
            if (r <= 0) {
                cp = CP_ACP;
                r = MultiByteToWideChar(CP_ACP, 0, (LPCCH)(buf + i), (int)lineLen, NULL, 0);
            }
            if (r > 0) {
                if (accLen + (size_t)r + 2 > accCap) {
                    while (accLen + (size_t)r + 2 > accCap)
                        accCap *= 2;
                    acc = (WCHAR *)realloc(acc, accCap * sizeof(WCHAR));
                    if (!acc)
                        goto out;
                }
                MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0,
                                    (LPCCH)(buf + i), (int)lineLen, acc + accLen, r);
                accLen += r;
                acc[accLen++] = L'\r';
                acc[accLen++] = L'\n';
            }
            i = nl + 1;
        }
        if (i > 0) {
            memmove(buf, buf + i, rawLen - i);
            rawLen -= i;
        }
        if (accLen >= 512 || GetTickCount() - lastFlush > 250) {
            PostBlock(acc, accLen);
            accLen = 0;
            lastFlush = GetTickCount();
        }
    }
    if (rawLen > 0) {
        int r = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                    (LPCCH)buf, (int)rawLen, NULL, 0);
        UINT cp = CP_UTF8;
        if (r <= 0) {
            cp = CP_ACP;
            r = MultiByteToWideChar(CP_ACP, 0, (LPCCH)buf, (int)rawLen, NULL, 0);
        }
        if (r > 0) {
            if (accLen + (size_t)r + 2 > accCap) {
                while (accLen + (size_t)r + 2 > accCap)
                    accCap *= 2;
                acc = (WCHAR *)realloc(acc, accCap * sizeof(WCHAR));
            }
            if (acc) {
                MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0,
                                    (LPCCH)buf, (int)rawLen, acc + accLen, r);
                accLen += r;
                acc[accLen++] = L'\r';
                acc[accLen++] = L'\n';
            }
        }
    }
    if (accLen > 0)
        PostBlock(acc, accLen);

out:
    free(buf);
    free(acc);
    if (g.hProcess)
        GetExitCodeProcess(g.hProcess, &code);
    CloseHandle(hRead);
    PostMessageW(g.hwnd, WM_APP_DONE, (WPARAM)code, 0);
    return 0;
}

static void StartDownload(void)
{
    int len, nurl = 0, i, cl = 0;
    WCHAR *text = NULL, *cmd = NULL;
    WCHAR **urls = NULL;
    WCHAR dir[MAX_PATH + 64];
    WCHAR ffmpeg[MAX_PATH + 64];
    size_t cap;
    SECURITY_ATTRIBUTES sa;
    HANDLE hRead = NULL, hWrite = NULL, hThread;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    if (g.running) {
        Log(L"[提示] 正在下载中，请稍候");
        return;
    }

    if (!(g.ytdlp[0] && FileNonEmpty(g.ytdlp)) && !ResolveYtdlp()) {
        if (!AskLocateYtdlp(g.hwnd)) {
            Log(L"[错误] 没有找到 yt-dlp.exe，无法下载。可以点「检查更新」自动下载安装。");
            return;
        }
    }

    len = GetWindowTextLengthW(g.hUrl);
    if (len <= 0) {
        MessageBoxW(g.hwnd, L"请先粘贴视频链接（支持多行，每行一个）", L"提示",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    text = (WCHAR *)malloc((len + 2) * sizeof(WCHAR));
    if (!text)
        return;
    GetWindowTextW(g.hUrl, text, len + 1);

    {
        int maxurls = 64;
        WCHAR *p = text;
        urls = (WCHAR **)malloc(sizeof(WCHAR *) * maxurls);
        if (!urls) {
            free(text);
            return;
        }
        while (*p) {
            WCHAR *st, *en;
            size_t l;
            while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n')
                p++;
            if (!*p)
                break;
            st = p;
            while (*p && *p != L'\r' && *p != L'\n')
                p++;
            en = p;
            while (en > st && (en[-1] == L' ' || en[-1] == L'\t'))
                en--;
            if (en > st) {
                if (nurl >= maxurls) {
                    maxurls *= 2;
                    urls = (WCHAR **)realloc(urls, sizeof(WCHAR *) * maxurls);
                    if (!urls) {
                        free(text);
                        return;
                    }
                }
                l = (size_t)(en - st);
                urls[nurl] = (WCHAR *)malloc((l + 1) * sizeof(WCHAR));
                if (!urls[nurl]) {
                    free(text);
                    return;
                }
                wcsncpy(urls[nurl], st, l);
                urls[nurl][l] = 0;
                nurl++;
            }
        }
    }
    if (nurl == 0) {
        MessageBoxW(g.hwnd, L"没有找到有效的视频链接", L"提示", MB_OK | MB_ICONINFORMATION);
        free(urls);
        free(text);
        return;
    }

    if (!GetWindowTextW(g.hDir, dir, MAX_PATH + 64) || !dir[0]) {
        MessageBoxW(g.hwnd, L"请先点「选择文件夹…」设置保存位置", L"提示",
                    MB_OK | MB_ICONINFORMATION);
        for (i = 0; i < nurl; i++) free(urls[i]);
        free(urls);
        free(text);
        return;
    }
    SHCreateDirectoryExW(NULL, dir, NULL);

    /* 强制最高画质：必须有 ffmpeg，否则引导自动安装，绝不降级 */
    if (!FindFfmpeg(ffmpeg, MAX_PATH + 64)) {
        int r = MessageBoxW(g.hwnd,
                            L"下载最高画质需要 ffmpeg.exe（用于合并视频和音频），\n"
                            L"当前没有检测到。\n\n"
                            L"是否现在自动下载安装？（约 80MB，之后自动开始下载）",
                            L"需要 ffmpeg", MB_YESNO | MB_ICONQUESTION);
        for (i = 0; i < nurl; i++) free(urls[i]);
        free(urls);
        free(text);
        if (r == IDYES) {
            HANDLE hThread = CreateThread(NULL, 0, FfInstallThread, NULL, 0, NULL);
            if (hThread)
                CloseHandle(hThread);
            else
                FfInstallThread(NULL);
        } else {
            Log(L"[已取消] 未安装 ffmpeg，无法下载最高画质（点「检查更新」可安装 ffmpeg）");
        }
        return;
    }

    cap = (size_t)len * 2 + 4096;
    cmd = (WCHAR *)malloc(cap * sizeof(WCHAR));
    if (!cmd) {
        for (i = 0; i < nurl; i++) free(urls[i]);
        free(urls);
        free(text);
        return;
    }
    cl += _snwprintf(cmd + cl, cap - cl, L"\"%s\" --newline --windows-filenames --encoding utf-8",
                     g.ytdlp);
    if (SendMessageW(g.hCkPl, BM_GETCHECK, 0, 0) != BST_CHECKED)
        cl += _snwprintf(cmd + cl, cap - cl, L" --no-playlist");
    cl += _snwprintf(cmd + cl, cap - cl, L" --");
    for (i = 0; i < nurl; i++) {
        WCHAR *q;
        cl += _snwprintf(cmd + cl, cap - cl, L" \"");
        for (q = urls[i]; *q && cl < (int)cap - 3; q++) {
            if (*q == L'"') {
                cmd[cl++] = L'\\';
                cmd[cl++] = L'"';
            } else {
                cmd[cl++] = *q;
            }
        }
        cl += _snwprintf(cmd + cl, cap - cl, L"\"");
    }
    cmd[cl] = 0;

    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        Log(L"[错误] 创建管道失败");
        for (i = 0; i < nurl; i++) free(urls[i]);
        free(urls); free(text); free(cmd);
        return;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;

    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessW(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, dir, &si, &pi)) {
        DWORD err = GetLastError();
        Log(L"[错误] 启动 yt-dlp 失败（错误代码 %lu）", (unsigned long)err);
        CloseHandle(hRead);
        CloseHandle(hWrite);
        for (i = 0; i < nurl; i++) free(urls[i]);
        free(urls); free(text); free(cmd);
        return;
    }
    CloseHandle(hWrite);

    g.hProcess = pi.hProcess;
    CloseHandle(pi.hThread);
    g.running = 1;
    g.stopped = 0;
    SetBusy(TRUE);
    SetWindowTextW(g.hwnd, L"yt-dlp 视频下载器 — 正在下载…");

    Log(L"[开始] 下载 %d 个链接 → %s", nurl, dir);
    Log(L"[画质] 最高画质（ffmpeg: %s）", ffmpeg);
    Log(L"[命令] %s", cmd);

    hThread = CreateThread(NULL, 0, ReaderThread, hRead, 0, NULL);
    if (!hThread) {
        ReaderThread(hRead);
    } else {
        CloseHandle(hThread);
    }

    for (i = 0; i < nurl; i++) free(urls[i]);
    free(urls); free(text); free(cmd);
}

static void StopDownload(void)
{
    if (!g.running || !g.hProcess)
        return;
    g.stopped = 1;
    Log(L"[停止] 正在终止下载…");
    TerminateProcess(g.hProcess, 1);
}

/* ------------------------------------------------------------------ */
/* URL 输入框子类：Ctrl+Enter 开始下载                                  */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK UrlSubclassProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_KEYDOWN && w == VK_RETURN && (GetKeyState(VK_CONTROL) & 0x8000)) {
        StartDownload();
        return 0;
    }
    return CallWindowProcW(g_oldUrlProc, h, m, w, l);
}

/* ------------------------------------------------------------------ */
/* 窗口过程                                                            */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        unsigned dpi;
        NONCLIENTMETRICSW ncm;
        HWND c;
        HFONT f;

        g.hwnd = hwnd;
        dpi = GetDpiForSystem();
        if (!dpi)
            dpi = 96;
        g_scale = (float)dpi / 96.0f;

        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        ncm.lfMessageFont.lfHeight = -MulDiv(9, dpi, 72);
        ncm.lfMessageFont.lfWeight = FW_NORMAL;
        f = CreateFontIndirectW(&ncm.lfMessageFont);
        g_font = f;

#define NEWCTL(cls, text, style, x, y, w, h, id) \
        CreateWindowExW(0, cls, text, style, SX(x), SX(y), SX(w), SX(h), \
                        hwnd, (HMENU)(id), g_hInst, NULL)

        c = NEWCTL(L"STATIC", L"视频链接（支持多行，每行一个）：",
                   WS_CHILD | WS_VISIBLE, 12, 10, 420, 18, IDC_LBL_URL);
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);

        c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE |
                            ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL,
                            SX(12), SX(30), SX(536), SX(56),
                            hwnd, (HMENU)IDC_URL, g_hInst, NULL);
        g.hUrl = c;
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
        g_oldUrlProc = (WNDPROC)(LONG_PTR)SetWindowLongPtrW(c, GWLP_WNDPROC,
                                                            (LONG_PTR)UrlSubclassProc);

        c = NEWCTL(L"STATIC", L"保存位置：", WS_CHILD | WS_VISIBLE, 12, 94, 200, 18, IDC_LBL_DIR);
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);

        c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_READONLY,
                            SX(12), SX(116), SX(436), SX(24),
                            hwnd, (HMENU)IDC_DIR, g_hInst, NULL);
        g.hDir = c;
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);

        c = NEWCTL(L"BUTTON", L"选择文件夹…",
                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                   456, 114, 92, 28, IDC_BROWSE);
        g.hBrowse = c;
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);

        c = NEWCTL(L"BUTTON", L"下载整个播放列表",
                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                   12, 150, 138, 20, IDC_CK_PL);
        g.hCkPl = c;
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);

        c = NEWCTL(L"BUTTON", L"窗口置顶",
                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                   158, 150, 84, 20, IDC_CK_TOP);
        g.hCkTop = c;
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);

        c = NEWCTL(L"BUTTON", L"启动时自动检查更新",
                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                   250, 150, 140, 20, IDC_CK_UPD);
        g.hCkUpd = c;
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);

        c = NEWCTL(L"BUTTON", L"开始下载",
                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                   12, 178, 110, 32, IDC_DL);
        g.hDl = c;
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);

        c = NEWCTL(L"BUTTON", L"停止",
                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                   130, 178, 80, 32, IDC_STOP);
        g.hStop = c;
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
        EnableWindow(c, FALSE);

        c = NEWCTL(L"BUTTON", L"打开文件夹",
                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                   218, 178, 110, 32, IDC_OPEN);
        g.hOpen = c;
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);

        c = NEWCTL(L"BUTTON", L"检查更新",
                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                   336, 178, 110, 32, IDC_UPDATE);
        g.hUpdate = c;
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);

        c = NEWCTL(L"STATIC", L"日志：", WS_CHILD | WS_VISIBLE, 12, 218, 100, 18, IDC_LBL_LOG);
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);

        c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                            ES_AUTOVSCROLL | WS_VSCROLL,
                            SX(12), SX(240), SX(536), SX(248),
                            hwnd, (HMENU)IDC_LOG, g_hInst, NULL);
        g.hLog = c;
        SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);

        ConfigLoad();
        ApplyTopmost();
        LayoutControls(SX(560), SX(500));
        SetFocus(g.hUrl);
        if (g.startupUrl[0])
            SetWindowTextW(g.hUrl, g.startupUrl);
        if (!ResolveYtdlp())
            PostMessageW(hwnd, WM_APP_ASKYT, 0, 0);

        if (!RegisterHotKey(hwnd, HOTKEY_ID, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'Y'))
            Log(L"[提示] 全局快捷键 Ctrl+Shift+Y 注册失败（可能被其他程序占用）");

        Log(L"就绪：粘贴链接 → 选择保存位置 → 点「开始下载」");
        Log(L"[快捷键] 任何程序里按 Ctrl+Shift+Y = 自动下载剪贴板中的链接");
        if (g.ytdlp[0])
            Log(L"[信息] yt-dlp: %s", g.ytdlp);
        PostMessageW(hwnd, WM_APP_AUTOCHK, 0, 0);
        return 0;
    }

    case WM_SETFOCUS:
        if (g.hUrl && IsWindow(g.hUrl))
            SetFocus(g.hUrl);
        return 0;

    case WM_SIZE:
        LayoutControls(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = SX(520);
        mmi->ptMinTrackSize.y = SX(430);
        return 0;
    }

    case WM_HOTKEY:
        if (wp == HOTKEY_ID) {
            WCHAR clip[4096];
            if (GetClipboardUrl(clip, 4096)) {
                SetWindowTextW(g.hUrl, clip);
                Log(L"[快捷键] 剪贴板链接：%s", clip);
                SetForegroundWindow(g.hwnd);
                if (g.running) {
                    Log(L"[提示] 正在下载中，新链接已填入，完成后再按一次即可");
                    MessageBeep(MB_ICONASTERISK);
                } else {
                    StartDownload();
                }
            } else {
                SetForegroundWindow(g.hwnd);
                Log(L"[快捷键] 剪贴板中没有有效链接（需 http:// 或 https:// 开头）");
                MessageBeep(MB_ICONASTERISK);
            }
        }
        return 0;

    case WM_APP_ASKYT:
        if (!(g.ytdlp[0] && FileNonEmpty(g.ytdlp)))
            AskLocateYtdlp(g.hwnd);
        return 0;

    case WM_APP_AUTOCHK:
        if (SendMessageW(g.hCkUpd, BM_GETCHECK, 0, 0) == BST_CHECKED)
            StartCheckUpdates();
        return 0;

    case WM_APP_UPDONE:
        g.updating = 0;
        EnableWindow(g.hUpdate, TRUE);
        return 0;

    case WM_APP_FFRESULT:
        if (wp) {
            Log(L"[ffmpeg] 安装完成，继续下载");
            StartDownload();
        } else {
            Log(L"[ffmpeg] 安装失败，请检查网络后点「检查更新」重试");
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BROWSE:
            BrowseFolder();
            return 0;
        case IDC_DL:
            StartDownload();
            return 0;
        case IDC_STOP:
            StopDownload();
            return 0;
        case IDC_OPEN:
            OpenFolder();
            return 0;
        case IDC_UPDATE:
            StartCheckUpdates();
            return 0;
        case IDC_CK_PL:
        case IDC_CK_UPD:
            ConfigSave();
            return 0;
        case IDC_CK_TOP:
            ApplyTopmost();
            ConfigSave();
            return 0;
        }
        break;

    case WM_APP_LOG: {
        const WCHAR *s = (const WCHAR *)wp;
        if (s && g.hLog) {
            int cur = GetWindowTextLengthW(g.hLog);
            if (cur > 400000)
                SetWindowTextW(g.hLog, L"");
            cur = GetWindowTextLengthW(g.hLog);
            SendMessageW(g.hLog, EM_SETSEL, (WPARAM)cur, (LPARAM)cur);
            SendMessageW(g.hLog, EM_REPLACESEL, FALSE, (LPARAM)s);
            SendMessageW(g.hLog, EM_SCROLLCARET, 0, 0);
        }
        free((void *)wp);
        return 0;
    }

    case WM_APP_DONE: {
        DWORD code = (DWORD)wp;
        g.running = 0;
        SetBusy(FALSE);
        SetWindowTextW(hwnd, L"yt-dlp 视频下载器");
        if (g.hProcess) {
            CloseHandle(g.hProcess);
            g.hProcess = NULL;
        }
        if (g.stopped) {
            Log(L"[已停止] 下载被终止");
        } else if (code == 0) {
            Log(L"[完成] 下载完成！");
            MessageBeep(MB_OK);
        } else {
            Log(L"[出错] yt-dlp 退出代码 %lu（请查看上方日志）", (unsigned long)code);
        }
        g.stopped = 0;
        return 0;
    }

    case WM_CLOSE:
        if (g.running) {
            if (MessageBoxW(hwnd, L"正在下载，确定要退出吗？", L"确认",
                            MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        UnregisterHotKey(hwnd, HOTKEY_ID);
        if (g.running && g.hProcess)
            TerminateProcess(g.hProcess, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR pCmd, int nShow)
{
    HMODULE u32;
    BOOL (WINAPI *pfnDpi)(DPI_AWARENESS_CONTEXT);
    WNDCLASSEXW wc;
    HWND hwnd;
    MSG msg;
    RECT rc;
    DWORD style;
    unsigned dpi;
    int sw, sh, x, y;
    INITCOMMONCONTROLSEX icc;

    (void)hPrev; (void)nShow;
    g_hInst = hInst;

    if (pCmd && pCmd[0]) {
        PWSTR s = pCmd;
        size_t n = 0;
        while (*s == L' ' || *s == L'\t')
            s++;
        if (*s == L'"') {
            s++;
            while (s[n] && s[n] != L'"' && n < 4095)
                n++;
        } else {
            while (s[n] && n < 4095)
                n++;
        }
        if (n) {
            memcpy(g.startupUrl, s, n * sizeof(WCHAR));
            g.startupUrl[n] = 0;
        }
    }

    u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        pfnDpi = (void *)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (pfnDpi)
            pfnDpi(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    ConfigInit();

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"YtDlpGuiMainWnd";
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(101));
    wc.hIconSm = wc.hIcon ? wc.hIcon : LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    if (!RegisterClassExW(&wc))
        return 1;

    dpi = GetDpiForSystem();
    if (!dpi)
        dpi = 96;
    g_scale = (float)dpi / 96.0f;

    rc.left = rc.top = 0;
    rc.right = SX(560);
    rc.bottom = SX(500);
    style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME |
            WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    AdjustWindowRectEx(&rc, style, FALSE, 0);
    sw = GetSystemMetrics(SM_CXSCREEN);
    sh = GetSystemMetrics(SM_CYSCREEN);
    x = (sw - (rc.right - rc.left)) / 2;
    y = (sh - (rc.bottom - rc.top)) / 3;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    hwnd = CreateWindowExW(0, wc.lpszClassName, L"yt-dlp 视频下载器", style,
                           x, y, rc.right - rc.left, rc.bottom - rc.top,
                           NULL, NULL, hInst, NULL);
    if (!hwnd)
        return 1;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CoUninitialize();
    return (int)msg.wParam;
}
