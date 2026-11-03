// acmvr_floorfix.exe - sets SteamVR's floor height from a controller lying on the real floor.
// Start SteamVR, run this, lay ONE controller flat on the floor, keep the headset tracking, press Escape.
// It moves SteamVR's standing origin up or down so the controller reads its true resting height,
// saves the change to SteamVR's live room setup, then measures the headset height again.
// The previous setting is written to acmvr_floorfix.txt; SteamVR / headset room setup can always redo the floor.
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <openvr.h>

static const double kRestHeight = 0.04;   // tracking centre of a controller lying flat, metres above the floor

extern "C" int WINAPI FloorFixMain() {
    wchar_t dir[MAX_PATH], p[MAX_PATH], out[MAX_PATH];
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    wchar_t* sl = wcsrchr(dir, L'\\'); if (sl) sl[1] = 0;
    _snwprintf(p, MAX_PATH, L"%sopenvr_api.dll", dir);
    _snwprintf(out, MAX_PATH, L"%sacmvr_floorfix.txt", dir);
    HMODULE ovr = LoadLibraryW(p);
    if (!ovr) { MessageBoxW(NULL, L"openvr_api.dll not found - put this program in the ACMVR folder.", L"ACM-VR floor fix", MB_ICONERROR); ExitProcess(1); }
    typedef uint32_t (*Init_t)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
    typedef void* (*GetIf_t)(const char*, vr::EVRInitError*);
    typedef void (*Shut_t)();
    Init_t init = (Init_t)GetProcAddress(ovr, "VR_InitInternal2");
    GetIf_t gi = (GetIf_t)GetProcAddress(ovr, "VR_GetGenericInterface");
    Shut_t shut = (Shut_t)GetProcAddress(ovr, "VR_ShutdownInternal");
    if (!init || !gi || !shut) ExitProcess(1);
    vr::EVRInitError e = vr::VRInitError_None;
    init(&e, vr::VRApplication_Background, NULL);
    if (e != vr::VRInitError_None) { MessageBoxW(NULL, L"SteamVR isn't running. Start SteamVR first, then run this again.", L"ACM-VR floor fix", MB_ICONERROR); ExitProcess(1); }
    vr::IVRSystem* sys = (vr::IVRSystem*)gi(vr::IVRSystem_Version, &e);
    vr::IVRChaperoneSetup* cs = (vr::IVRChaperoneSetup*)gi(vr::IVRChaperoneSetup_Version, &e);
    FILE* f = _wfopen(out, L"w");
    if (!sys || !cs) { if (f) { fputs("SteamVR room-setup interface not available.\n", f); fclose(f); } shut(); ExitProcess(1); }
    MessageBeep(MB_OK);
    while (GetAsyncKeyState(VK_ESCAPE) & 0x8000) Sleep(20);
    bool pressed = false;
    for (int i = 0; i < 15 * 60 * 50 && !pressed; i++) { if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) pressed = true; else Sleep(20); }
    if (!pressed) { if (f) { fputs("No Escape press within 15 minutes - nothing changed.\n", f); fclose(f); } shut(); ExitProcess(0); }

    // the lower controller is the one on the floor; average it for half a second
    auto measure = [&](double* ctl, double* hmd) -> bool {
        double s[2] = {0, 0}, sh = 0; int n[2] = {0, 0}, nh = 0;
        for (int k = 0; k < 25; k++) {
            vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
            sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
            for (int c = 0; c < 2; c++) {
                vr::TrackedDeviceIndex_t idx = sys->GetTrackedDeviceIndexForControllerRole(c == 0 ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
                if (idx < vr::k_unMaxTrackedDeviceCount && poses[idx].bPoseIsValid) { s[c] += poses[idx].mDeviceToAbsoluteTracking.m[1][3]; n[c]++; }
            }
            if (poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid) { sh += poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking.m[1][3]; nh++; }
            Sleep(20);
        }
        bool any = false; double lo = 1e9;
        for (int c = 0; c < 2; c++) if (n[c]) { double y = s[c] / n[c]; if (y < lo) lo = y; any = true; }
        *ctl = lo; *hmd = nh ? sh / nh : -99;
        return any;
    };
    double ctl, hmd;
    if (!measure(&ctl, &hmd)) { if (f) { fputs("No controller was tracked - nothing changed.\n", f); fclose(f); } shut(); ExitProcess(0); }
    double shift = ctl - kRestHeight;   // how far SteamVR's floor is above the real one (negative = below)
    cs->RevertWorkingCopy();
    vr::HmdMatrix34_t m;
    if (!cs->GetWorkingStandingZeroPoseToRawTrackingPose(&m)) { if (f) { fputs("Couldn't read SteamVR's floor setting - nothing changed.\n", f); fclose(f); } shut(); ExitProcess(1); }
    if (f) {
        fprintf(f, "Before: lowest controller %.3f m, headset %.3f m above SteamVR's floor\n", ctl, hmd);
        fprintf(f, "Previous standing origin (raw tracking space): %.4f %.4f %.4f\n", m.m[0][3], m.m[1][3], m.m[2][3]);
        fprintf(f, "Moving SteamVR's floor by %.1f cm %s\n", -shift * 100, shift > 0 ? "(down)" : "(up)");
    }
    m.m[1][3] += (float)shift;
    cs->SetWorkingStandingZeroPoseToRawTrackingPose(&m);
    bool ok = cs->CommitWorkingCopy(vr::EChaperoneConfigFile_Live);
    Sleep(500);
    double ctl2, hmd2;
    measure(&ctl2, &hmd2);
    if (f) {
        fprintf(f, "Saved to SteamVR: %s\n", ok ? "yes" : "NO (SteamVR refused the change)");
        fprintf(f, "After: lowest controller %.3f m, headset %.3f m above the floor\n", ctl2, hmd2);
        fclose(f);
    }
    MessageBeep(ok ? MB_ICONASTERISK : MB_ICONHAND);
    shut();
    ExitProcess(0);
}
