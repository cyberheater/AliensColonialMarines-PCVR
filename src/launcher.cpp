// ACM-VR launcher: starts the untouched ACM.exe and loads the VR mod (acmvr.dll) into it before the game's own code runs.
//
// Use from Steam (Properties > General > Launch Options):
//     "C:\path\to\acmvr_launcher.exe" %command%
// or run it directly (it finds the game itself).
//
// Settings live in acmvr.ini next to the launcher, section [Launcher]:
//     GamePath=            full path to ACM.exe (only needed if it can't be found)
//     AutoResolution=1     size the game window from SteamVR's per-eye resolution (unless -ResX/-ResY are given)
//     ExtraArgs=-windowed -nostartupmovies
//     ResolutionScale=100  percent of SteamVR's per-eye size (set with the slider in the launcher window)
//     ShowWindow=1         show the launcher window before starting (hold Shift while starting to show it anyway)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <openvr.h>

static wchar_t g_dir[MAX_PATH], g_ini[MAX_PATH];
static FILE* g_log;

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d] ", t.wHour, t.wMinute, t.wSecond);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}
static void Fail(const wchar_t* msg) {
    char m[512]; WideCharToMultiByte(CP_UTF8, 0, msg, -1, m, sizeof(m), NULL, NULL);
    Log("ERROR: %s", m);
    MessageBoxW(NULL, msg, L"ACM-VR launcher", MB_OK | MB_ICONERROR);
}
static bool Exists(const wchar_t* p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static bool HasArgI(const wchar_t* args, const wchar_t* key) {   // case-insensitive substring
    size_t k = wcslen(key);
    for (const wchar_t* p = args; *p; p++) if (!_wcsnicmp(p, key, k)) return true;
    return false;
}

static bool IsElevated() {
    typedef BOOL(WINAPI * OPT_t)(HANDLE, DWORD, PHANDLE);
    typedef BOOL(WINAPI * GTI_t)(HANDLE, TOKEN_INFORMATION_CLASS, LPVOID, DWORD, PDWORD);
    HMODULE a = LoadLibraryW(L"advapi32.dll");
    OPT_t opt = a ? (OPT_t)GetProcAddress(a, "OpenProcessToken") : NULL;
    GTI_t gti = a ? (GTI_t)GetProcAddress(a, "GetTokenInformation") : NULL;
    if (!opt || !gti) return false;
    HANDLE t; TOKEN_ELEVATION e = {0}; DWORD n = 0;
    if (!opt(GetCurrentProcess(), TOKEN_QUERY, &t)) return false;
    BOOL ok = gti(t, TokenElevation, &e, sizeof(e), &n);
    CloseHandle(t);
    return ok && e.TokenIsElevated;
}
// Splits our command line into the game exe (first argument, if it is an .exe) and the rest.
static void ParseCommandLine(wchar_t* exe, wchar_t* rest) {
    exe[0] = rest[0] = 0;
    const wchar_t* c = GetCommandLineW();
    // skip our own path
    if (*c == L'"') { c++; while (*c && *c != L'"') c++; if (*c) c++; }
    else while (*c && *c != L' ' && *c != L'\t') c++;
    while (*c == L' ' || *c == L'\t') c++;
    // next token: the game exe (Steam's %command%)
    const wchar_t* s = c; wchar_t tok[MAX_PATH]; int n = 0;
    if (*s == L'"') { s++; while (*s && *s != L'"' && n < MAX_PATH - 1) tok[n++] = *s++; if (*s) s++; }
    else while (*s && *s != L' ' && *s != L'\t' && n < MAX_PATH - 1) tok[n++] = *s++;
    tok[n] = 0;
    if (n > 4 && !_wcsicmp(tok + n - 4, L".exe")) { wcscpy(exe, tok); c = s; while (*c == L' ' || *c == L'\t') c++; }
    wcsncpy(rest, c, 2047); rest[2047] = 0;
}

static bool FindGame(wchar_t* exe) {
    wchar_t p[MAX_PATH];
    GetPrivateProfileStringW(L"Launcher", L"GamePath", L"", p, MAX_PATH, g_ini);
    if (p[0] && Exists(p)) { wcscpy(exe, p); return true; }
    // installed inside the game folder (…\Aliens Colonial Marines\ACMVR\)
    _snwprintf(p, MAX_PATH, L"%s..\\Binaries\\Win32\\ACM.exe", g_dir);
    if (Exists(p)) { GetFullPathNameW(p, MAX_PATH, exe, NULL); return true; }
    // default Steam library
    typedef LONG(WINAPI * RegGetValueW_t)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
    HMODULE adv = LoadLibraryW(L"advapi32.dll");
    RegGetValueW_t rg = adv ? (RegGetValueW_t)GetProcAddress(adv, "RegGetValueW") : NULL;
    wchar_t steam[MAX_PATH]; DWORD sz = sizeof(steam);
    if (rg && rg(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", 0x0000ffff /*RRF_RT_ANY*/, NULL, steam, &sz) == 0) {
        for (wchar_t* q = steam; *q; q++) if (*q == L'/') *q = L'\\';
        _snwprintf(p, MAX_PATH, L"%s\\steamapps\\common\\Aliens Colonial Marines\\Binaries\\Win32\\ACM.exe", steam);
        if (Exists(p)) { wcscpy(exe, p); return true; }
    }
    return false;
}

static bool VrRenderSizeOnce(UINT* w, UINT* h, bool quiet);
// If SteamVR isn't running, start it and wait (up to a minute) until it answers, so its windows have settled
// before the game starts and the resolution comes from the headset that is actually connected.
static bool VrRenderSize(UINT* w, UINT* h) {
    if (VrRenderSizeOnce(w, h, true)) return true;
    if (!GetPrivateProfileIntW(L"Launcher", L"StartSteamVR", 1, g_ini)) return VrRenderSizeOnce(w, h, false);
    typedef HINSTANCE(WINAPI * ShellExecuteW_t)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
    HMODULE sh = LoadLibraryW(L"shell32.dll");
    ShellExecuteW_t se = sh ? (ShellExecuteW_t)GetProcAddress(sh, "ShellExecuteW") : NULL;
    if (!se) return VrRenderSizeOnce(w, h, false);
    Log("SteamVR not running - starting it");
    se(NULL, L"open", L"steam://rungameid/250820", NULL, NULL, SW_SHOWNORMAL);
    for (int i = 0; i < 60; i++) {
        Sleep(1000);
        if (VrRenderSizeOnce(w, h, true)) { Log("SteamVR ready after %d s", i + 1); Sleep(3000); return true; }
    }
    Log("SteamVR did not answer within a minute");
    return VrRenderSizeOnce(w, h, false);
}
static bool VrRenderSizeOnce(UINT* w, UINT* h, bool quiet) {
    *w = GetPrivateProfileIntW(L"Launcher", L"LastEyeWidth", 0, g_ini);
    *h = GetPrivateProfileIntW(L"Launcher", L"LastEyeHeight", 0, g_ini);
    wchar_t p[MAX_PATH]; _snwprintf(p, MAX_PATH, L"%sopenvr_api.dll", g_dir);
    HMODULE ovr = LoadLibraryW(p);
    if (!ovr) { Log("openvr_api.dll not found next to the launcher"); return *w && *h; }
    typedef uint32_t (*Init_t)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
    typedef void* (*GetIf_t)(const char*, vr::EVRInitError*);
    typedef void (*Shut_t)();
    Init_t init = (Init_t)GetProcAddress(ovr, "VR_InitInternal2");
    GetIf_t gi = (GetIf_t)GetProcAddress(ovr, "VR_GetGenericInterface");
    Shut_t shut = (Shut_t)GetProcAddress(ovr, "VR_ShutdownInternal");
    bool ok = false;
    if (init && gi && shut) {
        vr::EVRInitError e = vr::VRInitError_None;
        init(&e, vr::VRApplication_Background, NULL);   // only works if SteamVR is already running; never starts it
        if (e == vr::VRInitError_None) {
            vr::IVRSystem* sys = (vr::IVRSystem*)gi(vr::IVRSystem_Version, &e);
            uint32_t rw = 0, rh = 0;
            if (sys) sys->GetRecommendedRenderTargetSize(&rw, &rh);
            shut();
            if (rw >= 512 && rh >= 512) {
                *w = rw; *h = rh; ok = true;
                wchar_t v[32];
                _snwprintf(v, 32, L"%u", rw); WritePrivateProfileStringW(L"Launcher", L"LastEyeWidth", v, g_ini);
                _snwprintf(v, 32, L"%u", rh); WritePrivateProfileStringW(L"Launcher", L"LastEyeHeight", v, g_ini);
                Log("SteamVR per-eye size %ux%u", rw, rh);
            }
        } else if (!quiet) Log("SteamVR not running (error %d) - using the last known size %ux%u", (int)e, *w, *h);
    }
    FreeLibrary(ovr);
    return quiet ? ok : (ok || (*w && *h));
}

// ---------------------------------------------------------------- launcher window ----
#define TBM_GETPOS (WM_USER)
#define TBM_SETPOS (WM_USER + 5)
#define TBM_SETRANGE (WM_USER + 6)
#define TBM_SETTICFREQ (WM_USER + 20)
#define TBM_SETPAGESIZE (WM_USER + 21)
#define TBM_SETLINESIZE (WM_USER + 23)
enum { ID_SLIDER = 100, ID_SIZE, ID_INFO, ID_SKIP };
static UINT g_vrW, g_vrH; static int g_scale = 100, g_uiResult = -1, g_fixedRes; static HWND g_hSize, g_hSlider, g_hSkip, g_hWarn, g_hSmooth; static int g_snapDeg = 30; static bool g_skipNext, g_overlayWarn;
static int Sc(int v) { static int dpi; if (!dpi) { HDC dc = GetDC(NULL); dpi = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(NULL, dc); } return MulDiv(v, dpi, 96); }
static void EyeSize(int scale, UINT* w, UINT* h) { *w = (UINT)((g_vrW * scale / 100 + 1) & ~1u); *h = (UINT)((g_vrH * scale / 100 + 1) & ~1u); }
static void UpdateSizeLabel() {
    wchar_t t[160];
    if (g_fixedRes) _snwprintf(t, 160, L"Set by your launch options (-ResX / -ResY)");
    else if (!g_vrW) _snwprintf(t, 160, L"%d%%   (start SteamVR first to see the size)", g_scale);
    else { UINT w, h; EyeSize(g_scale, &w, &h);
        _snwprintf(t, 160, L"%d%%   =   %u x %u per eye   (%.1f megapixels per frame)", g_scale, w, h, 2.0 * w * h / 1e6); }
    SetWindowTextW(g_hSize, t);
}
static LRESULT CALLBACK UiProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_HSCROLL:
        if ((HWND)l == g_hSlider) { int p = (int)SendMessageW(g_hSlider, TBM_GETPOS, 0, 0); p = (p + 2) / 5 * 5; g_scale = p; UpdateSizeLabel(); }
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            g_uiResult = 1; g_skipNext = SendMessageW(g_hSkip, BM_GETCHECK, 0, 0) == BST_CHECKED;
            // smooth turning = SnapTurnDegrees 0; unticking goes back to the last snap angle (30 by default)
            bool smooth = SendMessageW(g_hSmooth, BM_GETCHECK, 0, 0) == BST_CHECKED;
            int last = GetPrivateProfileIntW(L"Launcher", L"LastSnapTurnDegrees", 30, g_ini); if (last <= 0) last = 30;
            if (g_snapDeg > 0) last = g_snapDeg;
            wchar_t v[16]; _snwprintf(v, 16, L"%d", smooth ? 0 : last);
            WritePrivateProfileStringW(L"Controls", L"SnapTurnDegrees", v, g_ini);
            _snwprintf(v, 16, L"%d", last); WritePrivateProfileStringW(L"Launcher", L"LastSnapTurnDegrees", v, g_ini);
            Log("Turning: %s", smooth ? "smooth" : "snap");
            DestroyWindow(h);
        }
        else if (LOWORD(w) == IDCANCEL) { g_uiResult = 0; DestroyWindow(h); }
        return 0;
    case WM_CLOSE: g_uiResult = 0; DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_CTLCOLORSTATIC: { HDC dc = (HDC)w; SetBkMode(dc, TRANSPARENT); if ((HWND)l == g_hWarn) SetTextColor(dc, RGB(190, 40, 20)); return (LRESULT)GetSysColorBrush(COLOR_WINDOW); }
    }
    return DefWindowProcW(h, m, w, l);
}
// Shows the window; returns false if the player cancelled.
static bool RunLauncherWindow() {
    typedef BOOL(WINAPI * Icc_t)(const void*);
    HMODULE cc = LoadLibraryW(L"comctl32.dll");
    Icc_t icc = cc ? (Icc_t)GetProcAddress(cc, "InitCommonControlsEx") : NULL;
    struct { DWORD size, flags; } ic = {8, 0x4 /*ICC_BAR_CLASSES*/ | 0x1 /*ICC_LISTVIEW*/};
    if (icc) icc(&ic);
    HINSTANCE hi = GetModuleHandleW(NULL);
    WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = UiProc; wc.hInstance = hi; wc.lpszClassName = L"AcmVrLauncher";
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW); wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    int W = Sc(470), H = Sc(g_overlayWarn ? 326 : 276);
    g_snapDeg = GetPrivateProfileIntW(L"Controls", L"SnapTurnDegrees", 30, g_ini);
    RECT r = {0, 0, W, H}; AdjustWindowRect(&r, WS_CAPTION | WS_SYSMENU, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    HWND win = CreateWindowExW(0, wc.lpszClassName, L"Aliens: Colonial Marines VR", WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                               (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh, NULL, NULL, hi, NULL);
    HFONT f = CreateFontW(-Sc(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT fb = CreateFontW(-Sc(15), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    int x = Sc(20), cw = W - 2 * x, y = Sc(16);
    HWND t1 = CreateWindowW(L"STATIC", L"Render resolution", WS_CHILD | WS_VISIBLE, x, y, cw, Sc(22), win, NULL, hi, NULL);
    SendMessageW(t1, WM_SETFONT, (WPARAM)fb, 0); y += Sc(30);
    g_hSlider = CreateWindowW(L"msctls_trackbar32", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | 0x1 /*TBS_AUTOTICKS*/, x - Sc(4), y, cw + Sc(8), Sc(32), win, (HMENU)ID_SLIDER, hi, NULL);
    SendMessageW(g_hSlider, TBM_SETRANGE, TRUE, MAKELPARAM(50, 150));
    SendMessageW(g_hSlider, TBM_SETTICFREQ, 10, 0); SendMessageW(g_hSlider, TBM_SETLINESIZE, 0, 5); SendMessageW(g_hSlider, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(g_hSlider, TBM_SETPOS, TRUE, g_scale);
    if (g_fixedRes) EnableWindow(g_hSlider, FALSE);
    y += Sc(36);
    g_hSize = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, x, y, cw, Sc(20), win, (HMENU)ID_SIZE, hi, NULL);
    y += Sc(22);
    wchar_t info[200];
    if (g_vrW) _snwprintf(info, 200, L"100%% = SteamVR's size (%u x %u). Lower runs faster, higher looks sharper.", g_vrW, g_vrH);
    else wcscpy(info, L"100% = SteamVR's size. Lower runs faster, higher looks sharper.");
    HWND t2 = CreateWindowW(L"STATIC", info, WS_CHILD | WS_VISIBLE, x, y, cw, Sc(20), win, (HMENU)ID_INFO, hi, NULL);
    y += Sc(30);
    if (g_overlayWarn) {
        g_hWarn = CreateWindowW(L"STATIC", L"The Steam overlay is on for this game. In VR it shows its own panel and pauses the game. "
                                L"Turn it off: Steam > right-click the game > Properties > General > \"Enable the Steam Overlay while in-game\".",
                                WS_CHILD | WS_VISIBLE, x, y, cw, Sc(48), win, NULL, hi, NULL);
        y += Sc(52);
    } else y += Sc(4);
    g_hSmooth = CreateWindowW(L"BUTTON", L"Smooth turning with the right stick (untick for snap turns)", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                              x, y, cw, Sc(22), win, NULL, hi, NULL);
    SendMessageW(g_hSmooth, BM_SETCHECK, g_snapDeg <= 0 ? BST_CHECKED : BST_UNCHECKED, 0);
    y += Sc(26);
    g_hSkip = CreateWindowW(L"BUTTON", L"Start the game straight away next time (hold Shift to see this again)", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                            x, y, cw, Sc(22), win, (HMENU)ID_SKIP, hi, NULL);
    y = H - Sc(48);
    HWND bp = CreateWindowW(L"BUTTON", L"Play", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, W - x - Sc(190), y, Sc(90), Sc(30), win, (HMENU)IDOK, hi, NULL);
    HWND bc = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, W - x - Sc(90), y, Sc(90), Sc(30), win, (HMENU)IDCANCEL, hi, NULL);
    HWND all[] = {g_hSlider, g_hSize, t2, g_hSmooth, g_hSkip, bp, bc, g_hWarn};
    for (HWND c : all) if (c) SendMessageW(c, WM_SETFONT, (WPARAM)f, 0);
    UpdateSizeLabel();
    ShowWindow(win, SW_SHOW); SetForegroundWindow(win); SetFocus(bp);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(win, &msg)) continue;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return g_uiResult == 1;
}


// The game's own settings file (Documents\My Games\...\PecanEngine.ini): a fresh install caps the game at 62 fps
// (frame smoothing) and turns on screen effects that break with two eyes side by side (light shafts, depth of field,
// distortion, lens flares, bloom). Set the VR-friendly values before each start; the first change keeps a backup.
static void TuneEngineIni() {
    if (!GetPrivateProfileIntW(L"Launcher", L"TuneGameSettings", 1, g_ini)) { Log("Game settings: left alone (TuneGameSettings=0)"); return; }
    typedef HRESULT(WINAPI* SHGFP_t)(HWND, int, HANDLE, DWORD, LPWSTR);
    HMODULE sh = LoadLibraryW(L"shell32.dll");
    SHGFP_t gfp = sh ? (SHGFP_t)GetProcAddress(sh, "SHGetFolderPathW") : NULL;
    wchar_t docs[MAX_PATH] = L"";
    if (!gfp || FAILED(gfp(NULL, 0x0005 /*CSIDL_PERSONAL*/, NULL, 0, docs))) { Log("Game settings: Documents folder not found"); return; }
    wchar_t ini[MAX_PATH], bak[MAX_PATH];
    _snwprintf(ini, MAX_PATH, L"%s\\My Games\\Aliens Colonial Marines\\PecanGame\\Config\\PecanEngine.ini", docs);
    _snwprintf(bak, MAX_PATH, L"%s.acmvr-backup", ini);
    if (!Exists(ini)) { Log("Game settings: PecanEngine.ini not there yet (the game creates it on its first start) - start once more after this"); return; }
    static const wchar_t* set[][3] = {
        {L"Engine.Engine", L"bSmoothFrameRate", L"FALSE"},        // no 62 fps cap: the headset paces the game
        {L"Engine.Engine", L"MaxSmoothedFrameRate", L"125"},
        {L"SystemSettings", L"DepthOfField", L"False"},           // screen effects that misbehave across two eyes
        {L"SystemSettings", L"bAllowLightShafts", L"False"},
        {L"SystemSettings", L"Distortion", L"False"},
        {L"SystemSettings", L"FilteredDistortion", L"False"},
        {L"SystemSettings", L"LensFlares", L"False"},
        {L"SystemSettings", L"Bloom", L"False"},
        {L"SystemSettings", L"bUseFilmGrain", L"False"},
        {L"SystemSettings", L"DynamicDecals", L"False"},          // lighter, as played and tested
        {L"SystemSettings", L"DetailMode", L"1"},
        {L"SystemSettings", L"MaxShadows", L"2"},
        {L"SystemSettings", L"WindowMode", L"1"},
    };
    int changed = 0;
    for (auto& e : set) {
        wchar_t cur[64]; GetPrivateProfileStringW(e[0], e[1], L"", cur, 64, ini);
        if (!_wcsicmp(cur, e[2])) continue;
        if (!changed && !Exists(bak)) { CopyFileW(ini, bak, TRUE); Log("Game settings: backup saved as PecanEngine.ini.acmvr-backup"); }
        WritePrivateProfileStringW(e[0], e[1], e[2], ini);
        char a[160]; _snprintf(a, sizeof(a), "%ls %ls -> %ls", e[1], cur[0] ? cur : L"(missing)", e[2]); Log("Game settings: %s", a);
        changed++;
    }
    if (!changed) Log("Game settings: already set for VR");
}

extern "C" int WINAPI LauncherMain() {
    GetModuleFileNameW(NULL, g_dir, MAX_PATH);
    wchar_t* sl = wcsrchr(g_dir, L'\\'); if (sl) sl[1] = 0;
    _snwprintf(g_ini, MAX_PATH, L"%sacmvr.ini", g_dir);
    wchar_t lp[MAX_PATH]; _snwprintf(lp, MAX_PATH, L"%sacmvr_launcher.log", g_dir);
    g_log = _wfopen(lp, L"w");
    Log("ACM-VR launcher");

    wchar_t dll[MAX_PATH]; _snwprintf(dll, MAX_PATH, L"%sacmvr.dll", g_dir);
    if (!Exists(dll)) { Fail(L"acmvr.dll is missing - it must be in the same folder as the launcher."); ExitProcess(1); }

    static wchar_t exe[MAX_PATH], args[2048], cmd[4096];
    ParseCommandLine(exe, args);
    if (!exe[0] && !FindGame(exe)) {
        Fail(L"Couldn't find ACM.exe.\n\nStart the launcher from Steam with the launch option\n    \"<path to>\\acmvr_launcher.exe\" %command%\nor set GamePath= under [Launcher] in acmvr.ini.");
        ExitProcess(1);
    }

    // game arguments: what Steam passed, plus our defaults
    wchar_t extra[512];
    GetPrivateProfileStringW(L"Launcher", L"ExtraArgs", L"-windowed -nostartupmovies", extra, 512, g_ini);
    wchar_t full[3072]; wcscpy(full, args);
    for (wchar_t* t = extra; *t;) {   // add each default switch unless it's already there
        while (*t == L' ') t++;
        wchar_t* e = t; while (*e && *e != L' ') e++;
        wchar_t sw[128]; int n = (int)(e - t); if (n > 127) n = 127;
        wcsncpy(sw, t, n); sw[n] = 0;
        if (n && !HasArgI(full, sw)) { if (full[0]) wcscat(full, L" "); wcscat(full, sw); }
        t = e;
    }
    bool autoRes = GetPrivateProfileIntW(L"Launcher", L"AutoResolution", 1, g_ini) != 0;
    g_fixedRes = HasArgI(full, L"-ResX") || !autoRes;
    if (!g_fixedRes) VrRenderSize(&g_vrW, &g_vrH);
    g_scale = GetPrivateProfileIntW(L"Launcher", L"ResolutionScale", 100, g_ini);
    if (g_scale < 50) g_scale = 50; if (g_scale > 150) g_scale = 150;
    g_overlayWarn = GetPrivateProfileIntW(L"Launcher", L"SteamOverlaySeen", 0, g_ini) != 0;   // reported by the mod last session
    bool show = GetPrivateProfileIntW(L"Launcher", L"ShowWindow", 1, g_ini) != 0 || (GetAsyncKeyState(VK_SHIFT) & 0x8000) || g_overlayWarn;
    if (show) {
        if (!RunLauncherWindow()) { Log("Cancelled"); if (g_log) fclose(g_log); ExitProcess(0); }
        wchar_t v[16]; _snwprintf(v, 16, L"%d", g_scale);
        WritePrivateProfileStringW(L"Launcher", L"ResolutionScale", v, g_ini);
        if (g_skipNext) WritePrivateProfileStringW(L"Launcher", L"ShowWindow", L"0", g_ini);
    }
    if (!g_fixedRes && g_vrW) {
        UINT w, h; EyeSize(g_scale, &w, &h);
        wchar_t r[64]; _snwprintf(r, 64, L" -ResX=%u -ResY=%u", w * 2, h);   // both eyes side by side
        wcscat(full, r);
        Log("Resolution %d%% of SteamVR's %ux%u -> %ux%u per eye", g_scale, g_vrW, g_vrH, w, h);
    }
    TuneEngineIni();
    _snwprintf(cmd, 4096, L"\"%s\" %s", exe, full);
    wchar_t wd[MAX_PATH]; wcscpy(wd, exe); wchar_t* ws = wcsrchr(wd, L'\\'); if (ws) *ws = 0;
    { char a[4096]; WideCharToMultiByte(CP_UTF8, 0, cmd, -1, a, sizeof(a), NULL, NULL); Log("Starting: %s", a); }

    // Make SteamVR (and the Steam API) see the game as the Steam app even when started outside Steam,
    // so per-game SteamVR settings such as Motion Smoothing apply.
    {
        wchar_t id[32] = L"";
        wchar_t ap[MAX_PATH]; wcscpy(ap, wd); wcscat(ap, L"\\steam_appid.txt");
        FILE* f = _wfopen(ap, L"r");
        if (f) { char b[16] = ""; fgets(b, sizeof(b), f); fclose(f); for (int i = 0; b[i] >= '0' && b[i] <= '9' && i < 15; i++) { id[i] = b[i]; id[i + 1] = 0; } }
        if (!id[0]) wcscpy(id, L"49540");
        wchar_t cur[32];
        if (!GetEnvironmentVariableW(L"SteamAppId", cur, 32)) SetEnvironmentVariableW(L"SteamAppId", id);
        if (!GetEnvironmentVariableW(L"SteamGameId", cur, 32)) SetEnvironmentVariableW(L"SteamGameId", id);
        // the Steam overlay connects to SteamVR from inside the game and takes the controllers / pauses the game
        if (GetPrivateProfileIntW(L"Launcher", L"BlockSteamOverlay", 1, g_ini)) {
            SetEnvironmentVariableW(L"SteamNoOverlayUIDrawing", L"1");
            Log("Steam overlay drawing disabled for the game");
        }
        char a[32]; WideCharToMultiByte(CP_UTF8, 0, id, -1, a, 32, NULL, NULL); Log("Steam app id %s", a);
    }
    STARTUPINFOW si; memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, wd, &si, &pi)) {
        DWORD err = GetLastError();
        // 740: the game is set to "Run as administrator" (Compatibility tab). A non-admin launcher can't start it
        // or load the mod into it, so start ourselves again as administrator with the same arguments.
        if (err == ERROR_ELEVATION_REQUIRED && !IsElevated()) {
            Log("The game needs administrator rights (error 740) - restarting the launcher as administrator");
            typedef HINSTANCE(WINAPI * ShellExecuteW_t)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
            HMODULE sh = LoadLibraryW(L"shell32.dll");
            ShellExecuteW_t se = sh ? (ShellExecuteW_t)GetProcAddress(sh, "ShellExecuteW") : NULL;
            wchar_t self[MAX_PATH]; GetModuleFileNameW(NULL, self, MAX_PATH);
            const wchar_t* c = GetCommandLineW();
            if (*c == L'"') { c++; while (*c && *c != L'"') c++; if (*c) c++; }
            else while (*c && *c != L' ' && *c != L'\t') c++;
            while (*c == L' ' || *c == L'\t') c++;
            if (se && (INT_PTR)se(NULL, L"runas", self, c, NULL, SW_SHOWNORMAL) > 32) ExitProcess(0);
            Log("Restart as administrator was cancelled or failed");
        }
        SetLastError(err);
        wchar_t m[600]; _snwprintf(m, 600, L"Couldn't start the game (error %lu):\n%s%s", GetLastError(), exe, GetLastError() == 740 ? L"\n\nThe game is set to run as administrator. Untick \"Run this program as an administrator\" on ACM.exe (Properties > Compatibility), or allow the launcher to run as administrator." : L"");
        Fail(m); ExitProcess(1);
    }
    // Load the mod in the game process: queue LoadLibraryW on the game's first thread. It runs once Windows has
    // loaded the game's own DLLs, before any of the game's code.
    SIZE_T bytes = (wcslen(dll) + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(pi.hProcess, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ok = remote && WriteProcessMemory(pi.hProcess, remote, dll, bytes, NULL);
    FARPROC ll = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    ok = ok && ll && QueueUserAPC((PAPCFUNC)ll, pi.hThread, (ULONG_PTR)remote);
    if (!ok) {
        wchar_t m[200]; _snwprintf(m, 200, L"Couldn't load the VR mod into the game (error %lu).", GetLastError());
        Fail(m); TerminateProcess(pi.hProcess, 1); ExitProcess(1);
    }
    ResumeThread(pi.hThread);
    Log("Game started (pid %lu); waiting for it to exit", pi.dwProcessId);
    // stay alive while the game runs, so Steam sees the game as running
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    Log("Game exited (code %lu)", code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    if (g_log) fclose(g_log);
    ExitProcess(code);
    return 0;
}
