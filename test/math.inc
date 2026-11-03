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
