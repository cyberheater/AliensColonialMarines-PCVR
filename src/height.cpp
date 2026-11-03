// acmvr_height.exe - measures how high the headset is above SteamVR's floor when you press Escape.
// Put it in the ACMVR folder (it uses openvr_api.dll from there), start SteamVR, run it, put the headset on
// and press Escape. It beeps, writes acmvr_height.txt next to itself, and exits.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <openvr.h>

static wchar_t g_dir[MAX_PATH];

extern "C" int WINAPI HeightMain() {
    GetModuleFileNameW(NULL, g_dir, MAX_PATH);
    wchar_t* sl = wcsrchr(g_dir, L'\\'); if (sl) sl[1] = 0;
    wchar_t p[MAX_PATH], out[MAX_PATH];
    _snwprintf(p, MAX_PATH, L"%sopenvr_api.dll", g_dir);
    _snwprintf(out, MAX_PATH, L"%sacmvr_height.txt", g_dir);
    HMODULE ovr = LoadLibraryW(p);
    if (!ovr) { MessageBoxW(NULL, L"openvr_api.dll not found - put this program in the ACMVR folder.", L"ACM-VR height", MB_ICONERROR); ExitProcess(1); }
    typedef uint32_t (*Init_t)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
    typedef void* (*GetIf_t)(const char*, vr::EVRInitError*);
    typedef void (*Shut_t)();
    Init_t init = (Init_t)GetProcAddress(ovr, "VR_InitInternal2");
    GetIf_t gi = (GetIf_t)GetProcAddress(ovr, "VR_GetGenericInterface");
    Shut_t shut = (Shut_t)GetProcAddress(ovr, "VR_ShutdownInternal");
    if (!init || !gi || !shut) { MessageBoxW(NULL, L"openvr_api.dll is not usable.", L"ACM-VR height", MB_ICONERROR); ExitProcess(1); }
    vr::EVRInitError e = vr::VRInitError_None;
    init(&e, vr::VRApplication_Background, NULL);
    if (e != vr::VRInitError_None) { MessageBoxW(NULL, L"SteamVR isn't running. Start SteamVR first, then run this again.", L"ACM-VR height", MB_ICONERROR); ExitProcess(1); }
    vr::IVRSystem* sys = (vr::IVRSystem*)gi(vr::IVRSystem_Version, &e);
    if (!sys) { shut(); ExitProcess(1); }
    MessageBeep(MB_OK);
    // wait for Escape (up to 15 minutes); a key already held when we start doesn't count
    while (GetAsyncKeyState(VK_ESCAPE) & 0x8000) Sleep(20);
    bool pressed = false;
    for (int i = 0; i < 15 * 60 * 50 && !pressed; i++) { if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) pressed = true; else Sleep(20); }
    FILE* f = _wfopen(out, L"w");
    if (!pressed) { if (f) { fputs("No Escape press within 15 minutes.\n", f); fclose(f); } shut(); ExitProcess(0); }
    // average the head height over half a second so a small movement doesn't skew it
    double sum = 0, mn = 1e9, mx = -1e9, ctl[2] = {0, 0}; int n = 0, nc[2] = {0, 0};
    for (int k = 0; k < 25; k++) {
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
        sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
        const vr::TrackedDevicePose_t& h = poses[vr::k_unTrackedDeviceIndex_Hmd];
        if (h.bPoseIsValid) { double y = h.mDeviceToAbsoluteTracking.m[1][3]; sum += y; n++; if (y < mn) mn = y; if (y > mx) mx = y; }
        for (int c = 0; c < 2; c++) {
            vr::TrackedDeviceIndex_t idx = sys->GetTrackedDeviceIndexForControllerRole(c == 0 ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
            if (idx < vr::k_unMaxTrackedDeviceCount && poses[idx].bPoseIsValid) { ctl[c] += poses[idx].mDeviceToAbsoluteTracking.m[1][3]; nc[c]++; }
        }
        Sleep(20);
    }
    if (f) {
        if (n) {
            double y = sum / n;
            fprintf(f, "Headset height above SteamVR's floor: %.3f m (%.1f cm), varied %.1f cm over 0.5 s\n", y, y * 100, (mx - mn) * 100);
            fprintf(f, "Estimated standing height (eyes are about 11 cm below the top of the head): %.2f m\n", y + 0.11);
        } else fputs("The headset wasn't tracked when Escape was pressed.\n", f);
        for (int c = 0; c < 2; c++)
            if (nc[c]) fprintf(f, "%s controller height: %.3f m\n", c == 0 ? "Left" : "Right", ctl[c] / nc[c]);
        fclose(f);
    }
    MessageBeep(MB_ICONASTERISK);
    shut();
    ExitProcess(0);
}
