#include <math.h>
#include <stdio.h>
#include <stdlib.h>
namespace vr { struct HmdMatrix34_t { float m[3][4]; }; }
// ---
#include "math.inc"
struct PoseSnap { vr::HmdMatrix34_t hmd; float recYaw; V3 recPos; };
static void PoseToUe(const PoseSnap& ps, const vr::HmdMatrix34_t& m, Basis* b, V3* pos) {
    V3 X = mk(m.m[0][0], m.m[1][0], m.m[2][0]); V3 Y = mk(m.m[0][1], m.m[1][1], m.m[2][1]);
    V3 Z = mk(m.m[0][2], m.m[1][2], m.m[2][2]); V3 P = mk(m.m[0][3], m.m[1][3], m.m[2][3]);
    float a = -ps.recYaw; X = rotYvr(X, a); Y = rotYvr(Y, a); Z = rotYvr(Z, a); P = rotYvr(sub(P, ps.recPos), a);
    b->f = vrToUe(mul(Z, -1.0f)); b->r = vrToUe(X); b->u = vrToUe(Y); *pos = vrToUe(P);
}
static float HmdYawVr(const vr::HmdMatrix34_t& m) { float fx = -m.m[0][2], fz = -m.m[2][2]; return fatan2(-fx, -fz); }
// build VR pose: yaw about +Y (left turn positive in RH), then pitch about +X (up positive)
static vr::HmdMatrix34_t pose(float yawDeg, float pitchDeg, float rollDeg, float px=0,float py=0,float pz=0){
  double y=yawDeg*M_PI/180, p=pitchDeg*M_PI/180, r=rollDeg*M_PI/180;
  double Ry[3][3]={{cos(y),0,sin(y)},{0,1,0},{-sin(y),0,cos(y)}};
  double Rx[3][3]={{1,0,0},{0,cos(p),-sin(p)},{0,sin(p),cos(p)}};
  double Rz[3][3]={{cos(r),-sin(r),0},{sin(r),cos(r),0},{0,0,1}};
  double T[3][3]={0},M[3][3]={0};
  for(int i=0;i<3;i++)for(int j=0;j<3;j++)for(int k=0;k<3;k++)T[i][j]+=Ry[i][k]*Rx[k][j];
  for(int i=0;i<3;i++)for(int j=0;j<3;j++)for(int k=0;k<3;k++)M[i][j]+=T[i][k]*Rz[k][j];
  vr::HmdMatrix34_t m; for(int i=0;i<3;i++){for(int j=0;j<3;j++)m.m[i][j]=M[i][j];} m.m[0][3]=px;m.m[1][3]=py;m.m[2][3]=pz; return m;}
static float deg(int u){return u*360.0f/65536.0f;}
int main(){
  PoseSnap ps={}; ps.recYaw=0; ps.recPos=mk(0,0,0);
  struct {float y,p,r; const char* what;} cases[]={{0,0,0,"identity"},{-90,0,0,"turn head RIGHT 90"},{30,0,0,"turn head LEFT 30"},{0,20,0,"look UP 20"},{0,0,-15,"tilt head right (roll cw)"},{-45,30,0,"right45+up30"}};
  for(auto& c: cases){ ps.hmd=pose(c.y,c.p,c.r); Basis h; V3 hp; PoseToUe(ps,ps.hmd,&h,&hp); FRotator rel=BasisToRot(h);
    FRotator base={0, 16384, 0}; // game facing +Y (90 deg)
    Basis W=toWorldB(RotToBasis(base),h); FRotator v=BasisToRot(W);
    printf("%-26s rel P%7.2f Y%7.2f R%7.2f | world(base yaw 90) P%7.2f Y%7.2f R%7.2f\n",c.what,deg(rel.Pitch),deg(rel.Yaw),deg(rel.Roll),deg(v.Pitch),deg(v.Yaw),deg(v.Roll)); }
  // position: step 1m forward (VR -Z) and 0.5m right (+X); recenter at yaw 0
  ps.hmd=pose(0,0,0, 0.5f,1.7f,-1.0f); ps.recPos=mk(0,1.7f,0); Basis h; V3 hp; PoseToUe(ps,ps.hmd,&h,&hp);
  printf("pos fwd1 right0.5 -> UE (%.2f %.2f %.2f) expect (1, 0.5, 0)\n",hp.x,hp.y,hp.z);
  // recenter while facing left 90: then looking straight should be rel yaw 0
  ps.recPos=mk(0,0,0); ps.hmd=pose(90,0,0); ps.recYaw=HmdYawVr(ps.hmd); PoseToUe(ps,ps.hmd,&h,&hp); FRotator r=BasisToRot(h);
  printf("recentred at left90: rel yaw %.2f (expect 0); recYaw %.2f deg\n",deg(r.Yaw),ps.recYaw*180/M_PI);
  // round trip RotToBasis/BasisToRot
  FRotator q={3000,-12000,1500}; FRotator q2=BasisToRot(RotToBasis(q)); printf("roundtrip %d %d %d -> %d %d %d\n",q.Pitch,q.Yaw,q.Roll,q2.Pitch,q2.Yaw,q2.Roll);
}
