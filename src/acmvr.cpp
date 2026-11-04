// ACM-VR: PCVR mod for Aliens: Colonial Marines (Steam, UE3, DX9, 32-bit).
// Delivered as a d3d9.dll proxy placed next to ACM.exe.
//
//  * Stereo: alternate-eye rendering (AFR). Each game frame renders one eye;
//    the eye offset and head pose are injected into the camera through a
//    vtable hook on APecanPlayerController::GetPlayerViewPoint (slot 0x494).
//  * The renderer's call (from ULocalPlayer::CalcSceneView) is recognised by
//    its return address, so view = head while gameplay aim = hand.
//  * FOV is forced for the render path (GetFOVAngle, slot 0x5D4) and the HMD's
//    asymmetric frustum is cropped out of the wider image via texture bounds.
//  * Frames are copied D3D9 -> system memory -> D3D11 and submitted to OpenVR.
//  * Menus / loading screens are shown on a virtual screen (OpenVR overlay).
//  * VR controllers are exposed to the game as an Xbox pad (XInput IAT hook),
//    with snap turning, button combos for Start/Back/recentre, and haptics.

#define WIN32_LEAN_AND_MEAN
#define __USE_MINGW_ANSI_STDIO 0
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>
#include <xinput.h>
#include <tlhelp32.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include "openvr.h"

// ---- DXVK D3D9 <-> Vulkan interop (from DXVK's d3d9_interfaces.h) ----
struct ID3D9VkInteropTexture : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(VkImage* pHandle, VkImageLayout* pLayout, VkImageCreateInfo* pInfo) = 0;
};
struct ID3D9VkInteropDevice : public IUnknown {
    virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance* pInstance, VkPhysicalDevice* pPhysDev, VkDevice* pDevice) = 0;
    virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue* pQueue, uint32_t* pQueueIndex, uint32_t* pQueueFamilyIndex) = 0;
    virtual void STDMETHODCALLTYPE TransitionTextureLayout(ID3D9VkInteropTexture* pTexture, const VkImageSubresourceRange* pSubresources, VkImageLayout OldLayout, VkImageLayout NewLayout) = 0;
    virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
    virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE LockDevice() = 0;
    virtual void STDMETHODCALLTYPE UnlockDevice() = 0;
    virtual bool STDMETHODCALLTYPE WaitForResource(IDirect3DResource9* pResource, DWORD MapFlags) = 0;
};
static const GUID kIID_ID3D9VkInteropTexture = {0xd56344f5, 0x8d35, 0x46fd, {0x80, 0x6d, 0x94, 0xc3, 0x51, 0xb4, 0x72, 0xc1}};
static const GUID kIID_ID3D9VkInteropDevice  = {0x2eaa4b89, 0x0107, 0x4bdb, {0x87, 0xf7, 0x0f, 0x54, 0x1c, 0x49, 0x3c, 0xe0}};

#define THISCALL __attribute__((thiscall))
#define ACMVR_VERSION "0.30.4"

// ---------------------------------------------------------------------------
// Basic types / math
// ---------------------------------------------------------------------------
struct FVector { float X, Y, Z; };
struct FRotator { int Pitch, Yaw, Roll; };
struct V3 { float x, y, z; };
struct Basis { V3 f, r, u; };  // forward, right, up (UE axes)

static inline V3 mk(float x, float y, float z) { V3 v = {x, y, z}; return v; }
static inline V3 add(V3 a, V3 b) { return mk(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 sub(V3 a, V3 b) { return mk(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 mul(V3 a, float s) { return mk(a.x * s, a.y * s, a.z * s); }
static inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// msvcrt.dll on x86 only has the double versions; keep everything in double.
static volatile double g_mathGuard = 0.0;  // prevents float-libcall shrinking
static inline float fsin(float a) { return (float)sin((double)a + g_mathGuard); }
static inline float fcos(float a) { return (float)cos((double)a + g_mathGuard); }
static inline float fatan2(float y, float x) { return (float)atan2((double)y + g_mathGuard, (double)x); }
static inline float fsqrt(float a) { return (float)sqrt((double)a + g_mathGuard); }
static inline float ftan(float a) { return (float)tan((double)a + g_mathGuard); }
static inline float fatan(float a) { return (float)atan((double)a + g_mathGuard); }
static inline float fabsf_(float a) { return a < 0 ? -a : a; }
static inline int iround(float f) { return (int)(f < 0 ? f - 0.5f : f + 0.5f); }

static const float PI_F = 3.14159265358979f;
static const float R2U = 32768.0f / PI_F;   // radians -> UE rotator units
static const float U2R = PI_F / 32768.0f;

// RotY in OpenVR (right-handed, Y up)
static inline V3 rotYvr(V3 v, float a) {
    float c = fcos(a), s = fsin(a);
    return mk(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}
// OpenVR (x right, y up, -z forward) -> UE (x forward, y right, z up)
static inline V3 vrToUe(V3 v) { return mk(-v.z, v.x, v.y); }

// UE3 FRotationMatrix axes
static Basis RotToBasis(FRotator r) {
    float p = r.Pitch * U2R, y = r.Yaw * U2R, ro = r.Roll * U2R;
    float SP = fsin(p), CP = fcos(p), SY = fsin(y), CY = fcos(y), SR = fsin(ro), CR = fcos(ro);
    Basis b;
    b.f = mk(CP * CY, CP * SY, SP);
    b.r = mk(SR * SP * CY - CR * SY, SR * SP * SY + CR * CY, -SR * CP);
    b.u = mk(-(CR * SP * CY + SR * SY), CY * SR - CR * SP * SY, CR * CP);
    return b;
}
// UE3 FMatrix::Rotator
static FRotator BasisToRot(const Basis& b) {
    float pitch = fatan2(b.f.z, fsqrt(b.f.x * b.f.x + b.f.y * b.f.y));
    float yaw = fatan2(b.f.y, b.f.x);
    V3 sy = mk(-fsin(yaw), fcos(yaw), 0);
    float roll = fatan2(dot(b.u, sy), dot(b.r, sy));
    FRotator r = {iround(pitch * R2U), iround(yaw * R2U), iround(roll * R2U)};
    return r;
}
static inline V3 toWorld(const Basis& B, V3 v) { return add(add(mul(B.f, v.x), mul(B.r, v.y)), mul(B.u, v.z)); }
static inline Basis toWorldB(const Basis& B, const Basis& l) {
    Basis o; o.f = toWorld(B, l.f); o.r = toWorld(B, l.r); o.u = toWorld(B, l.u); return o;
}
static inline int wrap16(int d) { return (int)(short)(d & 0xFFFF); }

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static HMODULE g_self;
static DWORD g_base;               // ACM.exe module base
static char g_dir[MAX_PATH];        // directory of this DLL (with trailing '\')

struct Config {
    int enabled, positional, snapTurnDeg, controllerAim, aimHand, aimTraceFromHand, keepShake, swapEyes;
    int haptics, vrControllers, logCallers, cinematicPitch, forceNoVsync, gameHooks, screenOnly;
    int menusOnScreen, cutscenesOnScreen, largeWindow, dpiAware, weaponFollowsHand, learnHeadCallers, screenCrop;
    float gunX, gunY, gunZ, armsX, armsY, armsZ, corrPitch, corrYaw, gunScale;
    int corrOnController;
    int armsDirect, hideCrosshair, hideHUD, fastCopy, autoCapture, showMessages, showFps, gpuWait, offAxis, offAxisV, asyncSubmit, frameCap, useDXVK, noSteamVkLayers, dxvkVulkanSubmit, vkSubmitPose, autoPlay, vkPlainEyes, vkColorSpace, vkReportSrgb, calibTap, leftHandTracking, wheelRightStick, handsOnly, lockGun, noLensEffects, alignRightHand, hudScanlineOff, taa, levelGun, dummyUse, keepFocus, earlyVr, hideIdleLeftHand, dpadOnRightClick, extraButtonsMenu, gunUpAxis, stickUpDown, heightOnGameplay, faceChords, autoResume, keepForeground, focusNudge, inputLog, hideUpperArms, dynRes, dynMin, dynMax, boostThreads, moveWithHead, overlaysOnScreen, objectivesOnScreen, wristTwist;
    float objDown, objScale, objBack, hudScale, objRight, hudCX, hudCY;
    float dynTargetGpu, lhOff[3], rhOff[3];
    char postFxOff[256], postFxOn[64];
    float taaBlend, taaJitter, useRange, useWidth;
    char hideBones[512];
    float frameCapRatio;
    char headCallers[512];
    float screenCurve;
    float worldScale, ipdScale, forceFov, fovMargin, aimPitchOffset, cinematicThreshold, screenDist, screenWidth;
    DWORD rvaPecanPVP, rvaPecanFOV, rvaRenderPVPRet, rvaRenderFOVRet, slotPVP, slotFOV, offRotation;
};
static Config g_cfg;

// logging
static CRITICAL_SECTION g_logCs;
static FILE* g_log;
static void Log(const char* fmt, ...) {
    if (!g_log) return;
    char buf[1024];
    va_list ap; va_start(ap, fmt); _vsnprintf(buf, sizeof(buf) - 1, fmt, ap); va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    EnterCriticalSection(&g_logCs);
    fprintf(g_log, "[%8lu] %s\n", (unsigned long)GetTickCount(), buf);
    fflush(g_log);
    LeaveCriticalSection(&g_logCs);
}

// ---- crash catcher: logs faults on any thread (with module+offset) and writes a minidump ----
static volatile DWORD g_vrThreadId, g_renderThreadId, g_gameThreadId;
static volatile LONG g_faultsLogged, g_dumpWritten;
static void AddrToModule(void* a, char* out, int n) {
    HMODULE m = NULL; char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m &&
        GetModuleFileNameA(m, path, MAX_PATH)) {
        const char* b = strrchr(path, '\\'); b = b ? b + 1 : path;
        _snprintf(out, n, "%s+0x%lX", b, (unsigned long)((char*)a - (char*)m));
    } else _snprintf(out, n, "%p", a);
    out[n - 1] = 0;
}
typedef BOOL(WINAPI* MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
struct MdExceptionInfo { DWORD ThreadId; EXCEPTION_POINTERS* ExceptionPointers; BOOL ClientPointers; };
static void WriteOwnDump(EXCEPTION_POINTERS* ep) {
    if (InterlockedExchange(&g_dumpWritten, 1)) return;
    HMODULE dh = LoadLibraryA("dbghelp.dll");
    MiniDumpWriteDump_t fn = dh ? (MiniDumpWriteDump_t)GetProcAddress(dh, "MiniDumpWriteDump") : NULL;
    if (!fn) return;
    char path[MAX_PATH]; lstrcpyA(path, g_dir); lstrcatA(path, "acmvr_crash.dmp");
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    MdExceptionInfo mi = {GetCurrentThreadId(), ep, FALSE};
    fn(GetCurrentProcess(), GetCurrentProcessId(), f, 0x00000001 /*WithDataSegs*/ | 0x00000040 /*WithIndirectlyReferencedMemory*/, &mi, NULL, NULL);
    CloseHandle(f);
}
static void Log(const char* fmt, ...);
static LONG CALLBACK FaultHandler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != 0xC0000409 /*stack buffer overrun / fast fail*/ && code != 0xC0000374 /*heap corruption*/ &&
        code != 0xE06D7363 /*C++ exception*/)
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_faultsLogged) > 12) return EXCEPTION_CONTINUE_SEARCH;
    DWORD tid = GetCurrentThreadId();
    char where[160]; AddrToModule(ep->ExceptionRecord->ExceptionAddress, where, sizeof(where));
    const char* who = tid == g_vrThreadId ? "VR thread" : tid == g_renderThreadId ? "render thread" : "other thread";
    if (code == EXCEPTION_ACCESS_VIOLATION)
        Log("FAULT 0x%08lX at %s (%s %lu) %s address %p", (unsigned long)code, where, who, (unsigned long)tid,
            ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading", (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    else Log("FAULT 0x%08lX at %s (%s %lu)", (unsigned long)code, where, who, (unsigned long)tid);
    // caller chain (raw stack scan for return addresses into loaded modules)
    if (code != 0xE06D7363) {
        DWORD* sp = (DWORD*)ep->ContextRecord->Esp;
        char line[900]; int len = 0; line[0] = 0;
        for (int i = 0, found = 0; i < 1024 && found < 14; i++) {
            MEMORY_BASIC_INFORMATION mb;
            if (!VirtualQuery(sp + i, &mb, sizeof(mb)) || mb.State != MEM_COMMIT) break;
            void* v = (void*)sp[i];
            if (!VirtualQuery(v, &mb, sizeof(mb)) || !(mb.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) continue;
            char m[96]; AddrToModule(v, m, sizeof(m));
            len += _snprintf(line + len, sizeof(line) - len - 1, " %s", m); found++;
            if (len > (int)sizeof(line) - 100) break;
        }
        Log("  stack:%s", line);
        if (tid != g_renderThreadId || code != EXCEPTION_ACCESS_VIOLATION) WriteOwnDump(ep);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void LogAddressSpace(const char* tag) {
    SIZE_T freeTot = 0, big = 0; char* p = (char*)0x10000;
    MEMORY_BASIC_INFORMATION mb;
    while ((SIZE_T)p < 0xFFFF0000u && VirtualQuery(p, &mb, sizeof(mb))) {
        if (mb.State == MEM_FREE) { freeTot += mb.RegionSize; if (mb.RegionSize > big) big = mb.RegionSize; }
        char* n = (char*)mb.BaseAddress + mb.RegionSize; if (n <= p) break; p = n;
    }
    Log("Address space %s: %lu MB free, largest block %lu MB", tag, (unsigned long)(freeTot >> 20), (unsigned long)(big >> 20));
}
static volatile LONG g_vkTrace = 6;   // step markers for the first few Vulkan frames

// real d3d9
static HMODULE g_realD3D9;
typedef IDirect3D9*(WINAPI* Direct3DCreate9_t)(UINT);
typedef HRESULT(WINAPI* Direct3DCreate9Ex_t)(UINT, IDirect3D9Ex**);
typedef int(WINAPI* PERF_BeginEvent_t)(D3DCOLOR, LPCWSTR);
typedef int(WINAPI* PERF_EndEvent_t)(void);
typedef void(WINAPI* PERF_SetMarker_t)(D3DCOLOR, LPCWSTR);
typedef void(WINAPI* PERF_SetRegion_t)(D3DCOLOR, LPCWSTR);
typedef BOOL(WINAPI* PERF_QueryRepeatFrame_t)(void);
typedef void(WINAPI* PERF_SetOptions_t)(DWORD);
typedef DWORD(WINAPI* PERF_GetStatus_t)(void);
static Direct3DCreate9_t p_Create9;
static Direct3DCreate9Ex_t p_Create9Ex;
static PERF_BeginEvent_t p_BeginEvent;
static PERF_EndEvent_t p_EndEvent;
static PERF_SetMarker_t p_SetMarker;
static PERF_SetRegion_t p_SetRegion;
static PERF_QueryRepeatFrame_t p_QueryRepeatFrame;
static PERF_SetOptions_t p_SetOptions;
static PERF_GetStatus_t p_GetStatus;

// D3D9 hooks
typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT(STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE* SCPresent_t)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
static CreateDevice_t o_CreateDevice;
static Reset_t o_Reset;
static Present_t o_Present;
static SCPresent_t o_SCPresent;
static volatile LONG g_inPresent;

// D3D9-side copy resources
static IDirect3DSurface9* g_rt;     // default-pool RT (resolves MSAA / format)
static IDirect3DSurface9* g_sys;    // system-memory readback surface
static UINT g_bbW, g_bbH;
static volatile LONG g_bbAspectMilli;   // W/H * 1000, read by game thread

// D3D11 / OpenVR
static ID3D11Device* g_d11;
static ID3D11DeviceContext* g_ctx;
static ID3D11Texture2D* g_eyeTex[2];
static ID3D11Texture2D* g_screenTex;
static ID3D11Texture2D* g_blackTex;   // submitted to the eyes while the virtual screen is shown
static UINT g_texW, g_texH;
static int g_eyeHas[2];

static HMODULE g_ovrDll;
static vr::IVRSystem* g_sys_vr;
static vr::IVRCompositor* g_comp;
static vr::IVROverlay* g_ovl;
static vr::VROverlayHandle_t g_overlay;
static int g_overlayShown;
static volatile LONG g_vrOK;          // VR initialised (render thread)
static volatile LONG g_vrActive;      // poses flowing
static volatile LONG g_stereoOn = 1;  // F9 toggles
static volatile LONG g_swapHotkey;    // F7 toggles
static volatile LONG g_recenterReq = 1;
static DWORD g_lastStereoTick;
static DWORD g_vrRetryTick;
static float g_projRaw[2][4];         // l r t b
static float g_ipd = 0.064f;
static volatile float g_gameFovDeg;   // forced render FOV (0 = not yet)
static volatile float g_gameTanH;

// pose exchange (render thread -> game/input threads)
struct PoseSnap {
    vr::HmdMatrix34_t hmd, ctrl[2];
    int hmdValid, ctrlValid[2];
    float recYaw; V3 recPos;
};
static CRITICAL_SECTION g_poseCs;
static PoseSnap g_pose;
static volatile LONG g_ctrlIdx[2] = {-1, -1};  // left, right tracked device indices

// eye FIFO (game thread produces, render thread consumes)
static volatile LONG g_eyeRing[8];
static volatile LONG g_eyeHead, g_eyeTail;
static int g_nextEye;

// game camera state (game thread)
typedef void(THISCALL* PVP_t)(void*, FVector*, FRotator*);
typedef float(THISCALL* FOV_t)(void*);
static PVP_t o_PVP;
static FOV_t o_FOV;
static void* g_lastPC;
static int g_baseYaw;
static FRotator g_lastWritten;
static int g_haveWritten;
static volatile LONG g_snapRequest;
static volatile LONG g_aimToggle;     // F10 flips controller aim
static volatile LONG g_posToggle;     // F11 flips positional
static FVector g_handLoc; static FRotator g_handRot; static int g_handValid;
static V3 g_lhandLoc; static Basis g_lhandB; static int g_lhandValid;   // off-hand controller, world space
static unsigned g_renderCalls;
static int g_frameStereo;      // this frame's view was stereo (game thread)
static FVector g_gunCamLoc;     // virtual camera that puts the game's 1st-person gun in the hand
static float g_aimPitch;
static char g_ctlType[48]; static volatile LONG g_ctlTypeKnown;   // kind of controller (SteamVR's controller type)        // runtime-adjustable copies of the ini values
static float g_gunX, g_gunY, g_gunZ;
static float g_worldScale = 50.0f;
static float g_armsX, g_armsY, g_armsZ;
static float g_corrPitch, g_corrYaw;
static float g_gunScale = 1.0f;
static FRotator g_fireRot;   // bullet direction (hand aim + correction)
static volatile float g_moveTurn;   // radians from the gun's direction to the head's (for walking where you look)
static DWORD g_headCallers[64]; static int g_numHeadCallers;
static void AddHeadCaller(DWORD rva) {
    for (int i = 0; i < g_numHeadCallers; i++) if (g_headCallers[i] == rva) return;
    if (g_numHeadCallers < 64) { g_headCallers[g_numHeadCallers++] = rva; Log("Camera caller %06lX marked as engine (head)", (unsigned long)rva); }
}
static bool IsHeadCaller(DWORD rva) {
    for (int i = 0; i < g_numHeadCallers; i++) if (g_headCallers[i] == rva) return true;
    return false;
}
static int g_lastMode = -1;
static volatile LONG g_offAxisActive;   // projection matrices were made off-centre this frame
static float g_offAxisUp = 1, g_offAxisDn = 1;   // up/down tangents the off-centre projection covers
static int g_aimCamFromArms;   // aim camera derived from the arms placement
static void StartCapture(int frames);
// ---- profiler ----
static double QpcMs() {
    static LARGE_INTEGER f; LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}
static double ThreadCpuMs() {
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    ULARGE_INTEGER a, b; a.LowPart = k.dwLowDateTime; a.HighPart = k.dwHighDateTime; b.LowPart = u.dwLowDateTime; b.HighPart = u.dwHighDateTime;
    return (double)(a.QuadPart + b.QuadPart) / 10000.0;
}
struct ProfRT { double frame, gameRender, copy, submit, wait, present, gpu; int n, gpuN; double lastEnd, cpu0, wall0; double gpuMax, drawMax; };
static float g_ft[1024]; static int g_ftN;          // per-frame frame times this report
// SteamVR compositor's own per-frame timing (filled on the VR thread)
struct VrTiming { int n, reused, cpuRepro, gpuRepro, motion, dropped, mispresented; double appGpu, appGpuMax, compGpu, interval, intervalMax, ready; };
static VrTiming g_vt; static CRITICAL_SECTION g_vtCs;
static ProfRT g_prt;
static volatile double g_refreshMs;
static volatile float g_gpuEma = -1, g_drawEma = -1;   // smoothed per-frame GPU / render-thread times (ms)
static volatile LONG g_screenPct = 100;                 // current dynamic-resolution ScreenPercentage
static volatile double g_gtFrame, g_gtCsv, g_gtCpu;   // game-thread figures (latest report)
static volatile LONG g_perfOverlay;                    // F4
// ---- in-headset message panel ----
static CRITICAL_SECTION g_msgCs;
static char g_msgText[256];
static volatile LONG g_msgSerial;
static DWORD g_msgExpire;
static void ShowMessage(DWORD ms, const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt); _vsnprintf(buf, sizeof(buf) - 1, fmt, ap); va_end(ap);
    buf[sizeof(buf) - 1] = 0;
    EnterCriticalSection(&g_msgCs);
    lstrcpyA(g_msgText, buf);
    g_msgExpire = GetTickCount() + ms;
    InterlockedIncrement(&g_msgSerial);
    LeaveCriticalSection(&g_msgCs);
}
static int g_inMainMenu;
static int g_inSBS;          // inside a side-by-side CalcSceneView pair (game thread)

// XInput
typedef DWORD(WINAPI* XGetState_t)(DWORD, XINPUT_STATE*);
typedef DWORD(WINAPI* XSetState_t)(DWORD, XINPUT_VIBRATION*);
static XGetState_t o_XGetState;
static XSetState_t o_XSetState;
static volatile LONG g_motorL, g_motorR;
static DWORD g_packet;
static DWORD g_lastStartTick;
static volatile DWORD g_autoResumeAt; static int g_autoResumeTries;   // press Resume on pauses the player didn't ask for
static volatile SHORT g_sentLX, g_sentLY;   // left stick as last sent to the game
static float g_rawAx[2][5][2]; static uint64_t g_rawBtn[2], g_rawTouch[2]; static float g_rawMaxL[5];   // raw SteamVR controller state (diagnostics)

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static char g_ini[MAX_PATH];
static int IniInt(const char* sec, const char* key, int def) {
    char b[64];
    GetPrivateProfileStringA(sec, key, "", b, sizeof(b), g_ini);
    if (!b[0]) return def;
    return (int)strtol(b, NULL, 0);
}
static float IniFloat(const char* sec, const char* key, float def) {
    char b[64];
    GetPrivateProfileStringA(sec, key, "", b, sizeof(b), g_ini);
    if (!b[0]) return def;
    return (float)atof(b);
}
static void LoadConfig() {
    Config& c = g_cfg;
    c.enabled = IniInt("VR", "Enabled", 1);
    c.screenOnly = IniInt("VR", "ScreenModeOnly", 0);
    c.gameHooks = IniInt("VR", "GameHooks", 1);
    c.worldScale = IniFloat("VR", "WorldScale", 50.0f);
    c.ipdScale = IniFloat("VR", "IPDScale", 1.0f);
    c.positional = IniInt("VR", "PositionalTracking", 1);
    c.forceFov = IniFloat("VR", "ForceFOV", 0.0f);
    c.fovMargin = IniFloat("VR", "FOVMarginPercent", 2.0f);
    c.swapEyes = IniInt("VR", "SwapEyes", 0);
    c.keepShake = IniInt("VR", "KeepCameraShake", 0);
    c.cinematicThreshold = IniFloat("VR", "CinematicThresholdDeg", 25.0f);
    c.cinematicPitch = IniInt("VR", "CinematicFollowPitch", 0);
    c.forceNoVsync = IniInt("VR", "ForceNoVsync", 1);
    c.screenDist = IniFloat("VR", "ScreenDistance", 2.5f);
    c.screenWidth = IniFloat("VR", "ScreenWidth", 5.0f);
    c.screenCurve = IniFloat("VR", "ScreenCurvature", 0.25f);
    c.menusOnScreen = IniInt("VR", "MenusOnScreen", 1);
    c.overlaysOnScreen = IniInt("VR", "OverlaysOnScreen", 1);   // objectives etc. opened during play
    c.objectivesOnScreen = IniInt("VR", "ObjectivesOnScreen", 0);   // 1 = Back (objectives) shows on the flat screen
    c.objDown = IniFloat("VR", "ObjectivesMoveDown", 200.0f);       // objectives panel: move down (Flash stage pixels)
    c.objScale = IniFloat("VR", "ObjectivesScale", 0.7f);           // ... size (1 = game's own)
    c.objBack = IniFloat("VR", "ObjectivesPushBack", 0.0f);         // ... extra depth (Flash 3D z)
    c.hudScale = IniFloat("VR", "HUDScale", 0.7f);                  // in-play HUD (ammo, health...) size, shrunk towards the centre
    c.objRight = IniFloat("VR", "ObjectivesMoveRight", 110.0f);     // objectives panel: move right (keeps it centred after shrinking)
    c.hudCX = IniFloat("VR", "HUDCentreX", 640.0f);                 // centre the HUD shrinks towards (Flash stage units; 1280x720 stage)
    c.hudCY = IniFloat("VR", "HUDCentreY", 360.0f);
    c.cutscenesOnScreen = IniInt("VR", "CutscenesOnScreen", 1);
    c.largeWindow = IniInt("VR", "AllowWindowLargerThanDesktop", 1);
    c.dpiAware = IniInt("VR", "DPIAware", 1);
    c.vrControllers = IniInt("Controls", "VRControllers", 1);
    c.controllerAim = IniInt("Controls", "ControllerAim", 1);
    c.moveWithHead = IniInt("Controls", "MoveWithHead", 1);   // 1 = left stick walks where you look, not where the gun points
    c.aimHand = IniInt("Controls", "AimHand", 1);  // 0 left, 1 right
    c.aimPitchOffset = IniFloat("Controls", "AimPitchOffsetDeg", 0.0f);
    c.aimTraceFromHand = IniInt("Controls", "AimTraceFromHand", 0);
    c.weaponFollowsHand = IniInt("Controls", "WeaponFollowsHand", 1);
    c.armsDirect = IniInt("Controls", "PlaceArmsAtHand", 1);
    c.hideCrosshair = IniInt("VR", "HideCrosshair", 1);
    c.hideHUD = IniInt("VR", "HideHUD", 0);
    c.fastCopy = IniInt("VR", "FastCopy", 1);
    c.gpuWait = IniInt("VR", "WaitForGPU", 0);
    c.offAxis = IniInt("VR", "OffCentreProjection", 1);
    c.offAxisV = IniInt("VR", "VerticalOffCentre", 1);   // 1 = also fit the eye's uneven up/down view (Quest 3)
    c.asyncSubmit = IniInt("VR", "AsyncSubmit", 1);
    c.frameCap = IniInt("VR", "FrameCap", 1);
    c.useDXVK = IniInt("Renderer", "UseDXVK", 1);   // only matters if dxvk_d3d9.dll is present
    c.noSteamVkLayers = IniInt("Renderer", "DisableSteamVulkanLayers", 1);
    c.dxvkVulkanSubmit = IniInt("Renderer", "DXVKVulkanSubmit", 1);   // 0 = try Windows shared textures first (DXVK can't on Windows)
    c.vkSubmitPose = IniInt("Renderer", "VulkanSubmitWithPose", 1);
    c.autoPlay = IniInt("Debug", "AutoPlay", 0);
    c.vkPlainEyes = IniInt("Renderer", "VulkanPlainEyeImages", 1);
    c.vkColorSpace = IniInt("Renderer", "VulkanColorSpace", 1);   // 0 = Auto (old), 1 = Gamma, 2 = Linear
    c.vkReportSrgb = IniInt("Renderer", "VulkanReportSrgb", 1);
    c.hideUpperArms = IniInt("Arms", "HideUpperArms", 1);
    c.handsOnly = IniInt("Arms", "HandsOnly", 1);
    c.lockGun = IniInt("Arms", "LockGunToHand", 1);
    c.noLensEffects = IniInt("Graphics", "RemoveCameraLensEffects", 1);   // blood/damage splats in front of the camera   // also hide the forearms: just the gloved hands
    c.calibTap = IniInt("Controls", "CalibrationOnLeftClick", 0);
    c.leftHandTracking = IniInt("Arms", "LeftHandTracking", 1);
    c.wheelRightStick = IniInt("Controls", "WheelWithRightStick", 1);
    c.lhOff[0] = IniFloat("Arms", "LeftHandOffsetForward", -6.0f);   // cm: wrist sits a little behind the controller grip
    c.lhOff[1] = IniFloat("Arms", "LeftHandOffsetRight", 0.0f);
    c.lhOff[2] = IniFloat("Arms", "LeftHandOffsetUp", 0.0f);
    c.alignRightHand = IniInt("Arms", "RightHandOnController", 1);
    c.wristTwist = IniInt("Arms", "GunFollowsWristTwist", 1);   // 0 = the gun stays level however the wrist turns (old behaviour)
    c.levelGun = IniInt("Arms", "StraightenGunTwist", 1);
    c.keepFocus = IniInt("Controls", "KeepGameFocused", 1);
    c.earlyVr = IniInt("VR", "ConnectEarly", 0);
    c.hideIdleLeftHand = IniInt("Arms", "HideLeftHandWithoutTracker", 1);
    c.gunUpAxis = IniInt("Arms", "GunUpAxis", 0);
    c.dpadOnRightClick = IniInt("Controls", "DPadOnRightStickClick", 0);
    c.stickUpDown = IniInt("Controls", "RightStickFlashlightSidearm", 1);
    c.faceChords = IniInt("Controls", "FaceButtonPairsStartBack", 1);
    c.autoResume = IniInt("Controls", "AutoResumeUnwantedPause", 0);
    c.keepForeground = IniInt("Controls", "KeepGameInFront", 0);
    c.focusNudge = IniInt("Controls", "TellGameItHasFocus", 1);
    c.inputLog = IniInt("Debug", "InputLog", 0);
    c.heightOnGameplay = IniInt("VR", "ResetHeightWhenPlayStarts", 1);   // 1 = first time in a session, 2 = every time
    c.extraButtonsMenu = IniInt("Controls", "OptionsCreateAsStartBack", 1);
    c.useRange = IniFloat("Controls", "UseRangeScale", 1.6f);
    c.useWidth = IniFloat("Controls", "UseWidthScale", 2.0f);
    c.hudScanlineOff = IniInt("Graphics", "HideHudScanlineEffect", 0);
    c.taa = IniInt("Graphics", "TemporalAA", 1);
    c.taaBlend = IniFloat("Graphics", "TemporalAABlend", 0.12f);
    c.taaJitter = IniFloat("Graphics", "TemporalAAJitter", 1.0f);
    { char tp[MAX_PATH]; lstrcpyA(tp, g_dir); lstrcatA(tp, "acmvr_taa.txt");   // test override: "0" / "1"
      FILE* tf = fopen(tp, "r"); if (tf) { char b[8]; if (fgets(b, sizeof(b), tf)) c.taa = atoi(b); fclose(tf); } }
    GetPrivateProfileStringA("Graphics", "DisablePostEffects", "", c.postFxOff, sizeof(c.postFxOff), g_ini);
    GetPrivateProfileStringA("Graphics", "KeepPostEffect", "", c.postFxOn, sizeof(c.postFxOn), g_ini);
    c.rhOff[0] = IniFloat("Arms", "RightHandOffsetForward", -6.0f);
    c.rhOff[1] = IniFloat("Arms", "RightHandOffsetRight", 0.0f);
    c.rhOff[2] = IniFloat("Arms", "RightHandOffsetUp", 0.0f);   // tap the left stick (standing still) to calibrate the gun
    c.dynRes = IniInt("Performance", "DynamicResolution", 0);
    c.boostThreads = IniInt("Performance", "BoostThreads", 1);
    { char tp[MAX_PATH]; lstrcpyA(tp, g_dir); lstrcatA(tp, "acmvr_boost.txt");
      FILE* tf = fopen(tp, "r"); if (tf) { char b[8]; if (fgets(b, sizeof(b), tf)) c.boostThreads = atoi(b); fclose(tf); } }
    c.dynMin = IniInt("Performance", "MinScreenPercentage", 65);
    c.dynMax = IniInt("Performance", "MaxScreenPercentage", 100);
    c.dynTargetGpu = IniFloat("Performance", "TargetGpuMs", 9.3f);
    {   // test override: a file holding a GPU target in ms
        char tp[MAX_PATH]; lstrcpyA(tp, g_dir); lstrcatA(tp, "acmvr_dyntest.txt");
        FILE* tf = fopen(tp, "r");
        if (tf) { char b[32]; if (fgets(b, sizeof(b), tf)) { float v = (float)atof(b); if (v > 1 && v < 30) c.dynTargetGpu = v; } fclose(tf); }
    }
    if (c.dynMin < 30) c.dynMin = 30; if (c.dynMax > 200) c.dynMax = 200; if (c.dynMin > c.dynMax) c.dynMin = c.dynMax;
    GetPrivateProfileStringA("Arms", "HideBones", "Bip01-L-Clavicle,Bip01-L-UpperArm,Bip01-R-Clavicle,Bip01-R-UpperArm",
                             c.hideBones, sizeof(c.hideBones), g_ini);
    {   // test override: a file listing bones to hide
        char tp[MAX_PATH]; lstrcpyA(tp, g_dir); lstrcatA(tp, "acmvr_hidebones.txt");
        FILE* tf = fopen(tp, "r");
        if (tf) { if (fgets(c.hideBones, sizeof(c.hideBones), tf)) { char* nl = strpbrk(c.hideBones, "\r\n"); if (nl) *nl = 0; } fclose(tf); }
    }
    { char ap[MAX_PATH]; lstrcpyA(ap, g_dir); lstrcatA(ap, "acmvr_autoplay.txt"); if (GetFileAttributesA(ap) != INVALID_FILE_ATTRIBUTES) c.autoPlay = 1; }
    c.frameCapRatio = IniFloat("VR", "FrameCapRatio", 1.0f);
    if (c.frameCapRatio < 1.0f || c.frameCapRatio > 3.0f) c.frameCapRatio = 1.25f;
    { char tp[MAX_PATH]; lstrcpyA(tp, g_dir); lstrcatA(tp, "acmvr_framecap.txt");   // test override
      FILE* tf = fopen(tp, "r"); if (tf) { char b[16]; if (fgets(b, sizeof(b), tf)) { float v = (float)atof(b); if (v >= 1.0f && v <= 3.0f) c.frameCapRatio = v; } fclose(tf); } }
    c.autoCapture = IniInt("Debug", "AutoCapture", 1);
    c.showMessages = IniInt("Debug", "ShowMessages", 0);
    c.showFps = IniInt("Debug", "ShowFps", 0);
    c.armsX = IniFloat("Controls", "ArmsOffsetForward", -13.0f);
    c.armsY = IniFloat("Controls", "ArmsOffsetRight", -18.0f);
    c.armsZ = IniFloat("Controls", "ArmsOffsetUp", 28.0f);
    c.corrPitch = IniFloat("Controls", "BulletCorrectionUpDeg", 0.0f);
    c.corrYaw = IniFloat("Controls", "BulletCorrectionRightDeg", 0.0f);
    c.gunScale = IniFloat("Controls", "GunScale", 1.0f);
    c.corrOnController = IniInt("Controls", "BulletCorrectionTurnsController", 0);
    c.learnHeadCallers = IniInt("Debug", "LearnHeadCallers", 0);
    c.screenCrop = IniInt("VR", "ScreenCropTo16x9", 0);
    c.gunX = IniFloat("Controls", "WeaponOffsetForward", 30.0f);
    c.gunY = IniFloat("Controls", "WeaponOffsetRight", -17.0f);
    c.gunZ = IniFloat("Controls", "WeaponOffsetUp", -57.0f);
    GetPrivateProfileStringA("Debug", "HeadCallers", "0xD12EA7,0xE578A3,0x511B11,0x6EA606,0x1B24F1,0xBD7A1E,0xDCFA9E,0x280626",
                             c.headCallers, sizeof(c.headCallers), g_ini);
    c.snapTurnDeg = IniInt("Controls", "SnapTurnDegrees", 30);
    c.haptics = IniInt("Controls", "Haptics", 1);
    c.logCallers = IniInt("Debug", "LogCameraCallers", 1);
    c.rvaPecanPVP = (DWORD)IniInt("Offsets", "PecanGetPlayerViewPoint", 0x7FEF0);
    c.rvaPecanFOV = (DWORD)IniInt("Offsets", "PecanGetFOVAngle", 0x66A4A0);
    c.rvaRenderPVPRet = (DWORD)IniInt("Offsets", "CalcSceneViewPVPReturn", 0xDA1BA2);
    c.rvaRenderFOVRet = (DWORD)IniInt("Offsets", "CalcSceneViewFOVReturn", 0xDA1BC7);
    c.slotPVP = (DWORD)IniInt("Offsets", "VtblGetPlayerViewPoint", 0x494);
    c.slotFOV = (DWORD)IniInt("Offsets", "VtblGetFOVAngle", 0x5D4);
    c.offRotation = (DWORD)IniInt("Offsets", "ActorRotation", 0x68);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void* PatchPtr(void** where, void* fn) {
    DWORD old;
    if (!VirtualProtect(where, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return NULL;
    void* o = *where;
    *where = fn;
    VirtualProtect(where, sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), where, sizeof(void*));
    return o;
}

static int g_usingDXVK;
// ---- Vulkan loader hook: make DXVK's instance/device enable the extensions SteamVR uses by name ----
// (they are core in Vulkan 1.1+, so DXVK doesn't list them, but SteamVR calls the *KHR entry points and the
//  NVIDIA driver crashes if the extension wasn't explicitly enabled)
static PFN_vkGetInstanceProcAddr o_vkGIPA;
static PFN_vkCreateInstance o_vkCreateInstance;
static PFN_vkCreateDevice o_vkCreateDevice;
static PFN_vkEnumerateDeviceExtensionProperties o_vkEnumDevExt;
static PFN_vkEnumerateInstanceExtensionProperties o_vkEnumInstExt;
static const char* const kVrInstExt[] = {"VK_KHR_get_physical_device_properties2", "VK_KHR_external_memory_capabilities",
                                         "VK_KHR_external_semaphore_capabilities", "VK_KHR_external_fence_capabilities"};
static const char* const kVrDevExt[] = {"VK_KHR_external_memory", "VK_KHR_external_semaphore", "VK_KHR_timeline_semaphore",
                                        "VK_KHR_dedicated_allocation", "VK_KHR_get_memory_requirements2", "VK_KHR_external_memory_win32",
                                        "VK_KHR_win32_keyed_mutex", "VK_KHR_external_semaphore_win32", "VK_KHR_external_fence",
                                        "VK_KHR_external_fence_win32", "VK_KHR_bind_memory2"};
static int AddExts(const char* const* have, uint32_t nHave, const VkExtensionProperties* avail, uint32_t nAvail,
                   const char* const* want, int nWant, const char** out) {
    int n = 0;
    for (uint32_t i = 0; i < nHave; i++) out[n++] = have[i];
    for (int w = 0; w < nWant; w++) {
        bool dup = false, ok = false;
        for (int i = 0; i < n; i++) if (!strcmp(out[i], want[w])) dup = true;
        for (uint32_t a = 0; a < nAvail; a++) if (!strcmp(avail[a].extensionName, want[w])) ok = true;
        if (!dup && ok) { out[n++] = want[w]; Log("  + %s", want[w]); }
    }
    return n;
}
static VkResult VKAPI_CALL hk_vkCreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* al, VkInstance* inst) {
    VkExtensionProperties avail[256]; uint32_t na = 256;
    if (o_vkEnumInstExt(NULL, &na, avail) < 0) na = 0;
    const char* names[320];
    Log("Vulkan instance: adding SteamVR extensions");
    VkInstanceCreateInfo c = *ci;
    c.enabledExtensionCount = (uint32_t)AddExts(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, avail, na, kVrInstExt, 4, names);
    c.ppEnabledExtensionNames = names;
    VkResult r = o_vkCreateInstance(&c, al, inst);
    if (r != VK_SUCCESS) { Log("Vulkan instance with extra extensions failed (%d) - retrying without", (int)r); r = o_vkCreateInstance(ci, al, inst); }
    return r;
}
static VkResult VKAPI_CALL hk_vkCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* al, VkDevice* dev) {
    static VkExtensionProperties avail[512]; uint32_t na = 512;
    if (o_vkEnumDevExt(pd, NULL, &na, avail) < 0) na = 0;
    const char* names[600];
    Log("Vulkan device: adding SteamVR extensions");
    VkDeviceCreateInfo c = *ci;
    c.enabledExtensionCount = (uint32_t)AddExts(ci->ppEnabledExtensionNames, ci->enabledExtensionCount, avail, na, kVrDevExt,
                                                (int)(sizeof(kVrDevExt) / sizeof(kVrDevExt[0])), names);
    c.ppEnabledExtensionNames = names;
    VkResult r = o_vkCreateDevice(pd, &c, al, dev);
    if (r != VK_SUCCESS) { Log("Vulkan device with extra extensions failed (%d) - retrying without", (int)r); r = o_vkCreateDevice(pd, ci, al, dev); }
    return r;
}
// ---- Vulkan call tracing (diagnostics): logs the name of each Vulkan call SteamVR makes on the VR thread ----
static volatile LONG g_vkCallTrace;
static void __cdecl LogVkCall(const char* name) {
    if (g_vkCallTrace && GetCurrentThreadId() == g_vrThreadId) Log("    vk> %s", name);
}
static BYTE* g_thunkPool; static int g_thunkUsed;
struct ThunkEnt { const char* name; void* orig; void* thunk; };
static ThunkEnt g_thunks[1500]; static int g_nThunks;
static CRITICAL_SECTION g_thunkCs; static int g_thunkCsInit;
static void* MakeThunk(const char* name, void* orig) {
    if (!orig || !name) return orig;
    if (!g_thunkCsInit) { InitializeCriticalSection(&g_thunkCs); g_thunkCsInit = 1; }
    EnterCriticalSection(&g_thunkCs);
    void* res = orig;
    for (int i = 0; i < g_nThunks; i++) if (g_thunks[i].orig == orig) { res = g_thunks[i].thunk; goto done; }
    if (!g_thunkPool) g_thunkPool = (BYTE*)VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_thunkPool || g_thunkUsed + 20 > 65536 || g_nThunks >= 1500) goto done;
    {
        size_t n = strlen(name) + 1; char* nm = (char*)malloc(n); memcpy(nm, name, n);
        BYTE* t = g_thunkPool + g_thunkUsed; g_thunkUsed += 20;
        t[0] = 0x68; *(DWORD*)(t + 1) = (DWORD)nm;                                      // push name
        t[5] = 0xE8; *(DWORD*)(t + 6) = (DWORD)((BYTE*)LogVkCall - (t + 10));           // call LogVkCall
        t[10] = 0x83; t[11] = 0xC4; t[12] = 0x04;                                       // add esp, 4
        t[13] = 0xE9; *(DWORD*)(t + 14) = (DWORD)((BYTE*)orig - (t + 18));              // jmp orig
        g_thunks[g_nThunks].name = nm; g_thunks[g_nThunks].orig = orig; g_thunks[g_nThunks].thunk = t; g_nThunks++;
        res = t;
    }
done:
    LeaveCriticalSection(&g_thunkCs);
    return res;
}
static bool CallerIsVrClient(void* ret) {
    HMODULE m = NULL; char path[MAX_PATH];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)ret, &m) || !m) return false;
    if (!GetModuleFileNameA(m, path, MAX_PATH)) return false;
    const char* b = strrchr(path, '\\'); b = b ? b + 1 : path;
    return !lstrcmpiA(b, "vrclient.dll");
}
static PFN_vkGetDeviceProcAddr o_vkGDPA;
static PFN_vkVoidFunction VKAPI_CALL hk_vkGDPA(VkDevice dev, const char* name) {
    PFN_vkVoidFunction f = o_vkGDPA(dev, name);
    if (f && CallerIsVrClient(__builtin_return_address(0))) return (PFN_vkVoidFunction)MakeThunk(name, (void*)f);
    return f;
}
static PFN_vkVoidFunction VKAPI_CALL hk_vkGIPA(VkInstance inst, const char* name) {
    if (name && !strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)hk_vkCreateInstance;
    if (name && !strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)hk_vkCreateDevice;
    if (name && !strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)hk_vkGIPA;
    bool vrc = CallerIsVrClient(__builtin_return_address(0));
    if (name && !strcmp(name, "vkGetDeviceProcAddr") && vrc) {
        if (!o_vkGDPA) o_vkGDPA = (PFN_vkGetDeviceProcAddr)o_vkGIPA(inst, name);
        return (PFN_vkVoidFunction)hk_vkGDPA;
    }
    PFN_vkVoidFunction f = o_vkGIPA(inst, name);
    if (f && vrc) return (PFN_vkVoidFunction)MakeThunk(name, (void*)f);
    return f;
}
static bool PatchExport(HMODULE m, const char* name, void* fn, void** orig) {
    BYTE* b = (BYTE*)m;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dd.VirtualAddress) return false;
    IMAGE_EXPORT_DIRECTORY* ed = (IMAGE_EXPORT_DIRECTORY*)(b + dd.VirtualAddress);
    DWORD* names = (DWORD*)(b + ed->AddressOfNames); WORD* ords = (WORD*)(b + ed->AddressOfNameOrdinals); DWORD* funcs = (DWORD*)(b + ed->AddressOfFunctions);
    for (DWORD i = 0; i < ed->NumberOfNames; i++) {
        if (strcmp((char*)(b + names[i]), name)) continue;
        DWORD* slot = &funcs[ords[i]];
        *orig = b + *slot;
        DWORD old;
        if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) return false;
        *slot = (DWORD)((BYTE*)fn - b);   // 32-bit wrap-around is fine
        VirtualProtect(slot, 4, old, &old);
        return true;
    }
    return false;
}
static void HookVulkanLoader() {
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) { Log("vulkan-1.dll not found"); return; }
    o_vkCreateInstance = (PFN_vkCreateInstance)GetProcAddress(vk, "vkCreateInstance");
    o_vkCreateDevice = (PFN_vkCreateDevice)GetProcAddress(vk, "vkCreateDevice");
    o_vkEnumDevExt = (PFN_vkEnumerateDeviceExtensionProperties)GetProcAddress(vk, "vkEnumerateDeviceExtensionProperties");
    o_vkEnumInstExt = (PFN_vkEnumerateInstanceExtensionProperties)GetProcAddress(vk, "vkEnumerateInstanceExtensionProperties");
    if (!o_vkCreateInstance || !o_vkCreateDevice || !o_vkEnumDevExt || !o_vkEnumInstExt) { Log("Vulkan loader exports missing"); return; }
    void* orig = NULL;
    if (PatchExport(vk, "vkGetInstanceProcAddr", (void*)hk_vkGIPA, &orig)) { o_vkGIPA = (PFN_vkGetInstanceProcAddr)orig; Log("Vulkan loader hooked (SteamVR extensions will be enabled)"); }
    else { Log("Vulkan loader hook failed"); return; }
    o_vkGDPA = (PFN_vkGetDeviceProcAddr)GetProcAddress(vk, "vkGetDeviceProcAddr");
    // tracing thunks on the loader's other exports (only log on the VR thread while tracing is switched on)
    BYTE* b = (BYTE*)vk;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    IMAGE_EXPORT_DIRECTORY* ed = (IMAGE_EXPORT_DIRECTORY*)(b + dd.VirtualAddress);
    DWORD* names = (DWORD*)(b + ed->AddressOfNames); WORD* ords = (WORD*)(b + ed->AddressOfNameOrdinals); DWORD* funcs = (DWORD*)(b + ed->AddressOfFunctions);
    int patched = 0;
    for (DWORD i = 0; i < ed->NumberOfNames; i++) {
        const char* nm = (const char*)(b + names[i]);
        if (strncmp(nm, "vk", 2) || !strcmp(nm, "vkGetInstanceProcAddr")) continue;
        DWORD* slot = &funcs[ords[i]];
        if (*slot >= dd.VirtualAddress && *slot < dd.VirtualAddress + dd.Size) continue;   // forwarder
        void* th = MakeThunk(nm, b + *slot);
        if (!strcmp(nm, "vkGetDeviceProcAddr")) th = (void*)hk_vkGDPA;   // vrclient asking for device functions
        DWORD old;
        if (VirtualProtect(slot, 4, PAGE_READWRITE, &old)) { *slot = (DWORD)((BYTE*)th - b); VirtualProtect(slot, 4, old, &old); patched++; }
    }
    Log("Vulkan call tracing ready (%d loader exports)", patched);
}

// ---- connect to SteamVR as soon as the game starts, in parallel with the game's own start-up ----
static HANDLE g_earlyVrThread; static volatile LONG g_earlyVrOK; static double g_earlyVrMs;
static DWORD WINAPI EarlyVrThread(LPVOID) {
    double t0 = QpcMs();
    char path[MAX_PATH]; lstrcpyA(path, g_dir); lstrcatA(path, "openvr_api.dll");
    HMODULE m = LoadLibraryA(path);
    typedef bool (*Present_t)(); typedef uint32_t (*Init_t)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
    Present_t present = m ? (Present_t)GetProcAddress(m, "VR_IsHmdPresent") : NULL;
    Init_t init = m ? (Init_t)GetProcAddress(m, "VR_InitInternal2") : NULL;
    if (init && (!present || present())) {
        vr::EVRInitError err = vr::VRInitError_None;
        init(&err, vr::VRApplication_Scene, NULL);
        if (err == vr::VRInitError_None) InterlockedExchange(&g_earlyVrOK, 1);
        else Log("Early SteamVR connection failed (error %d) - will connect later", (int)err);
    }
    if (m && !g_ovrDll) g_ovrDll = m;
    g_earlyVrMs = QpcMs() - t0;
    return 0;
}
static void WaitEarlyVr(const char* where) {
    if (!g_earlyVrThread) return;
    double t0 = QpcMs();
    WaitForSingleObject(g_earlyVrThread, 20000);
    CloseHandle(g_earlyVrThread); g_earlyVrThread = NULL;
    Log("Early SteamVR connection %s in %.0f ms (waited %.0f ms %s)", g_earlyVrOK ? "made" : "not made", g_earlyVrMs, QpcMs() - t0, where);
}
static bool EnsureRealD3D9() {
    if (g_realD3D9) return true;
    WaitEarlyVr("before starting DXVK");
    char path[MAX_PATH];
    if (g_cfg.useDXVK) {
        lstrcpyA(path, g_dir); lstrcatA(path, "dxvk_d3d9.dll");
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            if (g_cfg.noSteamVkLayers) {
                // Steam injects implicit Vulkan layers (shader pre-caching, overlay); keep them out of DXVK's device
                SetEnvironmentVariableA("DISABLE_VK_LAYER_VALVE_steam_fossilize_1", "1");
                SetEnvironmentVariableA("DISABLE_VK_LAYER_VALVE_steam_overlay_1", "1");
                SetEnvironmentVariableA("DISABLE_VK_LAYER_VALVE_steam_fossilize_64", "1");
                SetEnvironmentVariableA("DISABLE_VK_LAYER_VALVE_steam_overlay_64", "1");
                SetEnvironmentVariableA("VK_LOADER_LAYERS_DISABLE", "*steam*");   // newer Vulkan loaders
                Log("Steam Vulkan layers disabled for DXVK");
            }
            // DXVK only enables the Vulkan extensions SteamVR needs if openvr_api.dll is already loaded when it starts
            if (!g_ovrDll) {
                char ovr[MAX_PATH]; lstrcpyA(ovr, g_dir); lstrcatA(ovr, "openvr_api.dll");
                g_ovrDll = LoadLibraryA(ovr);
                Log("openvr_api.dll pre-loaded for DXVK: %s", g_ovrDll ? "yes" : "FAILED");
            }
            HookVulkanLoader();
            g_realD3D9 = LoadLibraryA(path);
            if (g_realD3D9) { g_usingDXVK = 1; Log("Renderer: DXVK (D3D9 -> Vulkan) from %s", path); }
            else Log("DXVK found but failed to load (error %lu) - using Windows D3D9", GetLastError());
        }
    }
    UINT n = g_realD3D9 ? 1 : GetSystemDirectoryA(path, MAX_PATH);
    if (!n || n > MAX_PATH - 12) return false;
    if (!g_realD3D9) { lstrcatA(path, "\\d3d9.dll"); g_realD3D9 = LoadLibraryA(path); }
    if (!g_realD3D9) { Log("ERROR: could not load system d3d9.dll (%s)", path); return false; }
    p_Create9 = (Direct3DCreate9_t)GetProcAddress(g_realD3D9, "Direct3DCreate9");
    p_Create9Ex = (Direct3DCreate9Ex_t)GetProcAddress(g_realD3D9, "Direct3DCreate9Ex");
    p_BeginEvent = (PERF_BeginEvent_t)GetProcAddress(g_realD3D9, "D3DPERF_BeginEvent");
    p_EndEvent = (PERF_EndEvent_t)GetProcAddress(g_realD3D9, "D3DPERF_EndEvent");
    p_SetMarker = (PERF_SetMarker_t)GetProcAddress(g_realD3D9, "D3DPERF_SetMarker");
    p_SetRegion = (PERF_SetRegion_t)GetProcAddress(g_realD3D9, "D3DPERF_SetRegion");
    p_QueryRepeatFrame = (PERF_QueryRepeatFrame_t)GetProcAddress(g_realD3D9, "D3DPERF_QueryRepeatFrame");
    p_SetOptions = (PERF_SetOptions_t)GetProcAddress(g_realD3D9, "D3DPERF_SetOptions");
    p_GetStatus = (PERF_GetStatus_t)GetProcAddress(g_realD3D9, "D3DPERF_GetStatus");
    return p_Create9 != NULL;
}

// ---------------------------------------------------------------------------
// Runtime reflection (GObjects / GNames) – used to read engine variables by name
// ---------------------------------------------------------------------------
// UObject layout in this build (from UObject::FindFunction): Outer 0x28, Name 0x2C, Class 0x34
static const DWORD OBJ_OUTER = 0x28, OBJ_NAME = 0x2C, OBJ_CLASS = 0x34;
static const DWORD RVA_GOBJECTS = 0x1D8B2BC;
static void*** g_namesData;     // TArray<FNameEntry*>.Data (pointer to the global)
static int* g_namesNum;
static DWORD g_nameStrOff;      // offset of the string inside FNameEntry
static DWORD g_propOffField;    // offset of UProperty::Offset
static DWORD g_boolMaskField;   // offset of UBoolProperty::BitMask
static int g_reflOK;

static bool CanRead(const void* p, DWORD n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY))) return false;
    return (const BYTE*)p + n <= (const BYTE*)mbi.BaseAddress + mbi.RegionSize;
}
static const char* EntryStr(void* entry) { return entry ? (const char*)entry + g_nameStrOff : ""; }
static const char* NameOf(void* obj) {
    if ((DWORD)obj < 0x10000) return "";
    int idx = *(int*)((BYTE*)obj + OBJ_NAME);
    if (idx < 0 || idx >= *g_namesNum) return "";
    return EntryStr((*g_namesData)[idx]);
}
static inline void* Ptr(void* obj, DWORD off) { return obj ? *(void**)((BYTE*)obj + off) : NULL; }

static bool FindGNames() {
    BYTE* base = (BYTE*)g_base;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (memcmp(sec[i].Name, ".data", 5) != 0) continue;
        DWORD* p = (DWORD*)(base + sec[i].VirtualAddress);
        DWORD n = sec[i].Misc.VirtualSize / 4;
        for (DWORD k = 0; k + 2 < n; k++) {
            int num = (int)p[k + 1], max = (int)p[k + 2];
            if (num < 20000 || num > 3000000 || max < num || max > 4000000) continue;
            void** data = (void**)p[k];
            if (!CanRead(data, 8)) continue;
            BYTE* e0 = (BYTE*)data[0]; BYTE* e1 = (BYTE*)data[1];
            if (!CanRead(e0, 0x30) || !CanRead(e1, 0x30)) continue;
            for (DWORD so = 0; so <= 0x20; so += 4) {
                if (memcmp(e0 + so, "None", 5) == 0 && memcmp(e1 + so, "ByteProperty", 13) == 0) {
                    g_namesData = (void***)&p[k];
                    g_namesNum = (int*)&p[k + 1];
                    g_nameStrOff = so;
                    Log("GNames found at RVA %06lX (%d names, string @+0x%lX)", (unsigned long)((DWORD)&p[k] - g_base), num, (unsigned long)so);
                    return true;
                }
            }
        }
    }
    return false;
}
// Find a field object (property) by owner class name + field name.
static void* FindField(const char* cls, const char* field) {
    void** objs = *(void***)(g_base + RVA_GOBJECTS);
    int num = *(int*)(g_base + RVA_GOBJECTS + 4);
    for (int i = 0; i < num; i++) {
        void* o = objs[i];
        if (!o) continue;
        if (lstrcmpA(NameOf(o), field) != 0) continue;
        void* outer = Ptr(o, OBJ_OUTER);
        if (outer && lstrcmpA(NameOf(outer), cls) == 0) return o;
    }
    return NULL;
}
static int PropOffset(const char* cls, const char* field) {
    void* p = FindField(cls, field);
    return p ? *(int*)((BYTE*)p + g_propOffField) : -1;
}
static bool InitReflection() {
    if (!FindGNames()) { Log("Reflection: GNames not found"); return false; }
    // calibrate UProperty::Offset using PlayerController.PlayerCamera (0x3FC) and Controller.Pawn (0x238)
    void* pc = FindField("PlayerController", "PlayerCamera");
    void* pw = FindField("Controller", "Pawn");
    if (!pc || !pw) { Log("Reflection: calibration fields missing (%p %p)", pc, pw); return false; }
    for (DWORD o = 0x3C; o < 0xA0; o += 4)
        if (*(DWORD*)((BYTE*)pc + o) == 0x3FC && *(DWORD*)((BYTE*)pw + o) == 0x238) { g_propOffField = o; break; }
    if (!g_propOffField) { Log("Reflection: could not calibrate property offset field"); return false; }
    void* dm = FindField("Actor", "bDeleteMe");
    if (dm && *(DWORD*)((BYTE*)dm + g_propOffField) == 0xC4)
        for (DWORD o = g_propOffField + 4; o < g_propOffField + 0x40; o += 4)
            if (*(DWORD*)((BYTE*)dm + o) == 8) { g_boolMaskField = o; break; }
    Log("Reflection ready: UProperty::Offset @+0x%lX, BitMask @+0x%lX", (unsigned long)g_propOffField, (unsigned long)g_boolMaskField);
    g_reflOK = 1;
    return true;
}
struct BoolProp { int off; DWORD mask; };
static BoolProp BoolOffset(const char* cls, const char* field) {
    BoolProp b = {-1, 0};
    void* p = FindField(cls, field);
    if (p && g_boolMaskField) { b.off = *(int*)((BYTE*)p + g_propOffField); b.mask = *(DWORD*)((BYTE*)p + g_boolMaskField); }
    return b;
}

// game-state offsets (filled by InitGameState)
static const DWORD OFF_PAWN = 0x238, OFF_PLAYERCAMERA = 0x3FC, OFF_CAM_VIEWTARGET = 0x3CC, OFF_CAM_PENDINGVT = 0x3F8;
static int g_offWorldInfo = -1, g_offPauser = -1;
static BoolProp g_bCinematic = {-1, 0};
static int g_gameStateInit;
static void InitGameState() {
    g_gameStateInit = 1;
    if (!InitReflection()) return;
    g_offWorldInfo = PropOffset("Actor", "WorldInfo");
    g_offPauser = PropOffset("WorldInfo", "Pauser");
    g_bCinematic = BoolOffset("PlayerController", "bCinematicMode");
    int chkPawn = PropOffset("Controller", "Pawn"), chkCam = PropOffset("PlayerController", "PlayerCamera");
    int chkVT = PropOffset("Camera", "ViewTarget");
    Log("Offsets: Actor.WorldInfo=0x%X WorldInfo.Pauser=0x%X PC.bCinematicMode=0x%X/%lX  (Pawn 0x%X, PlayerCamera 0x%X, Camera.ViewTarget 0x%X)",
        g_offWorldInfo, g_offPauser, g_bCinematic.off, (unsigned long)g_bCinematic.mask, chkPawn, chkCam, chkVT);
}

enum ViewMode { VM_GAMEPLAY, VM_MENU, VM_PAUSED, VM_CUTSCENE };
static const char* kModeNames[] = {"gameplay", "menu (no pawn / camera elsewhere)", "paused", "cutscene"};
static ViewMode ClassifyView(void* pc) {
    void* pawn = Ptr(pc, OFF_PAWN);
    if (!pawn) return VM_MENU;
    if (g_offWorldInfo > 0 && g_offPauser > 0) {
        void* wi = Ptr(pc, (DWORD)g_offWorldInfo);
        if (wi && Ptr(wi, (DWORD)g_offPauser)) return VM_PAUSED;
    }
    if (g_bCinematic.off > 0 && (*(DWORD*)((BYTE*)pc + g_bCinematic.off) & g_bCinematic.mask)) return VM_CUTSCENE;
    void* cam = Ptr(pc, OFF_PLAYERCAMERA);
    if (cam) {
        void* vt = Ptr(cam, OFF_CAM_PENDINGVT);
        if (!vt) vt = Ptr(cam, OFF_CAM_VIEWTARGET);
        if (vt && vt != pawn && vt != pc) return VM_CUTSCENE;
    }
    return VM_GAMEPLAY;
}

// ---------------------------------------------------------------------------
// Window size: let the game window exceed the desktop so it can render at VR resolution
// ---------------------------------------------------------------------------
typedef int(WINAPI* GetSystemMetrics_t)(int);
typedef BOOL(WINAPI* SystemParametersInfoW_t)(UINT, UINT, PVOID, UINT);
typedef BOOL(WINAPI* GetMonitorInfoW_t)(HMONITOR, LPMONITORINFO);
typedef ATOM(WINAPI* RegisterClassExW_t)(const WNDCLASSEXW*);
typedef ATOM(WINAPI* RegisterClassW_t)(const WNDCLASSW*);
static GetSystemMetrics_t o_GetSystemMetrics;
static SystemParametersInfoW_t o_SPIW;
static GetMonitorInfoW_t o_GetMonitorInfoW;
static RegisterClassExW_t o_RegisterClassExW;
static RegisterClassW_t o_RegisterClassW;
static WNDPROC g_origWndProc[8];
static const int BIG = 8192;

static int WINAPI hkGetSystemMetrics(int i) {
    int v = o_GetSystemMetrics(i);
    switch (i) {
        case SM_CXSCREEN: case SM_CYSCREEN: case SM_CXVIRTUALSCREEN: case SM_CYVIRTUALSCREEN:
        case SM_CXMAXTRACK: case SM_CYMAXTRACK: case SM_CXMAXIMIZED: case SM_CYMAXIMIZED:
            return v < BIG ? BIG : v;
    }
    return v;
}
static BOOL WINAPI hkSPIW(UINT a, UINT b, PVOID p, UINT c) {
    BOOL r = o_SPIW(a, b, p, c);
    if (r && a == SPI_GETWORKAREA && p) { RECT* rc = (RECT*)p; rc->right = rc->left + BIG; rc->bottom = rc->top + BIG; }
    return r;
}
static BOOL WINAPI hkGetMonitorInfoW(HMONITOR m, LPMONITORINFO mi) {
    BOOL r = o_GetMonitorInfoW(m, mi);
    if (r && mi) {
        mi->rcMonitor.right = mi->rcMonitor.left + BIG; mi->rcMonitor.bottom = mi->rcMonitor.top + BIG;
        mi->rcWork.right = mi->rcWork.left + BIG; mi->rcWork.bottom = mi->rcWork.top + BIG;
    }
    return r;
}
// Keep the game believing its window is in front: SteamVR's windows can take the focus, and the game then pauses.
static HWND g_gameHwnd;
static void NudgeGameFocus(const char* why);
static bool FocusLossMsg(UINT m, WPARAM w) {
    return (m == WM_ACTIVATEAPP && !w) || (m == WM_ACTIVATE && LOWORD(w) == WA_INACTIVE) || m == WM_KILLFOCUS;
}
template <int N> static LRESULT CALLBACK WrapProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_cfg.keepFocus && g_gameHwnd && h == g_gameHwnd && FocusLossMsg(m, w)) {
        static int n;
        if (n++ < 20) {
            HWND fg = GetForegroundWindow(); char cls[64] = "", title[64] = "";
            if (fg) { GetClassNameA(fg, cls, 64); GetWindowTextA(fg, title, 64); }
            Log("Focus: game lost focus (msg 0x%X) to '%s' [%s] - hidden from the game", m, title, cls);
        }
        return 0;
    }
    LRESULT r = CallWindowProcW(g_origWndProc[N], h, m, w, l);
    if (m == WM_GETMINMAXINFO) {
        MINMAXINFO* mm = (MINMAXINFO*)l;
        mm->ptMaxTrackSize.x = mm->ptMaxTrackSize.y = BIG;
        mm->ptMaxSize.x = mm->ptMaxSize.y = BIG;
    }
    return r;
}
static WNDPROC kWrappers[8] = {WrapProc<0>, WrapProc<1>, WrapProc<2>, WrapProc<3>, WrapProc<4>, WrapProc<5>, WrapProc<6>, WrapProc<7>};
static int g_numWrapped;
static WNDPROC WrapWndProc(WNDPROC p) {
    if (!p || g_numWrapped >= 8) return p;
    g_origWndProc[g_numWrapped] = p;
    return kWrappers[g_numWrapped++];
}
static ATOM WINAPI hkRegisterClassExW(const WNDCLASSEXW* wc) {
    if (!wc) return o_RegisterClassExW(wc);
    WNDCLASSEXW c = *wc; c.lpfnWndProc = WrapWndProc(c.lpfnWndProc);
    return o_RegisterClassExW(&c);
}
static ATOM WINAPI hkRegisterClassW(const WNDCLASSW* wc) {
    if (!wc) return o_RegisterClassW(wc);
    WNDCLASSW c = *wc; c.lpfnWndProc = WrapWndProc(c.lpfnWndProc);
    return o_RegisterClassW(&c);
}

// Generic IAT patcher for imports-by-name of ACM.exe
static void* PatchImport(const char* dll, const char* func, void* hook) {
    BYTE* base = (BYTE*)g_base;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dd.VirtualAddress);
    for (; imp->Name; imp++) {
        if (lstrcmpiA((const char*)(base + imp->Name), dll) != 0) continue;
        IMAGE_THUNK_DATA32* oft = (IMAGE_THUNK_DATA32*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA32* ft = (IMAGE_THUNK_DATA32*)(base + imp->FirstThunk);
        for (; oft->u1.Ordinal; oft++, ft++) {
            if (IMAGE_SNAP_BY_ORDINAL32(oft->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData);
            if (!lstrcmpA((const char*)ibn->Name, func)) return PatchPtr((void**)&ft->u1.Function, hook);
        }
    }
    return NULL;
}
typedef HWND(WINAPI* GetWnd_t)();
static GetWnd_t o_GetForegroundWindow, o_GetActiveWindow, o_GetFocus;
static bool OwnWindow(HWND w) { DWORD pid = 0; if (w) GetWindowThreadProcessId(w, &pid); return pid == GetCurrentProcessId(); }
static HWND FocusAnswer(HWND real) {
    if (!g_cfg.keepFocus || !g_gameHwnd || !IsWindow(g_gameHwnd) || !g_vrOK) return real;
    if (real && OwnWindow(real)) return real;   // one of the game's own windows (dialogs etc.): tell the truth
    return g_gameHwnd;                           // another program (SteamVR, overlays) has it: pretend the game does
}
static HWND WINAPI hkGetForegroundWindow() { return FocusAnswer(o_GetForegroundWindow()); }
static HWND WINAPI hkGetActiveWindow() { return FocusAnswer(o_GetActiveWindow()); }
static HWND WINAPI hkGetFocus() { return FocusAnswer(o_GetFocus()); }
static void InstallWindowHooks() {
    o_GetSystemMetrics = (GetSystemMetrics_t)PatchImport("USER32.dll", "GetSystemMetrics", (void*)hkGetSystemMetrics);
    o_SPIW = (SystemParametersInfoW_t)PatchImport("USER32.dll", "SystemParametersInfoW", (void*)hkSPIW);
    o_GetMonitorInfoW = (GetMonitorInfoW_t)PatchImport("USER32.dll", "GetMonitorInfoW", (void*)hkGetMonitorInfoW);
    o_RegisterClassExW = (RegisterClassExW_t)PatchImport("USER32.dll", "RegisterClassExW", (void*)hkRegisterClassExW);
    o_RegisterClassW = (RegisterClassW_t)PatchImport("USER32.dll", "RegisterClassW", (void*)hkRegisterClassW);
    Log("Large-window hooks: metrics %d, workarea %d, monitor %d, class %d/%d", !!o_GetSystemMetrics, !!o_SPIW,
        !!o_GetMonitorInfoW, !!o_RegisterClassExW, !!o_RegisterClassW);
    if (g_cfg.keepFocus) {
        o_GetForegroundWindow = (GetWnd_t)PatchImport("USER32.dll", "GetForegroundWindow", (void*)hkGetForegroundWindow);
        o_GetActiveWindow = (GetWnd_t)PatchImport("USER32.dll", "GetActiveWindow", (void*)hkGetActiveWindow);
        o_GetFocus = (GetWnd_t)PatchImport("USER32.dll", "GetFocus", (void*)hkGetFocus);
        Log("Focus hooks: foreground %d, active %d, focus %d", !!o_GetForegroundWindow, !!o_GetActiveWindow, !!o_GetFocus);
    }
}

// ---------------------------------------------------------------------------
// Pose helpers
// ---------------------------------------------------------------------------
static float g_heightAdj;   // [VR] HeightOffset, metres
static void SnapPoses(PoseSnap* out) {
    EnterCriticalSection(&g_poseCs);
    *out = g_pose;
    LeaveCriticalSection(&g_poseCs);
}
// Convert an OpenVR pose to a UE-space basis and position (metres) relative to the recentre origin.
static void PoseToUe(const PoseSnap& ps, const vr::HmdMatrix34_t& m, Basis* b, V3* pos) {
    V3 X = mk(m.m[0][0], m.m[1][0], m.m[2][0]);
    V3 Y = mk(m.m[0][1], m.m[1][1], m.m[2][1]);
    V3 Z = mk(m.m[0][2], m.m[1][2], m.m[2][2]);
    V3 P = mk(m.m[0][3], m.m[1][3], m.m[2][3]);
    float a = -ps.recYaw;
    X = rotYvr(X, a); Y = rotYvr(Y, a); Z = rotYvr(Z, a);
    P = rotYvr(sub(P, ps.recPos), a);
    P.y += g_heightAdj;   // player's own height adjustment (metres; VR y is up)
    b->f = vrToUe(mul(Z, -1.0f));
    b->r = vrToUe(X);
    b->u = vrToUe(Y);
    *pos = vrToUe(P);
}
static float HmdYawVr(const vr::HmdMatrix34_t& m) {
    // forward = -Z column; yaw angle about +Y
    float fx = -m.m[0][2], fz = -m.m[2][2];
    return fatan2(-fx, -fz);
}

// ---------------------------------------------------------------------------
// Game-thread camera hooks
// ---------------------------------------------------------------------------
struct CallerStat { DWORD rva; unsigned count; };
static CallerStat g_callers[48];
static void CountCaller(DWORD rva) {
    for (int i = 0; i < 48; i++) {
        if (g_callers[i].rva == rva) { g_callers[i].count++; return; }
        if (!g_callers[i].rva) { g_callers[i].rva = rva; g_callers[i].count = 1; return; }
    }
}
static void DumpCallers() {
    char line[900]; int n = 0;
    n += _snprintf(line + n, sizeof(line) - n, "GetPlayerViewPoint callers (rva:count):");
    for (int i = 0; i < 48 && g_callers[i].rva && n < (int)sizeof(line) - 24; i++)
        n += _snprintf(line + n, sizeof(line) - n, " %06lX:%u", (unsigned long)g_callers[i].rva, g_callers[i].count);
    Log("%s", line);
}

static vr::HmdMatrix34_t g_eyePose[8];
static FRotator g_eyeView[8]; static int g_eyeViewOK[8];   // final view rotation each frame was drawn with (for TAA)
static FRotator g_poppedView; static int g_poppedViewOK;
static FRotator g_cacheView; static int g_cacheValid;
static vr::HmdMatrix34_t g_renderPose;   // HMD pose this frame's views were built from (game thread)
static vr::HmdMatrix34_t g_poppedPose;   // pose of the frame just popped (render thread)
static void PushEye(int eye) {
    LONG h = g_eyeHead;
    g_eyeRing[h & 7] = eye;
    g_eyePose[h & 7] = g_renderPose;
    g_eyeView[h & 7] = g_cacheView; g_eyeViewOK[h & 7] = g_cacheValid;
    InterlockedExchange(&g_eyeHead, h + 1);
}
static int PopEye() {
    LONG t = g_eyeTail, h = g_eyeHead;
    if (t == h) return -1;
    if (h - t > 6) {  // something went badly out of sync – resync to newest
        static int warned;
        if (warned++ < 10) Log("eye FIFO resync (backlog %ld)", (long)(h - t));
        t = h - 1;
    }
    int e = g_eyeRing[t & 7];
    g_poppedPose = g_eyePose[t & 7];
    g_poppedView = g_eyeView[t & 7]; g_poppedViewOK = g_eyeViewOK[t & 7];
    InterlockedExchange(&g_eyeTail, t + 1);
    return e;
}

static FVector g_cacheCam; static V3 g_cacheHeadOff, g_cacheRight;
static int g_curEye;           // eye being rendered by the current CalcSceneView call (side-by-side mode)
static void RenderView(void* self, FVector* loc, FRotator* rot) {
    const Config& c0 = g_cfg;
    if (g_curEye == 1 && g_cacheValid) {   // second eye of the same frame: identical pose, other eye offset
        float half = 0.5f * g_ipd * c0.ipdScale * g_worldScale;
        loc->X = g_cacheCam.X + g_cacheHeadOff.x + g_cacheRight.x * half;
        loc->Y = g_cacheCam.Y + g_cacheHeadOff.y + g_cacheRight.y * half;
        loc->Z = g_cacheCam.Z + g_cacheHeadOff.z + g_cacheRight.z * half;
        *rot = g_cacheView;
        return;
    }
    g_cacheValid = 0;
    PoseSnap ps; SnapPoses(&ps);
    g_renderPose = ps.hmd;
    if (!ps.hmdValid) return;
    const Config& c = g_cfg;

    FRotator* ctrl = (FRotator*)((char*)self + c.offRotation);
    FRotator game = *rot, cur = *ctrl;

    Basis head; V3 headPos;
    PoseToUe(ps, ps.hmd, &head, &headPos);
    FRotator headRel = BasisToRot(head);

    if (self != g_lastPC) {
        g_lastPC = self;
        g_haveWritten = 0;
        Log("Local PlayerController = %p", self);
    }
    // Is the game showing a camera that isn't driven by the player (cutscene, scripted)?
    float thr = c.cinematicThreshold * 65536.0f / 360.0f;
    bool cinematic = fabsf_((float)wrap16(game.Yaw - cur.Yaw)) > thr || fabsf_((float)wrap16(game.Pitch - cur.Pitch)) > thr;

    if (!cinematic) {
        if (!g_haveWritten) g_baseYaw = cur.Yaw - headRel.Yaw;   // keep view where it was
        else g_baseYaw += wrap16(cur.Yaw - g_lastWritten.Yaw);   // stick/mouse turning
        g_baseYaw += InterlockedExchange(&g_snapRequest, 0);
    }

    FRotator baseR;
    baseR.Pitch = (cinematic && c.cinematicPitch) ? game.Pitch : 0;
    baseR.Yaw = cinematic ? game.Yaw : g_baseYaw;
    baseR.Roll = 0;
    Basis B = RotToBasis(baseR);
    Basis W = toWorldB(B, head);
    FRotator view = BasisToRot(W);

    // position: head translation in yaw-only world frame
    FRotator yawOnly = {0, baseR.Yaw, 0};
    Basis Y = RotToBasis(yawOnly);
    bool positional = (c.positional != 0) ^ (g_posToggle != 0);
    V3 headOff = positional ? mul(toWorld(Y, headPos), g_worldScale) : mk(0, 0, 0);
    FVector camLoc = *loc;  // game's camera location (pawn eyes)

    if (!cinematic) {
        FRotator aim = view;
        bool useHand = (c.controllerAim != 0) ^ (g_aimToggle != 0);
        int hand = c.aimHand ? 1 : 0;
        g_handValid = 0;
        if (useHand && ps.ctrlValid[hand]) {
            Basis h; V3 hp;
            PoseToUe(ps, ps.ctrl[hand], &h, &hp);
            if (g_aimPitch != 0.0f) {
                float a = g_aimPitch * PI_F / 180.0f, ca = fcos(a), sa = fsin(a);
                Basis t = h;
                t.f = add(mul(h.f, ca), mul(h.u, sa));
                t.u = sub(mul(h.u, ca), mul(h.f, sa));
                h = t;
            }
            Basis HW = toWorldB(B, h);
            aim = BasisToRot(HW);
            V3 rel = mul(toWorld(Y, sub(hp, positional ? mk(0, 0, 0) : headPos)), g_worldScale);
            g_handLoc.X = camLoc.X + rel.x; g_handLoc.Y = camLoc.Y + rel.y; g_handLoc.Z = camLoc.Z + rel.z;
            g_handRot = aim; if (!g_cfg.wristTwist) g_handRot.Roll = 0;   // twist the wrist and the gun turns with it
            // place a virtual game camera behind the hand so the game's gun model lands in the hand
            Basis G = RotToBasis(g_handRot);
            V3 gc = sub(mk(g_handLoc.X, g_handLoc.Y, g_handLoc.Z),
                        add(add(mul(G.f, g_gunX), mul(G.r, g_gunY)), mul(G.u, g_gunZ)));
            if (!g_aimCamFromArms) { g_gunCamLoc.X = gc.x; g_gunCamLoc.Y = gc.y; g_gunCamLoc.Z = gc.z; }
            g_handValid = 1;
        }
        g_lhandValid = 0;
        if (useHand && ps.ctrlValid[1 - hand]) {   // off-hand controller (the scanner hand), same tilt as the gun hand
            Basis h; V3 hp;
            PoseToUe(ps, ps.ctrl[1 - hand], &h, &hp);
            if (g_aimPitch != 0.0f) {
                float a = g_aimPitch * PI_F / 180.0f, ca = fcos(a), sa = fsin(a);
                Basis t = h;
                t.f = add(mul(h.f, ca), mul(h.u, sa));
                t.u = sub(mul(h.u, ca), mul(h.f, sa));
                h = t;
            }
            g_lhandB = toWorldB(B, h);
            V3 rel = mul(toWorld(Y, sub(hp, positional ? mk(0, 0, 0) : headPos)), g_worldScale);
            g_lhandLoc = mk(camLoc.X + rel.x, camLoc.Y + rel.y, camLoc.Z + rel.z);
            g_lhandValid = 1;
        }
        FRotator fire = aim;
        if (useHand && g_handValid) {
            fire.Pitch += iround(g_corrPitch * 65536.0f / 360.0f);
            fire.Yaw += iround(g_corrYaw * 65536.0f / 360.0f);
        }
        fire.Roll = 0;
        g_fireRot = fire;
        // The controller rotation also drives the arms' aim animation, so by default it gets the UNcorrected
        // aim (gun stays put); only the weapon's view-point query gets the corrected bullet direction.
        FRotator ctrlAim = c.corrOnController ? fire : aim;
        FRotator w;
        w.Pitch = ctrlAim.Pitch & 0xFFFF;
        w.Yaw = g_baseYaw + wrap16(ctrlAim.Yaw - g_baseYaw);
        w.Roll = 0;
        *ctrl = w;
        // the game walks along the controller (gun) direction: remember how far the head is turned from it,
        // so the input hook can turn the left stick to match (skip when looking nearly straight up/down)
        if (useHand && abs(wrap16(view.Pitch)) < 13650) g_moveTurn = (float)wrap16(view.Yaw - w.Yaw) * (2.0f * PI_F / 65536.0f);
        else if (!useHand) g_moveTurn = 0;
        g_lastWritten = w;
        g_haveWritten = 1;
        if (c.keepShake) {
            view.Pitch += wrap16(game.Pitch - cur.Pitch);
            view.Yaw += wrap16(game.Yaw - cur.Yaw);
            view.Roll += wrap16(game.Roll - cur.Roll);
        }
    } else {
        g_haveWritten = 0;
        g_handValid = 0;
        g_moveTurn = 0;
    }

    float half = 0.5f * g_ipd * c.ipdScale * g_worldScale * (g_curEye == 1 ? 1.0f : -1.0f);   // eye 0 = left
    V3 eyeOff = mul(W.r, half);
    loc->X = camLoc.X + headOff.x + eyeOff.x;
    loc->Y = camLoc.Y + headOff.y + eyeOff.y;
    loc->Z = camLoc.Z + headOff.z + eyeOff.z;
    *rot = view;
    g_cacheView = view; g_cacheCam = camLoc; g_cacheHeadOff = headOff; g_cacheRight = W.r; g_cacheValid = 1;

    if (g_renderCalls < 3)
        Log("render view #%u: game loc(%.1f %.1f %.1f) rot(%d %d %d) ctrl(%d %d %d) -> view(%d %d %d)%s",
            g_renderCalls, camLoc.X, camLoc.Y, camLoc.Z, game.Pitch, game.Yaw, game.Roll,
            cur.Pitch, cur.Yaw, cur.Roll, view.Pitch, view.Yaw, view.Roll, cinematic ? " [cinematic]" : "");
}

static void THISCALL hkGetPlayerViewPoint(void* self, FVector* loc, FRotator* rot) {
    DWORD rva = (DWORD)__builtin_return_address(0) - g_base;
    o_PVP(self, loc, rot);
    if (g_cfg.logCallers) CountCaller(rva);
    if (rva == g_cfg.rvaRenderPVPRet) {
        g_renderCalls++;
        if (g_cfg.logCallers && (g_renderCalls == 600 || g_renderCalls == 6000 || g_renderCalls == 30000)) DumpCallers();
        if (g_inSBS) RenderView(self, loc, rot);
    } else {
        // Callers that also run in the main menu are engine systems (audio, streaming, shadows): keep them on the head.
        if (g_inMainMenu) { if (g_cfg.learnHeadCallers) AddHeadCaller(rva); return; }
        if (!(g_vrActive && g_stereoOn && g_frameStereo && g_handValid && self == g_lastPC)) return;
        if (IsHeadCaller(rva)) return;
        if (g_cfg.weaponFollowsHand) { *loc = g_gunCamLoc; *rot = g_fireRot; }
        else if (g_cfg.aimTraceFromHand) { *loc = g_handLoc; *rot = g_handRot; }
    }
}

static float THISCALL hkGetFOVAngle(void* self) {
    DWORD rva = (DWORD)__builtin_return_address(0) - g_base;
    float f = o_FOV(self);
    if (rva == g_cfg.rvaRenderFOVRet && g_frameStereo && g_vrActive && g_stereoOn && !g_cfg.screenOnly && g_gameFovDeg > 1.0f)
        return g_gameFovDeg;
    return f;
}

// ---------------------------------------------------------------------------
// Reflection-based report of the player's pawn / weapon parts (to find the 1st-person gun)
// ---------------------------------------------------------------------------
static DWORD g_offChildren, g_offNext;
static const DWORD STRUCT_SUPER = 0x48;
static void* FindClass(const char* name) {
    void** objs = *(void***)(g_base + RVA_GOBJECTS);
    int num = *(int*)(g_base + RVA_GOBJECTS + 4);
    for (int i = 0; i < num; i++) {
        void* o = objs[i];
        if (o && !lstrcmpA(NameOf(o), name) && !lstrcmpA(NameOf(Ptr(o, OBJ_CLASS)), "Class")) return o;
    }
    return NULL;
}
static bool CalibrateFields() {
    if (g_offChildren) return true;
    void* cls = FindClass("Controller");
    if (!cls) return false;
    static const DWORD cc[] = {0x4C, 0x50, 0x44, 0x54, 0x58};
    for (int i = 0; i < 5 && !g_offChildren; i++) { void* ch = Ptr(cls, cc[i]); if (ch && CanRead(ch, 0x40) && Ptr(ch, OBJ_OUTER) == cls) g_offChildren = cc[i]; }
    if (!g_offChildren) return false;
    static const DWORD nn[] = {0x3C, 0x40, 0x44};
    void* f = Ptr(cls, g_offChildren);
    for (int i = 0; i < 3 && !g_offNext; i++) { void* n = Ptr(f, nn[i]); if (n && CanRead(n, 0x40) && Ptr(n, OBJ_OUTER) == cls) g_offNext = nn[i]; }
    Log("Field chain: UStruct::Children @+0x%lX, UField::Next @+0x%lX", (unsigned long)g_offChildren, (unsigned long)g_offNext);
    return g_offNext != 0;
}
static void* FindPropIn(void* obj, const char* name) {   // property object in obj's class chain
    for (void* c = Ptr(obj, OBJ_CLASS); c; c = Ptr(c, STRUCT_SUPER))
        for (void* f = Ptr(c, g_offChildren); (DWORD)f >= 0x10000; f = Ptr(f, g_offNext))   // stop at junk links
            if (!lstrcmpA(NameOf(f), name)) return f;
    return NULL;
}
static void DumpObject(const char* label, void* obj, int depth) {
    if (!obj || depth > 1) return;
    Log("%s = %s (class %s)", label, NameOf(obj), NameOf(Ptr(obj, OBJ_CLASS)));
    int n = 0;
    for (void* c = Ptr(obj, OBJ_CLASS); c && lstrcmpA(NameOf(c), "Actor") && lstrcmpA(NameOf(c), "Object"); c = Ptr(c, STRUCT_SUPER)) {
        for (void* f = Ptr(c, g_offChildren); f && n < 400; f = Ptr(f, g_offNext)) {
            const char* type = NameOf(Ptr(f, OBJ_CLASS));
            if (lstrcmpA(type, "ObjectProperty") && lstrcmpA(type, "ComponentProperty")) continue;
            int off = *(int*)((BYTE*)f + g_propOffField);
            void* v = *(void**)((BYTE*)obj + off);
            if (!v) continue;
            const char* vc = NameOf(Ptr(v, OBJ_CLASS));
            Log("   %s.%s @0x%X -> %s (%s)", NameOf(c), NameOf(f), off, NameOf(v), vc);
            n++;
            if (strstr(vc, "MeshComponent")) {
                static const char* keys[] = {"bOnlyOwnerSee", "bOwnerNoSee", "HiddenGame", "AbsoluteTranslation", "AbsoluteRotation", "bUseViewOwnerDepthPriorityGroup"};
                char line[256]; int k = 0;
                for (int q = 0; q < 6; q++) {
                    void* p = FindPropIn(v, keys[q]);
                    if (!p || !g_boolMaskField) continue;
                    int bo = *(int*)((BYTE*)p + g_propOffField); DWORD bm = *(DWORD*)((BYTE*)p + g_boolMaskField);
                    k += _snprintf(line + k, sizeof(line) - k, " %s=%d", keys[q], (*(DWORD*)((BYTE*)v + bo) & bm) ? 1 : 0);
                }
                void* tr = FindPropIn(v, "Translation");
                if (tr) { float* t = (float*)((BYTE*)v + *(int*)((BYTE*)tr + g_propOffField)); k += _snprintf(line + k, sizeof(line) - k, " T(%.1f %.1f %.1f)", t[0], t[1], t[2]); }
                if (k > 0) Log("        %s", line);
            }
            if (!lstrcmpA(NameOf(f), "Weapon") || strstr(NameOf(f), "Arms") || strstr(NameOf(f), "FirstPerson") || strstr(NameOf(f), "1P"))
                DumpObject(NameOf(f), v, depth + 1);
        }
    }
}
static int g_dumped;
static void DumpPlayerParts(void* pc) {
    if (!g_reflOK || !CalibrateFields()) { Log("Parts report: reflection unavailable"); return; }
    Log("---- Player parts report (for weapon-in-hand) ----");
    DumpObject("Pawn", Ptr(pc, OFF_PAWN), 0);
    Log("---- end of report ----");
}

// ---------------------------------------------------------------------------
// Side-by-side stereo: render both eyes in one frame via two CalcSceneView calls
// ---------------------------------------------------------------------------
// Put the 1st-person arms (and the gun they hold) at the controller: set the component's absolute
// translation, then ActorComponent.ForceUpdate(true) so the renderer picks it up this frame.
typedef void(THISCALL* ProcessEvent_t)(void*, void*, void*, void*, int, int);   // (Function, Parms, Result, 0, 0)
static int g_offArmsT = -1, g_offArmsR = -1, g_offArmsS = -1;
static void* g_fnForceUpdate;
static int g_armsInit;
// ---- arm-model survey (diagnostics): class chain members related to bones, and the bone hierarchy ----
struct FNameRaw { int idx, num; };
struct TArrayRaw { void* data; int num, max; };
static const char* NameStrIdx(int idx) { return (idx >= 0 && idx < *g_namesNum) ? EntryStr((*g_namesData)[idx]) : "?"; }
static void SurveyArms(void* arms) {
    Log("---- arm model survey: %s (class %s) ----", NameOf(arms), NameOf(Ptr(arms, OBJ_CLASS)));
    for (void* c = Ptr(arms, OBJ_CLASS); c; c = Ptr(c, STRUCT_SUPER)) {
        const char* cn = NameOf(c);
        if (!lstrcmpA(cn, "Object")) break;
        for (void* f = Ptr(c, g_offChildren); f; f = Ptr(f, g_offNext)) {
            const char* n = NameOf(f), *t = NameOf(Ptr(f, OBJ_CLASS));
            if (!strstr(n, "Bone") && !strstr(n, "Hide") && !strstr(n, "Scale") && !strstr(n, "Space") && !strstr(n, "Atom") &&
                !strstr(n, "SkelControl") && !strstr(n, "Visib") && !strstr(n, "Morph")) continue;
            if (!lstrcmpA(t, "Function")) Log("  %s.%s()", cn, n);
            else Log("  %s.%s : %s @0x%X", cn, n, t, *(int*)((BYTE*)f + g_propOffField));
        }
    }
    void* fnNames = FindField("SkeletalMeshComponent", "GetBoneNames");
    void* fnParent = FindField("SkeletalMeshComponent", "GetParentBone");
    Log("  GetBoneNames %p, GetParentBone %p", fnNames, fnParent);
    {   // everything in the game's script classes that mentions the motion tracker
        void** objs = *(void***)(g_base + RVA_GOBJECTS);
        int num = *(int*)(g_base + RVA_GOBJECTS + 4), shown = 0;
        for (int i = 0; i < num && shown < 120; i++) {
            void* o = objs[i];
            if (!o) continue;
            const char* n = NameOf(o);
            if (!strstr(n, "Tracker") && !strstr(n, "tracker")) continue;
            void* outer = Ptr(o, OBJ_OUTER);
            const char* t = NameOf(Ptr(o, OBJ_CLASS));
            if (!strstr(t, "Property") && lstrcmpA(t, "Function")) continue;
            const char* on = NameOf(outer);
            if (!strstr(on, "Pawn") && !strstr(on, "Controller") && !strstr(on, "Weapon") && !strstr(on, "Inventory")) continue;
            if (strstr(t, "Property")) Log("  tracker: %s.%s : %s @0x%X", on, n, t, *(int*)((BYTE*)o + g_propOffField));
            else Log("  tracker: %s.%s()", on, n);
            shown++;
        }
    }
    if (!fnNames) return;
    ProcessEvent_t pe = (ProcessEvent_t)(*(void***)arms)[0x108 / 4];
    DWORD parms[16]; memset(parms, 0, sizeof(parms));
    pe(arms, fnNames, parms, NULL, 0, 0);
    TArrayRaw* a = (TArrayRaw*)parms;
    Log("  %d bones:", a->num);
    if (!a->data || a->num <= 0 || a->num > 400 || !CanRead(a->data, a->num * 8)) return;
    FNameRaw* nm = (FNameRaw*)a->data;
    for (int i = 0; i < a->num; i++) {
        const char* parent = "";
        if (fnParent) {
            DWORD pp[8]; memset(pp, 0, sizeof(pp));
            pp[0] = nm[i].idx; pp[1] = nm[i].num;
            pe(arms, fnParent, pp, NULL, 0, 0);
            parent = NameStrIdx((int)pp[2]);
        }
        Log("    [%d] %s%s%s  <- %s", i, NameStrIdx(nm[i].idx), nm[i].num ? "_" : "", nm[i].num ? "#" : "", parent);
    }
}
// ---- hide shoulders / upper arms: collapse those bones (zero scale) onto the elbow in the component-space pose ----
// SpaceBases (TArray<FBoneAtom>: quat 16, translation 12, scale 4) is what the renderer skins with; vertices of a bone
// with scale 0 all land on that bone's translation, so moving it to the elbow makes the upper arm vanish into it.
static const DWORD OFF_SPACEBASES = 0x28C;
static int g_hideIdx[24], g_hideTo[24], g_nHide = -1;
static void* g_fnForceUpdate2;
static int g_nBones, g_boneNameIdx[400], g_boneParent[400];
static int FindBone(const char* n) { for (int i = 0; i < g_nBones; i++) if (!lstrcmpiA(NameStrIdx(g_boneNameIdx[i]), n)) return i; return -1; }
static bool BoneIsUnder(int b, int anc) { for (int q = b; q >= 0; q = g_boneParent[q]) if (q == anc) return true; return false; }
static void ArmHideInit(void* arms) {
    g_nHide = 0;
    void* fnNames = FindField("SkeletalMeshComponent", "GetBoneNames");
    void* fnParent = FindField("SkeletalMeshComponent", "GetParentBone");
    g_fnForceUpdate2 = FindField("ActorComponent", "ForceUpdate");
    if (!fnNames || !fnParent || !g_fnForceUpdate2) { Log("Arm hiding: engine functions not found"); return; }
    ProcessEvent_t pe = (ProcessEvent_t)(*(void***)arms)[0x108 / 4];
    DWORD parms[16]; memset(parms, 0, sizeof(parms));
    pe(arms, fnNames, parms, NULL, 0, 0);
    TArrayRaw* a = (TArrayRaw*)parms;
    if (!a->data || a->num <= 0 || a->num > 400) { Log("Arm hiding: no bone list"); return; }
    FNameRaw* nm = (FNameRaw*)a->data;
    int parent[400];
    for (int i = 0; i < a->num; i++) {
        DWORD pp[8]; memset(pp, 0, sizeof(pp)); pp[0] = nm[i].idx; pp[1] = nm[i].num;
        pe(arms, fnParent, pp, NULL, 0, 0);
        parent[i] = -1;
        for (int k = 0; k < a->num; k++) if (nm[k].idx == (int)pp[2] && nm[k].num == (int)pp[3]) parent[i] = k;
    }
    g_nBones = a->num;
    for (int i = 0; i < a->num; i++) { g_boneNameIdx[i] = nm[i].idx; g_boneParent[i] = parent[i]; }
    if (!g_cfg.hideUpperArms) return;
    char list[512]; lstrcpynA(list, g_cfg.hideBones, sizeof(list));
    if (g_cfg.handsOnly)   // gloved hands only: forearms (and their twist bones) go too
        lstrcatA(list, ",Bip01-L-Forearm,Bip01-L-ForeTwist,Bip01-L-ForeTwist1,Bip01-R-Forearm,Bip01-R-ForeTwist,Bip01-R-ForeTwist1");
    for (char* p = list; *p && g_nHide < 24;) {
        char* e = p; while (*e && *e != ',') e++;
        char c = *e; *e = 0;
        while (*p == ' ') p++;
        for (int i = 0; i < a->num; i++) {
            if (lstrcmpiA(NameStrIdx(nm[i].idx), p)) continue;
            // collapse target: the first descendant (down the chain) that is NOT being hidden and is named *Forearm/*Hand,
            // otherwise the bone's first child
            int to = -1;
            for (int k = 0; k < a->num && to < 0; k++) {
                const char* kn = NameStrIdx(nm[k].idx);
                if (!strstr(kn, "Forearm") || strstr(kn, "Twist")) continue;
                for (int q = k; q >= 0; q = parent[q]) if (q == i) { to = k; break; }
            }
            if (to < 0) for (int k = 0; k < a->num; k++) if (parent[k] == i) { to = k; break; }
            if (g_cfg.handsOnly) {   // everything collapses into the same side's wrist
                const char* hn = strstr(p, "-L-") ? "Bip01-L-Hand" : strstr(p, "-R-") ? "Bip01-R-Hand" : NULL;
                for (int k = 0; hn && k < a->num; k++) if (!lstrcmpiA(NameStrIdx(nm[k].idx), hn)) to = k;
            }
            bool dup = false;
            for (int k = 0; k < g_nHide; k++) if (g_hideIdx[k] == i) dup = true;
            if (dup) continue;
            g_hideIdx[g_nHide] = i; g_hideTo[g_nHide] = to < 0 ? i : to; g_nHide++;
            Log("  hiding bone %d %s (collapsed onto %s)", i, p, to >= 0 ? NameStrIdx(nm[to].idx) : "itself");
        }
        if (!c) break;
        p = e + 1;
    }
}
// returns true if it changed the pose (caller must push it to the renderer with ForceUpdate)
static bool ApplyArmHide(void* arms) {
    if (!arms) return false;
    if (g_nHide < 0) ArmHideInit(arms);
    if (!g_cfg.hideUpperArms) return false;
    TArrayRaw* sb = (TArrayRaw*)((BYTE*)arms + OFF_SPACEBASES);
    if (g_nHide <= 0 || !sb->data || sb->num <= 0) return false;
    static int logged;
    if (!logged) { logged = 1; float* b0 = (float*)sb->data; Log("Arm hiding: SpaceBases %d, bone0 q(%.2f %.2f %.2f %.2f) t(%.1f %.1f %.1f) s %.2f", sb->num, b0[0], b0[1], b0[2], b0[3], b0[4], b0[5], b0[6], b0[7]); }
    for (int k = 0; k < g_nHide; k++) {
        int i = g_hideIdx[k], t = g_hideTo[k];
        if (i >= sb->num || t >= sb->num) continue;
        float* A = (float*)((BYTE*)sb->data + 32 * i);
        float* T = (float*)((BYTE*)sb->data + 32 * t);
        A[4] = T[4]; A[5] = T[5]; A[6] = T[6]; A[7] = 0.0f;
    }
    return true;
}
// ---- off hand: move the left forearm / hand / fingers (and the off-hand item bone) to the left controller ----
struct M3 { float m[3][3]; };   // column-vector convention: v' = M v
static M3 QuatToM3(const float* q) {
    float x = q[0], y = q[1], z = q[2], w = q[3]; M3 r;
    r.m[0][0] = 1 - 2 * (y * y + z * z); r.m[0][1] = 2 * (x * y - w * z);     r.m[0][2] = 2 * (x * z + w * y);
    r.m[1][0] = 2 * (x * y + w * z);     r.m[1][1] = 1 - 2 * (x * x + z * z); r.m[1][2] = 2 * (y * z - w * x);
    r.m[2][0] = 2 * (x * z - w * y);     r.m[2][1] = 2 * (y * z + w * x);     r.m[2][2] = 1 - 2 * (x * x + y * y);
    return r;
}
static void M3ToQuat(const M3& r, float* q) {
    float t = r.m[0][0] + r.m[1][1] + r.m[2][2];
    if (t > 0) { float s = fsqrt(t + 1.0f) * 2; q[3] = 0.25f * s; q[0] = (r.m[2][1] - r.m[1][2]) / s; q[1] = (r.m[0][2] - r.m[2][0]) / s; q[2] = (r.m[1][0] - r.m[0][1]) / s; }
    else if (r.m[0][0] > r.m[1][1] && r.m[0][0] > r.m[2][2]) { float s = fsqrt(1 + r.m[0][0] - r.m[1][1] - r.m[2][2]) * 2; q[3] = (r.m[2][1] - r.m[1][2]) / s; q[0] = 0.25f * s; q[1] = (r.m[0][1] + r.m[1][0]) / s; q[2] = (r.m[0][2] + r.m[2][0]) / s; }
    else if (r.m[1][1] > r.m[2][2]) { float s = fsqrt(1 + r.m[1][1] - r.m[0][0] - r.m[2][2]) * 2; q[3] = (r.m[0][2] - r.m[2][0]) / s; q[0] = (r.m[0][1] + r.m[1][0]) / s; q[1] = 0.25f * s; q[2] = (r.m[1][2] + r.m[2][1]) / s; }
    else { float s = fsqrt(1 + r.m[2][2] - r.m[0][0] - r.m[1][1]) * 2; q[3] = (r.m[1][0] - r.m[0][1]) / s; q[0] = (r.m[0][2] + r.m[2][0]) / s; q[1] = (r.m[1][2] + r.m[2][1]) / s; q[2] = 0.25f * s; }
}
static M3 Mul(const M3& a, const M3& b) { M3 r; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j]; return r; }
static M3 Tr(const M3& a) { M3 r; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r.m[i][j] = a.m[j][i]; return r; }
static V3 MulV(const M3& a, V3 v) { return mk(a.m[0][0] * v.x + a.m[0][1] * v.y + a.m[0][2] * v.z, a.m[1][0] * v.x + a.m[1][1] * v.y + a.m[1][2] * v.z, a.m[2][0] * v.x + a.m[2][1] * v.y + a.m[2][2] * v.z); }
static M3 BasisToM3(const Basis& b) { M3 r; r.m[0][0] = b.f.x; r.m[1][0] = b.f.y; r.m[2][0] = b.f.z; r.m[0][1] = b.r.x; r.m[1][1] = b.r.y; r.m[2][1] = b.r.z; r.m[0][2] = b.u.x; r.m[1][2] = b.u.y; r.m[2][2] = b.u.z; return r; }
static int g_offL2W = -1, g_leftChain[48], g_nLeftChain = -1, g_leftHandBone = -1;
static void ForceUpdateArms(void* arms);
static void AlignRightHand(void* arms, float* T);
// The left hand only shows while it holds the motion tracker: otherwise the whole left arm collapses to nothing.
static int g_leftAll[64], g_nLeftAll = -1;
static void HideIdleLeftHand(void* arms, void* pawn) {
    if (!g_cfg.hideIdleLeftHand || !arms || !pawn) return;
    void* weapon = Ptr(pawn, 0x488);   // Pawn.Weapon
    const char* wc = weapon ? NameOf(Ptr(weapon, OBJ_CLASS)) : "";
    bool tracker = strstr(wc, "MotionTracker") != NULL;
    static int lastState = -1;
    if (lastState != (int)tracker) { lastState = tracker; Log("Left hand %s (weapon %s)", tracker ? "shown - holding the tracker" : "hidden", *wc ? wc : "none"); }
    if (tracker) return;
    if (g_nLeftAll < 0) {
        g_nLeftAll = 0;
        if (g_nBones <= 0) ArmHideInit(arms);
        int clav = FindBone("Bip01-L-Clavicle"), off = FindBone("Bone_OffhandWeap");
        for (int i = 0; i < g_nBones && g_nLeftAll < 64; i++)
            if ((clav >= 0 && (i == clav || BoneIsUnder(i, clav))) || i == off) g_leftAll[g_nLeftAll++] = i;
        Log("Left hand hiding: %d bones", g_nLeftAll);
    }
    TArrayRaw* sb = (TArrayRaw*)((BYTE*)arms + OFF_SPACEBASES);
    if (!sb->data || sb->num < g_nBones || g_nLeftAll <= 0) return;
    float* P = (float*)((BYTE*)sb->data + 32 * g_leftAll[0]);   // collapse everything onto the clavicle (hidden already)
    float px = P[4], py = P[5], pz = P[6];
    for (int k = 0; k < g_nLeftAll; k++) {
        float* A = (float*)((BYTE*)sb->data + 32 * g_leftAll[k]);
        A[4] = px; A[5] = py; A[6] = pz; A[7] = 0.0f;
    }
}
static bool PlaceOffHand(void* arms) {
    if (!g_cfg.leftHandTracking || !g_lhandValid || !arms) return false;
    if (g_nLeftChain < 0) {
        g_nLeftChain = 0;
        void* lp = FindPropIn(arms, "LocalToWorldBoneAtom");   // FBoneAtom: quat, translation, scale
        if (lp) g_offL2W = *(int*)((BYTE*)lp + g_propOffField);
        if (g_nBones <= 0) ArmHideInit(arms);
        int fore = FindBone("Bip01-L-Forearm"), upper = FindBone("Bip01-L-UpperArm");
        g_leftHandBone = FindBone("Bip01-L-Hand");
        if (fore < 0 || g_leftHandBone < 0 || g_offL2W < 0) { Log("Off hand: bones/LocalToWorld not found (forearm %d hand %d L2W %d)", fore, g_leftHandBone, g_offL2W); return false; }
        for (int i = 0; i < g_nBones && g_nLeftChain < 46; i++) {
            const char* n = NameStrIdx(g_boneNameIdx[i]);
            bool take = BoneIsUnder(i, fore) || (upper >= 0 && g_boneParent[i] == upper && strstr(n, "ForeTwist")) ||
                        (strstr(n, "ForeTwist") && strstr(n, "-L-")) || !lstrcmpiA(n, "Bone_OffhandWeap");
            if (take) g_leftChain[g_nLeftChain++] = i;
        }
        Log("Off hand: %d bones follow the left controller (LocalToWorld @0x%X)", g_nLeftChain, g_offL2W);
    }
    if (g_nLeftChain <= 0) return false;
    TArrayRaw* sb = (TArrayRaw*)((BYTE*)arms + OFF_SPACEBASES);
    if (!sb->data || sb->num < g_nBones) return false;
    ForceUpdateArms(arms);                      // make LocalToWorld match this frame's placement
    const float* L = (const float*)((BYTE*)arms + g_offL2W);   // component -> world: rotation quat, translation, scale
    V3 org = mk(L[4], L[5], L[6]);
    float sc = L[7];
    if (sc < 1e-4f) return false;
    M3 Rc = QuatToM3(L), RcT = Tr(Rc);
    static int logged;
    if (!logged) { logged = 1; float* T = (float*)((BYTE*)arms + g_offArmsT);
        Log("Off hand: component q(%.3f %.3f %.3f %.3f) t(%.1f %.1f %.1f) s %.3f | arms Translation (%.1f %.1f %.1f) | left ctrl (%.1f %.1f %.1f)",
            L[0], L[1], L[2], L[3], org.x, org.y, org.z, sc, T[0], T[1], T[2], g_lhandLoc.x, g_lhandLoc.y, g_lhandLoc.z); }
    // desired hand rotation: the hand keeps its animated pose relative to the arms, re-aimed by how the left
    // controller is turned relative to the right one
    M3 WR = BasisToM3(RotToBasis(g_handRot)), WL = BasisToM3(g_lhandB);
    M3 delta = Mul(Mul(RcT, Mul(WL, Tr(WR))), Rc);   // component-space rotation
    float* H = (float*)((BYTE*)sb->data + 32 * g_leftHandBone);
    V3 ph = mk(H[4], H[5], H[6]);
    // desired hand position: the left controller (+ offset along the controller), in component space
    V3 want = add(g_lhandLoc, add(add(mul(g_lhandB.f, g_cfg.lhOff[0]), mul(g_lhandB.r, g_cfg.lhOff[1])), mul(g_lhandB.u, g_cfg.lhOff[2])));
    V3 phNew = mul(MulV(RcT, sub(want, org)), 1 / sc);
    for (int k = 0; k < g_nLeftChain; k++) {
        float* A = (float*)((BYTE*)sb->data + 32 * g_leftChain[k]);
        V3 p = MulV(delta, sub(mk(A[4], A[5], A[6]), ph));
        M3 q = Mul(delta, QuatToM3(A));
        M3ToQuat(q, A);
        A[4] = phNew.x + p.x; A[5] = phNew.y + p.y; A[6] = phNew.z + p.z;
    }
    return true;
}
// ---- camera lens effects (blood / damage splats): UE3 spawns these as particle quads a fixed distance in front of
// the camera, sized to fill a flat screen. With the VR field of view they show up as a floating red square.
// Null the particle templates on every lens-effect class default object, so new ones spawn empty.
static void DisableLensEffects() {
    if (!g_cfg.noLensEffects || !g_reflOK) return;
    static int offA = -2, offB = -2;
    if (offA == -2) {
        void* a = FindField("EmitterCameraLensEffectBase", "PS_CameraEffect");
        void* b = FindField("EmitterCameraLensEffectBase", "PS_CameraEffectNonExtremeContent");
        offA = a ? *(int*)((BYTE*)a + g_propOffField) : -1;
        offB = b ? *(int*)((BYTE*)b + g_propOffField) : -1;
        Log("Lens effects: PS_CameraEffect @0x%X, NonExtreme @0x%X", offA, offB);
    }
    if (offA < 0 && offB < 0) return;
    void** objs = *(void***)(g_base + RVA_GOBJECTS);
    int num = *(int*)(g_base + RVA_GOBJECTS + 4), cleared = 0;
    for (int i = 0; i < num; i++) {
        void* o = objs[i];
        if (!o) continue;
        const char* n = NameOf(o);
        if (strncmp(n, "Default__", 9)) continue;
        bool lens = false;
        for (void* c = Ptr(o, OBJ_CLASS); c && !lens; c = Ptr(c, STRUCT_SUPER)) if (!lstrcmpA(NameOf(c), "EmitterCameraLensEffectBase")) lens = true;
        if (!lens) continue;
        void** pa = offA > 0 ? (void**)((BYTE*)o + offA) : NULL;
        void** pb = offB > 0 ? (void**)((BYTE*)o + offB) : NULL;
        if ((pa && *pa) || (pb && *pb)) {
            Log("Lens effects: disabled %s (%s)", n + 9, pa && *pa ? NameOf(*pa) : NameOf(*pb));
            if (pa) *pa = NULL;
            if (pb) *pb = NULL;
            cleared++;
        }
    }
    (void)cleared;
}

// ---- post-process effects: log the chain and switch off chromatic aberration (rainbow edges shimmer in VR) ----
static bool IsA(void* o, const char* cls) {
    for (void* c = Ptr(o, OBJ_CLASS); c; c = Ptr(c, STRUCT_SUPER)) if (!lstrcmpA(NameOf(c), cls)) return true;
    return false;
}
static bool ContainsI(const char* s, const char* sub) {
    for (; *s; s++) { int i = 0; while (sub[i] && s[i] && ((s[i] | 32) == (sub[i] | 32))) i++; if (!sub[i]) return true; }
    return false;
}
static void DisablePostEffects() {
    if (!g_reflOK) return;
    static int offShow = -2; static DWORD maskShow; static int offMat = -2;
    if (offShow == -2) {
        void* p = FindField("PostProcessEffect", "bShowInGame");
        offShow = p ? *(int*)((BYTE*)p + g_propOffField) : -1;
        maskShow = p ? *(DWORD*)((BYTE*)p + g_boolMaskField) : 0;
        void* m = FindField("MaterialEffect", "Material");
        offMat = m ? *(int*)((BYTE*)m + g_propOffField) : -1;
        Log("PostFX: bShowInGame @0x%X mask %lX, Material @0x%X", offShow, (unsigned long)maskShow, offMat);
    }
    if (offShow < 0 || !maskShow) return;
    static const char* kOff[] = {"chrom", "aberr", "fringe", "rgbsplit", "rgb_split", "colorsplit", "damage", "hurt", "pain", "blood", "hitfx", "hit_fx", "lowhealth", NULL};
    void** objs = *(void***)(g_base + RVA_GOBJECTS);
    int num = *(int*)(g_base + RVA_GOBJECTS + 4);
    static int logged;
    for (int i = 0; i < num; i++) {
        void* o = objs[i];
        if (!o || !IsA(o, "PostProcessEffect")) continue;
        const char* n = NameOf(o);
        void* mat = (offMat > 0 && IsA(o, "MaterialEffect")) ? *(void**)((BYTE*)o + offMat) : NULL;
        const char* mn = mat ? NameOf(mat) : "";
        void* outer = Ptr(o, OBJ_OUTER);
        DWORD* flags = (DWORD*)((BYTE*)o + offShow);
        bool off = false;
        for (int k = 0; kOff[k] && !off; k++) if (ContainsI(n, kOff[k]) || ContainsI(mn, kOff[k])) off = true;
        if (g_cfg.postFxOff[0]) {    // [Graphics] DisablePostEffects=name1,name2 (effect or material names, partial match)
            char buf[256]; lstrcpynA(buf, g_cfg.postFxOff, 256);
            for (char* t = buf; t && *t;) { char* c = strchr(t, ','); if (c) *c = 0; if (*t && (ContainsI(n, t) || ContainsI(mn, t))) off = true; t = c ? c + 1 : NULL; }
        }
        if (g_cfg.postFxOn[0] && (ContainsI(n, g_cfg.postFxOn) || ContainsI(mn, g_cfg.postFxOn))) off = false;
        if (logged < 200) {
            logged++;
            Log("PostFX: %s.%s (%s) material=%s shown=%d%s", outer ? NameOf(outer) : "?", n, NameOf(Ptr(o, OBJ_CLASS)), mn, (*flags & maskShow) ? 1 : 0, off ? "  -> OFF" : "");
        }
        if (off) *flags &= ~maskShow;
    }
}

// ---- HUD scanline effect (suspected source of the red square): log its material parameters, F2 toggles it ----
static int g_uiFxOff = -1;
static volatile LONG g_uiFxToggleReq;   // -1 = not decided yet (config), 0 = shown, 1 = hidden
static void DumpMIC(void* mic, int depth) {
    if (!mic || depth > 3 || !CanRead(mic, 0x80)) return;
    Log("  material %s (%s)", NameOf(mic), NameOf(Ptr(mic, OBJ_CLASS)));
    if (!IsA(mic, "MaterialInstance")) return;     // base materials keep their parameters elsewhere
    static const char* arrs[] = {"ScalarParameterValues", "VectorParameterValues", "TextureParameterValues"};
    static const int sz[] = {28, 40, 28};
    for (int a = 0; a < 3; a++) {
        void* p = FindPropIn(mic, arrs[a]);
        if (!p) continue;
        TArrayRaw* t = (TArrayRaw*)((BYTE*)mic + *(int*)((BYTE*)p + g_propOffField));
        if (!CanRead(t, 12) || !t->data || t->num <= 0 || t->num > 64 || !CanRead(t->data, t->num * sz[a])) continue;
        for (int i = 0; i < t->num; i++) {
            BYTE* e = (BYTE*)t->data + i * sz[a];
            const char* pn = NameOf(e - OBJ_NAME);
            float* v = (float*)(e + 8);
            if (a == 0) Log("    scalar %s = %.3f", pn, v[0]);
            else if (a == 1) Log("    vector %s = (%.3f %.3f %.3f %.3f)", pn, v[0], v[1], v[2], v[3]);
            else Log("    texture %s = %s", pn, *(void**)v ? NameOf(*(void**)v) : "none");
        }
    }
    void* pp = FindPropIn(mic, "Parent");
    if (pp) { void* par = *(void**)((BYTE*)mic + *(int*)((BYTE*)pp + g_propOffField)); if (par && par != mic) DumpMIC(par, depth + 1); }
}
static void ApplyUiFx(bool dump) {
    if (!g_reflOK) return;
    if (g_uiFxOff < 0) g_uiFxOff = g_cfg.hudScanlineOff;
    void* p = FindField("PostProcessEffect", "bShowInGame");
    void* m = FindField("MaterialEffect", "Material");
    if (!p) return;
    int offShow = *(int*)((BYTE*)p + g_propOffField); DWORD mask = *(DWORD*)((BYTE*)p + g_boolMaskField);
    int offMat = m ? *(int*)((BYTE*)m + g_propOffField) : -1;
    void** objs = *(void***)(g_base + RVA_GOBJECTS);
    int num = *(int*)(g_base + RVA_GOBJECTS + 4);
    for (int i = 0; i < num; i++) {
        void* o = objs[i];
        if (!o || !IsA(o, "UIMaterialEffect") || !strncmp(NameOf(o), "Default__", 9)) continue;
        DWORD* f = (DWORD*)((BYTE*)o + offShow);
        if (g_uiFxOff) *f &= ~mask; else *f |= mask;
        if (dump) {
            void* outer = Ptr(o, OBJ_OUTER);
            Log("HUD effect %s.%s now %s", outer ? NameOf(outer) : "?", NameOf(o), g_uiFxOff ? "OFF" : "ON");
            if (offMat > 0) DumpMIC(*(void**)((BYTE*)o + offMat), 0);
        }
    }
}

// ---- reach for "use / pick up" prompts: find the game's use-range and use-angle settings and widen them ----
struct UseProp { void* cls; int off; float orig, val; char name[64]; };
static UseProp g_useProps[48]; static int g_nUseProps = -1;
static bool HasTok(const char* n, const char* const* toks) { for (int i = 0; toks[i]; i++) if (strstr(n, toks[i])) return true; return false; }
static void UseReachInit() {
    g_nUseProps = 0;
    static const char* kUse[] = {"Use", "Interact", "Usable", "Pickup", "PickUp", NULL};
    static const char* kDist[] = {"Dist", "Range", "Radius", "Reach", "Length", NULL};
    static const char* kDot[] = {"Dot", "Cosine", NULL};
    static const char* kAng[] = {"Angle", "FOV", "Cone", "Degree", NULL};
    void** objs = *(void***)(g_base + RVA_GOBJECTS);
    int num = *(int*)(g_base + RVA_GOBJECTS + 4), logged = 0;
    for (int i = 0; i < num; i++) {
        void* p = objs[i];
        if (!p) continue;
        const char* pt = NameOf(Ptr(p, OBJ_CLASS));
        if (!strstr(pt, "Property")) continue;
        void* cls = Ptr(p, OBJ_OUTER);
        if ((DWORD)cls < 0x10000 || lstrcmpA(NameOf(Ptr(cls, OBJ_CLASS)), "Class")) continue;   // class members only
        const char* n = NameOf(p);
        if (!HasTok(n, kUse)) continue;
        const char* cn = NameOf(cls);
        bool isF = !lstrcmpA(pt, "FloatProperty");
        int kind = !isF ? 0 : HasTok(n, kDist) ? 1 : HasTok(n, kDot) ? 2 : HasTok(n, kAng) ? 3 : 0;
        // the class default object holds the untouched value
        char dn[96]; _snprintf(dn, sizeof(dn), "Default__%s", cn);
        void* def = NULL;
        for (int j = 0; j < num && !def; j++) if (objs[j] && Ptr(objs[j], OBJ_CLASS) == cls && !lstrcmpA(NameOf(objs[j]), dn)) def = objs[j];
        int off = *(int*)((BYTE*)p + g_propOffField);
        float v = (def && isF) ? *(float*)((BYTE*)def + off) : 0;
        if (lstrcmpA(pt, "BoolProperty") && logged++ < 80) Log("Use reach: %s.%s (%s) default %s%.3f%s", cn, n, pt, isF ? "" : "(", v, kind ? (kind == 1 ? "  -> range" : "  -> width") : "");
        if (!strncmp(n, "Min", 3)) kind = 0;   // minimum distances stay as they are
        if (!kind || !def || g_nUseProps >= 48 || v == 0) continue;
        float nv = v;
        if (kind == 1) nv = v * g_cfg.useRange;
        else if (kind == 2) { if (v > -1 && v < 1) { float a = fatan2(fsqrt(1 - v * v), v) * g_cfg.useWidth; if (a > 1.5f) a = 1.5f; nv = fcos(a); } }
        else nv = v * g_cfg.useWidth < 170 ? v * g_cfg.useWidth : 170;
        UseProp& u = g_useProps[g_nUseProps++];
        u.cls = cls; u.off = off; u.orig = v; u.val = nv; lstrcpynA(u.name, n, 64);
        Log("Use reach:   %s %.3f -> %.3f", n, v, nv);
    }
    Log("Use reach: %d settings widened (range x%.2f, width x%.2f)", g_nUseProps, g_cfg.useRange, g_cfg.useWidth);
}
static void ApplyUseReach() {
    if (!g_reflOK || (g_cfg.useRange == 1.0f && g_cfg.useWidth == 1.0f)) return;
    if (g_nUseProps < 0) UseReachInit();
    if (g_nUseProps <= 0) return;
    void** objs = *(void***)(g_base + RVA_GOBJECTS);
    int num = *(int*)(g_base + RVA_GOBJECTS + 4);
    for (int i = 0; i < num; i++) {
        void* o = objs[i];
        if (!o) continue;
        void* c = Ptr(o, OBJ_CLASS);
        for (int k = 0; k < g_nUseProps; k++) {
            bool isA = false;
            for (void* cc = c; (DWORD)cc >= 0x10000 && !isA; cc = Ptr(cc, STRUCT_SUPER)) if (cc == g_useProps[k].cls) isA = true;
            if (!isA) continue;
            float* f = (float*)((BYTE*)o + g_useProps[k].off);
            if (fabsf_(*f - g_useProps[k].orig) < 1e-4f) *f = g_useProps[k].val;   // only values still at the game's default
        }
    }
}

// ---- gun lock: cancel animation moving the gun bone (e.g. the scanner pose tips the gun up) ----
// The gun's held pose is recorded once per weapon (1.5 s after it is equipped); afterwards every frame the gun bone
// and the right hand are moved back onto that recorded pose, so the gun always lines up with the controller.
static int g_gunChain[48], g_nGunChain = -1, g_gunBone = -1;
static float g_gunRef[8]; static int g_gunRefOK; static void* g_gunRefWeapon; static DWORD g_gunWeaponSince;
static inline V3 cross(V3 a, V3 b) { return mk(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
// Removes twist (roll about the barrel) from the recorded gun pose, so a pose caught mid-animation
// doesn't leave the gun permanently tilted in the hand. Barrel direction (aim) is kept as recorded.
static int g_gunUpAxis = -1, g_gunUpSign = 1;   // which gun-bone axis is "up" when the gun is held normally (learned, saved)
static float LevelGunRef(float* q) {
    M3 R = QuatToM3(q);
    V3 col[3]; for (int i = 0; i < 3; i++) col[i] = mk(R.m[0][i], R.m[1][i], R.m[2][i]);
    V3 X = mk(1, 0, 0), Z = mk(0, 0, 1);
    int b = 0; for (int i = 1; i < 3; i++) if (fabsf_(dot(col[i], X)) > fabsf_(dot(col[b], X))) b = i;
    int u = -1; for (int i = 0; i < 3; i++) if (i != b && (u < 0 || fabsf_(dot(col[i], Z)) > fabsf_(dot(col[u], Z)))) u = i;
    if (fabsf_(dot(col[b], X)) < 0.7f) return 0;          // no bone axis clearly along the aim: don't guess
    V3 f = col[b];
    // A pose caught mid-animation (a reload roll, say) can be upside down. Learn the normal "up" axis and side from a
    // clearly level pose once, keep it in acmvr.ini, and always turn the gun to that side, flipping it back if needed.
    if (g_gunUpAxis < 0 || g_gunUpAxis == b) {
        if (fabsf_(dot(col[u], Z)) > 0.85f) {
            g_gunUpAxis = u; g_gunUpSign = dot(col[u], Z) >= 0 ? 1 : -1;
            char v[8]; _snprintf(v, 8, "%d", g_gunUpSign * (u + 1)); WritePrivateProfileStringA("Arms", "GunUpAxis", v, g_ini);
            Log("Gun lock: learned gun up axis %d (side %+d)", u, g_gunUpSign);
        }
    } else u = g_gunUpAxis;
    float side = g_gunUpAxis == u ? (float)g_gunUpSign : (dot(col[u], Z) >= 0 ? 1.0f : -1.0f);
    V3 zt = mul(Z, side);
    V3 up = sub(zt, mul(f, dot(f, zt)));
    float n = fsqrt(dot(up, up)); if (n < 0.2f) return 0;   // barrel nearly vertical: leave it
    up = mul(up, 1.0f / n);
    float ang = fatan2(dot(cross(col[u], up), f), dot(col[u], up));
    float c = fcos(ang), s = fsin(ang), t = 1 - c;
    M3 Rf;   // rotation by ang about f (Rodrigues)
    Rf.m[0][0] = t * f.x * f.x + c;       Rf.m[0][1] = t * f.x * f.y - s * f.z; Rf.m[0][2] = t * f.x * f.z + s * f.y;
    Rf.m[1][0] = t * f.x * f.y + s * f.z; Rf.m[1][1] = t * f.y * f.y + c;       Rf.m[1][2] = t * f.y * f.z - s * f.x;
    Rf.m[2][0] = t * f.x * f.z - s * f.y; Rf.m[2][1] = t * f.y * f.z + s * f.x; Rf.m[2][2] = t * f.z * f.z + c;
    M3ToQuat(Mul(Rf, R), q);
    return ang * 57.29578f;
}
static void LockGun(void* arms, void* pawn) {
    if (!g_cfg.lockGun || !arms) return;
    if (g_nGunChain < 0) {
        g_nGunChain = 0;
        if (g_nBones <= 0) ArmHideInit(arms);
        g_gunBone = FindBone("Bone_WEAPBONE");
        int rhand = FindBone("Bip01-R-Hand"), rfore = FindBone("Bip01-R-Forearm");
        if (g_gunBone < 0 || rhand < 0) { Log("Gun lock: bones not found"); return; }
        for (int i = 0; i < g_nBones && g_nGunChain < 46; i++) {
            const char* n = NameStrIdx(g_boneNameIdx[i]);
            if (i == g_gunBone || BoneIsUnder(i, g_gunBone) || BoneIsUnder(i, rhand) || (rfore >= 0 && i == rfore) ||
                (strstr(n, "ForeTwist") && strstr(n, "-R-")))
                g_gunChain[g_nGunChain++] = i;
        }
        Log("Gun lock: %d bones (gun bone + right hand) held to the controller", g_nGunChain);
    }
    if (g_nGunChain <= 0) return;
    TArrayRaw* sb = (TArrayRaw*)((BYTE*)arms + OFF_SPACEBASES);
    if (!sb->data || sb->num < g_nBones) return;
    float* G = (float*)((BYTE*)sb->data + 32 * g_gunBone);
    void* weapon = pawn ? Ptr(pawn, 0x488) : NULL;   // Pawn.Weapon
    DWORD now = GetTickCount();
    if (weapon != g_gunRefWeapon) {
        g_gunRefWeapon = weapon; g_gunWeaponSince = now; g_gunRefOK = 0;
        void* wd = weapon ? FindPropIn(weapon, "WeaponDefinition") : NULL;
        void* def = wd ? *(void**)((BYTE*)weapon + *(int*)((BYTE*)wd + g_propOffField)) : NULL;
        Log("Weapon changed to %s (%s)", def ? NameOf(def) : "?", weapon ? NameOf(Ptr(weapon, OBJ_CLASS)) : "none");
    }
    if (!g_gunRefOK) {
        static float lastQ[4]; static DWORD stillSince;
        float d = fabsf_(lastQ[0] * G[0] + lastQ[1] * G[1] + lastQ[2] * G[2] + lastQ[3] * G[3]);   // |cos(half angle)| between frames
        memcpy(lastQ, G, sizeof(lastQ));
        if (d < 0.99996f) stillSince = now;                 // moved more than ~1 degree since last frame
        if (now - g_gunWeaponSince < 1500) return;          // let the equip animation finish
        if (now - stillSince < 300 && now - g_gunWeaponSince < 6000) return;   // and wait until the gun is steady
        memcpy(g_gunRef, G, sizeof(g_gunRef)); g_gunRefOK = 1;
        float roll = g_cfg.levelGun ? LevelGunRef(g_gunRef) : 0;
        Log("Gun lock: held pose recorded for %s (twist straightened by %.1f deg)", weapon ? NameOf(Ptr(weapon, OBJ_CLASS)) : "(no weapon)", roll);
        return;
    }
    // delta that takes the gun bone from its animated pose back to the recorded one
    M3 Rref = QuatToM3(g_gunRef), Rcur = QuatToM3(G);
    M3 D = Mul(Rref, Tr(Rcur));
    V3 pcur = mk(G[4], G[5], G[6]), pref = mk(g_gunRef[4], g_gunRef[5], g_gunRef[6]);
    for (int k = 0; k < g_nGunChain; k++) {
        float* A = (float*)((BYTE*)sb->data + 32 * g_gunChain[k]);
        V3 p = MulV(D, sub(mk(A[4], A[5], A[6]), pcur));
        M3 q = Mul(D, QuatToM3(A));
        M3ToQuat(q, A);
        A[4] = pref.x + p.x; A[5] = pref.y + p.y; A[6] = pref.z + p.z;
    }
}
// ---- right hand on the controller: measure where the right hand bone ends up and shift the whole arm model ----
static int g_rightHandBone = -2;
static bool ReadL2W(void* arms, M3* Rc, V3* org, float* sc) {
    if (g_offL2W < 0) { void* lp = FindPropIn(arms, "LocalToWorldBoneAtom"); if (lp) g_offL2W = *(int*)((BYTE*)lp + g_propOffField); }
    if (g_offL2W < 0) return false;
    const float* L = (const float*)((BYTE*)arms + g_offL2W);
    if (L[7] < 1e-4f) return false;
    *Rc = QuatToM3(L); *org = mk(L[4], L[5], L[6]); *sc = L[7];
    return true;
}
static void AlignRightHand(void* arms, float* T) {
    if (!g_cfg.alignRightHand || !arms || !T) return;
    if (g_rightHandBone == -2) { if (g_nBones <= 0) ArmHideInit(arms); g_rightHandBone = FindBone("Bip01-R-Hand"); }
    if (g_rightHandBone < 0) return;
    TArrayRaw* sb = (TArrayRaw*)((BYTE*)arms + OFF_SPACEBASES);
    if (!sb->data || sb->num <= g_rightHandBone) return;
    ForceUpdateArms(arms);                       // component transform for this frame's placement
    M3 Rc; V3 org; float sc;
    if (!ReadL2W(arms, &Rc, &org, &sc)) return;
    float* H = (float*)((BYTE*)sb->data + 32 * g_rightHandBone);
    V3 handW = add(org, mul(MulV(Rc, mk(H[4], H[5], H[6])), sc));
    Basis G = RotToBasis(g_handRot);
    V3 target = add(mk(g_handLoc.X, g_handLoc.Y, g_handLoc.Z),
                    add(add(mul(G.f, g_cfg.rhOff[0]), mul(G.r, g_cfg.rhOff[1])), mul(G.u, g_cfg.rhOff[2])));
    V3 e = sub(handW, target);
    {   // how far the game's own arm pose leaves the hand from the controller (arm-reach check), every 10 s
        static DWORD t0; static float worst; static int skips;
        float d = fsqrt(dot(e, e)); if (d > worst) worst = d;
        if (dot(e, e) > 300.0f * 300.0f) skips++;
        DWORD now = GetTickCount();
        if (now - t0 > 10000) { if (t0) Log("Right hand: largest correction %.0f units in 10 s%s", worst, skips ? " (some frames skipped as too far)" : ""); t0 = now; worst = 0; skips = 0; }
    }
    if (dot(e, e) > 300.0f * 300.0f) return;     // nonsense (teleport / level change): leave it
    T[0] -= e.x; T[1] -= e.y; T[2] -= e.z;
    g_gunCamLoc.X -= e.x; g_gunCamLoc.Y -= e.y; g_gunCamLoc.Z -= e.z;   // the aim follows the gun
    static int logged;
    if (!logged) { logged = 1; Log("Right hand: moved arms by (%.1f %.1f %.1f) to put the hand on the controller", e.x, e.y, e.z); }
}
static void PushArmPose(void* arms) {
    if (!g_fnForceUpdate2) return;
    DWORD parms[8] = {1, 0, 0, 0, 0, 0, 0, 0};   // bTransformOnly
    ProcessEvent_t pe = (ProcessEvent_t)(*(void***)arms)[0x108 / 4];
    pe(arms, g_fnForceUpdate2, parms, NULL, 0, 0);
}
// ---- dynamic resolution: steer the engine's ScreenPercentage from measured GPU time (game thread) ----
static void* g_fnConsoleCmd;
static bool GameConsoleCommand(void* pc, const char* cmd) {
    if (!g_fnConsoleCmd) g_fnConsoleCmd = FindField("Actor", "ConsoleCommand");
    if (!g_fnConsoleCmd || !pc) return false;
    static wchar_t wbuf[128];
    int n = 0; for (; cmd[n] && n < 127; n++) wbuf[n] = (wchar_t)cmd[n]; wbuf[n] = 0;
    DWORD parms[16]; memset(parms, 0, sizeof(parms));
    parms[0] = (DWORD)wbuf; parms[1] = n + 1; parms[2] = n + 1;   // FString Command
    parms[3] = 0;                                                  // bWriteToLog = false
    ProcessEvent_t pe = (ProcessEvent_t)(*(void***)pc)[0x108 / 4];
    pe(pc, g_fnConsoleCmd, parms, NULL, 0, 0);
    return true;
}
static void DynamicResolution(void* pc) {
    if (!g_cfg.dynRes || !g_vrOK || g_refreshMs <= 0) return;
    static DWORD lastChange, lowSince, lastCall, gameplaySince, holdUntil;
    static float gpuBeforeDrop = -1; static int pctBeforeDrop;
    DWORD now = GetTickCount();
    if (now - lastCall > 500) gameplaySince = now;    // (re)entered gameplay: a level just loaded or a cutscene ended
    lastCall = now;
    if (now - gameplaySince < 6000) return;            // ignore loading / streaming spikes
    float gpu = g_gpuEma;
    if (gpu <= 0 || now - lastChange < 1500) return;  // let the measurement settle after a change
    int cur = (int)g_screenPct, want = cur;
    float target = g_cfg.dynTargetGpu;
    if (target > g_refreshMs * 0.95f) target = (float)(g_refreshMs * 0.95);
    const char* why = "";
    if (gpuBeforeDrop > 0) {
        // did the last drop help? The GPU timer also stretches when the render thread is the slow part, and then
        // lowering resolution does nothing - undo it and leave resolution alone for a while.
        float gain = gpuBeforeDrop - gpu;
        if (gain < gpuBeforeDrop * 0.04f) { want = pctBeforeDrop; holdUntil = now + 10000; why = " (no gain: CPU-limited, undo)"; }
        gpuBeforeDrop = -1;
    }
    if (*why) {
    } else if (gpu > target) {                        // too slow: drop quicker the further over we are
        if ((int)(now - holdUntil) < 0) return;
        want = cur - (gpu > target * 1.15f ? 10 : 5); lowSince = 0;
        if (want >= g_cfg.dynMin) { gpuBeforeDrop = gpu; pctBeforeDrop = cur; }
    } else if (gpu < target * 0.80f) {                // comfortably under: creep back up after 3 s
        if (!lowSince) lowSince = now;
        else if (now - lowSince > 3000) { want = cur + 5; lowSince = now; }
    } else lowSince = 0;
    if (want < g_cfg.dynMin) want = g_cfg.dynMin;
    if (want > g_cfg.dynMax) want = g_cfg.dynMax;
    if (want == cur) return;
    char cmd[64]; wsprintfA(cmd, "SCALE SET ScreenPercentage %d", want);
    double t0 = QpcMs();
    if (!GameConsoleCommand(pc, cmd)) { static int warned; if (!warned++) Log("Dynamic resolution: ConsoleCommand not found"); return; }
    double dt = QpcMs() - t0;
    static int logs;
    if (logs++ < 60) Log("Dynamic resolution: GPU %.2f ms (target %.1f) -> ScreenPercentage %d -> %d%s (command took %.2f ms)", gpu, target, cur, want, why, dt);
    InterlockedExchange(&g_screenPct, want);
    lastChange = now;
}
// The first-person arms component of the player's pawn - only when it really is one. The pawn can briefly be
// a different kind of actor (death, respawn, level change), and then the same offset holds something else.
static void* ArmsOf(void* pc) {
    void* pawn = Ptr(pc, OFF_PAWN);
    if ((DWORD)pawn < 0x10000 || !CanRead((BYTE*)pawn + 0x18E0, 4)) return NULL;
    void* arms = Ptr(pawn, 0x18E0);
    if ((DWORD)arms < 0x10000 || !CanRead(arms, 0x400)) return NULL;
    static void* lastOk; static void* lastBad;
    if (arms == lastOk) return arms;
    if (arms == lastBad) return NULL;
    if (g_reflOK && !IsA(arms, "SkeletalMeshComponent")) {
        lastBad = arms; Log("Arms: pawn %s holds no arms component (%s) - skipping arm placement", NameOf(pawn), NameOf(Ptr(arms, OBJ_CLASS)));
        return NULL;
    }
    lastOk = arms;
    return arms;
}
static void PlaceArmsAtHand(void* pc) {
    void* arms = ArmsOf(pc);
    if (!arms) return;
    void* pawn = Ptr(pc, OFF_PAWN);
    if (!g_armsInit) {
        g_armsInit = 1;
        if (g_reflOK && CalibrateFields()) {
            void* tp = FindPropIn(arms, "Translation");
            if (tp) g_offArmsT = *(int*)((BYTE*)tp + g_propOffField);
            void* sp = FindPropIn(arms, "Scale");
            if (sp) g_offArmsS = *(int*)((BYTE*)sp + g_propOffField);
            void* rp = FindPropIn(arms, "Rotation");
            if (rp) g_offArmsR = *(int*)((BYTE*)rp + g_propOffField);
            g_fnForceUpdate = FindField("ActorComponent", "ForceUpdate");
        }
        Log("Arms placement: Translation @0x%X Rotation @0x%X, ForceUpdate %p", g_offArmsT, g_offArmsR, g_fnForceUpdate);

    }
    if (g_offArmsT < 0 || !g_fnForceUpdate) return;
    Basis G = RotToBasis(g_handRot);
    V3 p = add(mk(g_handLoc.X, g_handLoc.Y, g_handLoc.Z), add(add(mul(G.f, g_armsX), mul(G.r, g_armsY)), mul(G.u, g_armsZ)));
    float* T = (float*)((BYTE*)arms + g_offArmsT);
    // Where would the flat game's camera be relative to these arms? The game just placed the arms relative
    // to its own camera (g_cacheCam) - measure that offset and put the gameplay "aim camera" at the same
    // offset from OUR arm position, so shots line up with the gun's sights.
    {
        V3 rel = mk(T[0] - g_cacheCam.X, T[1] - g_cacheCam.Y, T[2] - g_cacheCam.Z);
        V3 A = mk(dot(rel, G.f), dot(rel, G.r), dot(rel, G.u));
        static V3 sA; static int have;
        float len2 = dot(A, A);
        if (len2 < 200.0f * 200.0f) {           // ignore garbage (e.g. arms hidden/teleported)
            if (!have) { sA = A; have = 1; Log("Arms offset from game camera: %.1f %.1f %.1f", A.x, A.y, A.z); }
            else sA = add(mul(sA, 0.9f), mul(A, 0.1f));
            V3 cam = sub(p, add(add(mul(G.f, sA.x), mul(G.r, sA.y)), mul(G.u, sA.z)));
            g_gunCamLoc.X = cam.x; g_gunCamLoc.Y = cam.y; g_gunCamLoc.Z = cam.z;
            g_aimCamFromArms = 1;
        }
    }
    T[0] = p.x; T[1] = p.y; T[2] = p.z;
    if (g_offArmsR > 0) { FRotator* R = (FRotator*)((BYTE*)arms + g_offArmsR); *R = g_handRot; }
    if (g_offArmsS > 0) *(float*)((BYTE*)arms + g_offArmsS) = g_gunScale;
    LockGun(arms, pawn);     // gun + right hand back onto the recorded held pose
    AlignRightHand(arms, T); // slide the arms so the right hand sits exactly on the right controller
    PlaceOffHand(arms);      // left forearm/hand to the left controller (needs this frame's LocalToWorld)
    ApplyArmHide(arms);      // then collapse shoulders/upper arms onto the (possibly moved) elbows
    HideIdleLeftHand(arms, pawn);
    ForceUpdateArms(arms);
}
static void ForceUpdateArms(void* arms) {
    DWORD parms[8] = {1, 0, 0, 0, 0, 0, 0, 0};   // bTransformOnly = true
    ProcessEvent_t pe = (ProcessEvent_t)(*(void***)arms)[0x108 / 4];
    pe(arms, g_fnForceUpdate, parms, NULL, 0, 0);
}

// ---- crosshair / HUD ----
struct BoolSet { int off; DWORD mask; int value; };
static BoolSet g_xhair[16]; static int g_numXhair;
static int g_hudInit; static int g_offMyHUD = -1; static BoolProp g_bShowHUD = {-1, 0};
static volatile LONG g_hudToggle;   // F6
static void CollectCrosshairBools(void* obj, const char* label) {
    if (!obj) return;
    for (void* c = Ptr(obj, OBJ_CLASS); c; c = Ptr(c, STRUCT_SUPER))
        for (void* f = Ptr(c, g_offChildren); f; f = Ptr(f, g_offNext)) {
            const char* n = NameOf(f);
            if (!strstr(n, "Crosshair") && !strstr(n, "CrossHair") && !strstr(n, "Reticle") && !strstr(n, "Reticule")) continue;
            const char* type = NameOf(Ptr(f, OBJ_CLASS));
            Log("  %s.%s.%s (%s)", label, NameOf(c), n, type);
            if (lstrcmpA(type, "BoolProperty") || g_numXhair >= 16 || !g_boolMaskField) continue;
            int v = (!strncmp(n, "bHide", 5) || !strncmp(n, "bNo", 3) || !strncmp(n, "bDisable", 8)) ? 1 : 0;
            BoolSet& b = g_xhair[g_numXhair++];
            b.off = *(int*)((BYTE*)f + g_propOffField); b.mask = *(DWORD*)((BYTE*)f + g_boolMaskField); b.value = v;
            Log("    -> will force %s = %d", n, v);
        }
}
static void* g_xhairObjs[4];
static void ApplyHUD(void* pc) {
    if (!g_reflOK || !CalibrateFields()) return;
    void* pawn = Ptr(pc, OFF_PAWN);
    void* weapon = Ptr(pawn, 0x488);
    if (!g_hudInit) {
        g_hudInit = 1;
        g_offMyHUD = PropOffset("PlayerController", "myHUD");
        g_bShowHUD = BoolOffset("HUD", "bShowHUD");
        void* hud = g_offMyHUD > 0 ? Ptr(pc, (DWORD)g_offMyHUD) : NULL;
        Log("HUD: %s (class %s), myHUD @0x%X, bShowHUD @0x%X/%lX. Crosshair-related properties:", NameOf(hud), NameOf(Ptr(hud, OBJ_CLASS)),
            g_offMyHUD, g_bShowHUD.off, (unsigned long)g_bShowHUD.mask);
        // record which object each bool belongs to: 0 = HUD, 1 = controller, 2 = pawn, 3 = weapon
        int start;
        start = g_numXhair; CollectCrosshairBools(hud, "HUD");        for (int i = start; i < g_numXhair; i++) g_xhair[i].value |= 0 << 4;
        start = g_numXhair; CollectCrosshairBools(pc, "Controller");  for (int i = start; i < g_numXhair; i++) g_xhair[i].value |= 1 << 4;
        start = g_numXhair; CollectCrosshairBools(pawn, "Pawn");      for (int i = start; i < g_numXhair; i++) g_xhair[i].value |= 2 << 4;
        start = g_numXhair; CollectCrosshairBools(weapon, "Weapon");  for (int i = start; i < g_numXhair; i++) g_xhair[i].value |= 3 << 4;
    }
    void* hud = g_offMyHUD > 0 ? Ptr(pc, (DWORD)g_offMyHUD) : NULL;
    void* objs[4] = {hud, pc, pawn, weapon};
    if (g_cfg.hideCrosshair)
        for (int i = 0; i < g_numXhair; i++) {
            void* o = objs[g_xhair[i].value >> 4];
            if (!o) continue;
            DWORD* w = (DWORD*)((BYTE*)o + g_xhair[i].off);
            if (g_xhair[i].value & 1) *w |= g_xhair[i].mask; else *w &= ~g_xhair[i].mask;
        }
    bool hideAll = (g_cfg.hideHUD != 0) ^ (g_hudToggle != 0);
    if (hud && g_bShowHUD.off > 0) {
        DWORD* w = (DWORD*)((BYTE*)hud + g_bShowHUD.off);
        static int weHid;
        if (hideAll) { *w &= ~g_bShowHUD.mask; weHid = 1; }
        else if (weHid) { *w |= g_bShowHUD.mask; weHid = 0; }   // only undo our own change
    }
}

// ---- red-square hunt: F3 logs the HUD's settings and every Flash (GFx) screen that is open ----
static volatile LONG g_hudSurveyReq;
static void SurveyHudProps(void* obj, const char* label) {
    if (!obj) return;
    int n = 0;
    for (void* c = Ptr(obj, OBJ_CLASS); (DWORD)c >= 0x10000 && lstrcmpA(NameOf(c), "Actor") && lstrcmpA(NameOf(c), "Object"); c = Ptr(c, STRUCT_SUPER))
        for (void* f = Ptr(c, g_offChildren); (DWORD)f >= 0x10000 && n < 600; f = Ptr(f, g_offNext)) {
            const char* type = NameOf(Ptr(f, OBJ_CLASS));
            if (!strstr(type, "Property")) continue;
            int off = *(int*)((BYTE*)f + g_propOffField);
            BYTE* p = (BYTE*)obj + off;
            char v[96]; v[0] = 0;
            if (!lstrcmpA(type, "FloatProperty")) _snprintf(v, sizeof(v), "%.3f", *(float*)p);
            else if (!lstrcmpA(type, "IntProperty")) _snprintf(v, sizeof(v), "%d", *(int*)p);
            else if (!lstrcmpA(type, "ByteProperty")) _snprintf(v, sizeof(v), "%d", *p);
            else if (!lstrcmpA(type, "BoolProperty")) _snprintf(v, sizeof(v), "%d", (*(DWORD*)p & *(DWORD*)((BYTE*)f + g_boolMaskField)) ? 1 : 0);
            else if (!lstrcmpA(type, "ObjectProperty") || !lstrcmpA(type, "ComponentProperty")) { void* o = *(void**)p; _snprintf(v, sizeof(v), "%s (%s)", o ? NameOf(o) : "none", o ? NameOf(Ptr(o, OBJ_CLASS)) : ""); }
            else if (!lstrcmpA(type, "ArrayProperty")) _snprintf(v, sizeof(v), "[%d]", ((TArrayRaw*)p)->num);
            else continue;
            Log("   %s %s.%s @0x%X = %s", label, NameOf(c), NameOf(f), off, v);
            n++;
        }
}
static void HudSurvey(void* pc) {
    if (!g_reflOK) return;
    void* hud = g_offMyHUD > 0 ? Ptr(pc, (DWORD)g_offMyHUD) : NULL;
    Log("F3 survey: HUD %s (%s)", NameOf(hud), NameOf(Ptr(hud, OBJ_CLASS)));
    SurveyHudProps(hud, "HUD");
    void** objs = *(void***)(g_base + RVA_GOBJECTS);
    int num = *(int*)(g_base + RVA_GOBJECTS + 4), k = 0;
    for (int i = 0; i < num && k < 60; i++) {
        void* o = objs[i];
        if (!o || !strncmp(NameOf(o), "Default__", 9) || !IsA(o, "GFxMoviePlayer")) continue;
        void* mi = FindPropIn(o, "MovieInfo");
        void* swf = mi ? *(void**)((BYTE*)o + *(int*)((BYTE*)mi + g_propOffField)) : NULL;
        Log("  GFx movie %s (%s) swf=%s", NameOf(o), NameOf(Ptr(o, OBJ_CLASS)), swf ? NameOf(swf) : "?");
        if (k < 6) SurveyHudProps(o, "  movie");
        k++;
    }
}

// ---- a real view state for the right eye ----
// Found at runtime: player's FSceneViewState vtable -> constructor (the code storing that vtable) ->
// the tiny engine function that allocates + constructs one (AllocateViewState / Reference::Allocate).
static void* g_rightState;
static int g_rightStateTried;
static BYTE* g_textLo; static BYTE* g_textHi;
static BYTE* FuncStartRT(BYTE* p) {
    for (BYTE* q = p; q > p - 0x400 && q > g_textLo + 4; q--) {
        BYTE prev = q[-1];
        if ((prev == 0xCC || prev == 0xC3 || prev == 0x90) && ((DWORD)q & 0xF) == 0) return q;
    }
    return NULL;
}
typedef void*(THISCALL* AllocVS_t)(void*);
static void* CreateRightEyeState(void* leftState) {
    // AllocateViewState() at RVA 0x6FE5F0: no arguments, returns new FSceneViewState (the game stores the
    // result straight into ULocalPlayer::ViewState at 0x47F67A). Verify the prologue before calling.
    BYTE* fn = (BYTE*)(g_base + 0x6FE5F0);
    static const BYTE pro[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68};
    if (memcmp(fn, pro, 6) != 0 || !leftState) { Log("ViewState: allocator not recognised - right eye stays stateless"); return NULL; }
    typedef void*(*AllocVS_t)();
    void* st = ((AllocVS_t)fn)();
    bool ok = st && CanRead(st, 4) && *(DWORD*)st == *(DWORD*)leftState;
    Log("ViewState: right-eye state %s (%p)", ok ? "created" : "FAILED", st);
    return ok ? st : NULL;
}

// ---- temporal anti-aliasing state shared by the game thread (jitter) and render thread (resolve) ----
static volatile LONG g_taaOn;          // TAA running (config + F1 toggle, and resources OK)
static volatile LONG g_taaUserOff;     // F1 toggled it off
static float g_taaJx, g_taaJy;         // this frame's jitter in pixels
static unsigned g_taaSeq;
static float Halton(unsigned i, unsigned b) { float f = 1, r = 0; while (i) { f /= b; r += f * (i % b); i /= b; } return r; }
static void TaaNextJitter() {
    unsigned i = (g_taaSeq++ % 8) + 1;
    g_taaJx = (Halton(i, 2) - 0.5f) * g_cfg.taaJitter;
    g_taaJy = (Halton(i, 3) - 0.5f) * g_cfg.taaJitter;
}

// ---- off-centre (asymmetric) per-eye projection ----
// CalcSceneView builds a symmetric projection and passes it (argument 16) to the FSceneView constructor,
// which derives every other matrix and the culling frustum from it. We make it match the headset's
// asymmetric per-eye frustum just before the constructor runs, so no pixels are rendered outside the eye's view.
typedef void*(THISCALL* SVCtor_t)(void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*,
                                   void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*, void*);
static SVCtor_t o_SVCtor;
static void* THISCALL hkSceneViewCtor(void* self, void* a1, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8,
                                      void* a9, void* a10, void* a11, void* a12, void* a13, void* a14, void* a15, void* a16,
                                      void* a17, void* a18, void* a19, void* a20, void* a21, void* a22, void* a23, void* a24) {
    if (g_inSBS && g_cfg.offAxis && a16) {
        float* P = (float*)a16;   // FMatrix, row-major, row-vector convention: clip = v * P
        bool looksPerspective = fabsf_(P[3]) < 1e-4f && fabsf_(P[7]) < 1e-4f && fabsf_(P[11] - 1.0f) < 1e-3f && fabsf_(P[15]) < 1e-4f && P[0] > 0 && P[5] > 0;
        if (looksPerspective) {
            bool swap = (g_cfg.swapEyes != 0) ^ (g_swapHotkey != 0);
            int eye = swap ? 1 - g_curEye : g_curEye;   // which headset eye this half of the image goes to
            float l = g_projRaw[eye][0], r = g_projRaw[eye][1], t = g_projRaw[eye][2], b = g_projRaw[eye][3];
            float tv = (fabsf_(t) > fabsf_(b) ? fabsf_(t) : fabsf_(b)) * (1.0f + g_cfg.fovMargin / 100.0f);
            static int logged;
            if (!logged) { logged = 1; Log("Off-centre projection: before M00 %.4f M11 %.4f M20 %.4f", P[0], P[5], P[8]); }
            P[0] = 2.0f / (r - l);          // x scale over the eye's own tangent range
            P[8] = -(r + l) / (r - l);      // horizontal shift (multiplies view depth)
            // OpenVR naming: in its reference projection (ComposeProjection) the top of the image is at tangent
            // pfBottom and the bottom at pfTop, so "b" is the upward extent and -"t" the downward one
            float up = tv, dn = tv;
            if (g_cfg.offAxisV) {           // fit the eye's own up and down range, so nothing has to be cropped off
                float m = 1.0f + g_cfg.fovMargin / 100.0f;
                up = b * m; dn = -t * m;
            }
            P[5] = 2.0f / (up + dn);        // vertical scale over the covered range
            P[9] = (dn - up) / (up + dn);   // vertical shift (0 when up == down)
            if (logged == 1) { logged = 2; Log("Off-centre projection: eye %d tangents l %.3f r %.3f t %.3f b %.3f -> M00 %.4f M20 %.4f M11 %.4f M21 %.4f", eye, l, r, t, b, P[0], P[8], P[5], P[9]); }
            g_offAxisUp = up; g_offAxisDn = dn;
            InterlockedExchange(&g_offAxisActive, 1);
            if (g_taaOn && g_bbW && g_bbH) {   // TAA: sub-pixel camera shift, same for both eyes of a frame
                P[8] += 2.0f * g_taaJx / (0.5f * (float)g_bbW);
                P[9] += 2.0f * g_taaJy / (float)g_bbH;
            }
        }
    }
    return o_SVCtor(self, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21, a22, a23, a24);
}

// ---- full-screen game overlays during play (objectives etc.): show them on the flat screen ----
// The game draws these as Flash (GFx) screens sized to the whole image, which in VR fills your entire view.
// We remember which Flash screens are open during normal play (the HUD), and when a new one opens just after
// Start/Back, show the game on the flat screen until it closes.
static volatile LONG g_backHeld, g_backLatch;   // Back (objectives/score screen): held now / tapped to keep it shown
static void* g_ovlBase[24]; static int g_ovlBaseN;
static void* g_ovlMovie;              // the overlay currently shown on the screen
static DWORD g_ovlLastScan, g_ovlPressSeen;
static bool MovieOpen(void* o) {
    void* f = FindPropIn(o, "bMovieIsOpen");
    if (!f) return true;
    int off = *(int*)((BYTE*)f + g_propOffField);
    return (*(DWORD*)((BYTE*)o + off) & *(DWORD*)((BYTE*)f + g_boolMaskField)) != 0;
}
// Walking every object in the game is slow (a visible hitch), so it is only done once when play starts and
// for a moment after Start/Back (when a new screen may be created); otherwise only the known Flash screens are checked.
static void* g_gfxAll[64]; static int g_gfxIdx[64]; static int g_gfxAllN; static bool g_gfxListed;
static void ListAllMovies() {
    void** objs = *(void***)(g_base + RVA_GOBJECTS);
    int num = *(int*)(g_base + RVA_GOBJECTS + 4); g_gfxAllN = 0;
    for (int i = 0; i < num && g_gfxAllN < 64; i++) {
        void* o = objs[i];
        if (!o || !strncmp(NameOf(o), "Default__", 9) || !IsA(o, "GFxMoviePlayer")) continue;
        g_gfxIdx[g_gfxAllN] = i; g_gfxAll[g_gfxAllN++] = o;
    }
    g_gfxListed = true;
}
static int OpenMovies(void** out, int max, bool fullScan = false) {
    if (fullScan || !g_gfxListed) ListAllMovies();
    void** objs = *(void***)(g_base + RVA_GOBJECTS);
    int num = *(int*)(g_base + RVA_GOBJECTS + 4), n = 0;
    for (int i = 0; i < g_gfxAllN && n < max; i++) {
        // a level change can free a screen: only use entries still in the object table at the same slot
        if (g_gfxIdx[i] >= num || objs[g_gfxIdx[i]] != g_gfxAll[i]) { g_gfxListed = false; continue; }
        if (MovieOpen(g_gfxAll[i])) out[n++] = g_gfxAll[i];
    }
    return n;
}

// ---- objectives / scoreboard panel (Back): move it down, shrink it and push it back ----
// It is a Flash screen (PecanUIScreenGUScoreboard) in the game's UI shell; its display info (position, scale,
// depth) is set through the game's own GFxValue.GetDisplayInfo / SetDisplayInfo.
static struct { int tried, ok, retOff, dOff, oX, oY, oZ, oXS, oYS, oFlags; DWORD mX, mY, mZ, mXS, mYS; void* fnGet; void* fnSet; void* fnVW; void* fnVH; int vwOff, vhOff; } g_di;
static bool DispInfoInit() {
    if (g_di.tried) return g_di.ok != 0;
    g_di.tried = 1;
    g_di.fnGet = FindField("GFxValue", "GetDisplayInfo");
    g_di.fnSet = FindField("GFxValue", "SetDisplayInfo");
    void* rv = FindField("GetDisplayInfo", "ReturnValue");
    void* dp = FindField("SetDisplayInfo", "D");
    void* fX = FindField("ASDisplayInfo", "X"); void* hX = FindField("ASDisplayInfo", "hasX");
    g_di.fnVW = FindField("TGGFxUIComponent", "GetViewportWidth"); g_di.fnVH = FindField("TGGFxUIComponent", "GetViewportHeight");
    void* rw = FindField("GetViewportWidth", "ReturnValue"); void* rh = FindField("GetViewportHeight", "ReturnValue");
    void* fY = FindField("ASDisplayInfo", "Y"); void* fZ = FindField("ASDisplayInfo", "Z");
    void* fXS = FindField("ASDisplayInfo", "XScale"); void* fYS = FindField("ASDisplayInfo", "YScale");
    void* hY = FindField("ASDisplayInfo", "hasY"); void* hZ = FindField("ASDisplayInfo", "hasZ");
    void* hXS = FindField("ASDisplayInfo", "hasXScale"); void* hYS = FindField("ASDisplayInfo", "hasYScale");
    if (!dp) { void* f = g_di.fnSet ? Ptr(g_di.fnSet, g_offChildren) : NULL; if ((DWORD)f >= 0x10000) dp = f; }   // first parameter
    Log("Objectives panel: GetDisplayInfo %p SetDisplayInfo %p ret %p param %p | Y %p Z %p XScale %p YScale %p | hasY %p hasZ %p hasXScale %p hasYScale %p",
        g_di.fnGet, g_di.fnSet, rv, dp, fY, fZ, fXS, fYS, hY, hZ, hXS, hYS);
    if (!g_di.fnGet || !g_di.fnSet || !rv || !dp || !fY || !fXS || !fYS || !hY || !hXS || !hYS) return false;
    #define OFF(p) (*(int*)((BYTE*)(p) + g_propOffField))
    #define MSK(p) (*(DWORD*)((BYTE*)(p) + g_boolMaskField))
    g_di.retOff = OFF(rv); g_di.dOff = OFF(dp);
    g_di.oX = fX ? OFF(fX) : -1; g_di.mX = hX ? MSK(hX) : 0;
    g_di.vwOff = rw ? OFF(rw) : -1; g_di.vhOff = rh ? OFF(rh) : -1;
    g_di.oY = OFF(fY); g_di.oZ = fZ ? OFF(fZ) : -1; g_di.oXS = OFF(fXS); g_di.oYS = OFF(fYS);
    g_di.oFlags = OFF(hY); g_di.mY = MSK(hY); g_di.mXS = MSK(hXS); g_di.mYS = MSK(hYS); g_di.mZ = hZ ? MSK(hZ) : 0;
    #undef OFF
    #undef MSK
    Log("Objectives panel: return @%d param @%d, Y @%d Z @%d XScale @%d YScale @%d, flags @%d (Y %lX Z %lX XS %lX YS %lX)",
        g_di.retOff, g_di.dOff, g_di.oY, g_di.oZ, g_di.oXS, g_di.oYS, g_di.oFlags,
        (unsigned long)g_di.mY, (unsigned long)g_di.mZ, (unsigned long)g_di.mXS, (unsigned long)g_di.mYS);
    if (g_di.retOff < 0 || g_di.retOff > 200 || g_di.dOff < 0 || g_di.dOff > 200 || g_di.oFlags < 0 || g_di.oFlags > 56) return false;
    g_di.ok = 1;
    return true;
}
static void* g_scoreObj; static int g_scoreIdx = -1;
static void* FindScoreboard() {
    void** objs = *(void***)(g_base + RVA_GOBJECTS);
    int num = *(int*)(g_base + RVA_GOBJECTS + 4);
    if (g_scoreObj && g_scoreIdx >= 0 && g_scoreIdx < num && objs[g_scoreIdx] == g_scoreObj) return g_scoreObj;
    g_scoreObj = NULL; g_scoreIdx = -1;
    for (int i = 0; i < num; i++) {
        void* o = objs[i];
        if (!o || !lstrcmpA(NameOf(o), "Default__PecanUIScreenGUScoreboard")) continue;
        if (!lstrcmpA(NameOf(Ptr(o, OBJ_CLASS)), "PecanUIScreenGUScoreboard")) { g_scoreObj = o; g_scoreIdx = i; break; }
    }
    return g_scoreObj;
}
// Re-lay out one Flash display object: scale it by sc about the point (cx, cy) of its parent, then move it down.
// The game may lay it out again at any time, so this is checked every frame; our own values are recognised.
struct DispAdj { void* obj; int have; float ox, oy, oxs, oys, sx, sy, sxs, sys; };
static void AdjustDisplay(void* o, DispAdj& st, float sc, float down, float cx, float cy, const char* what, float right = 0) {
    ProcessEvent_t pe = (ProcessEvent_t)(*(void***)o)[0x108 / 4];
    BYTE get[256]; memset(get, 0, sizeof(get));
    pe(o, g_di.fnGet, get, NULL, 0, 0);
    BYTE* d = get + g_di.retOff;
    float x = g_di.oX >= 0 ? *(float*)(d + g_di.oX) : 0, y = *(float*)(d + g_di.oY);
    float xs = *(float*)(d + g_di.oXS), ys = *(float*)(d + g_di.oYS);
    if (st.have && o == st.obj && fabsf_(x - st.sx) < 0.01f && fabsf_(y - st.sy) < 0.01f && fabsf_(xs - st.sxs) < 0.01f && fabsf_(ys - st.sys) < 0.01f) return;
    st.ox = x; st.oy = y; st.oxs = xs; st.oys = ys; st.obj = o; st.have = 1;   // the game's own layout
    static int logs; if (logs++ < 12) Log("%s: game layout X %.1f Y %.1f scale %.1f/%.1f -> scale x%.2f about (%.0f, %.0f), down %.0f", what, x, y, xs, ys, sc, cx, cy, down);
    st.sx = cx + (x - cx) * sc + right; st.sy = cy + (y - cy) * sc + down; st.sxs = xs * sc; st.sys = ys * sc;
    BYTE put[256]; memset(put, 0, sizeof(put));
    BYTE* D = put + g_di.dOff;
    DWORD fl = g_di.mY | g_di.mXS | g_di.mYS;
    if (g_di.oX >= 0 && g_di.mX) { *(float*)(D + g_di.oX) = st.sx; fl |= g_di.mX; }
    *(float*)(D + g_di.oY) = st.sy; *(float*)(D + g_di.oXS) = st.sxs; *(float*)(D + g_di.oYS) = st.sys;
    *(DWORD*)(D + g_di.oFlags) |= fl;
    pe(o, g_di.fnSet, put, NULL, 0, 0);
}
// Size of the Flash stage the UI is laid out in (per eye): the screen's own GetViewportWidth/Height when they
// give a sensible answer, otherwise the HUD canvas size (the same per-eye view size)
static bool StageSize(void* o, float* w, float* h) {
    static int logged;
    float vw = 0, vh = 0;
    if (g_di.fnVW && g_di.fnVH) {
        ProcessEvent_t pe = (ProcessEvent_t)(*(void***)o)[0x108 / 4];
        BYTE p[64]; memset(p, 0, sizeof(p)); pe(o, g_di.fnVW, p, NULL, 0, 0); vw = (float)*(int*)(p + (g_di.vwOff >= 0 && g_di.vwOff < 60 ? g_di.vwOff : 0));
        memset(p, 0, sizeof(p)); pe(o, g_di.fnVH, p, NULL, 0, 0); vh = (float)*(int*)(p + (g_di.vhOff >= 0 && g_di.vhOff < 60 ? g_di.vhOff : 0));
    }
    bool ok = vw >= 100 && vw <= 20000 && vh >= 100 && vh <= 20000;
    float hw = 0, hh = 0;
    void* hud = (g_offMyHUD > 0 && g_lastPC) ? Ptr(g_lastPC, (DWORD)g_offMyHUD) : NULL;
    static int oSX = -2, oSY = -2;
    if (oSX == -2) { oSX = PropOffset("HUD", "SizeX"); oSY = PropOffset("HUD", "SizeY"); }
    if (hud && oSX > 0 && oSY > 0) { hw = *(float*)((BYTE*)hud + oSX); hh = *(float*)((BYTE*)hud + oSY); }
    if (logged++ < 3) Log("UI stage size: viewport functions %.0f x %.0f (offsets %d/%d)%s, HUD canvas %.0f x %.0f", vw, vh, g_di.vwOff, g_di.vhOff, ok ? "" : " - not usable", hw, hh);
    if (ok) { *w = vw; *h = vh; return true; }
    if (hw >= 100 && hh >= 100 && hw <= 20000 && hh <= 20000) { *w = hw; *h = hh; return true; }
    return false;
}
static void AdjustObjectivesPanel() {   // game thread, while Back is held
    if (!g_reflOK) return;
    float down = g_cfg.objDown, sc = g_cfg.objScale;
    if (down == 0 && sc == 1.0f) return;
    if (!DispInfoInit()) return;
    void* o = FindScoreboard();
    if (!o) { static int warned; if (!warned++) Log("Objectives panel: scoreboard screen not found"); return; }
    static DispAdj st;
    // shrink about the stage's top-left corner, then move down and right (keeps it centred), as tuned in the headset
    AdjustDisplay(o, st, sc, down, 0, 0, "Objectives panel", g_cfg.objRight);
}
// The in-play HUD: every HUD view of the UI shell, shrunk towards the centre of the view
static void* g_hudViews[8]; static int g_hudViewIdx[8], g_hudViewN; static DWORD g_hudScanAt;
static void AdjustHudViews() {   // game thread, during play
    if (!g_reflOK || g_cfg.hudScale == 1.0f) return;
    if (!DispInfoInit()) return;
    void** objs = *(void***)(g_base + RVA_GOBJECTS);
    int num = *(int*)(g_base + RVA_GOBJECTS + 4);
    DWORD now = GetTickCount();
    bool stale = false;
    for (int i = 0; i < g_hudViewN; i++) if (g_hudViewIdx[i] >= num || objs[g_hudViewIdx[i]] != g_hudViews[i]) stale = true;
    static int misses;   // a full scan is slow (a small hitch): at most every 5 s, and give up after 3 empty ones
    if ((stale || (!g_hudViewN && misses < 3)) && now - g_hudScanAt > 5000) {
        g_hudScanAt = now; g_hudViewN = 0;
        for (int i = 0; i < num && g_hudViewN < 8; i++) {
            void* o = objs[i];
            if (!o || !strncmp(NameOf(o), "Default__", 9) || !IsA(o, "TGGFxHUDView")) continue;
            g_hudViewIdx[g_hudViewN] = i; g_hudViews[g_hudViewN++] = o;
            Log("HUD scale: HUD view %s (%s)", NameOf(o), NameOf(Ptr(o, OBJ_CLASS)));
        }
        if (!g_hudViewN) { misses++; Log("HUD scale: no HUD view found (%d of 3 tries)", misses); } else misses = 0;
        stale = false;
    }
    if (stale) return;
    static DispAdj st[8];
    for (int i = 0; i < g_hudViewN; i++) {
        void* o = g_hudViews[i];
        // the HUD is laid out on a 1280x720 Flash stage: shrink it towards that stage's centre
        AdjustDisplay(o, st[i], g_cfg.hudScale, 0, g_cfg.hudCX, g_cfg.hudCY, "HUD scale");
    }
}

// One-off survey while the objectives panel (Back) is up: what draws it, so it can be moved
static void ObjectivesSurvey(void* pc) {
    Log("---- objectives survey 2 (Back held) ----");
    void** objs = *(void***)(g_base + RVA_GOBJECTS);
    int num = *(int*)(g_base + RVA_GOBJECTS + 4), k = 0;
    // every live object of a Flash/movie class, with its class chain; full properties for the ones showing the scoreboard
    for (int i = 0; i < num && k < 80; i++) {
        void* o = objs[i];
        if (!o) continue;
        const char* n = NameOf(o);
        if (!strncmp(n, "Default__", 9)) continue;
        const char* cn = NameOf(Ptr(o, OBJ_CLASS));
        if (!(ContainsI(cn, "GFx") || ContainsI(cn, "Movie")) || !lstrcmpA(cn, "GFxMovieInfo") || !lstrcmpA(cn, "Class") || strstr(cn, "Property")) continue;
        char chain[256]; int m = 0; chain[0] = 0;
        for (void* c = Ptr(o, OBJ_CLASS); (DWORD)c >= 0x10000 && m < 200; c = Ptr(c, STRUCT_SUPER)) m += _snprintf(chain + m, sizeof(chain) - m, "%s<", NameOf(c));
        void* outer = Ptr(o, OBJ_OUTER);
        Log("  movie obj %s [%s] in %s", n, chain, outer ? NameOf(outer) : "-");
        k++;
        if (ContainsI(n, "score") || ContainsI(cn, "score")) SurveyHudProps(o, "    score");
    }
    k = 0;
    static const char* keys[] = {"scoreboard", NULL};
    for (int i = 0; i < num && k < 120; i++) {
        void* o = objs[i];
        if (!o) continue;
        const char* n = NameOf(o);
        if (!strncmp(n, "Default__", 9)) continue;
        const char* cn = NameOf(Ptr(o, OBJ_CLASS));
        bool hit = false;
        for (int j = 0; keys[j] && !hit; j++) if (ContainsI(n, keys[j]) || ContainsI(cn, keys[j])) hit = true;
        if (!hit) continue;
        if (!lstrcmpA(cn, "Class") || !lstrcmpA(cn, "Function") || !lstrcmpA(cn, "ScriptStruct") || strstr(cn, "Property")) continue;
        void* outer = Ptr(o, OBJ_OUTER);
        Log("  obj %s (%s) in %s", n, cn, outer ? NameOf(outer) : "-");
        if (lstrcmpA(cn, "GFxMovieInfo") && lstrcmpA(cn, "Const") && lstrcmpA(cn, "Enum") && lstrcmpA(cn, "State") && lstrcmpA(cn, "Package") && !strstr(cn, "Definition")) SurveyHudProps(o, "    props");
        k++;
    }
    Log("---- end of objectives survey ----");
}
static bool OverlayOnScreen(ViewMode vm) {   // game thread; true = show this frame on the flat screen
    if (vm == VM_GAMEPLAY) AdjustHudViews();
    {   // survey the objectives panel once, a moment after Back is first held in play
        static int done; static DWORD heldSince;
        if (vm == VM_GAMEPLAY && g_backHeld && g_reflOK) {
            AdjustObjectivesPanel();
            DWORD t = GetTickCount();
            if (!heldSince) heldSince = t;
            else if (done < 0 && t - heldSince > 700) { done++; ObjectivesSurvey(g_lastPC); }
        } else heldSince = 0;
    }
    if (!g_cfg.overlaysOnScreen) return false;
    // The objectives/score screen (Back) is drawn as a flat panel in the top corner of each eye, far too high
    // and too far out in VR: show the game on the flat screen while Back is held (or after a quick tap, until the next tap)
    if (vm != VM_GAMEPLAY) InterlockedExchange(&g_backLatch, 0);
    else if (g_cfg.objectivesOnScreen && (g_backHeld || g_backLatch)) {
        static bool was; if (!was) { was = true; Log("Objectives (Back): showing on the flat screen"); }
        return true;
    }
    if (!g_reflOK) return false;
    DWORD now = GetTickCount();
    if (g_ovlMovie) {
        bool still = false;
        if (now - g_ovlLastScan >= 150) {
            g_ovlLastScan = now;
            void* cur[48]; int n = OpenMovies(cur, 48);
            for (int i = 0; i < n; i++) if (cur[i] == g_ovlMovie) still = true;
            if (!still) { Log("Overlay %s closed - back to VR", NameOf(g_ovlMovie)); g_ovlMovie = NULL; return false; }
        }
        return true;
    }
    if (vm != VM_GAMEPLAY) return false;   // menus/pause already go to the screen
    bool pressedRecently = g_lastStartTick && now - g_lastStartTick < 2000;
    if (!pressedRecently) {   // normal play: refresh the list of screens that are always open (HUD)
        if (now - g_ovlLastScan >= 1000) { g_ovlLastScan = now; g_ovlBaseN = OpenMovies(g_ovlBase, 24); }
        return false;
    }
    if (!g_ovlBaseN || now - g_ovlLastScan < 250) return false;   // need a picture of normal play first
    g_ovlLastScan = now;
    void* cur[48]; int n = OpenMovies(cur, 48, true);
    for (int i = 0; i < n; i++) {
        bool known = false;
        for (int j = 0; j < g_ovlBaseN; j++) if (g_ovlBase[j] == cur[i]) known = true;
        if (known) continue;
        void* mi = FindPropIn(cur[i], "MovieInfo"); void* swf = mi ? *(void**)((BYTE*)cur[i] + *(int*)((BYTE*)mi + g_propOffField)) : NULL;
        Log("Overlay opened: %s (%s) swf=%s - showing it on the flat screen", NameOf(cur[i]), NameOf(Ptr(cur[i], OBJ_CLASS)), swf ? NameOf(swf) : "?");
        g_ovlMovie = cur[i];
        return true;
    }
    if (g_lastStartTick != g_ovlPressSeen && now - g_lastStartTick > 1500) {
        g_ovlPressSeen = g_lastStartTick;
        Log("Overlay check: no new Flash screen opened after Start/Back (%d open)", n);
        for (int i = 0; i < n && i < 12; i++) Log("  open: %s (%s)", NameOf(cur[i]), NameOf(Ptr(cur[i], OBJ_CLASS)));
    }
    return false;
}

typedef void*(THISCALL* CSV_t)(void*, void*, FVector*, FRotator*, void*, void*, int);
static CSV_t o_CSV;
static int g_offOrigin = 0x6C, g_offSize = 0x74;
static unsigned g_gameplayFrames;
static double* g_csvSumPtr;
static void* THISCALL hkCalcSceneView(void* lp, void* fam, FVector* l, FRotator* r, void* vp, void* drawer, int a6) {
    void* pc = Ptr(lp, 0x40);
    if (!(g_vrActive && g_stereoOn && !g_cfg.screenOnly && pc)) { g_frameStereo = 0; return o_CSV(lp, fam, l, r, vp, drawer, a6); }
    if (!g_gameStateInit) {
        InitGameState();
        int o = g_reflOK ? PropOffset("LocalPlayer", "Origin") : -1, sz = g_reflOK ? PropOffset("LocalPlayer", "Size") : -1;
        if (o > 0 && sz > 0) { g_offOrigin = o; g_offSize = sz; }
        Log("LocalPlayer Origin @0x%X Size @0x%X", g_offOrigin, g_offSize);
    }
    g_gameThreadId = GetCurrentThreadId();
    ViewMode vm = ClassifyView(pc);
    if ((int)vm != g_lastMode) {
        PoseSnap ps; SnapPoses(&ps);
        Log("View mode: %s (controllers tracked: L=%d R=%d)", kModeNames[vm], ps.ctrlValid[0], ps.ctrlValid[1]);
        if (vm != VM_PAUSED && g_autoResumeAt) { g_autoResumeAt = 0; Log("  auto-resume done"); }
        if (g_cfg.focusNudge && (vm == VM_GAMEPLAY || vm == VM_MENU)) NudgeGameFocus(vm == VM_GAMEPLAY ? "play started" : "menu");
        if (vm == VM_GAMEPLAY && g_cfg.heightOnGameplay) {
            // The height reference was taken when tracking started, possibly with the headset on the desk. Re-take it
            // now, when the player is surely wearing it, so the in-game eye height matches standing normally.
            static int done;
            if (!done || g_cfg.heightOnGameplay > 1) {
                EnterCriticalSection(&g_poseCs);
                if (g_pose.hmdValid) {
                    float y = g_pose.hmd.m[1][3];
                    Log("Height: reference reset to the current head height (%.2f m, was %.2f m)", y, g_pose.recPos.y);
                    g_pose.recPos.y = y; done = 1;
                }
                LeaveCriticalSection(&g_poseCs);
            }
        }
        if (vm == VM_PAUSED && g_offWorldInfo > 0 && g_offPauser > 0) {
            void* wi = Ptr(pc, (DWORD)g_offWorldInfo); void* pz = wi ? Ptr(wi, (DWORD)g_offPauser) : NULL;
            DWORD since = GetTickCount() - g_lastStartTick;
            Log("  paused by %s (%s); last Start/Back %s; Esc key %s", NameOf(pz), NameOf(Ptr(pz, OBJ_CLASS)),
                g_lastStartTick && since < 3000 ? "just before" : "not pressed recently", (GetAsyncKeyState(VK_ESCAPE) & 0x8000) ? "down" : "up");
            bool asked = (g_lastStartTick && since < 3000) || (GetAsyncKeyState(VK_ESCAPE) & 0x8000);
            if (g_cfg.focusNudge && !asked) NudgeGameFocus("unrequested pause");
            if (g_cfg.autoResume && !asked) { g_autoResumeAt = GetTickCount() + 500; g_autoResumeTries = 0; Log("  the player didn't ask for this pause - pressing Resume"); }
            void** objs = *(void***)(g_base + RVA_GOBJECTS); int num = *(int*)(g_base + RVA_GOBJECTS + 4), k = 0;
            for (int i = 0; i < num && k < 30; i++) {
                void* o = objs[i];
                if (!o || !strncmp(NameOf(o), "Default__", 9) || !IsA(o, "GFxMoviePlayer")) continue;
                void* mi = FindPropIn(o, "MovieInfo"); void* swf = mi ? *(void**)((BYTE*)o + *(int*)((BYTE*)mi + g_propOffField)) : NULL;
                Log("  open screen: %s (%s) swf=%s", NameOf(o), NameOf(Ptr(o, OBJ_CLASS)), swf ? NameOf(swf) : "?"); k++;
            }
        }
        if (vm == VM_GAMEPLAY) { DisableLensEffects(); DisablePostEffects(); ApplyUiFx(false); }
        if (g_lastMode >= 0) ShowMessage(2000, "View: %s", vm == VM_GAMEPLAY ? "VR" : vm == VM_PAUSED ? "paused (screen)" : vm == VM_CUTSCENE ? "cutscene (screen)" : "menu (screen)");
        g_lastMode = (int)vm;
    }
    g_inMainMenu = (vm == VM_MENU);
    bool stereo = vm == VM_GAMEPLAY || (vm == VM_CUTSCENE && !g_cfg.cutscenesOnScreen) ||
                  ((vm == VM_MENU || vm == VM_PAUSED) && !g_cfg.menusOnScreen);
    if (stereo && OverlayOnScreen(vm)) stereo = false;
    if (!stereo) {
        g_frameStereo = 0; g_haveWritten = 0; g_handValid = 0;
        void* v = o_CSV(lp, fam, l, r, vp, drawer, a6);
        PushEye(2);
        return v;
    }
    g_frameStereo = 1;
    double tCsv0 = QpcMs();
    {
        static double last, sumF, sumC, cpu0, wall0; static int n;
        if (last > 0) sumF += tCsv0 - last;
        last = tCsv0;
        if (++n >= 300) {
            double cpu = ThreadCpuMs();
            g_gtFrame = sumF / (n - 1 > 0 ? n - 1 : 1); g_gtCsv = sumC / n;
            g_gtCpu = wall0 > 0 ? 100.0 * (cpu - cpu0) / (tCsv0 - wall0) : 0;
            cpu0 = cpu; wall0 = tCsv0; n = 0; sumF = 0; sumC = 0;
        }
        g_csvSumPtr = &sumC;
    }
    float* O = (float*)((BYTE*)lp + g_offOrigin);
    float* S = (float*)((BYTE*)lp + g_offSize);
    float o0 = O[0], s0 = S[0];
    void* v0;
    {
    g_inSBS = 1;
    if (g_taaOn) TaaNextJitter(); else g_taaJx = g_taaJy = 0;
    g_curEye = 0; S[0] = s0 * 0.5f;
    v0 = o_CSV(lp, fam, l, r, vp, drawer, a6);
    FVector l0 = *l; FRotator r0 = *r;
    g_curEye = 1; O[0] = o0 + s0 * 0.5f;
    // Each view in a family needs its own view state (or none): render the right eye stateless.
    void** vs = (void**)((BYTE*)lp + 0x8C);   // ULocalPlayer::ViewState.Reference (passed to FSceneView ctor)
    void* savedState = *vs;
    if (!g_rightStateTried && savedState) { g_rightStateTried = 1; g_rightState = CreateRightEyeState(savedState); }
    *vs = g_rightState;   // own state if we could create one, otherwise none
    o_CSV(lp, fam, l, r, vp, drawer, a6);
    *vs = savedState;
    O[0] = o0; S[0] = s0;
    g_inSBS = 0;
    if (g_csvSumPtr) *g_csvSumPtr += QpcMs() - tCsv0;
    *l = l0; *r = r0;
    PushEye(3);
    }
    if (vm == VM_GAMEPLAY && g_cfg.armsDirect && g_handValid) PlaceArmsAtHand(pc);
    if (vm == VM_GAMEPLAY) DynamicResolution(pc);
    if (vm == VM_GAMEPLAY && g_cfg.noLensEffects) {   // lens-effect classes can load later in a level: check again a few times
        static DWORD since, done; DWORD now = GetTickCount();
        if (now - since > 2000) { since = now; done = 0; }    // re-entered gameplay
        since = now;
        static DWORD entered; if (!done) { entered = now; done = 1; }
        static int stage;
        if (InterlockedExchange(&g_hudSurveyReq, 0)) HudSurvey(pc);
        {   // why can't the player move? log stick input against the pawn's speed and the game's input locks
            static DWORD lastMv; static int offVel = -2, offIgM = -2, offIgL = -2, offPhys = -2, nLogs;
            if (g_cfg.inputLog && now - lastMv > 2000 && nLogs < 40) {
                lastMv = now; nLogs++;
                void* pawn = Ptr(pc, OFF_PAWN);
                if (offVel == -2) { void* p;
                    p = FindPropIn(pawn, "Velocity"); offVel = p ? *(int*)((BYTE*)p + g_propOffField) : -1;
                    p = FindPropIn(pc, "IgnoreMoveInput"); offIgM = p ? *(int*)((BYTE*)p + g_propOffField) : -1;
                    p = FindPropIn(pc, "IgnoreLookInput"); offIgL = p ? *(int*)((BYTE*)p + g_propOffField) : -1;
                    p = FindPropIn(pawn, "Physics"); offPhys = p ? *(int*)((BYTE*)p + g_propOffField) : -1; }
                float sp = -1; if (pawn && offVel > 0) { float* v = (float*)((BYTE*)pawn + offVel); sp = fsqrt(v[0] * v[0] + v[1] * v[1]); }
                Log("Move check: left stick sent (%d, %d), pawn speed %.0f, IgnoreMoveInput %d, IgnoreLookInput %d, physics %d",
                    (int)g_sentLX, (int)g_sentLY, sp, offIgM > 0 ? *((BYTE*)pc + offIgM) : -1, offIgL > 0 ? *((BYTE*)pc + offIgL) : -1,
                    (pawn && offPhys > 0) ? *((BYTE*)pawn + offPhys) : -1);
                Log("  left controller: biggest movement per axis since last check %.2f %.2f %.2f %.2f %.2f | now a0(%.2f %.2f) a1(%.2f %.2f) a2(%.2f %.2f) | pressed %llX touched %llX | idx L=%ld R=%ld",
                    g_rawMaxL[0], g_rawMaxL[1], g_rawMaxL[2], g_rawMaxL[3], g_rawMaxL[4], g_rawAx[0][0][0], g_rawAx[0][0][1], g_rawAx[0][1][0], g_rawAx[0][1][1],
                    g_rawAx[0][2][0], g_rawAx[0][2][1], (unsigned long long)g_rawBtn[0], (unsigned long long)g_rawTouch[0], (long)g_ctrlIdx[0], (long)g_ctrlIdx[1]);
                Log("  right controller now a0(%.2f %.2f) a1(%.2f %.2f) | pressed %llX touched %llX", g_rawAx[1][0][0], g_rawAx[1][0][1], g_rawAx[1][1][0], g_rawAx[1][1][1],
                    (unsigned long long)g_rawBtn[1], (unsigned long long)g_rawTouch[1]);
                for (int a = 0; a < 5; a++) g_rawMaxL[a] = 0;
            }
        }
        { static DWORD lastUse; if (now - lastUse > 3000) { lastUse = now; ApplyUseReach(); } }   // new pickups spawn during play
        if (InterlockedExchange(&g_uiFxToggleReq, 0)) { g_uiFxOff = g_uiFxOff > 0 ? 0 : 1; ApplyUiFx(true); }
        if (done == 1 && now - entered > 10000 && stage == 0) { stage = 1; DisableLensEffects(); DisablePostEffects(); ApplyUiFx(false); }
        if (done == 1 && now - entered > 60000 && stage == 1) { stage = 2; DisableLensEffects(); DisablePostEffects(); ApplyUiFx(false); }
    }
    if (vm == VM_GAMEPLAY && g_reflOK) {
        static int surveyed;
        void* arms = ArmsOf(pc);
        if (!surveyed && arms && CalibrateFields()) { surveyed = 1; SurveyArms(arms); }
        // without tracked controllers the arm placement above doesn't run: still hide (and push the pose)
        if (arms && g_offChildren && !(g_cfg.armsDirect && g_handValid)) {
            LockGun(arms, Ptr(pc, OFF_PAWN));
            ApplyArmHide(arms);
            PushArmPose(arms);
        }
    }
    if (vm == VM_GAMEPLAY) ApplyHUD(pc);
    if (vm == VM_GAMEPLAY && ++g_gameplayFrames == 120 && !g_dumped) { g_dumped = 1; DumpPlayerParts(pc); }
    if (vm == VM_GAMEPLAY && g_gameplayFrames == 1200) DumpCallers();
    if (vm == VM_GAMEPLAY && g_cfg.autoCapture && (g_gameplayFrames == 1200 || g_gameplayFrames == 3600 || g_gameplayFrames == 7200)) StartCapture(6);
    if (vm == VM_GAMEPLAY && g_handValid && g_reflOK && g_offChildren && (g_gameplayFrames % 600) == 300) {
        void* arms = ArmsOf(pc);
        void* tp = arms ? FindPropIn(arms, "Translation") : NULL;
        if (tp) {
            float* T = (float*)((BYTE*)arms + *(int*)((BYTE*)tp + g_propOffField));
            Basis G = RotToBasis(g_handRot);
            V3 rel = mk(T[0] - g_gunCamLoc.X, T[1] - g_gunCamLoc.Y, T[2] - g_gunCamLoc.Z);
            V3 relH = mk(T[0] - g_handLoc.X, T[1] - g_handLoc.Y, T[2] - g_handLoc.Z);
            Log("Arms origin vs virtual camera (fwd,right,up): %.1f %.1f %.1f | vs hand: %.1f %.1f %.1f",
                dot(rel, G.f), dot(rel, G.r), dot(rel, G.u), dot(relH, G.f), dot(relH, G.r), dot(relH, G.u));
        }
    }
    return v0;
}

// The 1st-person arms/gun are drawn in a "foreground" pass with their own FOV. Match it to the VR FOV,
// otherwise the gun is magnified (the game's ~60-70 deg foreground FOV inside a ~124 deg VR view).
static FOV_t o_FOVFg;
static float THISCALL hkGetFOVAngleForeground(void* self) {
    DWORD rva = (DWORD)__builtin_return_address(0) - g_base;
    float f = o_FOVFg(self);
    static int logged;
    if (rva == 0xDA297D && g_frameStereo && g_vrActive && g_gameFovDeg > 1.0f) {
        if (!logged) { logged = 1; Log("Foreground (gun) FOV %.1f -> %.1f", f, g_gameFovDeg); }
        return g_gameFovDeg;
    }
    return f;
}

static void InstallGameHooks() {
    BYTE* base = (BYTE*)g_base;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    DWORD pvp = g_base + g_cfg.rvaPecanPVP, fov = g_base + g_cfg.rvaPecanFOV;
    int patched = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (memcmp(sec[i].Name, ".rdata", 6) != 0) continue;
        DWORD* p = (DWORD*)(base + sec[i].VirtualAddress);
        DWORD n = sec[i].Misc.VirtualSize / 4;
        DWORD sp = g_cfg.slotPVP / 4, sf = g_cfg.slotFOV / 4;
        for (DWORD k = sp; k < n && k + (sf - sp) < n; k++) {
            if (p[k] != pvp) continue;
            DWORD* vt = p + (k - sp);
            if (vt[sf] != fov) continue;
            PatchPtr((void**)&vt[sp], (void*)hkGetPlayerViewPoint);
            PatchPtr((void**)&vt[sf], (void*)hkGetFOVAngle);
            if (vt[0x61C / 4] == g_base + 0x291990) {
                o_FOVFg = (FOV_t)(g_base + 0x291990);
                PatchPtr((void**)&vt[0x61C / 4], (void*)hkGetFOVAngleForeground);
            }
            Log("Patched PlayerController vtable at RVA %06lX", (unsigned long)((DWORD)vt - g_base));
            patched++;
        }
    }
    if (patched) {
        o_PVP = (PVP_t)pvp;
        o_FOV = (FOV_t)fov;
        // redirect the viewport's call to ULocalPlayer::CalcSceneView (side-by-side stereo)
        BYTE* call = (BYTE*)(g_base + 0xD117EC);
        DWORD target = (DWORD)(call + 5) + *(int*)(call + 1);
        if (call[0] == 0xE8 && target == g_base + 0xDA19F0) {
            o_CSV = (CSV_t)target;
            int rel = (int)((DWORD)hkCalcSceneView - (DWORD)(call + 5));
            PatchPtr((void**)(call + 1), (void*)rel);
            Log("Side-by-side stereo hook installed");
            BYTE* cc = (BYTE*)(g_base + 0xDA28A6);   // call FSceneView::FSceneView inside CalcSceneView
            DWORD ct = (DWORD)(cc + 5) + *(int*)(cc + 1);
            if (cc[0] == 0xE8 && ct == g_base + 0x145DB0) {
                o_SVCtor = (SVCtor_t)ct;
                PatchPtr((void**)(cc + 1), (void*)(int)((DWORD)hkSceneViewCtor - (DWORD)(cc + 5)));
                Log("Off-centre projection hook installed");
            } else Log("FSceneView constructor call not recognised - symmetric projection");
        } else {
            Log("WARNING: CalcSceneView call site not recognised - screen mode only");
            g_cfg.screenOnly = 1;
        }
    } else {
        Log("WARNING: PecanPlayerController vtable not found - unsupported ACM.exe build? "
            "Head tracking/stereo disabled; screen mode only.");
        g_cfg.screenOnly = 1;
    }
}

// ---------------------------------------------------------------------------
// XInput (VR controllers -> virtual Xbox pad)
// ---------------------------------------------------------------------------
static inline bool Btn(const vr::VRControllerState_t& s, vr::EVRButtonId id) {
    return (s.ulButtonPressed & vr::ButtonMaskFromId(id)) != 0;
}
static inline SHORT AxisS(float v) {
    if (v > 1) v = 1; if (v < -1) v = -1;
    return (SHORT)(v * 32767.0f);
}
static DWORD g_bothClickStart;
static DWORD g_calibTick; static int g_calibDirty;
static int g_calStage, g_lWasDown, g_lComboUsed; static DWORD g_lDownTick, g_calLastTick, g_calMsgTick;
static int g_snapLatched;

static void StartCapture(int frames);
static DWORD WINAPI hkXInputGetState(DWORD user, XINPUT_STATE* st) {
    DWORD r = o_XGetState(user, st);
    {   // diagnostics: once a few seconds after VR starts, report whether input is flowing
        static LONG calls, noState; static DWORD lastRep; static int reps;
        InterlockedIncrement(&calls);
        DWORD t = GetTickCount();
        if (g_vrOK && user == 0 && reps < 6 && t - lastRep > 10000) {
            if (lastRep) { Log("Input check: game asked for the controller %ld times in 10 s; SteamVR gave no controller state %ld times; game window in front: %s",
                               calls, noState, (g_gameHwnd && (o_GetForegroundWindow ? o_GetForegroundWindow() : GetForegroundWindow()) == g_gameHwnd) ? "yes" : "no"); reps++; }
            lastRep = t; calls = 0; noState = 0;
        }
        if (g_vrOK && user == 0 && g_sys_vr) {
            vr::VRControllerState_t cs; LONG ix = g_ctrlIdx[0], iy = g_ctrlIdx[1];
            bool any = (ix >= 0 && g_sys_vr->GetControllerState((vr::TrackedDeviceIndex_t)ix, &cs, sizeof(cs))) ||
                       (iy >= 0 && g_sys_vr->GetControllerState((vr::TrackedDeviceIndex_t)iy, &cs, sizeof(cs)));
            if (!any) InterlockedIncrement(&noState);
        }
    }
    if (g_cfg.autoPlay && user == 0 && st && g_lastMode != 0 /*not gameplay*/) {
        // test autopilot: tap A every 1.5 s (and Start every 4th tap) until gameplay, snapshot the screen every 4 s
        static DWORD t0, lastCap; static int taps; static DWORD pkt = 1;
        DWORD now = GetTickCount();
        if (!t0) t0 = now;
        memset(st, 0, sizeof(*st));
        DWORD ph = (now - t0) % 1500;
        if (ph < 150) { if (ph < 20) taps = (int)((now - t0) / 1500); st->Gamepad.wButtons = (taps % 4 == 3) ? XINPUT_GAMEPAD_START : XINPUT_GAMEPAD_A; }
        st->dwPacketNumber = ++pkt;
        if (now - lastCap > 4000) { lastCap = now; StartCapture(1); }
        return ERROR_SUCCESS;
    }
    if (user != 0 || !g_vrOK || !g_cfg.vrControllers || !g_sys_vr || !st) return r;
    vr::VRControllerState_t L, R;
    memset(&L, 0, sizeof(L)); memset(&R, 0, sizeof(R));
    LONG il = g_ctrlIdx[0], ir = g_ctrlIdx[1];
    bool hl = il >= 0 && g_sys_vr->GetControllerState((vr::TrackedDeviceIndex_t)il, &L, sizeof(L));
    bool hr = ir >= 0 && g_sys_vr->GetControllerState((vr::TrackedDeviceIndex_t)ir, &R, sizeof(R));
    if (!hl && !hr) {
        // SteamVR can't read the motion controllers right now (asleep, lost, starting up). Don't let the game see an
        // unplugged gamepad - it would pause with the "Resume" screen. Report an idle connected pad instead.
        if (r != ERROR_SUCCESS) { memset(st, 0, sizeof(*st)); st->dwPacketNumber = g_packet; r = ERROR_SUCCESS; }
        return r;
    }
    if (r != ERROR_SUCCESS) memset(st, 0, sizeof(*st));

    for (int a = 0; a < 5; a++) {
        g_rawAx[0][a][0] = L.rAxis[a].x; g_rawAx[0][a][1] = L.rAxis[a].y; g_rawAx[1][a][0] = R.rAxis[a].x; g_rawAx[1][a][1] = R.rAxis[a].y;
        float m = fabsf_(L.rAxis[a].x) + fabsf_(L.rAxis[a].y); if (m > g_rawMaxL[a]) g_rawMaxL[a] = m;
    }
    g_rawBtn[0] = L.ulButtonPressed; g_rawBtn[1] = R.ulButtonPressed; g_rawTouch[0] = L.ulButtonTouched; g_rawTouch[1] = R.ulButtonTouched;
    XINPUT_GAMEPAD& g = st->Gamepad;
    WORD b = g.wButtons;
    bool rawButtonsValid = true; WORD rawButtons = b;
    bool lClick = Btn(L, vr::k_EButton_SteamVR_Touchpad), rClick = Btn(R, vr::k_EButton_SteamVR_Touchpad);
    bool lA = Btn(L, vr::k_EButton_A), lB = Btn(L, vr::k_EButton_ApplicationMenu);
    bool rA = Btn(R, vr::k_EButton_A), rB = Btn(R, vr::k_EButton_ApplicationMenu);
    bool lGrip = Btn(L, vr::k_EButton_Grip), rGrip = Btn(R, vr::k_EButton_Grip);

    // combos: L-stick-click + X = Start, L-stick-click + Y = Back, both stick clicks held 1s = recentre
    DWORD now = GetTickCount();
    // ---- stepped gun calibration: tap LEFT STICK CLICK (left stick centred) to go through the stages ----
    //   1 position: right stick = gun left/right/up/down, left stick = tilt gun forward/back
    //   2 size:     right stick up/down = bigger/smaller
    //   3 bullets:  right stick = move the bullet aim point
    //   4 done:     save
    float dt = (now - g_calLastTick) / 1000.0f; if (dt > 0.05f) dt = 0.05f; g_calLastTick = now;
    {
        bool leftCentred = !hl || (fabsf_(L.rAxis[0].x) < 0.35f && fabsf_(L.rAxis[0].y) < 0.35f);
        if (lClick && !g_lWasDown) { g_lDownTick = now; g_lComboUsed = !leftCentred || rClick; }
        if (lClick && (lA || lB || rClick)) g_lComboUsed = 1;
        if (g_cfg.calibTap && !lClick && g_lWasDown && !g_lComboUsed && now - g_lDownTick < 600) {
            g_calStage = (g_calStage + 1) % 4;
            if (g_calStage == 0) {
                char v[32];
                _snprintf(v, sizeof(v), "%.1f", g_armsX); WritePrivateProfileStringA("Controls", "ArmsOffsetForward", v, g_ini);
                _snprintf(v, sizeof(v), "%.1f", g_armsY); WritePrivateProfileStringA("Controls", "ArmsOffsetRight", v, g_ini);
                _snprintf(v, sizeof(v), "%.1f", g_armsZ); WritePrivateProfileStringA("Controls", "ArmsOffsetUp", v, g_ini);
                _snprintf(v, sizeof(v), "%.1f", g_aimPitch); WritePrivateProfileStringA("Controls", "AimPitchOffsetDeg", v, g_ini);
                if (g_ctlType[0]) { char key[96]; _snprintf(key, sizeof(key), "AimPitchOffsetDeg.%s", g_ctlType); WritePrivateProfileStringA("Controls", key, v, g_ini); }
                _snprintf(v, sizeof(v), "%.3f", g_gunScale); WritePrivateProfileStringA("Controls", "GunScale", v, g_ini);
                _snprintf(v, sizeof(v), "%.2f", g_corrPitch); WritePrivateProfileStringA("Controls", "BulletCorrectionUpDeg", v, g_ini);
                _snprintf(v, sizeof(v), "%.2f", g_corrYaw); WritePrivateProfileStringA("Controls", "BulletCorrectionRightDeg", v, g_ini);
                Log("Gun calibration saved: arms %.1f %.1f %.1f tilt %.1f scale %.3f bullets up %.2f right %.2f",
                    g_armsX, g_armsY, g_armsZ, g_aimPitch, g_gunScale, g_corrPitch, g_corrYaw);
                ShowMessage(3000, "Gun Calibration Complete");
            }
            g_calMsgTick = 0;
            if (hr) g_sys_vr->TriggerHapticPulse((vr::TrackedDeviceIndex_t)ir, 0, 2500);
        }
        g_lWasDown = lClick;
        if (!lClick) g_lComboUsed = 0;
    }
    bool calibrating = g_calStage != 0;
    if (calibrating) {
        float rx = hr ? R.rAxis[0].x : 0, ry = hr ? R.rAxis[0].y : 0, ly = hl ? L.rAxis[0].y : 0;
        if (fabsf_(rx) < 0.15f) rx = 0; if (fabsf_(ry) < 0.15f) ry = 0; if (fabsf_(ly) < 0.15f) ly = 0;
        if (g_calStage == 1) {
            g_armsY += rx * 25.0f * dt;          // ~0.5 m/s at full deflection
            g_armsZ += ry * 25.0f * dt;
            g_aimPitch -= ly * 20.0f * dt;       // left stick forward = tilt the gun forward
        } else if (g_calStage == 2) {
            g_gunScale += ry * 0.4f * dt;
            if (g_gunScale < 0.3f) g_gunScale = 0.3f; if (g_gunScale > 2.0f) g_gunScale = 2.0f;
        } else if (g_calStage == 3) {
            g_corrPitch += ry * 3.0f * dt;       // degrees per second
            g_corrYaw += rx * 3.0f * dt;
        }
        if (now - g_calMsgTick > 150) {
            g_calMsgTick = now;
            if (g_calStage == 1)
                ShowMessage(60000, "Calibrating gun position\nright stick: move   left stick: tilt (%.0f\xB0)", g_aimPitch);
            else if (g_calStage == 2)
                ShowMessage(60000, "Gun size: %.0f%%\nright stick forward / back", g_gunScale * 100.0f);
            else
                ShowMessage(60000, "Bullet Aim: %s %.1f\xB0  %s %.1f\xB0\nright stick moves the aim point",
                            g_corrPitch >= 0 ? "up" : "down", fabsf_(g_corrPitch), g_corrYaw >= 0 ? "right" : "left", fabsf_(g_corrYaw));
        }
        // the sticks belong to the calibration while it is active
        L.rAxis[0].x = L.rAxis[0].y = 0;
        R.rAxis[0].x = R.rAxis[0].y = 0;
        g.sThumbLX = g.sThumbLY = g.sThumbRX = g.sThumbRY = 0;
        lClick = false;
    }
    static bool heightAdjusting;
    if (lClick && rClick) {
        // both stick clicks held + right stick up/down = raise/lower the view (saved); held still for 1 s = recentre
        float ry = hr ? R.rAxis[0].y : 0;
        if (fabsf_(ry) > 0.4f) {
            heightAdjusting = true;
            g_bothClickStart = 1;   // no recentre this time
            g_heightAdj += ry * 0.4f * dt;   // up to 0.4 m per second
            if (g_heightAdj > 1.0f) g_heightAdj = 1.0f; if (g_heightAdj < -1.0f) g_heightAdj = -1.0f;
            static DWORD lastPulse; if (now - lastPulse > 120) { lastPulse = now; if (ir >= 0) g_sys_vr->TriggerHapticPulse((vr::TrackedDeviceIndex_t)ir, 0, 800); }
        }
        if (!g_bothClickStart) g_bothClickStart = now;
        else if (now - g_bothClickStart > 1000 && g_bothClickStart != 1) {
            InterlockedExchange(&g_recenterReq, 1);
            g_bothClickStart = 1;  // fired; wait for release
        }
        lClick = rClick = false;
        R.rAxis[0].x = R.rAxis[0].y = 0;   // the stick is busy: no turning/D-pad
    } else {
        g_bothClickStart = 0;
        if (heightAdjusting) {
            heightAdjusting = false;
            char v[16]; _snprintf(v, sizeof(v), "%.2f", g_heightAdj); WritePrivateProfileStringA("VR", "HeightOffset", v, g_ini);
            Log("Height: view raised/lowered by %.2f m (saved)", g_heightAdj);
        }
    }
    if (g_cfg.faceChords) {
        // Square+Triangle together = Start (pause), Cross+Circle together = Back (objectives). A face button pressed on
        // its own reaches the game 60 ms late, so a chord's first button doesn't also reload/jump/etc.
        static DWORD downAt[4]; static bool chordL, chordR;
        bool* bt[4] = {&lA, &lB, &rA, &rB};
        for (int i = 0; i < 4; i++) { if (*bt[i]) { if (!downAt[i]) downAt[i] = now; } else downAt[i] = 0; }
        if (lA && lB && !chordL) { chordL = true; Log("Input: Square+Triangle -> Start (pause)"); }
        if (rA && rB && !chordR) { chordR = true; Log("Input: Cross+Circle -> Back (objectives)"); }
        if (!lA && !lB) chordL = false;
        if (!rA && !rB) chordR = false;
        if (chordL) { b |= XINPUT_GAMEPAD_START; lA = lB = false; }
        if (chordR) { b |= XINPUT_GAMEPAD_BACK; rA = rB = false; }
        for (int i = 0; i < 4; i++) if (*bt[i] && now - downAt[i] < 60) *bt[i] = false;
    }
    if (lClick && lA) { b |= XINPUT_GAMEPAD_START; lClick = lA = false; }
    if (lClick && lB) { b |= XINPUT_GAMEPAD_BACK; lClick = lB = false; }

    if (rA) b |= XINPUT_GAMEPAD_A;
    if (rB) b |= XINPUT_GAMEPAD_B;
    if (lA) b |= XINPUT_GAMEPAD_X;
    if (lB) b |= XINPUT_GAMEPAD_Y;
    if (lGrip) b |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (rGrip) b |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (lClick) b |= XINPUT_GAMEPAD_LEFT_THUMB;
    // Right stick click = D-pad shift: hold it and push the right stick for D-pad up/down/left/right;
    // a tap without a direction is melee (right stick click), sent on release.
    static bool rcPrev, rcUsedDir; static DWORD meleeUntil;
    bool dpadMode = false;
    if (g_cfg.dpadOnRightClick) {
        if (rClick && !rcPrev) rcUsedDir = false;
        if (rClick && hr) {
            dpadMode = true;
            float x = R.rAxis[0].x, y = R.rAxis[0].y;
            if (x * x + y * y > 0.36f) {
                rcUsedDir = true;
                if (fabsf_(y) >= fabsf_(x)) b |= y > 0 ? XINPUT_GAMEPAD_DPAD_UP : XINPUT_GAMEPAD_DPAD_DOWN;
                else b |= x > 0 ? XINPUT_GAMEPAD_DPAD_RIGHT : XINPUT_GAMEPAD_DPAD_LEFT;
            }
        }
        if (!rClick && rcPrev && !rcUsedDir) meleeUntil = now + 90;
        rcPrev = rClick;
        if (now < meleeUntil) b |= XINPUT_GAMEPAD_RIGHT_THUMB;
    } else if (rClick) b |= XINPUT_GAMEPAD_RIGHT_THUMB;
    // Options / Create (or any other button the mod doesn't use): right = Start (pause), left = Back (objectives)
    if (g_cfg.extraButtonsMenu) {
        const uint64_t used = (1ull << vr::k_EButton_System) | (1ull << vr::k_EButton_ApplicationMenu) | (1ull << vr::k_EButton_Grip) |
                              (1ull << vr::k_EButton_A) | (0x1Full << vr::k_EButton_Axis0);
        uint64_t xr = hr ? (R.ulButtonPressed & ~used) : 0, xl = hl ? (L.ulButtonPressed & ~used) : 0;
        static uint64_t lastXr, lastXl;
        if (xr && xr != lastXr) Log("Input: extra right-hand button 0x%llX -> Start (pause)", (unsigned long long)xr);
        if (xl && xl != lastXl) Log("Input: extra left-hand button 0x%llX -> Back (objectives)", (unsigned long long)xl);
        lastXr = xr; lastXl = xl;
        if (xr) b |= XINPUT_GAMEPAD_START;
        if (xl) b |= XINPUT_GAMEPAD_BACK;
    }
    if (g_autoResumeAt) {   // tap A (confirm) on the pause menu's default item, Resume; retry a couple of times
        DWORD t = GetTickCount();
        if (t >= g_autoResumeAt && t < g_autoResumeAt + 120) b |= XINPUT_GAMEPAD_A;
        else if (t >= g_autoResumeAt + 1500) { if (++g_autoResumeTries < 3) g_autoResumeAt = t + 300; else { g_autoResumeAt = 0; Log("  auto-resume gave up (still paused)"); } }
    }
    g.wButtons = b;
    {   // Back held / tapped (see OverlayOnScreen)
        static DWORD downAt; bool down = (b & XINPUT_GAMEPAD_BACK) != 0;
        if (down && !g_backHeld) { downAt = now; InterlockedExchange(&g_backHeld, 1); }
        else if (!down && g_backHeld) {
            InterlockedExchange(&g_backHeld, 0);
            if (g_cfg.objectivesOnScreen && now - downAt < 400) { LONG l = !g_backLatch; InterlockedExchange(&g_backLatch, l); Log("Objectives (Back) tapped: %s", l ? "keep on the flat screen" : "back to VR"); }
            else if (g_backLatch) InterlockedExchange(&g_backLatch, 0);
        }
    }
    {   // record Start/Back presses and where they came from (to track down unwanted pauses)
        static WORD lastB; static int realPadLogged = -1;
        int real = (r == ERROR_SUCCESS && rawButtonsValid) ? 1 : 0;
        if (real != realPadLogged) { realPadLogged = real; Log("Input: %s", real ? "a gamepad is connected besides the VR controllers (real, or Steam's controller emulation)" : "no other gamepad connected"); }
        WORD pressed = b & ~lastB; lastB = b;
        if (pressed & (XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK)) {
            g_lastStartTick = now;
            Log("Input: %s pressed - from %s", (pressed & XINPUT_GAMEPAD_START) ? "Start" : "Back",
                (rawButtons & pressed) ? "the other gamepad / Steam emulation" : "the VR controllers (left stick click + X/Y)");
        }
    }

    if (hl) {
        if (fabsf_(L.rAxis[0].x) > 0.02f || fabsf_(L.rAxis[0].y) > 0.02f) { g.sThumbLX = AxisS(L.rAxis[0].x); g.sThumbLY = AxisS(L.rAxis[0].y); }
        BYTE t = (BYTE)(L.rAxis[1].x * 255.0f); if (t > g.bLeftTrigger) g.bLeftTrigger = t;
    }
    bool inGame = g_stereoOn && !g_cfg.screenOnly && (now - g_lastStereoTick) < 400;
    // weapon wheel: while the left triangle (Y) is held, the right stick drives the selection (left stick still works)
    // Weapon wheel (hold Y, pick, release Y to select): while Y is held the right stick drives the selection.
    // The last direction is kept when the stick springs back, and for a moment after Y is released,
    // so the game still sees the chosen direction at the instant it reads the release.
    static bool wheelPrevY; static DWORD wheelRelAt; static float wdx, wdy; static bool wHave;
    if (lB && !wheelPrevY) wHave = false;
    if (!lB && wheelPrevY) wheelRelAt = now;
    wheelPrevY = lB;
    bool afterRelease = !lB && wHave && now - wheelRelAt < 250;
    bool wheel = g_cfg.wheelRightStick && hr && (lB || afterRelease);
    if (dpadMode && !wheel) {   // stick is the D-pad now: no turning, but the trigger still fires
        g.sThumbRX = g.sThumbRY = 0; g_snapLatched = 1;
        BYTE t = (BYTE)(R.rAxis[1].x * 255.0f); if (t > g.bRightTrigger) g.bRightTrigger = t;
    }
    if (wheel) {
        float rx = R.rAxis[0].x, ry = R.rAxis[0].y;
        if (lB && rx * rx + ry * ry > 0.25f) { float m = fsqrt(rx * rx + ry * ry); wdx = rx / m; wdy = ry / m; wHave = true; }
        if (wHave) { g.sThumbLX = AxisS(wdx); g.sThumbLY = AxisS(wdy); }
        BYTE t = (BYTE)(R.rAxis[1].x * 255.0f); if (t > g.bRightTrigger) g.bRightTrigger = t;
        g.sThumbRX = g.sThumbRY = 0;   // no turning while choosing
        g_snapLatched = 1;             // and no snap turn when the stick is released
    } else if (hr && !dpadMode) {
        float rx = R.rAxis[0].x, ry = R.rAxis[0].y;
        BYTE t = (BYTE)(R.rAxis[1].x * 255.0f); if (t > g.bRightTrigger) g.bRightTrigger = t;
        if (inGame) {
            g.sThumbRY = 0;  // pitch comes from the headset/hand
            // right stick up = flashlight (D-pad down), down = sidearm (D-pad right) - only when clearly vertical
            static bool vertHeld; static DWORD vertSince;
            bool vert = g_cfg.stickUpDown && fabsf_(ry) > 0.85f && fabsf_(ry) > fabsf_(rx) * 2.0f;
            if (!vert) vertSince = 0; else if (!vertSince) vertSince = now;
            if (vert && !g_snapLatched && !vertHeld && now - vertSince >= 150) {
                vertHeld = true;
                Log("Input: right stick %s -> %s", ry > 0 ? "up" : "down", ry > 0 ? "flashlight (D-pad down)" : "sidearm (D-pad right)");
            }
            if (vertHeld) {
                if (fabsf_(ry) < 0.3f) vertHeld = false;
                else { g.wButtons |= ry > 0 ? XINPUT_GAMEPAD_DPAD_DOWN : XINPUT_GAMEPAD_DPAD_RIGHT; rx = 0; }
            }
            if (vert) rx = 0;   // a deliberate up/down push never turns
            if (g_cfg.snapTurnDeg > 0) {
                g.sThumbRX = 0;
                if (!g_snapLatched && fabsf_(rx) > 0.7f) {
                    int units = g_cfg.snapTurnDeg * 65536 / 360;
                    InterlockedExchangeAdd(&g_snapRequest, rx > 0 ? units : -units);
                    g_snapLatched = 1;
                } else if (g_snapLatched && fabsf_(rx) < 0.3f) g_snapLatched = 0;
            } else if (fabsf_(rx) > 0.02f) g.sThumbRX = AxisS(rx);
        } else if (fabsf_(rx) > 0.02f || fabsf_(ry) > 0.02f) {
            g.sThumbRX = AxisS(rx); g.sThumbRY = AxisS(ry);   // menus: pass through
        }
    }
    // rumble -> haptics (pulses are re-triggered every poll while the motor runs)
    if (g_cfg.haptics) {
        LONG ml = g_motorL, mr = g_motorR;
        if (ml > 0 && il >= 0) g_sys_vr->TriggerHapticPulse((vr::TrackedDeviceIndex_t)il, 0, (unsigned short)(ml * 3000 / 65535));
        if (mr > 0 && ir >= 0) g_sys_vr->TriggerHapticPulse((vr::TrackedDeviceIndex_t)ir, 0, (unsigned short)(mr * 3000 / 65535));
    }
    if (g_cfg.moveWithHead && inGame && !wheel && (g.sThumbLX || g.sThumbLY)) {   // walk where you look
        float d = g_moveTurn, cd = fcos(d), sd = fsin(d);
        float x = g.sThumbLX / 32767.0f, y = g.sThumbLY / 32767.0f;
        float nx = y * sd + x * cd, ny = y * cd - x * sd;
        g.sThumbLX = AxisS(nx < -1 ? -1 : nx > 1 ? 1 : nx); g.sThumbLY = AxisS(ny < -1 ? -1 : ny > 1 ? 1 : ny);
    }
    g_sentLX = g.sThumbLX; g_sentLY = g.sThumbLY;
    st->dwPacketNumber = ++g_packet;
    return ERROR_SUCCESS;
}
static DWORD WINAPI hkXInputSetState(DWORD user, XINPUT_VIBRATION* v) {
    if (user == 0 && v) { InterlockedExchange(&g_motorL, v->wLeftMotorSpeed); InterlockedExchange(&g_motorR, v->wRightMotorSpeed); }
    return o_XSetState(user, v);
}

static void InstallXInputHooks() {
    BYTE* base = (BYTE*)g_base;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd.VirtualAddress) return;
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dd.VirtualAddress);
    for (; imp->Name; imp++) {
        const char* name = (const char*)(base + imp->Name);
        if (lstrcmpiA(name, "XINPUT1_3.dll") != 0) continue;
        IMAGE_THUNK_DATA32* oft = (IMAGE_THUNK_DATA32*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA32* ft = (IMAGE_THUNK_DATA32*)(base + imp->FirstThunk);
        for (; oft->u1.Ordinal; oft++, ft++) {
            int which = 0;
            if (IMAGE_SNAP_BY_ORDINAL32(oft->u1.Ordinal)) {
                WORD o = (WORD)IMAGE_ORDINAL32(oft->u1.Ordinal);
                which = (o == 2) ? 1 : (o == 3) ? 2 : 0;
            } else {
                IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData);
                if (!lstrcmpA((const char*)ibn->Name, "XInputGetState")) which = 1;
                if (!lstrcmpA((const char*)ibn->Name, "XInputSetState")) which = 2;
            }
            if (which == 1) o_XGetState = (XGetState_t)PatchPtr((void**)&ft->u1.Function, (void*)hkXInputGetState);
            if (which == 2) o_XSetState = (XSetState_t)PatchPtr((void**)&ft->u1.Function, (void*)hkXInputSetState);
        }
    }
    Log("XInput hooks: GetState %s, SetState %s", o_XGetState ? "ok" : "MISSING", o_XSetState ? "ok" : "MISSING");
}

// ---------------------------------------------------------------------------
// OpenVR / D3D11 (render thread)
// ---------------------------------------------------------------------------
typedef uint32_t (*VR_InitInternal2_t)(vr::EVRInitError*, vr::EVRApplicationType, const char*);
typedef void* (*VR_GetGenericInterface_t)(const char*, vr::EVRInitError*);
typedef bool (*VR_IsHmdPresent_t)();

static const GUID kIID_IDXGIFactory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
typedef HRESULT(WINAPI* CreateDXGIFactory1_t)(REFIID, void**);

static bool CreateD3D11(int adapterIndex) {
    IDXGIAdapter1* adapter = NULL;
    HMODULE dxgi = LoadLibraryA("dxgi.dll");
    CreateDXGIFactory1_t cf = dxgi ? (CreateDXGIFactory1_t)GetProcAddress(dxgi, "CreateDXGIFactory1") : NULL;
    if (cf && adapterIndex >= 0) {
        IDXGIFactory1* f = NULL;
        if (SUCCEEDED(cf(kIID_IDXGIFactory1, (void**)&f)) && f) {
            if (FAILED(f->EnumAdapters1((UINT)adapterIndex, &adapter))) adapter = NULL;
            f->Release();
        }
    }
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                   NULL, 0, D3D11_SDK_VERSION, &g_d11, &fl, &g_ctx);
    if (adapter) adapter->Release();
    if (FAILED(hr)) { Log("ERROR: D3D11CreateDevice failed 0x%08lX", (unsigned long)hr); return false; }
    Log("D3D11 device created (adapter %d, feature level 0x%X)", adapterIndex, (unsigned)fl);
    return true;
}

static void UpdateFov() {
    if (!g_bbW || !g_bbH) return;
    float aspect = 0.5f * (float)g_bbW / (float)g_bbH;   // side-by-side: each eye gets half the width
    float tanH = 0;
    if (g_cfg.forceFov > 1.0f) {
        tanH = ftan(g_cfg.forceFov * PI_F / 360.0f);
    } else {
        for (int e = 0; e < 2; e++) {
            float l = fabsf_(g_projRaw[e][0]), r = fabsf_(g_projRaw[e][1]);
            float t = fabsf_(g_projRaw[e][2]), b = fabsf_(g_projRaw[e][3]);
            float h = l > r ? l : r, v = t > b ? t : b;
            if (h > tanH) tanH = h;
            if (v * aspect > tanH) tanH = v * aspect;
        }
        tanH *= 1.0f + g_cfg.fovMargin / 100.0f;
    }
    g_gameTanH = tanH;
    g_gameFovDeg = 2.0f * fatan(tanH) * 180.0f / PI_F;
    Log("Render FOV forced to %.2f deg (tan %.4f) for %ux%u (aspect %.3f)", g_gameFovDeg, tanH, g_bbW, g_bbH, aspect);
}

static void InitVR() {
    char path[MAX_PATH];
    lstrcpyA(path, g_dir); lstrcatA(path, "openvr_api.dll");
    if (!g_ovrDll) g_ovrDll = LoadLibraryA(path);
    if (!g_ovrDll) { Log("ERROR: openvr_api.dll not found next to d3d9.dll (%s)", path); return; }
    VR_InitInternal2_t init = (VR_InitInternal2_t)GetProcAddress(g_ovrDll, "VR_InitInternal2");
    VR_GetGenericInterface_t gi = (VR_GetGenericInterface_t)GetProcAddress(g_ovrDll, "VR_GetGenericInterface");
    VR_IsHmdPresent_t present = (VR_IsHmdPresent_t)GetProcAddress(g_ovrDll, "VR_IsHmdPresent");
    if (!init || !gi) { Log("ERROR: openvr_api.dll missing exports"); return; }
    if (present && !present()) { Log("No HMD detected (is SteamVR running and the headset connected?) - will retry"); return; }
    vr::EVRInitError err = vr::VRInitError_None;
    WaitEarlyVr("in InitVR");
    if (InterlockedExchange(&g_earlyVrOK, 0)) { /* already connected */ }
    else {
        init(&err, vr::VRApplication_Scene, NULL);
        if (err != vr::VRInitError_None) { Log("VR_Init failed: error %d - will retry", (int)err); return; }
    }
    g_sys_vr = (vr::IVRSystem*)gi(vr::IVRSystem_Version, &err);
    g_comp = (vr::IVRCompositor*)gi(vr::IVRCompositor_Version, &err);
    g_ovl = (vr::IVROverlay*)gi(vr::IVROverlay_Version, &err);
    if (!g_sys_vr || !g_comp) { Log("ERROR: could not get IVRSystem/IVRCompositor (err %d)", (int)err); return; }

    uint32_t rw = 0, rh = 0;
    g_sys_vr->GetRecommendedRenderTargetSize(&rw, &rh);
    for (int e = 0; e < 2; e++)
        g_sys_vr->GetProjectionRaw((vr::EVREye)e, &g_projRaw[e][0], &g_projRaw[e][1], &g_projRaw[e][2], &g_projRaw[e][3]);
    vr::ETrackedPropertyError pe;
    float ipd = g_sys_vr->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_UserIpdMeters_Float, &pe);
    if (pe == vr::TrackedProp_Success && ipd > 0.04f && ipd < 0.09f) g_ipd = ipd;
    Log("OpenVR ready. Recommended per-eye size %ux%u, IPD %.1f mm", rw, rh, g_ipd * 1000.0f);
    for (int e = 0; e < 2; e++)
        Log("  eye %d tangents: l %.3f r %.3f t %.3f b %.3f", e, g_projRaw[e][0], g_projRaw[e][1], g_projRaw[e][2], g_projRaw[e][3]);

    int32_t adapter = -1;
    g_sys_vr->GetDXGIOutputInfo(&adapter);
    if (!CreateD3D11(adapter)) return;

    g_comp->SetTrackingSpace(vr::TrackingUniverseStanding);
    if (g_ovl) {
        vr::EVROverlayError oe = g_ovl->CreateOverlay("acmvr.screen", "Aliens: Colonial Marines", &g_overlay);
        if (oe != vr::VROverlayError_None) { Log("CreateOverlay failed %d", (int)oe); g_overlay = vr::k_ulOverlayHandleInvalid; }
        else { g_ovl->SetOverlayWidthInMeters(g_overlay, g_cfg.screenWidth); g_ovl->SetOverlayCurvature(g_overlay, g_cfg.screenCurve); }
    }
    InterlockedExchange(&g_vrOK, 1);
    UpdateFov();
}

// Two copy paths:
//  * D3D9Ex (FastCopy=1): the game's device is created as D3D9Ex, the backbuffer is StretchRect'ed into a
//    shared texture that D3D11/SteamVR read directly on the GPU (no CPU copy).
//  * Fallback: GPU -> system memory -> D3D11 UpdateSubresource.
static int g_isEx;                       // IDirect3D9Ex in use
static int g_devIsEx;                    // device created with CreateDeviceEx
static int g_useShared;                  // shared-texture path active
static IDirect3DTexture9* g_shTex9[2];   // 0 = stereo (eyes), 1 = screen
static IDirect3DSurface9* g_shSurf9[2];
static IDirect3DQuery9* g_evQuery;
static IDirect3DSurface9* g_capSys;      // system-memory surface for frame captures
// async submission: ring of stereo textures written by the render thread, read by the VR thread
static IDirect3DTexture9* g_ringTex9[3];
static IDirect3DSurface9* g_ringSurf9[3];
static ID3D11Texture2D* g_ring11[3];
static int g_ringOK, g_ringNext;
static CRITICAL_SECTION g_pubCs;
static volatile LONG g_pubSerial, g_consumedSerial, g_pubIdx = -1, g_asyncScreen = 1;
static bool AsyncActive();
static volatile double g_slotMs;
static vr::HmdMatrix34_t g_pubPose, g_pubPoseE[2];   // per eye: with alternate eyes each eye has its own pose
static HANDLE g_consumeEvent;
static volatile LONG g_vrSubmits, g_vrRepeats;
static const GUID kIID_ID3D11Texture2D = {0x6f15aaf2, 0xd208, 0x4e89, {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};

// ---- DXVK mode: eye frames go to SteamVR as Vulkan images (no D3D11 sharing) ----
static const int VKR = 4;
static ID3D9VkInteropDevice* g_vkDev;
static int g_vk;                                      // DXVK interop path active
static VkInstance g_vkInst; static VkPhysicalDevice g_vkPhys; static VkDevice g_vkDevice; static VkQueue g_vkQueue; static uint32_t g_vkQFam;
static IDirect3DTexture9* g_vkTex9[VKR * 2]; static IDirect3DSurface9* g_vkSurf9[VKR * 2]; static ID3D9VkInteropTexture* g_vkItex[VKR * 2];
static VkImage g_vkImg[VKR * 2]; static VkImageLayout g_vkLayout[VKR * 2]; static IDirect3DQuery9* g_vkQ[VKR];
static UINT g_vkEW[2], g_vkEH[2]; static RECT g_vkSrcRect[2];
static IDirect3DTexture9* g_vkMid9[2]; static IDirect3DSurface9* g_vkMidSurf[2];   // render-target staging per eye   // per-eye image size and where it comes from in the back buffer
static int g_vkSrc[VKR], g_vkPending[VKR]; static LONG g_vkSeq[VKR], g_vkSeqNext; static vr::HmdMatrix34_t g_vkPose[VKR], g_vkPoseE[VKR][2];
static UINT g_vkW, g_vkH, g_vkFmt, g_vkSamples;
static IDirect3DTexture9* g_vkBlackTex9; static VkImage g_vkBlackImg; static UINT g_vkBlackFmt;
static int g_vkTimingSet;
// The game's image is gamma-encoded 8-bit; with "Auto" SteamVR treats a Vulkan UNORM image as linear and
// the picture comes out brighter / washed out compared with the DirectX path.
static vr::EColorSpace g_vkColorSpace = vr::ColorSpace_Gamma;
static const VkImageSubresourceRange kVkColor0 = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

static ID3D9VkInteropTexture* VkTexInfo(IUnknown* t, VkImage* img, VkImageLayout* lay, VkImageCreateInfo* ci) {
    ID3D9VkInteropTexture* it = NULL;
    if (!t || FAILED(t->QueryInterface(kIID_ID3D9VkInteropTexture, (void**)&it)) || !it) return NULL;
    memset(ci, 0, sizeof(*ci)); ci->sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    if (FAILED(it->GetVulkanImageInfo(img, lay, ci))) { it->Release(); return NULL; }
    return it;
}
static void ReleaseTaa();
static void ReleaseVk() {
    ReleaseTaa();
    for (int i = 0; i < VKR * 2; i++) {
        if (g_vkItex[i]) { g_vkItex[i]->Release(); g_vkItex[i] = NULL; }
        if (g_vkSurf9[i]) { g_vkSurf9[i]->Release(); g_vkSurf9[i] = NULL; }
        if (g_vkTex9[i]) { g_vkTex9[i]->Release(); g_vkTex9[i] = NULL; }
        g_vkImg[i] = VK_NULL_HANDLE;
    }
    for (int e = 0; e < 2; e++) {
        if (g_vkMidSurf[e]) { g_vkMidSurf[e]->Release(); g_vkMidSurf[e] = NULL; }
        if (g_vkMid9[e]) { g_vkMid9[e]->Release(); g_vkMid9[e] = NULL; }
    }
    for (int i = 0; i < VKR; i++) {
        if (g_vkQ[i]) { g_vkQ[i]->Release(); g_vkQ[i] = NULL; }
        g_vkSrc[i] = g_vkPending[i] = 0;
    }
    g_vkEW[0] = g_vkEW[1] = g_vkEH[0] = g_vkEH[1] = 0;
    if (g_vkBlackTex9) { g_vkBlackTex9->Release(); g_vkBlackTex9 = NULL; }
    g_vkBlackImg = VK_NULL_HANDLE;
}
static bool CreateVkResources(IDirect3DDevice9* dev, UINT w, UINT h) {
    g_vkDev->GetVulkanHandles(&g_vkInst, &g_vkPhys, &g_vkDevice);
    uint32_t qi = 0;
    g_vkDev->GetSubmissionQueue(&g_vkQueue, &qi, &g_vkQFam);
    if (!g_vkInst || !g_vkPhys || !g_vkDevice || !g_vkQueue) { Log("DXVK: no Vulkan handles"); return false; }
    for (int i = 0; i < VKR; i++)
        if (FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_vkQ[i]))) { Log("DXVK: event query failed"); return false; }
    // black frame for loading screens / menus, left in transfer-source layout for good
    IDirect3DSurface9* bs = NULL; VkImageCreateInfo ci; VkImageLayout lay;
    if (SUCCEEDED(dev->CreateTexture(16, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_vkBlackTex9, NULL)) &&
        SUCCEEDED(g_vkBlackTex9->GetSurfaceLevel(0, &bs))) {
        dev->ColorFill(bs, NULL, D3DCOLOR_ARGB(255, 0, 0, 0));
        bs->Release();
        ID3D9VkInteropTexture* it = VkTexInfo(g_vkBlackTex9, &g_vkBlackImg, &lay, &ci);
        if (it) { g_vkBlackFmt = (UINT)ci.format; g_vkDev->TransitionTextureLayout(it, &kVkColor0, lay, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); it->Release(); }
    }
    g_vkDev->FlushRenderingCommands();
    g_vkColorSpace = g_cfg.vkColorSpace == 2 ? vr::ColorSpace_Linear : g_cfg.vkColorSpace == 0 ? vr::ColorSpace_Auto : vr::ColorSpace_Gamma;
    if (!g_vkTimingSet) {
        g_vkTimingSet = 1;
        // stop WaitGetPoses touching DXVK's Vulkan queue; we hand off ourselves under DXVK's queue lock
        g_comp->SetExplicitTimingMode(vr::VRCompositorTimingMode_Explicit_ApplicationPerformsPostPresentHandoff);
        char ext[1024]; ext[0] = 0;
        g_comp->GetVulkanDeviceExtensionsRequired((VkPhysicalDevice_T*)g_vkPhys, ext, sizeof(ext));
        Log("DXVK: SteamVR wants Vulkan device extensions: %s", ext);
    }
    g_vkSeqNext = 0;
    return true;
}
// Render thread: one texture per eye, exactly the size of that eye's part of the back buffer, so each eye is
// submitted as a whole image (no sub-rectangle) - the same shape of submit as the black frames.
static void EyeBounds(int e, vr::VRTextureBounds_t* bnd);
static bool StretchRectFailed(HRESULT hr) { static int n; if (FAILED(hr) && n++ < 5) Log("TAA: copy failed 0x%08lX", (unsigned long)hr); return FAILED(hr); }
// ---- TAA resolve: blend each eye with its previous frame, re-aimed for the head turn, colour-clamped ----
struct IBlob { virtual HRESULT WINAPI QueryInterface(REFIID, void**) = 0; virtual ULONG WINAPI AddRef() = 0; virtual ULONG WINAPI Release() = 0;
               virtual void* WINAPI GetBufferPointer() = 0; virtual SIZE_T WINAPI GetBufferSize() = 0; };
typedef HRESULT(WINAPI* D3DCompile_t)(const void*, SIZE_T, const char*, const void*, void*, const char*, const char*, UINT, UINT, IBlob**, IBlob**);
static const char kTaaVS[] =
    "float4 off : register(c4);\n"
    "struct O { float4 p : POSITION; float2 t : TEXCOORD0; };\n"
    "O main(float4 p : POSITION, float2 t : TEXCOORD0) { O o; o.p = float4(p.xy + off.xy, 0.5, 1); o.t = t; return o; }\n";
static const char kTaaPS[] =
    "sampler2D cur : register(s0); sampler2D hist : register(s1);\n"
    "float4 H0 : register(c0); float4 H1 : register(c1); float4 H2 : register(c2); float4 prm : register(c3);\n"
    "float3 W(float3 c) { return c / (1 + max(c.r, max(c.g, c.b))); }\n"          // tame bright samples
    "float3 UW(float3 c) { return c / max(1 - max(c.r, max(c.g, c.b)), 0.0001); }\n"
    "float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
    "  float3 c = W(tex2Dlod(cur, float4(uv, 0, 0)).rgb);\n"
    "  if (prm.w < 0.5) return float4(UW(c), 1);\n"
    "  float3 m1 = c, m2 = c * c, mn = c, mx = c;\n"
    "  [unroll] for (int i = 0; i < 8; i++) {\n"
    "    float2 o = float2(i < 3 ? i - 1 : (i < 5 ? (i == 3 ? -1 : 1) : i - 6), i < 3 ? -1 : (i < 5 ? 0 : 1));\n"
    "    float3 s = W(tex2Dlod(cur, float4(uv + o * prm.xy, 0, 0)).rgb);\n"
    "    m1 += s; m2 += s * s; mn = min(mn, s); mx = max(mx, s); }\n"
    "  m1 /= 9; m2 /= 9;\n"
    "  float3 sd = sqrt(max(m2 - m1 * m1, 0));\n"
    "  float3 lo = max(mn, m1 - 1.25 * sd), hi = min(mx, m1 + 1.25 * sd);\n"
    "  float3 p = H0.xyz * uv.x + H1.xyz * uv.y + H2.xyz;\n"
    "  float2 q = p.xy / p.z;\n"
    "  if (p.z <= 0 || q.x < 0 || q.y < 0 || q.x > 1 || q.y > 1) return float4(UW(c), 1);\n"
    "  float3 h = W(tex2Dlod(hist, float4(q, 0, 0)).rgb);\n"
    "  h = clamp(h, lo, hi);\n"
    "  return float4(UW(lerp(h, c, prm.z)), 1);\n"
    "}\n";
static IDirect3DVertexShader9* g_taaVS; static IDirect3DPixelShader9* g_taaPS; static IDirect3DVertexDeclaration9* g_taaDecl;
static IDirect3DStateBlock9* g_taaSB;
static IDirect3DTexture9* g_taaCur[2]; static IDirect3DSurface9* g_taaCurS[2];
static IDirect3DTexture9* g_taaHist[2][2]; static IDirect3DSurface9* g_taaHistS[2][2];
static UINT g_taaW[2], g_taaH[2]; static int g_taaFlip[2], g_taaHistOK[2], g_taaFailed;
static Basis g_taaPrevB[2]; static int g_taaPrevOK[2];
static void ReleaseTaa() {
    for (int e = 0; e < 2; e++) {
        if (g_taaCurS[e]) { g_taaCurS[e]->Release(); g_taaCurS[e] = NULL; }
        if (g_taaCur[e]) { g_taaCur[e]->Release(); g_taaCur[e] = NULL; }
        for (int k = 0; k < 2; k++) {
            if (g_taaHistS[e][k]) { g_taaHistS[e][k]->Release(); g_taaHistS[e][k] = NULL; }
            if (g_taaHist[e][k]) { g_taaHist[e][k]->Release(); g_taaHist[e][k] = NULL; }
        }
        g_taaW[e] = g_taaH[e] = 0;
    }
    if (g_taaSB) { g_taaSB->Release(); g_taaSB = NULL; }
    g_taaHistOK[0] = g_taaHistOK[1] = 0;
}
static char g_taaMarker[MAX_PATH];
static void TaaCrashGuardInit() {   // a marker left behind by a session that crashed with TAA running turns TAA off
    lstrcpyA(g_taaMarker, g_dir); lstrcatA(g_taaMarker, "acmvr_taa_running.txt");
    char ver[32] = ""; FILE* f = fopen(g_taaMarker, "r");
    if (f) { fgets(ver, sizeof(ver), f); fclose(f); }
    if (f && !strncmp(ver, ACMVR_VERSION, strlen(ACMVR_VERSION))) {   // only a crash with this same version counts
        g_taaFailed = 1;
        Log("TAA: the last session ended while TAA was starting up - TAA turned OFF for safety (delete acmvr_taa_running.txt to try again)");
    }
}
static bool TaaEnsure(IDirect3DDevice9* dev) {
    if (g_taaFailed) return false;
    if (!g_taaPS) {
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        D3DCompile_t comp = dc ? (D3DCompile_t)GetProcAddress(dc, "D3DCompile") : NULL;
        if (!comp) { Log("TAA: d3dcompiler_47.dll not found - TAA off"); g_taaFailed = 1; return false; }
        IBlob *vb = NULL, *pb = NULL, *err = NULL;
        HRESULT h1 = comp(kTaaVS, sizeof(kTaaVS) - 1, "taa_vs", NULL, NULL, "main", "vs_3_0", 0, 0, &vb, &err);
        if (FAILED(h1)) { Log("TAA: vertex shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?"); if (err) err->Release(); g_taaFailed = 1; return false; }
        HRESULT h2 = comp(kTaaPS, sizeof(kTaaPS) - 1, "taa_ps", NULL, NULL, "main", "ps_3_0", 1u << 15 /*O3*/, 0, &pb, &err);
        if (FAILED(h2)) { Log("TAA: pixel shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?"); if (err) err->Release(); vb->Release(); g_taaFailed = 1; return false; }
        HRESULT h3 = dev->CreateVertexShader((const DWORD*)vb->GetBufferPointer(), &g_taaVS);
        HRESULT h4 = dev->CreatePixelShader((const DWORD*)pb->GetBufferPointer(), &g_taaPS);
        vb->Release(); pb->Release();
        static const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                               {0, 8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0}, D3DDECL_END()};
        HRESULT h5 = dev->CreateVertexDeclaration(el, &g_taaDecl);
        if (FAILED(h3) || FAILED(h4) || FAILED(h5)) { Log("TAA: shader creation failed %08lX %08lX %08lX", (unsigned long)h3, (unsigned long)h4, (unsigned long)h5); g_taaFailed = 1; return false; }
        Log("TAA: shaders ready");
    }
    if (!g_taaSB && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_taaSB))) { Log("TAA: state block failed"); g_taaFailed = 1; return false; }
    for (int e = 0; e < 2; e++) {
        if (g_taaW[e] == g_vkEW[e] && g_taaH[e] == g_vkEH[e] && g_taaHist[e][0]) continue;
        ReleaseTaa();
        if (!g_taaSB && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_taaSB))) { g_taaFailed = 1; return false; }
        for (int k = 0; k < 2; k++) {
            UINT w = g_vkEW[k], h = g_vkEH[k];
            HRESULT hr = dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_taaCur[k], NULL);
            if (SUCCEEDED(hr)) hr = g_taaCur[k]->GetSurfaceLevel(0, &g_taaCurS[k]);
            for (int j = 0; j < 2 && SUCCEEDED(hr); j++) {
                hr = dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_taaHist[k][j], NULL);
                if (SUCCEEDED(hr)) hr = g_taaHist[k][j]->GetSurfaceLevel(0, &g_taaHistS[k][j]);
            }
            if (FAILED(hr)) { Log("TAA: textures failed 0x%08lX - TAA off", (unsigned long)hr); ReleaseTaa(); g_taaFailed = 1; return false; }
            g_taaW[k] = w; g_taaH[k] = h;
        }
        Log("TAA: buffers %ux%u / %ux%u", g_taaW[0], g_taaH[0], g_taaW[1], g_taaH[1]);
        break;
    }
    return true;
}
// Maps this frame's eye-image uv to where the same direction was in the previous frame's eye image.
static void TaaHomography(int e, const Basis& C, const Basis& Pv, float H[3][4]) {
    float l = g_projRaw[e][0], r = g_projRaw[e][1], t = g_projRaw[e][2], b = g_projRaw[e][3];
    float in[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int k = 0; k < 3; k++) {
        float u = in[k][0], v = in[k][1], w = in[k][2];
        float f = w, rr = (r - l) * u + l * w, uu = b * w - (b - t) * v;
        V3 W = add(add(mul(C.f, f), mul(C.r, rr)), mul(C.u, uu));
        float fp = dot(Pv.f, W), rp = dot(Pv.r, W), up = dot(Pv.u, W);
        H[k][0] = (rp - l * fp) / (r - l);
        H[k][1] = (b * fp - up) / (b - t);
        H[k][2] = fp; H[k][3] = 0;
    }
}
// bb -> per-eye current image -> blend with history -> returns the surface holding the result for eye e (or NULL)
static IDirect3DSurface9* TaaResolve(IDirect3DDevice9* dev, IDirect3DSurface9* bb, int e) {
    if (StretchRectFailed(dev->StretchRect(bb, &g_vkSrcRect[e], g_taaCurS[e], NULL, D3DTEXF_NONE))) return NULL;
    IDirect3DSurface9* out = g_taaHistS[e][g_taaFlip[e]];
    IDirect3DTexture9* prev = g_taaHist[e][1 - g_taaFlip[e]];
    Basis C = RotToBasis(g_poppedView);
    float H[3][4]; TaaHomography(e, C, g_taaPrevB[e], H);
    float W = (float)g_taaW[e], Hh = (float)g_taaH[e];
    float prm[4] = {1.0f / W, 1.0f / Hh, g_cfg.taaBlend, g_taaHistOK[e] ? 1.0f : 0.0f};
    float off[4] = {-1.0f / W, 1.0f / Hh, 0, 0};
    struct V { float x, y, u, v; } q[4] = {{-1, 1, 0, 0}, {1, 1, 1, 0}, {-1, -1, 0, 1}, {1, -1, 1, 1}};
    dev->SetRenderTarget(0, out);
    dev->SetVertexShader(g_taaVS); dev->SetPixelShader(g_taaPS); dev->SetVertexDeclaration(g_taaDecl);
    dev->SetVertexShaderConstantF(4, off, 1);
    dev->SetPixelShaderConstantF(0, &H[0][0], 3);
    dev->SetPixelShaderConstantF(3, prm, 1);
    dev->SetTexture(0, g_taaCur[e]); dev->SetTexture(1, prev);
    for (int sidx = 0; sidx < 2; sidx++) {
        dev->SetSamplerState(sidx, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); dev->SetSamplerState(sidx, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(sidx, D3DSAMP_MINFILTER, sidx ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        dev->SetSamplerState(sidx, D3DSAMP_MAGFILTER, sidx ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        dev->SetSamplerState(sidx, D3DSAMP_MIPFILTER, D3DTEXF_NONE); dev->SetSamplerState(sidx, D3DSAMP_SRGBTEXTURE, 0);
    }
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    return out;
}
static IDirect3DSurface9* g_taaOut[2];
// Runs the resolve for both eyes; afterwards g_taaOut[e] holds each eye's image (NULL = TAA not used this frame).
static void TaaFrame(IDirect3DDevice9* dev, IDirect3DSurface9* bb) {
    g_taaOut[0] = g_taaOut[1] = NULL;
    bool want = g_cfg.taa && !g_taaUserOff && g_cfg.vkPlainEyes && g_poppedViewOK && !g_taaFailed;
    if (!want) { InterlockedExchange(&g_taaOn, 0); g_taaHistOK[0] = g_taaHistOK[1] = 0; g_taaPrevOK[0] = g_taaPrevOK[1] = 0; return; }
    if (!TaaEnsure(dev)) { InterlockedExchange(&g_taaOn, 0); return; }
    // big view jumps (snap turn, cutscene cut, respawn): start the history again
    Basis C = RotToBasis(g_poppedView);
    for (int e = 0; e < 2; e++)
        if (!g_taaPrevOK[e] || dot(C.f, g_taaPrevB[e].f) < 0.94f || dot(C.u, g_taaPrevB[e].u) < 0.94f) g_taaHistOK[e] = 0;
    IDirect3DSurface9* rt0 = NULL; IDirect3DSurface9* ds = NULL;
    g_taaSB->Capture();
    dev->GetRenderTarget(0, &rt0); dev->GetDepthStencilSurface(&ds);
    dev->SetDepthStencilSurface(NULL);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE); dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE); dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF); dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    bool ok = true;
    for (int e = 0; e < 2 && ok; e++) {
        g_taaOut[e] = TaaResolve(dev, bb, e); ok = g_taaOut[e] != NULL;
    }
    dev->SetRenderTarget(0, rt0); dev->SetDepthStencilSurface(ds);
    if (rt0) rt0->Release(); if (ds) ds->Release();
    g_taaSB->Apply();
    if (!ok) { g_taaOut[0] = g_taaOut[1] = NULL; g_taaHistOK[0] = g_taaHistOK[1] = 0; return; }
    for (int e = 0; e < 2; e++) {
        g_taaFlip[e] ^= 1; g_taaHistOK[e] = 1; g_taaPrevB[e] = C; g_taaPrevOK[e] = 1;
    }
    InterlockedExchange(&g_taaOn, 1);
    static int logged; static unsigned okFrames;
    if (!logged) { logged = 1; Log("TAA: running (blend %.2f, jitter %.2f px)", g_cfg.taaBlend, g_cfg.taaJitter);
        FILE* f = fopen(g_taaMarker, "w"); if (f) { fputs(ACMVR_VERSION " TAA start-up in progress\n", f); fclose(f); } }
    // the crash this guards against came within the first frames: 120 good frames clear it (900 also caught
    // sessions that were simply quit early, and turned TAA off for the next one)
    if (++okFrames == 120) { DeleteFileA(g_taaMarker); Log("TAA: stable for 120 frames"); }
}

static bool VkEnsureEyeTextures(IDirect3DDevice9* dev) {
    RECT r[2];
    for (int e = 0; e < 2; e++) {
        vr::VRTextureBounds_t b; EyeBounds(e, &b);
        float u0 = b.uMin < b.uMax ? b.uMin : b.uMax, u1 = b.uMin < b.uMax ? b.uMax : b.uMin;
        float v0 = b.vMin < b.vMax ? b.vMin : b.vMax, v1 = b.vMin < b.vMax ? b.vMax : b.vMin;
        LONG x0 = (LONG)(u0 * g_bbW + 0.5f), x1 = (LONG)(u1 * g_bbW + 0.5f), y0 = (LONG)(v0 * g_bbH + 0.5f), y1 = (LONG)(v1 * g_bbH + 0.5f);
        if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > (LONG)g_bbW) x1 = g_bbW; if (y1 > (LONG)g_bbH) y1 = g_bbH;
        if (x1 - x0 < 16 || y1 - y0 < 16) return false;
        r[e].left = x0; r[e].top = y0; r[e].right = x1; r[e].bottom = y1;
    }
    bool same = true;
    for (int e = 0; e < 2; e++) {
        if (g_vkEW[e] != (UINT)(r[e].right - r[e].left) || g_vkEH[e] != (UINT)(r[e].bottom - r[e].top)) same = false;
        g_vkSrcRect[e] = r[e];
    }
    if (same && g_vkImg[0]) return true;
    // (re)create: drop old eye textures; nothing may be pending or published from them
    for (int i = 0; i < VKR * 2; i++) {
        if (g_vkItex[i]) { g_vkItex[i]->Release(); g_vkItex[i] = NULL; }
        if (g_vkSurf9[i]) { g_vkSurf9[i]->Release(); g_vkSurf9[i] = NULL; }
        if (g_vkTex9[i]) { g_vkTex9[i]->Release(); g_vkTex9[i] = NULL; }
        g_vkImg[i] = VK_NULL_HANDLE;
    }
    for (int e = 0; e < 2; e++) {
        if (g_vkMidSurf[e]) { g_vkMidSurf[e]->Release(); g_vkMidSurf[e] = NULL; }
        if (g_vkMid9[e]) { g_vkMid9[e]->Release(); g_vkMid9[e] = NULL; }
    }
    for (int i = 0; i < VKR; i++) g_vkSrc[i] = g_vkPending[i] = 0;
    InterlockedExchange(&g_pubIdx, -1);
    for (int e = 0; e < 2; e++) { g_vkEW[e] = r[e].right - r[e].left; g_vkEH[e] = r[e].bottom - r[e].top; }
    for (int i = 0; i < VKR * 2; i++) {
        int e = i & 1; VkImageCreateInfo ci; VkImageLayout lay;
        // plain off-screen surface (not a render target): the driver keeps it uncompressed, which avoids an
        // NVIDIA crash in SteamVR's vkCmdCopyImage of large compressed render targets
        HRESULT hr;
        if (i < 2 && g_cfg.vkPlainEyes) {   // per-eye render-target staging textures
            hr = dev->CreateTexture(g_vkEW[e], g_vkEH[e], 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_vkMid9[e], NULL);
            if (SUCCEEDED(hr)) hr = g_vkMid9[e]->GetSurfaceLevel(0, &g_vkMidSurf[e]);
            if (FAILED(hr)) { Log("DXVK: staging texture failed 0x%08lX", (unsigned long)hr); g_vkEW[0] = 0; return false; }
        }
        hr = dev->CreateTexture(g_vkEW[e], g_vkEH[e], 1, g_cfg.vkPlainEyes ? 0 : D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_vkTex9[i], NULL);
        if (SUCCEEDED(hr)) hr = g_vkTex9[i]->GetSurfaceLevel(0, &g_vkSurf9[i]);
        if (FAILED(hr) || !g_vkSurf9[i]) { Log("DXVK: eye image %d failed 0x%08lX", i, (unsigned long)hr); g_vkEW[0] = 0; return false; }
        g_vkItex[i] = VkTexInfo(g_vkSurf9[i], &g_vkImg[i], &lay, &ci);
        if (!g_vkItex[i] || !g_vkImg[i]) { Log("DXVK: no Vulkan image for eye texture %d", i); g_vkEW[0] = 0; return false; }
        g_vkLayout[i] = lay;
        if (i == 0) { g_vkFmt = (UINT)ci.format; g_vkSamples = (UINT)ci.samples;
            Log("DXVK: eye image usage 0x%X flags 0x%X tiling %d layout %d (%s)", (unsigned)ci.usage, (unsigned)ci.flags, (int)ci.tiling, (int)lay, g_cfg.vkPlainEyes ? "plain surface" : "render target"); }
    }
    Log("DXVK: per-eye images %ux%u (from %ld,%ld) and %ux%u (from %ld,%ld), format %u", g_vkEW[0], g_vkEH[0], r[0].left, r[0].top,
        g_vkEW[1], g_vkEH[1], r[1].left, r[1].top, g_vkFmt);
    if (g_cfg.taa && g_cfg.vkPlainEyes) TaaEnsure(dev);   // allocate TAA buffers now, before anything is handed to SteamVR
    return true;
}

// VR thread, under DXVK's queue lock
static vr::EVRCompositorError VkSubmitImage(int eye, VkImage img, UINT w, UINT h, UINT fmt, UINT samples, const vr::VRTextureBounds_t* bnd, const vr::HmdMatrix34_t* pose) {
    vr::VRVulkanTextureData_t vd;
    vd.m_nImage = (uint64_t)img; vd.m_pDevice = (VkDevice_T*)g_vkDevice; vd.m_pPhysicalDevice = (VkPhysicalDevice_T*)g_vkPhys;
    vd.m_pInstance = (VkInstance_T*)g_vkInst; vd.m_pQueue = (VkQueue_T*)g_vkQueue; vd.m_nQueueFamilyIndex = g_vkQFam;
    // SteamVR decides how to decode a Vulkan image from its format: describe the game's gamma-encoded 8-bit image
    // as sRGB, otherwise it is shown as if it were linear light (too bright / washed out)
    if (g_cfg.vkReportSrgb) {
        if (fmt == VK_FORMAT_B8G8R8A8_UNORM) fmt = VK_FORMAT_B8G8R8A8_SRGB;
        else if (fmt == VK_FORMAT_R8G8B8A8_UNORM) fmt = VK_FORMAT_R8G8B8A8_SRGB;
    }
    vd.m_nWidth = w; vd.m_nHeight = h; vd.m_nFormat = fmt; vd.m_nSampleCount = samples ? samples : 1;
    if (pose) {
        vr::VRTextureWithPose_t tp;
        tp.handle = &vd; tp.eType = vr::TextureType_Vulkan; tp.eColorSpace = g_vkColorSpace; tp.mDeviceToAbsoluteTracking = *pose;
        return g_comp->Submit((vr::EVREye)eye, &tp, bnd, vr::Submit_TextureWithPose);
    }
    vr::Texture_t t = {&vd, vr::TextureType_Vulkan, g_vkColorSpace};
    return g_comp->Submit((vr::EVREye)eye, &t, bnd, vr::Submit_Default);
}

static void ReleaseD3D9Copies() {
    ReleaseVk();
    if (g_rt) { g_rt->Release(); g_rt = NULL; }
    if (g_sys) { g_sys->Release(); g_sys = NULL; }
    if (g_capSys) { g_capSys->Release(); g_capSys = NULL; }
    for (int i = 0; i < 2; i++) {
        if (g_shSurf9[i]) { g_shSurf9[i]->Release(); g_shSurf9[i] = NULL; }
        if (g_shTex9[i]) { g_shTex9[i]->Release(); g_shTex9[i] = NULL; }
    }
    if (g_evQuery) { g_evQuery->Release(); g_evQuery = NULL; }
    for (int i = 0; i < 3; i++) {
        if (g_ring11[i]) { g_ring11[i]->Release(); g_ring11[i] = NULL; }
        if (g_ringSurf9[i]) { g_ringSurf9[i]->Release(); g_ringSurf9[i] = NULL; }
        if (g_ringTex9[i]) { g_ringTex9[i]->Release(); g_ringTex9[i] = NULL; }
    }
    if (g_ringOK) { InterlockedExchange(&g_pubIdx, -1); g_ringOK = 0; }
    if (g_useShared) {   // the D3D11 views of the shared textures must go too
        for (int i = 0; i < 2; i++) if (g_eyeTex[i]) { g_eyeTex[i]->Release(); g_eyeTex[i] = NULL; }
        if (g_screenTex) { g_screenTex->Release(); g_screenTex = NULL; }
        g_texW = g_texH = 0;
    }
}

static bool CreateSharedTextures(IDirect3DDevice9* dev, UINT w, UINT h) {
    HANDLE hnd[2] = {NULL, NULL};
    for (int i = 0; i < 2; i++) {
        HRESULT hr = dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_shTex9[i], &hnd[i]);
        if (FAILED(hr) || !hnd[i]) { Log("Shared texture %d failed 0x%08lX", i, (unsigned long)hr); return false; }
        g_shTex9[i]->GetSurfaceLevel(0, &g_shSurf9[i]);
    }
    for (int i = 0; i < 2; i++) if (g_eyeTex[i]) { g_eyeTex[i]->Release(); g_eyeTex[i] = NULL; }
    if (g_screenTex) { g_screenTex->Release(); g_screenTex = NULL; }
    if (FAILED(g_d11->OpenSharedResource(hnd[0], kIID_ID3D11Texture2D, (void**)&g_eyeTex[0])) ||
        FAILED(g_d11->OpenSharedResource(hnd[1], kIID_ID3D11Texture2D, (void**)&g_screenTex))) {
        Log("OpenSharedResource failed");
        return false;
    }
    dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_evQuery);
    if (g_cfg.asyncSubmit) {
        g_ringOK = 1;
        for (int i = 0; i < 3 && g_ringOK; i++) {
            HANDLE sh = NULL;
            if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_ringTex9[i], &sh)) || !sh ||
                FAILED(g_ringTex9[i]->GetSurfaceLevel(0, &g_ringSurf9[i])) ||
                FAILED(g_d11->OpenSharedResource(sh, kIID_ID3D11Texture2D, (void**)&g_ring11[i]))) {
                Log("Async submit: ring texture %d failed - using synchronous submit", i);
                g_ringOK = 0;
            }
        }
        if (g_ringOK) Log("Async submit: 3-frame ring ready");
    }
    return true;
}

static bool EnsureResources(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = NULL;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    D3DSURFACE_DESC d; bb->GetDesc(&d); bb->Release();
    if ((g_rt || g_shSurf9[0]) && d.Width == g_bbW && d.Height == g_bbH) return true;
    ReleaseD3D9Copies();
    g_bbW = d.Width; g_bbH = d.Height;
    InterlockedExchange(&g_bbAspectMilli, (LONG)(1000.0f * d.Width / d.Height));
    Log("Backbuffer %ux%u format %d msaa %d", d.Width, d.Height, (int)d.Format, (int)d.MultiSampleType);

    if (!g_vkDev && g_usingDXVK) {
        if (FAILED(dev->QueryInterface(kIID_ID3D9VkInteropDevice, (void**)&g_vkDev))) g_vkDev = NULL;
        Log("DXVK interop %s", g_vkDev ? "available" : "NOT available");
    }
    if (g_vkDev && !g_cfg.dxvkVulkanSubmit && g_devIsEx && g_cfg.fastCopy) {
        // DXVK can export D3D-compatible shared textures; if the native D3D11 device can open them we use the
        // normal DirectX hand-off to SteamVR and avoid SteamVR's Vulkan path entirely
        g_useShared = 1;
        if (CreateSharedTextures(dev, d.Width, d.Height) && AsyncActive()) {
            g_texW = d.Width; g_texH = d.Height;
            g_eyeHas[0] = g_eyeHas[1] = 0;
            Log("DXVK: shared textures opened by D3D11 - using the DirectX hand-off to SteamVR");
            UpdateFov();
            return true;
        }
        Log("DXVK: shared textures not usable - trying the Vulkan hand-off");
        ReleaseD3D9Copies();
        g_useShared = 0;
    }
    if (g_vkDev) {
        if (CreateVkResources(dev, d.Width, d.Height)) {
            g_vk = 1; g_ringOK = 1; g_useShared = 0;
            InterlockedExchange(&g_pubIdx, -1);
            Log("DXVK: eyes go to SteamVR as Vulkan images; menus use the CPU copy");
        } else { ReleaseVk(); g_vk = 0; Log("DXVK: Vulkan hand-off unavailable - using CPU copy for everything"); }
    } else if (g_devIsEx && g_cfg.fastCopy) {
        g_useShared = 1;
        if (CreateSharedTextures(dev, d.Width, d.Height)) {
            g_texW = d.Width; g_texH = d.Height;
            g_eyeHas[0] = g_eyeHas[1] = 0;
            Log("Fast GPU copy (shared textures) active");
            UpdateFov();
            return true;
        }
        Log("Fast copy unavailable - using CPU copy");
        ReleaseD3D9Copies();
        g_useShared = 0;
    }
    HRESULT hr = dev->CreateRenderTarget(d.Width, d.Height, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &g_rt, NULL);
    if (SUCCEEDED(hr)) hr = dev->CreateOffscreenPlainSurface(d.Width, d.Height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g_sys, NULL);
    if (FAILED(hr)) { Log("ERROR: creating D3D9 copy surfaces failed 0x%08lX", (unsigned long)hr); ReleaseD3D9Copies(); return false; }

    if (g_texW != d.Width || g_texH != d.Height || !g_eyeTex[0]) {
        for (int i = 0; i < 2; i++) if (g_eyeTex[i]) { g_eyeTex[i]->Release(); g_eyeTex[i] = NULL; }
        if (g_screenTex) { g_screenTex->Release(); g_screenTex = NULL; }
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof(td));
        td.Width = d.Width; td.Height = d.Height; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        for (int i = 0; i < 2; i++) g_d11->CreateTexture2D(&td, NULL, &g_eyeTex[i]);
        g_d11->CreateTexture2D(&td, NULL, &g_screenTex);
        g_texW = d.Width; g_texH = d.Height;
        g_eyeHas[0] = g_eyeHas[1] = 0;
        if (!g_eyeTex[0] || !g_eyeTex[1] || !g_screenTex) { Log("ERROR: CreateTexture2D failed"); return false; }
    }
    UpdateFov();
    return true;
}

// ---- frame capture (for remote diagnosis): BMP, 1/3 size ----
static volatile LONG g_captureReq;   // frames still to capture
static int g_captureBurst, g_captureIdx, g_captureFull;
static void CaptureSurface(IDirect3DDevice9* dev, IDirect3DSurface9* src, const char* tag) {
    if (!src) return;
    D3DSURFACE_DESC sd; src->GetDesc(&sd);
    if (!g_capSys && FAILED(dev->CreateOffscreenPlainSurface(sd.Width, sd.Height, sd.Format, D3DPOOL_SYSTEMMEM, &g_capSys, NULL))) return;
    if (FAILED(dev->GetRenderTargetData(src, g_capSys))) return;
    D3DLOCKED_RECT lr;
    if (FAILED(g_capSys->LockRect(&lr, NULL, D3DLOCK_READONLY))) return;
    const int F = g_captureFull ? 1 : 3;
    int W = (int)sd.Width / F, H = (int)sd.Height / F, rowBytes = (W * 3 + 3) & ~3;
    char dir[MAX_PATH], path[MAX_PATH];
    lstrcpyA(dir, g_dir); lstrcatA(dir, "acmvr_capture");
    CreateDirectoryA(dir, NULL);
    _snprintf(path, sizeof(path), "%s\\burst%03d_frame%02d_%s.bmp", dir, g_captureBurst, g_captureIdx, tag);
    FILE* f = fopen(path, "wb");
    if (f) {
        BYTE hdr[54]; memset(hdr, 0, sizeof(hdr));
        DWORD fileSize = 54 + rowBytes * H;
        hdr[0] = 'B'; hdr[1] = 'M'; *(DWORD*)(hdr + 2) = fileSize; *(DWORD*)(hdr + 10) = 54;
        *(DWORD*)(hdr + 14) = 40; *(int*)(hdr + 18) = W; *(int*)(hdr + 22) = H;
        *(WORD*)(hdr + 26) = 1; *(WORD*)(hdr + 28) = 24; *(DWORD*)(hdr + 34) = rowBytes * H;
        fwrite(hdr, 1, 54, f);
        BYTE* row = (BYTE*)malloc(rowBytes);
        for (int y = H - 1; y >= 0; y--) {
            BYTE* srow = (BYTE*)lr.pBits + (y * F + F / 2) * lr.Pitch;
            memset(row, 0, rowBytes);
            for (int x = 0; x < W; x++) { BYTE* p = srow + (x * F + F / 2) * 4; row[x * 3] = p[0]; row[x * 3 + 1] = p[1]; row[x * 3 + 2] = p[2]; }
            fwrite(row, 1, rowBytes, f);
        }
        free(row);
        fclose(f);
    }
    g_capSys->UnlockRect();
    if (g_captureIdx == 0) Log("Capturing burst %d -> %s", g_captureBurst, dir);
    {   // which head pose this frame was drawn with (to check the picture moves with the pose that goes to SteamVR)
        const vr::HmdMatrix34_t& m = g_poppedPose;
        float fx = -m.m[0][2], fy = -m.m[1][2], fz = -m.m[2][2];
        float yaw = HmdYawVr(m) * 57.29578f, pitch = fatan2(fy, fsqrt(fx * fx + fz * fz)) * 57.29578f;
        Log("  frame %d: pose yaw %.3f pitch %.3f t=%.2f", g_captureIdx, yaw, pitch, QpcMs());
    }
    g_captureIdx++;
}

static bool CopyBackbuffer(IDirect3DDevice9* dev, ID3D11Texture2D* dst, bool opaque) {
    IDirect3DSurface9* bb = NULL;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    if (g_useShared) {
        IDirect3DSurface9* target = (dst == g_screenTex) ? g_shSurf9[1] : g_shSurf9[0];
        HRESULT hr = dev->StretchRect(bb, NULL, target, NULL, D3DTEXF_NONE);
        bb->Release();
        if (FAILED(hr)) return false;
        if (g_evQuery) {
            // Push the work to the GPU. Windows orders GPU access to shared textures between the game's device and
            // SteamVR, so waiting for completion (WaitForGPU=1) is only a fallback - it stalls this thread ~3 ms.
            g_evQuery->Issue(D3DISSUE_END);
            if (g_cfg.gpuWait) {
                for (int spin = 0; g_evQuery->GetData(NULL, 0, D3DGETDATA_FLUSH) == S_FALSE && spin < 200000; spin++) YieldProcessor();
            } else {
                g_evQuery->GetData(NULL, 0, D3DGETDATA_FLUSH);
            }
        }
        if (g_captureReq > 0) { CaptureSurface(dev, target, dst == g_screenTex ? "screen" : "stereo"); InterlockedDecrement(&g_captureReq); }
        return true;
    }
    HRESULT hr = dev->StretchRect(bb, NULL, g_rt, NULL, D3DTEXF_NONE);
    bb->Release();
    if (FAILED(hr)) return false;
    if (g_captureReq > 0) { CaptureSurface(dev, g_rt, dst == g_screenTex ? "screen" : "stereo"); InterlockedDecrement(&g_captureReq); }
    if (FAILED(dev->GetRenderTargetData(g_rt, g_sys))) return false;
    D3DLOCKED_RECT lr;
    if (FAILED(g_sys->LockRect(&lr, NULL, 0))) return false;
    if (opaque) {
        for (UINT y = 0; y < g_bbH; y++) {
            BYTE* row = (BYTE*)lr.pBits + y * lr.Pitch;
            for (UINT x = 0; x < g_bbW; x++) row[x * 4 + 3] = 0xFF;
        }
    }
    g_ctx->UpdateSubresource(dst, 0, NULL, lr.pBits, (UINT)lr.Pitch, 0);
    g_sys->UnlockRect();
    return true;
}
static void StartCapture(int frames) {
    if (g_captureReq > 0) return;
    static int scanned;
    if (!scanned) {   // never overwrite earlier captures: carry on from the highest burst number already there
        scanned = 1;
        char pat[MAX_PATH]; lstrcpyA(pat, g_dir); lstrcatA(pat, "acmvr_capture\\burst*.bmp");
        WIN32_FIND_DATAA fd; HANDLE fh = FindFirstFileA(pat, &fd);
        if (fh != INVALID_HANDLE_VALUE) {
            do { int n = atoi(fd.cFileName + 5); if (n > g_captureBurst) g_captureBurst = n; } while (FindNextFileA(fh, &fd));
            FindClose(fh);
        }
    }
    g_captureBurst++; g_captureIdx = 0;
    InterlockedExchange(&g_captureReq, frames);
}

static void SetOverlayPose(const vr::HmdMatrix34_t& hmd) {
    if (!g_ovl || g_overlay == vr::k_ulOverlayHandleInvalid) return;
    float th = HmdYawVr(hmd), c = fcos(th), s = fsin(th), d = g_cfg.screenDist;
    vr::HmdMatrix34_t m;
    memset(&m, 0, sizeof(m));
    m.m[0][0] = c;  m.m[0][2] = s;
    m.m[1][1] = 1;
    m.m[2][0] = -s; m.m[2][2] = c;
    m.m[0][3] = hmd.m[0][3] - d * s;
    m.m[1][3] = hmd.m[1][3];
    m.m[2][3] = hmd.m[2][3] - d * c;
    g_ovl->SetOverlayTransformAbsolute(g_overlay, vr::TrackingUniverseStanding, &m);
}

static int g_keyPrev[48];
static bool KeyPressed(int idx, int vk) {
    int down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    int pressed = down && !g_keyPrev[idx];
    g_keyPrev[idx] = down;
    return pressed;
}
static void PollHotkeys() {
    if (KeyPressed(0, VK_F7)) { g_swapHotkey ^= 1; Log("Swap eyes: %d", (int)(g_swapHotkey ^ g_cfg.swapEyes)); }
    if (KeyPressed(1, VK_F8)) { InterlockedExchange(&g_recenterReq, 1); }
    if (KeyPressed(2, VK_F9)) { g_stereoOn ^= 1; Log("Stereo/head tracking: %s", g_stereoOn ? "ON" : "OFF (screen mode)"); }
    if (KeyPressed(3, VK_F10)) { g_aimToggle ^= 1; Log("Controller aim: %s", ((g_cfg.controllerAim != 0) ^ (g_aimToggle != 0)) ? "ON" : "OFF (head aim)"); }
    if (KeyPressed(5, VK_F6)) { g_hudToggle ^= 1; Log("HUD toggled"); }
    if (KeyPressed(6, VK_F12)) { g_captureFull = 0; StartCapture(6); ShowMessage(2000, "Capturing 6 frames"); }
    if (KeyPressed(12, VK_F1)) { InterlockedExchange(&g_taaUserOff, !g_taaUserOff); Log("Temporal AA %s (F1)", g_taaUserOff ? "OFF" : "ON"); }
    if (KeyPressed(11, VK_F3)) InterlockedExchange(&g_hudSurveyReq, 1);
    if (KeyPressed(10, VK_F2)) InterlockedExchange(&g_uiFxToggleReq, 1);   // handled on the game thread
    if (KeyPressed(9, VK_CONTROL) && g_captureReq <= 0) { g_captureFull = 1; StartCapture(8); Log("Ctrl: full-size capture of 8 frames"); }
    if (KeyPressed(8, VK_F4)) { g_perfOverlay ^= 1; ShowMessage(1500, "Performance display %s", g_perfOverlay ? "ON" : "OFF"); }
    if (KeyPressed(7, VK_F5)) ShowMessage(4000, "ACM-VR %s test message\nIf you can read this, the message panel works.", ACMVR_VERSION);
    if (KeyPressed(4, VK_F11)) { g_posToggle ^= 1; Log("Positional tracking: %s", ((g_cfg.positional != 0) ^ (g_posToggle != 0)) ? "ON" : "OFF"); }
}

static CRITICAL_SECTION g_vrCs;          // serialises compositor / D3D11 use (render thread vs watchdog)
static volatile DWORD g_lastPresentTick;
static void SubmitBlack() {
    if (!g_blackTex) {
        static DWORD zeros[16 * 16];
        D3D11_TEXTURE2D_DESC td; memset(&td, 0, sizeof(td));
        td.Width = 16; td.Height = 16; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = {zeros, 16 * 4, 0};
        g_d11->CreateTexture2D(&td, &sd, &g_blackTex);
    }
    if (!g_blackTex) return;
    vr::Texture_t bt = {(void*)g_blackTex, vr::TextureType_DirectX, vr::ColorSpace_Auto};
    g_comp->Submit(vr::Eye_Left, &bt, NULL, vr::Submit_Default);
    g_comp->Submit(vr::Eye_Right, &bt, NULL, vr::Submit_Default);
}
static void UpdateOverlayCrop() {
    if (!g_ovl || g_overlay == vr::k_ulOverlayHandleInvalid || !g_bbW || !g_bbH) return;
    vr::VRTextureBounds_t b = {0, 0, 1, 1};
    float aspect = (float)g_bbW / (float)g_bbH, want = 16.0f / 9.0f;
    if (g_cfg.screenCrop && aspect > want + 0.01f) { float h = 0.5f * want / aspect; b.uMin = 0.5f - h; b.uMax = 0.5f + h; }
    g_ovl->SetOverlayTextureBounds(g_overlay, &b);
}
// Gun tilt per kind of controller, where it's known from the headset. -999 = use the general AimPitchOffsetDeg.
static float AimPitchDefaultFor(const char* type) {
    (void)type;   // none yet: PS VR2 Sense and Quest Touch both play well with the general -42.1 (fill in others as they're tuned)
    return -999.0f;
}
static bool WaitAndUpdatePoses() {
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    double tw = QpcMs();
    vr::EVRCompositorError we = g_comp->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, NULL, 0);
    if (!AsyncActive()) g_prt.wait += QpcMs() - tw;   // in async mode this wait is on the VR thread, not the game
    if (we != vr::VRCompositorError_None) return false;
    LONG il = (LONG)g_sys_vr->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
    LONG ir = (LONG)g_sys_vr->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_RightHand);
    if (il >= (LONG)vr::k_unMaxTrackedDeviceCount) il = -1;
    if (ir >= (LONG)vr::k_unMaxTrackedDeviceCount) ir = -1;
    InterlockedExchange(&g_ctrlIdx[0], il);
    InterlockedExchange(&g_ctrlIdx[1], ir);
    if (!g_ctlTypeKnown && ir >= 0) {   // which controllers? each kind reports its pose at its own angle
        char t[64] = "";
        g_sys_vr->GetStringTrackedDeviceProperty((vr::TrackedDeviceIndex_t)ir, vr::Prop_ControllerType_String, t, sizeof(t));
        if (t[0]) {
            for (char* q = t; *q; q++) if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_')) *q = '_';
            lstrcpynA(g_ctlType, t, sizeof(g_ctlType));
            char key[96]; _snprintf(key, sizeof(key), "AimPitchOffsetDeg.%s", g_ctlType);
            char v[32]; GetPrivateProfileStringA("Controls", key, "", v, sizeof(v), g_ini);
            float def = AimPitchDefaultFor(g_ctlType);
            if (v[0]) { g_aimPitch = (float)atof(v); Log("Controllers: %s - gun tilt %.1f (from %s)", g_ctlType, g_aimPitch, key); }
            else if (def > -900) { g_aimPitch = def; Log("Controllers: %s - gun tilt %.1f (built-in for these controllers; calibrate to save your own)", g_ctlType, g_aimPitch); }
            else Log("Controllers: %s - gun tilt %.1f (general AimPitchOffsetDeg; calibrate to save one for these controllers)", g_ctlType, g_aimPitch);
            g_ctlTypeKnown = 1;
        }
    }
    const vr::TrackedDevicePose_t& hp = poses[vr::k_unTrackedDeviceIndex_Hmd];
    EnterCriticalSection(&g_poseCs);
    g_pose.hmdValid = hp.bPoseIsValid;
    if (hp.bPoseIsValid) g_pose.hmd = hp.mDeviceToAbsoluteTracking;
    LONG idx[2] = {il, ir};
    for (int h = 0; h < 2; h++) {
        g_pose.ctrlValid[h] = idx[h] >= 0 && poses[idx[h]].bPoseIsValid;
        if (g_pose.ctrlValid[h]) g_pose.ctrl[h] = poses[idx[h]].mDeviceToAbsoluteTracking;
    }
    bool recenter = hp.bPoseIsValid && InterlockedExchange(&g_recenterReq, 0);
    if (recenter) {
        g_pose.recYaw = HmdYawVr(hp.mDeviceToAbsoluteTracking);
        g_pose.recPos = mk(hp.mDeviceToAbsoluteTracking.m[0][3], hp.mDeviceToAbsoluteTracking.m[1][3], hp.mDeviceToAbsoluteTracking.m[2][3]);
    }
    LeaveCriticalSection(&g_poseCs);
    if (recenter) { SetOverlayPose(hp.mDeviceToAbsoluteTracking); Log("Recentred"); ShowMessage(1500, "Recentred"); }
    if (hp.bPoseIsValid && !g_vrActive) { InterlockedExchange(&g_vrActive, 1); Log("Tracking active"); }
    return true;
}
// Keeps SteamVR fed while the game is loading / stalled, so it doesn't pause the app.
static DWORD WINAPI WatchdogThread(LPVOID) {
    int stalled = 0;
    for (;;) {
        Sleep(5);
        if (!g_vrOK || AsyncActive() || g_vk) continue;
        DWORD now = GetTickCount();
        if (now - g_lastPresentTick < 200) { if (stalled) { stalled = 0; } continue; }
        EnterCriticalSection(&g_vrCs);
        if (GetTickCount() - g_lastPresentTick >= 200) {
            if (!stalled) { stalled = 1; Log("Game stalled - keeping SteamVR fed"); }
            SubmitBlack();
            if (g_overlay != vr::k_ulOverlayHandleInvalid && !g_overlayShown && g_screenTex) {
                g_ovl->ShowOverlay(g_overlay); g_overlayShown = 1;
            }
            WaitAndUpdatePoses();
        }
        LeaveCriticalSection(&g_vrCs);
    }
    return 0;
}

static vr::VROverlayHandle_t g_msgOverlay = vr::k_ulOverlayHandleInvalid;
static ID3D11Texture2D* g_msgTex;
static int g_msgShown;
static LONG g_msgDrawn = -1;
static const int MSG_W = 1024, MSG_H = 160;
// GDI text -> BGRA texture (white text on translucent dark background)
static void DrawTextToTex(ID3D11Texture2D* tex, int W, int H, const char* text, int fontPx, UINT fmt) {
    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (dc && bmp && bits) {
        HGDIOBJ oldb = SelectObject(dc, bmp);
        memset(bits, 0, W * H * 4);
        HFONT font = CreateFontA(-fontPx, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
        HGDIOBJ oldf = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        RECT rc = {8, 0, W - 8, H};
        DrawTextA(dc, text, -1, &rc, fmt | DT_NOPREFIX);
        GdiFlush();
        DWORD* px = (DWORD*)bits;
        for (int i = 0; i < W * H; i++) {
            DWORD c = (px[i] >> 16) & 0xFF, a = 150 + (c * 105) / 255;
            px[i] = (a << 24) | (c << 16) | (c << 8) | c;
        }
        g_ctx->UpdateSubresource(tex, 0, NULL, bits, W * 4, 0);
        SelectObject(dc, oldf); DeleteObject(font); SelectObject(dc, oldb);
    }
    if (bmp) DeleteObject(bmp);
    if (dc) DeleteDC(dc);
}
static vr::VROverlayHandle_t g_fpsOverlay = vr::k_ulOverlayHandleInvalid;
static ID3D11Texture2D* g_fpsTex;
static const int FPS_W = 512, FPS_H = 96;
static double g_fpsT0; static int g_fpsN; static LONG g_fpsSub0, g_fpsRep0;
static volatile LONG g_vrSubmitsAll, g_vrRepeatsAll;   // never reset (for the on-screen counter)
static void UpdateFpsPanel() {
    if (!g_cfg.showFps || !g_ovl || !g_d11) return;
    if (g_fpsOverlay == vr::k_ulOverlayHandleInvalid) {
        if (g_ovl->CreateOverlay("acmvr.fps", "ACM-VR fps", &g_fpsOverlay) != vr::VROverlayError_None) { g_fpsOverlay = vr::k_ulOverlayHandleInvalid; g_cfg.showFps = 0; return; }
        g_ovl->SetOverlayWidthInMeters(g_fpsOverlay, 0.30f);
        g_ovl->SetOverlaySortOrder(g_fpsOverlay, 11);
        vr::HmdMatrix34_t m; memset(&m, 0, sizeof(m));
        m.m[0][0] = m.m[1][1] = m.m[2][2] = 1.0f;
        m.m[0][3] = 0.42f; m.m[1][3] = 0.30f; m.m[2][3] = -1.2f;   // upper right of view, 1.2 m away
        g_ovl->SetOverlayTransformTrackedDeviceRelative(g_fpsOverlay, vr::k_unTrackedDeviceIndex_Hmd, &m);
        D3D11_TEXTURE2D_DESC td; memset(&td, 0, sizeof(td));
        td.Width = FPS_W; td.Height = FPS_H; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        g_d11->CreateTexture2D(&td, NULL, &g_fpsTex);
        g_fpsT0 = QpcMs();
    }
    g_fpsN++;
    double now = QpcMs(), dt = now - g_fpsT0;
    if (dt < 500 || !g_fpsTex) return;
    char text[96];
    LONG sub = g_vrSubmitsAll, rep = g_vrRepeatsAll, ds = sub - g_fpsSub0, dr = rep - g_fpsRep0;
    double fps = g_fpsN * 1000.0 / dt;
    if (AsyncActive() && ds > 0 && !g_asyncScreen) {
        if (g_cfg.dynRes) wsprintfA(text, "%d fps  %d%% reused  res %d%%", (int)(fps + 0.5), (int)(100.0 * dr / ds + 0.5), (int)g_screenPct);
        else wsprintfA(text, "%d fps   %d%% reused", (int)(fps + 0.5), (int)(100.0 * dr / ds + 0.5));
    }
    else wsprintfA(text, "%d fps", (int)(fps + 0.5));
    g_fpsSub0 = sub; g_fpsRep0 = rep; g_fpsN = 0; g_fpsT0 = now;
    DrawTextToTex(g_fpsTex, FPS_W, FPS_H, text, 52, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    vr::Texture_t t = {(void*)g_fpsTex, vr::TextureType_DirectX, vr::ColorSpace_Auto};
    g_ovl->SetOverlayTexture(g_fpsOverlay, &t);
    g_ovl->ShowOverlay(g_fpsOverlay);
}
static void UpdateMessagePanel() {
    if (!g_cfg.showMessages || !g_ovl || !g_d11) return;
    if (g_msgOverlay == vr::k_ulOverlayHandleInvalid) {
        if (g_ovl->CreateOverlay("acmvr.msg", "ACM-VR messages", &g_msgOverlay) != vr::VROverlayError_None) { g_msgOverlay = vr::k_ulOverlayHandleInvalid; g_cfg.showMessages = 0; return; }
        g_ovl->SetOverlayWidthInMeters(g_msgOverlay, 0.9f);
        g_ovl->SetOverlaySortOrder(g_msgOverlay, 10);
        vr::HmdMatrix34_t m; memset(&m, 0, sizeof(m));
        m.m[0][0] = m.m[1][1] = m.m[2][2] = 1.0f;
        m.m[1][3] = -0.30f; m.m[2][3] = -1.5f;          // a little below the centre of view, 1.5 m away
        g_ovl->SetOverlayTransformTrackedDeviceRelative(g_msgOverlay, vr::k_unTrackedDeviceIndex_Hmd, &m);
        D3D11_TEXTURE2D_DESC td; memset(&td, 0, sizeof(td));
        td.Width = MSG_W; td.Height = MSG_H; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        g_d11->CreateTexture2D(&td, NULL, &g_msgTex);
    }
    DWORD now = GetTickCount();
    char text[256]; LONG serial; DWORD expire;
    EnterCriticalSection(&g_msgCs);
    lstrcpyA(text, g_msgText); serial = g_msgSerial; expire = g_msgExpire;
    LeaveCriticalSection(&g_msgCs);
    bool visible = text[0] && (int)(expire - now) > 0;
    if (!visible) { if (g_msgShown) { g_ovl->HideOverlay(g_msgOverlay); g_msgShown = 0; } return; }
    if (serial != g_msgDrawn && g_msgTex) {
        g_msgDrawn = serial;
        // draw the text with GDI into a 32-bit DIB, then upload
        BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader); bi.bmiHeader.biWidth = MSG_W; bi.bmiHeader.biHeight = -MSG_H;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        void* bits = NULL;
        HDC dc = CreateCompatibleDC(NULL);
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (dc && bmp && bits) {
            HGDIOBJ oldb = SelectObject(dc, bmp);
            memset(bits, 0, MSG_W * MSG_H * 4);
            HFONT font = CreateFontA(-56, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI");
            HGDIOBJ oldf = SelectObject(dc, font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            RECT rc = {0, 0, MSG_W, MSG_H};
            DrawTextA(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_WORDBREAK | DT_NOPREFIX);
            GdiFlush();
            // white text on a translucent dark background; text coverage taken from the red channel
            DWORD* px = (DWORD*)bits;
            for (int i = 0; i < MSG_W * MSG_H; i++) {
                DWORD c = (px[i] >> 16) & 0xFF;
                DWORD a = 150 + (c * 105) / 255;
                px[i] = (a << 24) | (c << 16) | (c << 8) | c;
            }
            g_ctx->UpdateSubresource(g_msgTex, 0, NULL, bits, MSG_W * 4, 0);
            SelectObject(dc, oldf); DeleteObject(font);
            SelectObject(dc, oldb);
        }
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
        vr::Texture_t t = {(void*)g_msgTex, vr::TextureType_DirectX, vr::ColorSpace_Auto};
        g_ovl->SetOverlayTexture(g_msgOverlay, &t);
    }
    if (!g_msgShown) { g_ovl->ShowOverlay(g_msgOverlay); g_msgShown = 1; }
}

static void EyeBounds(int e, vr::VRTextureBounds_t* bnd) {
    bool swap = (g_cfg.swapEyes != 0) ^ (g_swapHotkey != 0);
    int half = swap ? 1 - e : e;   // which half of the image holds this eye
    float T = g_gameTanH, a = 0.5f * (float)g_bbW / (float)g_bbH, Tv = T / a;
    if (g_offAxisActive) {
        float U = g_offAxisUp, D = g_offAxisDn;
        bnd->uMin = 0.5f * half; bnd->uMax = 0.5f * half + 0.5f;
        bnd->vMin = (U - g_projRaw[e][3]) / (U + D);   // top of the eye's view: tangent pfBottom (up)
        bnd->vMax = (U - g_projRaw[e][2]) / (U + D);   // bottom: tangent pfTop (down)
    } else {
        bnd->uMin = 0.5f * half + 0.5f * (0.5f + g_projRaw[e][0] / (2 * T));
        bnd->uMax = 0.5f * half + 0.5f * (0.5f + g_projRaw[e][1] / (2 * T));
        bnd->vMin = 0.5f - g_projRaw[e][3] / (2 * Tv);
        bnd->vMax = 0.5f - g_projRaw[e][2] / (2 * Tv);
    }
}
static bool AsyncActive() { return g_vk ? (g_ringOK != 0) : (g_cfg.asyncSubmit && g_ringOK && g_useShared); }

// VR thread (async mode): paces to the headset and always submits the newest finished frame with its pose.
// ---- thread monitor: CPU use of every thread in the game (names from Windows thread descriptions), and priority boost ----
typedef HRESULT(WINAPI* GetThreadDescription_t)(HANDLE, PWSTR*);
struct ThrRec { DWORD tid; double cpu; };
static DWORD WINAPI ThreadMonitor(LPVOID) {
    GetThreadDescription_t gtd = (GetThreadDescription_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    static ThrRec prev[512]; int nPrev = 0;
    double lastWall = QpcMs();
    bool boosted = false;
    for (;;) {
        Sleep(5000);
        if (!g_vrOK) continue;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) continue;
        THREADENTRY32 te; te.dwSize = sizeof(te);
        struct Row { double pct; DWORD tid; char name[48]; int prio; } rows[512]; int nr = 0;
        static ThrRec cur[512]; int nCur = 0;
        double now = QpcMs(), wall = now - lastWall; lastWall = now;
        DWORD pid = GetCurrentProcessId();
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid || nCur >= 512) continue;
            HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            FILETIME c, e, k, u;
            double cpu = 0;
            if (GetThreadTimes(h, &c, &e, &k, &u)) {
                ULARGE_INTEGER a, b; a.LowPart = k.dwLowDateTime; a.HighPart = k.dwHighDateTime; b.LowPart = u.dwLowDateTime; b.HighPart = u.dwHighDateTime;
                cpu = (double)(a.QuadPart + b.QuadPart) / 10000.0;
            }
            cur[nCur].tid = te.th32ThreadID; cur[nCur].cpu = cpu; nCur++;
            double before = -1;
            for (int i = 0; i < nPrev; i++) if (prev[i].tid == te.th32ThreadID) before = prev[i].cpu;
            char name[48] = "";
            if (te.th32ThreadID == g_renderThreadId) lstrcpyA(name, "GAME RENDER THREAD");
            else if (te.th32ThreadID == g_gameThreadId) lstrcpyA(name, "GAME THREAD");
            else if (te.th32ThreadID == g_vrThreadId) lstrcpyA(name, "mod VR thread");
            else if (gtd) {
                PWSTR w = NULL;
                if (SUCCEEDED(gtd(h, &w)) && w) { int i = 0; for (; w[i] && i < 47; i++) name[i] = (char)w[i]; name[i] = 0; LocalFree(w); }
            }
            // priority boost: the game's render thread and DXVK's command-stream thread
            if (g_cfg.boostThreads && !boosted && (te.th32ThreadID == g_renderThreadId || !lstrcmpA(name, "dxvk-cs")))
                SetThreadPriority(h, THREAD_PRIORITY_HIGHEST);
            int prio = GetThreadPriority(h);
            CloseHandle(h);
            if (before >= 0 && nr < 512) {
                rows[nr].pct = 100.0 * (cpu - before) / wall; rows[nr].tid = te.th32ThreadID; rows[nr].prio = prio;
                lstrcpynA(rows[nr].name, name[0] ? name : "(unnamed)", 48); nr++;
            }
        }
        CloseHandle(snap);
        if (g_cfg.boostThreads && g_renderThreadId && !boosted) { boosted = true; Log("Thread priority: render thread and dxvk-cs raised to HIGHEST"); }
        memcpy(prev, cur, nCur * sizeof(ThrRec)); nPrev = nCur;
        // top threads by CPU
        for (int i = 1; i < nr; i++) { Row x = rows[i]; int j = i - 1; while (j >= 0 && rows[j].pct < x.pct) { rows[j + 1] = rows[j]; j--; } rows[j + 1] = x; }
        char line[700]; int len = 0; line[0] = 0;
        double total = 0; for (int i = 0; i < nr; i++) total += rows[i].pct;
        for (int i = 0; i < nr && i < 10 && rows[i].pct >= 1.0; i++)
            len += _snprintf(line + len, sizeof(line) - len - 1, "%s%s %.0f%%%s", i ? ", " : "", rows[i].name, rows[i].pct, rows[i].prio >= 2 ? "(hi)" : "");
        Log("  threads (%% of one core, %d threads, %.0f%% total): %s", nr, total, line);
    }
    return 0;
}

static void VkPollPublish();
static DWORD WINAPI VrSubmitThread(LPVOID) {
    LONG lastSerial = -1;
    g_vrThreadId = GetCurrentThreadId();
    for (;;) {
        if (!g_vrOK || !AsyncActive()) { Sleep(5); continue; }
        if (g_refreshMs <= 0) {
            float hz = g_sys_vr->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_DisplayFrequency_Float, NULL);
            if (hz < 30 || hz > 400) hz = 90;
            g_refreshMs = 1000.0 / hz;
            Log("Headset refresh %.1f Hz - frame cap %s (x%.2f)", hz, g_cfg.frameCap ? "on" : "off", g_cfg.frameCapRatio);
        }
        if (!WaitAndUpdatePoses()) Sleep(1);        // blocks until the next frame slot
        g_slotMs = QpcMs();
        {   // SteamVR's own record of the previous frame
            static uint32_t lastIdx;
            vr::Compositor_FrameTiming ft; memset(&ft, 0, sizeof(ft)); ft.m_nSize = sizeof(ft);
            if (g_comp->GetFrameTiming(&ft, 1) && ft.m_nFrameIndex != lastIdx) {
                lastIdx = ft.m_nFrameIndex;
                EnterCriticalSection(&g_vtCs);
                VrTiming& t = g_vt;
                t.n++;
                if (ft.m_nNumFramePresents > 1) t.reused++;
                if (ft.m_nReprojectionFlags & vr::VRCompositor_ReprojectionReason_Cpu) t.cpuRepro++;
                if (ft.m_nReprojectionFlags & vr::VRCompositor_ReprojectionReason_Gpu) t.gpuRepro++;
                if (ft.m_nReprojectionFlags & vr::VRCompositor_ReprojectionMotion) t.motion++;
                t.dropped += ft.m_nNumDroppedFrames; t.mispresented += ft.m_nNumMisPresented;
                t.appGpu += ft.m_flTotalRenderGpuMs; if (ft.m_flTotalRenderGpuMs > t.appGpuMax) t.appGpuMax = ft.m_flTotalRenderGpuMs;
                t.compGpu += ft.m_flCompositorRenderGpuMs;
                t.interval += ft.m_flClientFrameIntervalMs; if (ft.m_flClientFrameIntervalMs > t.intervalMax) t.intervalMax = ft.m_flClientFrameIntervalMs;
                t.ready += ft.m_flNewFrameReadyMs;
                LeaveCriticalSection(&g_vtCs);
            }
        }

        EnterCriticalSection(&g_vrCs);
        if (g_vk) VkPollPublish();   // pick up any frame the GPU finished since the render thread last looked
        DWORD now = GetTickCount();
        bool stalled = now - g_lastPresentTick > 500;
        LONG idx, serial;
        vr::HmdMatrix34_t pose;
        EnterCriticalSection(&g_pubCs);
        vr::HmdMatrix34_t poseE[2];
        idx = g_pubIdx; serial = g_pubSerial; pose = g_pubPose; poseE[0] = g_pubPoseE[0]; poseE[1] = g_pubPoseE[1];
        LeaveCriticalSection(&g_pubCs);
        if (g_vk) {
            if (g_ringOK) {
                bool black = g_asyncScreen || stalled || idx < 0 || !g_vkImg[idx * 2] || !g_vkImg[idx * 2 + 1];
                if (!black && serial == lastSerial && !g_vkSrc[idx]) black = true;   // safety: never read an image mid-rewrite
                bool tr = !black && g_vkTrace > 0;
                static int blackTraced;
                bool trc = tr || (black && blackTraced < 1);
                if (black && trc) { blackTraced++; Log("vk trace: black submit (for comparison)"); }
                if (trc) InterlockedExchange(&g_vkCallTrace, 1);
                if (tr) Log("vk trace: VR thread submitting slot %ld (%ux%u + %ux%u fmt %u) - locking queue", idx, g_vkEW[0], g_vkEH[0], g_vkEW[1], g_vkEH[1], g_vkFmt);
                g_vkDev->LockSubmissionQueue();
                g_comp->SubmitExplicitTimingData();   // right before our (tiny) GPU work, so SteamVR measures only that
                if (tr) Log("vk trace: queue locked");
                for (int e = 0; e < 2; e++) {
                    vr::EVRCompositorError ce;
                    if (black) ce = g_vkBlackImg ? VkSubmitImage(e, g_vkBlackImg, 16, 16, g_vkBlackFmt, 1, NULL, NULL) : vr::VRCompositorError_None;
                    else ce = VkSubmitImage(e, g_vkImg[idx * 2 + e], g_vkEW[e], g_vkEH[e], g_vkFmt, g_vkSamples, NULL, g_cfg.vkSubmitPose ? &poseE[e] : NULL);
                    static int errs;
                    if (ce != vr::VRCompositorError_None && errs++ < 10) Log("Vulkan submit eye %d (%s) error %d", e, black ? "black" : "frame", (int)ce);
                    if (tr) Log("vk trace: eye %d submitted (%d)", e, (int)ce);
                }
                g_comp->PostPresentHandoff();
                if (tr) Log("vk trace: handoff done");
                g_vkDev->ReleaseSubmissionQueue();
                if (tr) { Log("vk trace: queue released"); InterlockedDecrement(&g_vkTrace); }
                if (trc) InterlockedExchange(&g_vkCallTrace, 0);
                if (stalled && g_overlay != vr::k_ulOverlayHandleInvalid && !g_overlayShown && g_screenTex) { g_ovl->ShowOverlay(g_overlay); g_overlayShown = 1; }
                if (!black) {
                    InterlockedIncrement(&g_vrSubmits); InterlockedIncrement(&g_vrSubmitsAll);
                    if (serial == lastSerial) { InterlockedIncrement(&g_vrRepeats); InterlockedIncrement(&g_vrRepeatsAll); }
                    lastSerial = serial;
                    InterlockedExchange(&g_consumedSerial, serial);
                }
            }
        } else if (g_asyncScreen || stalled || idx < 0 || !g_ring11[idx]) {
            SubmitBlack();
            if (stalled && g_overlay != vr::k_ulOverlayHandleInvalid && !g_overlayShown && g_screenTex) { g_ovl->ShowOverlay(g_overlay); g_overlayShown = 1; }
        } else {
            for (int e = 0; e < 2; e++) {
                vr::VRTextureBounds_t bnd; EyeBounds(e, &bnd);
                vr::VRTextureWithPose_t tp;
                tp.handle = (void*)g_ring11[idx]; tp.eType = vr::TextureType_DirectX; tp.eColorSpace = vr::ColorSpace_Auto;
                tp.mDeviceToAbsoluteTracking = pose;
                vr::EVRCompositorError ce = g_comp->Submit((vr::EVREye)e, &tp, &bnd, vr::Submit_TextureWithPose);
                static int errs;
                if (ce != vr::VRCompositorError_None && errs++ < 10) Log("Async submit eye %d error %d", e, (int)ce);
            }
            InterlockedIncrement(&g_vrSubmits); InterlockedIncrement(&g_vrSubmitsAll);
            if (serial == lastSerial) { InterlockedIncrement(&g_vrRepeats); InterlockedIncrement(&g_vrRepeatsAll); }
            lastSerial = serial;
            InterlockedExchange(&g_consumedSerial, serial);
            static int checks;
            if (g_usingDXVK && (g_vrSubmitsAll == 200 || g_vrSubmitsAll == 2000) && checks < 2) {
                // sanity check that D3D11 really sees DXVK's pixels: average a 64x64 patch from the middle
                checks++;
                ID3D11Texture2D* st = NULL;
                D3D11_TEXTURE2D_DESC td; memset(&td, 0, sizeof(td));
                td.Width = 64; td.Height = 64; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (SUCCEEDED(g_d11->CreateTexture2D(&td, NULL, &st)) && st) {
                    D3D11_BOX box = {g_bbW / 4 - 32, g_bbH / 2 - 32, 0, g_bbW / 4 + 32, g_bbH / 2 + 32, 1};
                    g_ctx->CopySubresourceRegion(st, 0, 0, 0, 0, g_ring11[idx], 0, &box);
                    D3D11_MAPPED_SUBRESOURCE mr;
                    if (SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &mr))) {
                        double sum = 0; int nz = 0;
                        for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
                            BYTE* p = (BYTE*)mr.pData + y * mr.RowPitch + x * 4; int v = p[0] + p[1] + p[2]; sum += v; if (v) nz++;
                        }
                        g_ctx->Unmap(st, 0);
                        Log("DXVK shared-texture check: average brightness %.1f/255, %d of 4096 pixels non-black", sum / (3 * 4096.0), nz);
                    }
                    st->Release();
                }
            }
        }
        LeaveCriticalSection(&g_vrCs);
        SetEvent(g_consumeEvent);
    }
    return 0;
}

// DXVK mode, render thread: publish the newest frame whose copy the GPU has finished, so it is already on
// DXVK's queue before SteamVR's copy is queued behind it.
static void VkPollPublish() {
    if (!g_vk || !g_ringOK) return;
    int best = -1;
    for (int i = 0; i < VKR; i++) {
        if (!g_vkPending[i] || !g_vkQ[i]) continue;
        if (g_vkQ[i]->GetData(NULL, 0, 0) != S_OK) continue;
        g_vkPending[i] = 0;
        if (best < 0 || g_vkSeq[i] - g_vkSeq[best] > 0) best = i;
    }
    if (best >= 0 && (g_pubIdx < 0 || g_vkSeq[best] - g_vkSeq[g_pubIdx] > 0)) {
        EnterCriticalSection(&g_pubCs);
        g_pubPose = g_vkPose[best]; g_pubPoseE[0] = g_vkPoseE[best][0]; g_pubPoseE[1] = g_vkPoseE[best][1];
        InterlockedExchange(&g_pubIdx, best);
        InterlockedIncrement(&g_pubSerial);
        LeaveCriticalSection(&g_pubCs);
    }
}

static void OnPresentVR(IDirect3DDevice9* dev, DWORD now);
// Tell the game its window is active and has the keyboard, exactly as a click on it would.
static void NudgeGameFocus(const char* why) {
    if (!g_gameHwnd || !IsWindow(g_gameHwnd)) return;
    PostMessageW(g_gameHwnd, WM_ACTIVATEAPP, TRUE, 0);
    PostMessageW(g_gameHwnd, WM_NCACTIVATE, TRUE, 0);
    PostMessageW(g_gameHwnd, WM_ACTIVATE, WA_CLICKACTIVE, 0);
    PostMessageW(g_gameHwnd, WM_SETFOCUS, 0, 0);
    Log("Focus: told the game its window is active (%s)", why);
}
// ---- keep the game in front: the game shows its pause menu when another program takes the foreground ----
static void CheckSteamOverlay(DWORD now) {
    static DWORD start; static int reported = -1;
    if (!start) start = now;
    if (now - start < 15000) return;   // the overlay loads during start-up
    if (!g_vrOK || !g_ovrDll) return;
    typedef void* (*GetIf_t)(const char*, vr::EVRInitError*);
    static GetIf_t gi = (GetIf_t)GetProcAddress(g_ovrDll, "VR_GetGenericInterface");
    vr::EVRInitError e; vr::IVRApplications* apps = gi ? (vr::IVRApplications*)gi(vr::IVRApplications_Version, &e) : NULL;
    if (!apps) return;
    char key[64]; _snprintf(key, 64, "steam.overlay.%s", "49540");
    int on = apps->GetApplicationProcessId(key) == GetCurrentProcessId() ? 1 : 0;
    if (on == reported) return;
    reported = on;
    WritePrivateProfileStringA("Launcher", "SteamOverlaySeen", on ? "1" : "0", g_ini);
    if (on) Log("Steam overlay has connected to SteamVR from the game: it shows its own panel in the headset and makes the game pause. Turn it off: Steam > game Properties > General > 'Enable the Steam Overlay while in-game'.");
    else Log("Steam overlay: not connected to SteamVR (good)");
}
static void WatchForeground(DWORD now) {
    static DWORD last; static HWND lastFg; static int locked;
    if (now - last < 250) return;
    last = now;
    HWND fg = o_GetForegroundWindow ? o_GetForegroundWindow() : GetForegroundWindow();
    DWORD pid = 0; if (fg) GetWindowThreadProcessId(fg, &pid);
    bool ours = pid == GetCurrentProcessId();
    if (ours && g_cfg.keepForeground && !locked) {
        // only the program in front may do this: stop other programs pulling their windows to the front
        locked = LockSetForegroundWindow(LSFW_LOCK) ? 1 : -1;
        Log("Focus: %s other programs from taking the foreground", locked > 0 ? "now blocking" : "could not block");
    }
    if (fg != lastFg) {
        lastFg = fg;
        char cls[64] = "", title[80] = "", exe[MAX_PATH] = "?";
        if (fg) { GetClassNameA(fg, cls, 64); GetWindowTextA(fg, title, 80); }
        HANDLE ph = pid ? OpenProcess(0x1000 /*PROCESS_QUERY_LIMITED_INFORMATION*/, FALSE, pid) : NULL;
        if (ph) {
            typedef BOOL(WINAPI * QFPIN_t)(HANDLE, DWORD, LPSTR, PDWORD);
            static QFPIN_t q = (QFPIN_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "QueryFullProcessImageNameA");
            DWORD n = MAX_PATH; if (q) q(ph, 0, exe, &n);
            CloseHandle(ph);
        }
        const char* bn = strrchr(exe, '\\'); bn = bn ? bn + 1 : exe;
        Log("Focus: foreground is now %s - '%s' [%s] (%s)", ours ? "the game" : "ANOTHER PROGRAM", title, cls, bn);
    }
}
static void OnPresent(IDirect3DDevice9* dev) {
    PollHotkeys();
    DWORD now = GetTickCount();
    CheckSteamOverlay(now);
    if (g_cfg.keepForeground || g_cfg.keepFocus) WatchForeground(now);
    if (!g_vrOK) {
        if (now - g_vrRetryTick < 5000 && g_vrRetryTick) return;
        g_vrRetryTick = now ? now : 1;
        InitVR();
        if (!g_vrOK) return;
        static int started;
        if (!started) {
            started = 1; g_lastPresentTick = now;
            g_consumeEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
            CreateThread(NULL, 0, WatchdogThread, NULL, 0, NULL);
            CreateThread(NULL, 0, VrSubmitThread, NULL, 0, NULL);
            CreateThread(NULL, 0, ThreadMonitor, NULL, 0, NULL);
        }
    }
    EnterCriticalSection(&g_vrCs);
    OnPresentVR(dev, now);
    UpdateMessagePanel();
    UpdateFpsPanel();
    g_lastPresentTick = GetTickCount();
    LeaveCriticalSection(&g_vrCs);
    if (!AsyncActive()) return;
    double tw = QpcMs();
    if (g_cfg.frameCap && g_refreshMs > 0) {
        // Soft cap: limit to FrameCapRatio x the headset rate. A hard 1:1 cap leaves no slack - every slow
        // frame misses its slot - so we keep some headroom and still skip most of the wasted frames.
        static double lastEnd;
        static HANDLE timer = (HANDLE)-1;
        if (timer == (HANDLE)-1) timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
        double target = lastEnd + g_refreshMs / g_cfg.frameCapRatio, left = target - tw;
        if (g_vk && left > 0 && left < 30) {
            // poll for finished frames while we wait, so they reach the headset sooner
            for (double t = QpcMs(); t < target; t = QpcMs()) {
                if (TryEnterCriticalSection(&g_vrCs)) { VkPollPublish(); LeaveCriticalSection(&g_vrCs); }
                double r = target - t;
                if (r > 1.2 && timer) { LARGE_INTEGER due; due.QuadPart = -5000; SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE); WaitForSingleObject(timer, 5); }
                else YieldProcessor();
            }
        } else {
            if (left > 1.5) {
                if (timer) { LARGE_INTEGER due; due.QuadPart = -(LONGLONG)((left - 1.0) * 10000.0); SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE); WaitForSingleObject(timer, 50); }
                else Sleep((DWORD)(left - 1.0));
            }
            if (left > 0 && left < 30) while (QpcMs() < target) YieldProcessor();
        }
        lastEnd = QpcMs();
        if (!g_asyncScreen) for (int i = 0; i < 3 && g_pubSerial - g_consumedSerial > 1; i++) WaitForSingleObject(g_consumeEvent, 8);
    } else if (!g_asyncScreen) {
        // no cap: just don't run more than two frames ahead of the headset
        for (int i = 0; i < 3 && g_pubSerial - g_consumedSerial > 1; i++) WaitForSingleObject(g_consumeEvent, 8);
    }
    g_prt.wait += QpcMs() - tw;
}
static void OnPresentVR(IDirect3DDevice9* dev, DWORD now) {
    if (!EnsureResources(dev)) return;
    static UINT cropW, cropH;
    if (cropW != g_bbW || cropH != g_bbH) { cropW = g_bbW; cropH = g_bbH; UpdateOverlayCrop(); }

    int tag = PopEye();                    // 3 = side-by-side stereo frame, 2 = flat frame, -1 = unknown
    if (tag == 2) g_lastStereoTick = 0;
    bool stereoFrame = tag == 3 && g_stereoOn && !g_cfg.screenOnly;
    if (stereoFrame) g_lastStereoTick = now;
    bool screenMode = !g_stereoOn || g_cfg.screenOnly || !g_lastStereoTick || (now - g_lastStereoTick) > 400;

    if (AsyncActive() && g_vk) {
        g_renderThreadId = GetCurrentThreadId();
        InterlockedExchange(&g_asyncScreen, screenMode ? 1 : 0);
        static int lastStereo = -1;
        if (lastStereo != (int)stereoFrame) { lastStereo = stereoFrame; LogAddressSpace(stereoFrame ? "(entering 3D view)" : "(screen mode)"); }
        VkPollPublish();
        if (stereoFrame) {
            double tc = QpcMs();
            int idx = -1;
            for (int k = 0; k < VKR; k++) {
                int i = (g_ringNext + k) % VKR;
                if (i != g_pubIdx && !g_vkPending[i]) { idx = i; break; }
            }
            IDirect3DSurface9* bb = NULL;
            if (!VkEnsureEyeTextures(dev)) idx = -1;
            if (idx >= 0 && SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
                g_ringNext = (idx + 1) % VKR;
                if (g_vkTrace > 0) Log("vk trace: render thread copying into slot %d (src %d)", idx, g_vkSrc[idx]);
                if (g_vkSrc[idx]) {
                    for (int e = 0; e < 2; e++) g_vkDev->TransitionTextureLayout(g_vkItex[idx * 2 + e], &kVkColor0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_vkLayout[idx * 2 + e]);
                    g_vkSrc[idx] = 0;
                }
                HRESULT hr = S_OK;
                TaaFrame(dev, bb);
                for (int e = 0; e < 2 && SUCCEEDED(hr); e++) {
                    if (g_taaOut[e]) {   // TAA result -> RT staging -> plain texture: the same last hop as the proven path
                        hr = dev->StretchRect(g_taaOut[e], NULL, g_vkMidSurf[e], NULL, D3DTEXF_NONE);
                        if (SUCCEEDED(hr)) hr = dev->StretchRect(g_vkMidSurf[e], NULL, g_vkSurf9[idx * 2 + e], NULL, D3DTEXF_NONE);
                    }
                    else if (g_cfg.vkPlainEyes) {   // back buffer -> RT staging (rect copy) -> plain texture (whole-image copy)
                        hr = dev->StretchRect(bb, &g_vkSrcRect[e], g_vkMidSurf[e], NULL, D3DTEXF_NONE);
                        if (SUCCEEDED(hr)) hr = dev->StretchRect(g_vkMidSurf[e], NULL, g_vkSurf9[idx * 2 + e], NULL, D3DTEXF_NONE);
                    } else hr = dev->StretchRect(bb, &g_vkSrcRect[e], g_vkSurf9[idx * 2 + e], NULL, D3DTEXF_NONE);
                    static int errs;
                    if (FAILED(hr) && errs++ < 5) Log("DXVK: eye %d copy failed 0x%08lX", e, (unsigned long)hr);
                }
                bb->Release();
                if (SUCCEEDED(hr)) {
                    if (g_captureReq > 0) {
                        CaptureSurface(dev, g_taaOut[0] ? g_taaOut[0] : g_cfg.vkPlainEyes ? g_vkMidSurf[0] : g_vkSurf9[idx * 2], "eyeL");
                        CaptureSurface(dev, g_taaOut[1] ? g_taaOut[1] : g_cfg.vkPlainEyes ? g_vkMidSurf[1] : g_vkSurf9[idx * 2 + 1], "eyeR");
                        InterlockedDecrement(&g_captureReq);
                    }
                    for (int e = 0; e < 2; e++) {
                        VkImage im = VK_NULL_HANDLE;
                        if (SUCCEEDED(g_vkItex[idx * 2 + e]->GetVulkanImageInfo(&im, NULL, NULL)) && im && im != g_vkImg[idx * 2 + e]) {
                            static int moved; if (moved++ < 5) Log("DXVK moved eye image %d - handle refreshed", idx * 2 + e);
                            g_vkImg[idx * 2 + e] = im;
                        }
                        g_vkDev->TransitionTextureLayout(g_vkItex[idx * 2 + e], &kVkColor0, g_vkLayout[idx * 2 + e], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    }
                    g_vkSrc[idx] = 1;
                    g_vkQ[idx]->Issue(D3DISSUE_END);
                    g_vkQ[idx]->GetData(NULL, 0, D3DGETDATA_FLUSH);
                    g_vkPoseE[idx][0] = g_vkPoseE[idx][1] = g_poppedPose;
                    g_vkPose[idx] = g_poppedPose; g_vkSeq[idx] = ++g_vkSeqNext; g_vkPending[idx] = 1;
                }
            }
            g_prt.copy += QpcMs() - tc;
            if (g_overlayShown) { g_ovl->HideOverlay(g_overlay); g_overlayShown = 0; }
        } else if (screenMode) {
            CopyBackbuffer(dev, g_screenTex, true);
            if (g_overlay != vr::k_ulOverlayHandleInvalid) {
                vr::Texture_t t = {(void*)g_screenTex, vr::TextureType_DirectX, vr::ColorSpace_Auto};
                g_ovl->SetOverlayTexture(g_overlay, &t);
                if (!g_overlayShown) { g_ovl->ShowOverlay(g_overlay); g_overlayShown = 1; }
            }
        }
        return;   // the VR thread submits and paces
    }
    if (AsyncActive()) {
        InterlockedExchange(&g_asyncScreen, screenMode ? 1 : 0);
        if (stereoFrame) {
            double tc = QpcMs();
            int idx = g_ringNext; g_ringNext = (g_ringNext + 1) % 3;
            IDirect3DSurface9* bb = NULL;
            if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
                HRESULT hr = dev->StretchRect(bb, NULL, g_ringSurf9[idx], NULL, D3DTEXF_NONE);
                bb->Release();
                if (SUCCEEDED(hr)) {
                    if (g_evQuery) {
                        g_evQuery->Issue(D3DISSUE_END); g_evQuery->GetData(NULL, 0, D3DGETDATA_FLUSH);
                        // DXVK's shared textures have no implicit cross-API sync: make sure the copy is done before SteamVR reads it
                        if (g_usingDXVK) for (int spin = 0; g_evQuery->GetData(NULL, 0, D3DGETDATA_FLUSH) == S_FALSE && spin < 400000; spin++) YieldProcessor();
                    }
                    if (g_captureReq > 0) { CaptureSurface(dev, g_ringSurf9[idx], "stereo"); InterlockedDecrement(&g_captureReq); }
                    EnterCriticalSection(&g_pubCs);
                    g_pubPose = g_poppedPose;
                    InterlockedExchange(&g_pubIdx, idx);
                    InterlockedIncrement(&g_pubSerial);
                    LeaveCriticalSection(&g_pubCs);
                }
            }
            g_prt.copy += QpcMs() - tc;
            if (g_overlayShown) { g_ovl->HideOverlay(g_overlay); g_overlayShown = 0; }
        } else if (screenMode) {
            CopyBackbuffer(dev, g_screenTex, true);
            if (g_overlay != vr::k_ulOverlayHandleInvalid) {
                vr::Texture_t t = {(void*)g_screenTex, vr::TextureType_DirectX, vr::ColorSpace_Auto};
                g_ovl->SetOverlayTexture(g_overlay, &t);
                if (!g_overlayShown) { g_ovl->ShowOverlay(g_overlay); g_overlayShown = 1; }
            }
        }
        return;   // the VR thread submits and paces
    }

    if (stereoFrame) {
        LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0);
        if (CopyBackbuffer(dev, g_eyeTex[0], false)) g_eyeHas[0] = g_eyeHas[1] = 1;
        QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
        g_prt.copy += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
        static double copyMs, sumFrame; static int n; static LARGE_INTEGER last;
        copyMs += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
        if (last.QuadPart) sumFrame += (double)(t0.QuadPart - last.QuadPart) * 1000.0 / (double)fq.QuadPart;
        last = t0;
        if (++n == 600) {
            n = 0; copyMs = 0; sumFrame = 0;
        }
    } else if (screenMode) {
        CopyBackbuffer(dev, g_screenTex, true);
    } else {
        return;   // a stray flat frame in the middle of stereo: don't submit or wait (keeps pacing)
    }

    if (screenMode) {
        // keep the compositor fed (otherwise SteamVR pauses/throttles the game): black eyes + screen overlay
        SubmitBlack();
        if (g_overlay != vr::k_ulOverlayHandleInvalid) {
            vr::Texture_t t = {(void*)g_screenTex, vr::TextureType_DirectX, vr::ColorSpace_Auto};
            g_ovl->SetOverlayTexture(g_overlay, &t);
            if (!g_overlayShown) { g_ovl->ShowOverlay(g_overlay); g_overlayShown = 1; }
        }
    } else {
        if (g_overlayShown) { g_ovl->HideOverlay(g_overlay); g_overlayShown = 0; }
        if (stereoFrame && g_eyeHas[0]) {
            float T = g_gameTanH, a = 0.5f * (float)g_bbW / (float)g_bbH, Tv = T / a;
            bool swap = (g_cfg.swapEyes != 0) ^ (g_swapHotkey != 0);
            for (int e = 0; e < 2; e++) {
                int half = swap ? 1 - e : e;   // which half of the image holds this eye
                vr::VRTextureBounds_t bnd;
                if (g_offAxisActive) {   // each half already covers exactly the eye's horizontal range
                    float U = g_offAxisUp, D = g_offAxisDn;
                    bnd.uMin = 0.5f * half; bnd.uMax = 0.5f * half + 0.5f;
                    bnd.vMin = (U - g_projRaw[e][3]) / (U + D);
                    bnd.vMax = (U - g_projRaw[e][2]) / (U + D);
                } else {
                    bnd.uMin = 0.5f * half + 0.5f * (0.5f + g_projRaw[e][0] / (2 * T));
                    bnd.uMax = 0.5f * half + 0.5f * (0.5f + g_projRaw[e][1] / (2 * T));
                    bnd.vMin = 0.5f - g_projRaw[e][3] / (2 * Tv);
                    bnd.vMax = 0.5f - g_projRaw[e][2] / (2 * Tv);
                }
                vr::Texture_t t = {(void*)g_eyeTex[0], vr::TextureType_DirectX, vr::ColorSpace_Auto};
                double ts = QpcMs();
                vr::EVRCompositorError ce = g_comp->Submit((vr::EVREye)e, &t, &bnd, vr::Submit_Default);
                g_prt.submit += QpcMs() - ts;
                static int errs;
                if (ce != vr::VRCompositorError_None && errs++ < 10) Log("Submit eye %d error %d", e, (int)ce);
            }
        }
    }

    WaitAndUpdatePoses();
}

// ---------------------------------------------------------------------------
// D3D9 hooks
// ---------------------------------------------------------------------------
// GPU timestamps: from just after one Present to just before the next = GPU time of the game's frame
struct GpuQ { IDirect3DQuery9 *dis, *freq, *t0, *t1; int pending; };
static GpuQ g_gq[4]; static int g_gqIdx; static int g_gqOK = -1;
static void GpuQueriesEnd(IDirect3DDevice9* dev) {   // called before Present
    if (g_gqOK <= 0) return;
    GpuQ& q = g_gq[g_gqIdx];
    if (q.pending == 1) { q.t1->Issue(D3DISSUE_END); q.dis->Issue(D3DISSUE_END); q.freq->Issue(D3DISSUE_END); q.pending = 2; }
    // collect any finished older frame
    for (int k = 0; k < 4; k++) {
        GpuQ& o = g_gq[k];
        if (o.pending != 2) continue;
        BOOL dj = TRUE; UINT64 fr = 0, a = 0, b = 0;
        if (o.dis->GetData(&dj, sizeof(dj), 0) != S_OK || o.freq->GetData(&fr, sizeof(fr), 0) != S_OK ||
            o.t0->GetData(&a, sizeof(a), 0) != S_OK || o.t1->GetData(&b, sizeof(b), 0) != S_OK) continue;
        o.pending = 0;
        if (!dj && fr && b > a) { double g = (double)(b - a) * 1000.0 / (double)fr; g_prt.gpu += g; g_prt.gpuN++; if (g > g_prt.gpuMax) g_prt.gpuMax = g;
            g_gpuEma = g_gpuEma < 0 ? (float)g : (float)(g_gpuEma * 0.92 + g * 0.08); }
    }
}
static void GpuQueriesBegin(IDirect3DDevice9* dev) {  // called after Present
    if (g_gqOK < 0) {
        g_gqOK = 1;
        for (int k = 0; k < 4; k++)
            if (FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &g_gq[k].dis)) || FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &g_gq[k].freq)) ||
                FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g_gq[k].t0)) || FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g_gq[k].t1))) { g_gqOK = 0; Log("GPU timestamp queries unavailable"); break; }
    }
    if (g_gqOK <= 0) return;
    g_gqIdx = (g_gqIdx + 1) & 3;
    GpuQ& q = g_gq[g_gqIdx];
    if (q.pending) return;   // still waiting for an old result: skip this frame
    q.dis->Issue(D3DISSUE_BEGIN); q.t0->Issue(D3DISSUE_END); q.pending = 1;
}
static void ProfileReport() {
    ProfRT& p = g_prt;
    double now = QpcMs(), cpu = ThreadCpuMs();
    double rtCpu = p.wall0 > 0 ? 100.0 * (cpu - p.cpu0) / (now - p.wall0) : 0;
    double n = p.n ? p.n : 1;
    char line[400];
    _snprintf(line, sizeof(line),
        "PROFILE %.1f fps (%.2f ms/frame) | GPU %.2f ms | render thread: game drawing %.2f, our copy+GPU wait %.2f, submit %.2f, "
        "frame-cap/SteamVR wait %.2f, Present %.2f ms, CPU %.0f%% | game thread: frame %.2f ms, scene setup %.2f ms, CPU %.0f%%",
        1000.0 / (p.frame / n), p.frame / n, p.gpuN ? p.gpu / p.gpuN : -1.0, p.gameRender / n, p.copy / n, p.submit / n, p.wait / n,
        p.present / n, rtCpu, (double)g_gtFrame, (double)g_gtCsv, (double)g_gtCpu);
    line[sizeof(line) - 1] = 0;
    Log("%s", line);
    if (AsyncActive()) {
        LONG subs = InterlockedExchange(&g_vrSubmits, 0), reps = InterlockedExchange(&g_vrRepeats, 0);
        if (subs > 0) Log("  headset: %ld refreshes, %ld (%.1f%%) reused the previous frame", subs, reps, 100.0 * reps / subs);
    }
    if (g_ftN > 10) {   // frame-time distribution (game frames)
        float v[1024]; int m = g_ftN; memcpy(v, g_ft, m * sizeof(float));
        for (int i = 1; i < m; i++) { float x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
        int over11 = 0, over16 = 0;
        for (int i = 0; i < m; i++) { if (v[i] > 11.11f) over11++; if (v[i] > 16.7f) over16++; }
        Log("  dynamic resolution: ScreenPercentage %d, GPU (smoothed) %.2f ms", (int)g_screenPct, (double)g_gpuEma);
        Log("  frame times: median %.2f, 95%% %.2f, 99%% %.2f, worst %.2f ms | %d of %d frames over 11.1 ms, %d over 16.7 | worst GPU %.2f, worst draw %.2f ms",
            v[m / 2], v[m * 95 / 100], v[m * 99 / 100], v[m - 1], over11, m, over16, p.gpuMax, p.drawMax);
    }
    g_ftN = 0;
    {
        EnterCriticalSection(&g_vtCs);
        VrTiming t = g_vt; memset(&g_vt, 0, sizeof(g_vt));
        LeaveCriticalSection(&g_vtCs);
        if (t.n > 0)
            Log("  SteamVR: %d frames | app GPU %.2f avg / %.2f worst ms, compositor GPU %.2f | reused %d (cpu-late %d, gpu-late %d), "
                "dropped %d, mispresented %d, motion smoothing %d | frame ready %.2f ms after vsync, interval %.2f avg / %.2f worst",
                t.n, t.appGpu / t.n, t.appGpuMax, t.compGpu / t.n, t.reused, t.cpuRepro, t.gpuRepro, t.dropped, t.mispresented, t.motion,
                t.ready / t.n, t.interval / t.n, t.intervalMax);
    }
    if (g_perfOverlay)
        ShowMessage(4000, "%.0f fps  GPU %.1f ms\nrender %.1f+%.1f wait %.1f | game %.1f ms CPU %.0f%%",
                    1000.0 / (p.frame / n), p.gpuN ? p.gpu / p.gpuN : -1.0, p.gameRender / n, p.copy / n, p.wait / n,
                    (double)g_gtFrame, (double)g_gtCpu);
    double keepEnd = p.lastEnd;
    memset(&p, 0, sizeof(p));
    p.lastEnd = keepEnd; p.cpu0 = cpu; p.wall0 = now;
}
static HRESULT STDMETHODCALLTYPE hkPresent(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    if (InterlockedExchange(&g_inPresent, 1) == 0) {
        double t0 = QpcMs();
        if (g_prt.lastEnd > 0) { double d = t0 - g_prt.lastEnd; g_prt.gameRender += d; if (d > g_prt.drawMax) g_prt.drawMax = d;
            g_drawEma = g_drawEma < 0 ? (float)d : (float)(g_drawEma * 0.92 + d * 0.08); }
        GpuQueriesEnd(dev);
        OnPresent(dev);
        double t1 = QpcMs();
        HRESULT hr = o_Present(dev, a, b, w, r);
        double t2 = QpcMs();
        GpuQueriesBegin(dev);
        static double lastStart;
        if (lastStart > 0) { g_prt.frame += t0 - lastStart; g_prt.present += t2 - t1; g_prt.n++; if (g_ftN < 1024) g_ft[g_ftN++] = (float)(t0 - lastStart); }
        lastStart = t0;
        g_prt.lastEnd = QpcMs();
        if (g_prt.n >= (g_perfOverlay ? 90 : 600)) ProfileReport();
        InterlockedExchange(&g_inPresent, 0);
        return hr;
    }
    return o_Present(dev, a, b, w, r);
}
static HRESULT STDMETHODCALLTYPE hkSCPresent(IDirect3DSwapChain9* sc, const RECT* a, const RECT* b, HWND w, const RGNDATA* r, DWORD f) {
    if (InterlockedExchange(&g_inPresent, 1) == 0) {
        IDirect3DDevice9* dev = NULL;
        if (SUCCEEDED(sc->GetDevice(&dev)) && dev) { OnPresent(dev); dev->Release(); }
        HRESULT hr = o_SCPresent(sc, a, b, w, r, f);
        InterlockedExchange(&g_inPresent, 0);
        return hr;
    }
    return o_SCPresent(sc, a, b, w, r, f);
}
static HRESULT STDMETHODCALLTYPE hkReset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    EnterCriticalSection(&g_vrCs);   // the VR thread must not be using our textures while they go
    ReleaseD3D9Copies();
    LeaveCriticalSection(&g_vrCs);
    g_bbW = g_bbH = 0;
    if (pp && g_cfg.forceNoVsync && g_vrOK) pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    HRESULT hr = o_Reset(dev, pp);
    Log("Device Reset -> 0x%08lX (%ux%u windowed=%d)", (unsigned long)hr, pp ? pp->BackBufferWidth : 0, pp ? pp->BackBufferHeight : 0, pp ? pp->Windowed : -1);
    return hr;
}
// D3D9Ex has no MANAGED pool: create those resources in DEFAULT (+DYNAMIC so they stay lockable)
typedef HRESULT(STDMETHODCALLTYPE* CreateTexture_t)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* CreateVolTex_t)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* CreateCubeTex_t)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* CreateVB_t)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* CreateIB_t)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);
static CreateTexture_t o_CreateTexture; static CreateVolTex_t o_CreateVolTex; static CreateCubeTex_t o_CreateCubeTex;
static CreateVB_t o_CreateVB; static CreateIB_t o_CreateIB;
static inline void FixPool(D3DPOOL& p, DWORD& u) {
    if (p == D3DPOOL_MANAGED) { p = D3DPOOL_DEFAULT; if (!(u & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))) u |= D3DUSAGE_DYNAMIC; }
}
static HRESULT STDMETHODCALLTYPE hkCreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9** t, HANDLE* s) {
    FixPool(p, u); return o_CreateTexture(d, w, h, l, u, f, p, t, s);
}
static HRESULT STDMETHODCALLTYPE hkCreateVolTex(IDirect3DDevice9* d, UINT w, UINT h, UINT dp, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DVolumeTexture9** t, HANDLE* s) {
    FixPool(p, u); return o_CreateVolTex(d, w, h, dp, l, u, f, p, t, s);
}
static HRESULT STDMETHODCALLTYPE hkCreateCubeTex(IDirect3DDevice9* d, UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DCubeTexture9** t, HANDLE* s) {
    FixPool(p, u); return o_CreateCubeTex(d, e, l, u, f, p, t, s);
}
static HRESULT STDMETHODCALLTYPE hkCreateVB(IDirect3DDevice9* d, UINT len, DWORD u, DWORD fvf, D3DPOOL p, IDirect3DVertexBuffer9** b, HANDLE* s) {
    FixPool(p, u); return o_CreateVB(d, len, u, fvf, p, b, s);
}
static HRESULT STDMETHODCALLTYPE hkCreateIB(IDirect3DDevice9* d, UINT len, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DIndexBuffer9** b, HANDLE* s) {
    FixPool(p, u); return o_CreateIB(d, len, u, f, p, b, s);
}

static HRESULT STDMETHODCALLTYPE hkCreateDevice(IDirect3D9* d3d, UINT ad, D3DDEVTYPE type, HWND wnd, DWORD flags,
                                                D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    if (pp && g_cfg.forceNoVsync) pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    {   // the window the game renders into is the one that must look focused
        HWND gw = (pp && pp->hDeviceWindow) ? pp->hDeviceWindow : wnd;
        while (gw && (GetWindowLongW(gw, GWL_STYLE) & WS_CHILD)) gw = GetParent(gw);
        if (gw) { g_gameHwnd = gw; char t[64] = ""; GetWindowTextA(gw, t, 64); Log("Focus: game window is '%s'", t); }
    }
    HRESULT hr = E_FAIL;
    g_devIsEx = 0;
    if (g_isEx && pp && pp->Windowed) {
        IDirect3DDevice9Ex* dx = NULL;
        hr = ((IDirect3D9Ex*)d3d)->CreateDeviceEx(ad, type, wnd, flags, pp, NULL, &dx);
        if (SUCCEEDED(hr) && dx) { *out = dx; g_devIsEx = 1; Log("Created D3D9Ex device (fast copy)"); }
        else Log("CreateDeviceEx failed 0x%08lX - falling back", (unsigned long)hr);
    }
    if (!g_devIsEx) hr = o_CreateDevice(d3d, ad, type, wnd, flags, pp, out);
    Log("CreateDevice -> 0x%08lX (%ux%u windowed=%d flags 0x%lX)", (unsigned long)hr, pp ? pp->BackBufferWidth : 0,
        pp ? pp->BackBufferHeight : 0, pp ? pp->Windowed : -1, (unsigned long)flags);
    if (SUCCEEDED(hr) && out && *out) {
        void** vt = *(void***)*out;
        if (vt[17] != (void*)hkPresent) o_Present = (Present_t)PatchPtr(&vt[17], (void*)hkPresent);
        if (vt[16] != (void*)hkReset) o_Reset = (Reset_t)PatchPtr(&vt[16], (void*)hkReset);
        if (g_devIsEx) {
            if (vt[23] != (void*)hkCreateTexture) o_CreateTexture = (CreateTexture_t)PatchPtr(&vt[23], (void*)hkCreateTexture);
            if (vt[24] != (void*)hkCreateVolTex) o_CreateVolTex = (CreateVolTex_t)PatchPtr(&vt[24], (void*)hkCreateVolTex);
            if (vt[25] != (void*)hkCreateCubeTex) o_CreateCubeTex = (CreateCubeTex_t)PatchPtr(&vt[25], (void*)hkCreateCubeTex);
            if (vt[26] != (void*)hkCreateVB) o_CreateVB = (CreateVB_t)PatchPtr(&vt[26], (void*)hkCreateVB);
            if (vt[27] != (void*)hkCreateIB) o_CreateIB = (CreateIB_t)PatchPtr(&vt[27], (void*)hkCreateIB);
        }
        IDirect3DSwapChain9* sc = NULL;
        if (SUCCEEDED((*out)->GetSwapChain(0, &sc)) && sc) {
            void** svt = *(void***)sc;
            if (svt[3] != (void*)hkSCPresent) o_SCPresent = (SCPresent_t)PatchPtr(&svt[3], (void*)hkSCPresent);
            sc->Release();
        }
    }
    return hr;
}

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------
extern "C" {
IDirect3D9* WINAPI Direct3DCreate9(UINT sdk) {
    if (!EnsureRealD3D9()) return NULL;
    IDirect3D9* d = NULL;
    if (g_cfg.enabled && g_cfg.fastCopy && p_Create9Ex) {
        IDirect3D9Ex* ex = NULL;
        if (SUCCEEDED(p_Create9Ex(sdk, &ex)) && ex) { d = ex; g_isEx = 1; }
    }
    if (!d) d = p_Create9(sdk);
    if (d && g_cfg.enabled) {
        void** vt = *(void***)d;
        if (vt[16] != (void*)hkCreateDevice) o_CreateDevice = (CreateDevice_t)PatchPtr(&vt[16], (void*)hkCreateDevice);
    }
    return d;
}
HRESULT WINAPI Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out) {
    if (!EnsureRealD3D9() || !p_Create9Ex) return E_FAIL;
    return p_Create9Ex(sdk, out);
}
int WINAPI D3DPERF_BeginEvent(D3DCOLOR c, LPCWSTR n) { return (EnsureRealD3D9() && p_BeginEvent) ? p_BeginEvent(c, n) : 0; }
int WINAPI D3DPERF_EndEvent(void) { return (EnsureRealD3D9() && p_EndEvent) ? p_EndEvent() : 0; }
void WINAPI D3DPERF_SetMarker(D3DCOLOR c, LPCWSTR n) { if (EnsureRealD3D9() && p_SetMarker) p_SetMarker(c, n); }
void WINAPI D3DPERF_SetRegion(D3DCOLOR c, LPCWSTR n) { if (EnsureRealD3D9() && p_SetRegion) p_SetRegion(c, n); }
BOOL WINAPI D3DPERF_QueryRepeatFrame(void) { return (EnsureRealD3D9() && p_QueryRepeatFrame) ? p_QueryRepeatFrame() : FALSE; }
void WINAPI D3DPERF_SetOptions(DWORD o) { if (EnsureRealD3D9() && p_SetOptions) p_SetOptions(o); }
DWORD WINAPI D3DPERF_GetStatus(void) { return (EnsureRealD3D9() && p_GetStatus) ? p_GetStatus() : 0; }

// Loaded by the launcher instead of as the game's d3d9.dll: point the game's Direct3D 9 imports at our functions.
static int PatchGameImport(const char* dll, const char* fn, void* repl) {
    BYTE* base = (BYTE*)g_base;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd.VirtualAddress) return 0;
    int n = 0;
    for (IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dd.VirtualAddress); imp->Name; imp++) {
        if (lstrcmpiA((const char*)(base + imp->Name), dll)) continue;
        IMAGE_THUNK_DATA32* oft = (IMAGE_THUNK_DATA32*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA32* ft = (IMAGE_THUNK_DATA32*)(base + imp->FirstThunk);
        for (; oft->u1.Ordinal; oft++, ft++) {
            if (IMAGE_SNAP_BY_ORDINAL32(oft->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + oft->u1.AddressOfData);
            if (lstrcmpA((const char*)ibn->Name, fn)) continue;
            PatchPtr((void**)&ft->u1.Function, repl); n++;
        }
    }
    return n;
}
extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT);
extern "C" int WINAPI D3DPERF_BeginEvent(D3DCOLOR, LPCWSTR);
extern "C" int WINAPI D3DPERF_EndEvent(void);
extern "C" void WINAPI D3DPERF_SetOptions(DWORD);
static int g_injected;
static bool SetupInjected() {   // returns false if the mod should stay passive
    char self[MAX_PATH]; GetModuleFileNameA(g_self, self, MAX_PATH);
    const char* bn = strrchr(self, '\\'); bn = bn ? bn + 1 : self;
    if (!lstrcmpiA(bn, "d3d9.dll")) return true;   // classic install: we ARE the game's d3d9.dll
    g_injected = 1;
    // an old-style d3d9.dll proxy in the game folder would run its own copy of the mod: then stay out of the way
    HMODULE d = GetModuleHandleA("d3d9.dll");
    char dp[MAX_PATH] = "", ep[MAX_PATH] = "";
    if (d) GetModuleFileNameA(d, dp, MAX_PATH);
    GetModuleFileNameA(NULL, ep, MAX_PATH);
    char* s1 = strrchr(dp, '\\'); char* s2 = strrchr(ep, '\\');
    if (s1 && s2) { *s1 = 0; *s2 = 0; if (!lstrcmpiA(dp, ep)) { Log("Launcher: a d3d9.dll mod is also in the game folder - leaving it in charge (remove it to use the launcher install)"); return false; } }
    int n = PatchGameImport("d3d9.dll", "Direct3DCreate9", (void*)Direct3DCreate9);
    n += PatchGameImport("d3d9.dll", "D3DPERF_BeginEvent", (void*)D3DPERF_BeginEvent);
    n += PatchGameImport("d3d9.dll", "D3DPERF_EndEvent", (void*)D3DPERF_EndEvent);
    n += PatchGameImport("d3d9.dll", "D3DPERF_SetOptions", (void*)D3DPERF_SetOptions);
    Log("Launcher: running from %s, %d Direct3D 9 imports redirected", g_dir, n);
    // keep DXVK's settings and log next to the mod, not in the game folder
    char p[MAX_PATH]; lstrcpyA(p, g_dir); lstrcatA(p, "dxvk.conf");
    if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) SetEnvironmentVariableA("DXVK_CONFIG_FILE", p);
    lstrcpyA(p, g_dir); p[lstrlenA(p) - 1] = 0; SetEnvironmentVariableA("DXVK_LOG_PATH", p);
    return n > 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    g_self = h;
    DisableThreadLibraryCalls(h);
    InitializeCriticalSection(&g_logCs);
    InitializeCriticalSection(&g_poseCs);
    InitializeCriticalSection(&g_vrCs);
    InitializeCriticalSection(&g_msgCs);
    InitializeCriticalSection(&g_pubCs);
    InitializeCriticalSection(&g_vtCs);
    GetModuleFileNameA(h, g_dir, MAX_PATH);
    char* slash = strrchr(g_dir, '\\');
    if (slash) slash[1] = 0;
    char logPath[MAX_PATH];
    lstrcpyA(g_ini, g_dir); lstrcatA(g_ini, "acmvr.ini");
    lstrcpyA(logPath, g_dir); lstrcatA(logPath, "acmvr.log");
    {   // keep the last 10 sessions: acmvr.log -> acmvr.1.log -> ... -> acmvr.9.log
        char from[MAX_PATH], to[MAX_PATH];
        for (int i = 8; i >= 1; i--) {
            _snprintf(from, MAX_PATH, "%sacmvr.%d.log", g_dir, i); _snprintf(to, MAX_PATH, "%sacmvr.%d.log", g_dir, i + 1);
            MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING);
        }
        _snprintf(to, MAX_PATH, "%sacmvr.1.log", g_dir);
        MoveFileExA(logPath, to, MOVEFILE_REPLACE_EXISTING);
    }
    g_log = fopen(logPath, "w");
    AddVectoredExceptionHandler(0, FaultHandler);   // last in line: only logs, never handles
    LoadConfig();
    TaaCrashGuardInit();
    g_heightAdj = IniFloat("VR", "HeightOffset", 0.0f);
    if (g_cfg.gunUpAxis) { int a = g_cfg.gunUpAxis < 0 ? -g_cfg.gunUpAxis : g_cfg.gunUpAxis; if (a >= 1 && a <= 3) { g_gunUpAxis = a - 1; g_gunUpSign = g_cfg.gunUpAxis < 0 ? -1 : 1; } }
    g_worldScale = g_cfg.worldScale;
    g_armsX = g_cfg.armsX; g_armsY = g_cfg.armsY; g_armsZ = g_cfg.armsZ;
    g_corrPitch = g_cfg.corrPitch; g_corrYaw = g_cfg.corrYaw;
    g_gunScale = g_cfg.gunScale;
    g_aimPitch = g_cfg.aimPitchOffset; g_gunX = g_cfg.gunX; g_gunY = g_cfg.gunY; g_gunZ = g_cfg.gunZ;
    for (char* p = g_cfg.headCallers; *p && g_numHeadCallers < 64;) {
        char* end; DWORD v = (DWORD)strtoul(p, &end, 0);
        if (end == p) { p++; continue; }
        g_headCallers[g_numHeadCallers++] = v; p = end;
    }
    g_base = (DWORD)GetModuleHandleA(NULL);
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH);
    Log("ACM-VR %s loaded into %s (base %08lX)", ACMVR_VERSION, exe, (unsigned long)g_base);
    if (!SetupInjected()) return TRUE;
    if (g_cfg.enabled && g_cfg.earlyVr) g_earlyVrThread = CreateThread(NULL, 0, EarlyVrThread, NULL, 0, NULL);   // runs once DllMain returns
    if (!g_cfg.enabled) { Log("Disabled in acmvr.ini - passthrough only"); return TRUE; }
    if (g_cfg.dpiAware) SetProcessDPIAware();
    if (g_cfg.largeWindow) InstallWindowHooks();
    if (g_cfg.gameHooks) InstallGameHooks(); else g_cfg.screenOnly = 1;
    InstallXInputHooks();
    return TRUE;
}
}
