/*
 * Direct3D 11 render back end for the NV2A pushbuffer executor.
 *
 * The executor (xboxrecomp src/kernel/nv2a_pb_exec.c) decodes the title's GPU
 * work and, by default, rasterises it on the CPU. That is what holds 3D scenes
 * to a few frames per second. Registered with nv2a_backend_register(), this
 * file draws the same triangles on the host GPU instead; the executor still
 * transforms the vertices.
 *
 * Coherence with guest memory is kept at surface granularity rather than per
 * draw. A colour surface lives in a GPU render target while it is the one
 * being drawn; when the title moves to another surface, and at every flip, it
 * is copied back to guest memory. When a surface becomes current again it is
 * uploaded from guest memory first. So the window (which shows guest memory),
 * render-to-texture (textures are decoded from guest memory) and anything the
 * CPU writes into a surface all see what they would without this back end.
 *
 * RECOMP_GPUBE_SCALE=<n> draws at n times the title's resolution (rt_scale):
 * guest memory still gets and gives every surface at the title's size, the
 * window gets the large one.
 *
 * Not done yet (the CPU rasteriser does them, this does not):
 *   - register combiners and texture stages 1-3 (nv2a_backend.h does not pass
 *     them on): stage 0 is modulated with the diffuse colour;
 *   - occlusion counts (zpass) and swizzled render targets.
 * Culling is off, as it is on the CPU rasteriser.
 *
 * Everything runs on the NV2A poll thread, one call at a time.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend.h"
#include "nv2a_vsh_interp.h"  /* the interpreter the GPU programs transcribe */
#include "d3d8_swizzle.h"     /* the executor's own swizzle, expand and DXT helpers */

/* A toolkit without swizzled-surface fields or the shadow mode: every surface
 * is linear and there is no comparison. */
#ifdef NV2A_BACKEND_SURFACE_SWIZZLE
#define SURF_SWZ(s)   ((s)->swizzled != 0)
#define SURF_SWZ_W(s) ((s)->swz_w)
#define SURF_SWZ_H(s) ((s)->swz_h)
#else
#define SURF_SWZ(s)   0
#define SURF_SWZ_W(s) 0u
#define SURF_SWZ_H(s) 0u
#endif
#ifndef NV2A_BACKEND_HAS_SHADOW
#define nv2a_backend_shadow() 0
#endif

extern ptrdiff_t g_xbox_mem_offset;
#define GUEST(va) ((uint8_t *)g_xbox_mem_offset + (uint32_t)(va))

/* The framebuffer window's larger frame (xboxrecomp video_player.h). */
typedef struct {
    void    *handle;
    uint32_t scale;             /* down; across too unless scale_x is set */
    uint32_t scale_x;           /* across */
    float    fx, fy;            /* the exact factors, when not 0 */
    uint32_t flags;             /* 1: shown as it is, not resolved */
} XboxFramebufferGpuFrame;
typedef int (*XboxFramebufferGpu)(uint32_t fb_va, uint32_t pitch,
                                  XboxFramebufferGpuFrame *out);
void xbox_FramebufferWindowSetGpu(XboxFramebufferGpu fn);
void xbox_FramebufferWindowSetFrameAspect(float aspect);

static ID3D11Device        *s_dev;
static ID3D11DeviceContext *s_ctx;
static ID3D11VertexShader  *s_vs;
static ID3D11PixelShader   *s_ps_tex, *s_ps_col;
static ID3D11PixelShader   *s_ps_tex_w, *s_ps_col_w, *s_ps_full_w;  /* W-buffer variants */
static ID3D11PixelShader   *s_ps_full_wv;   /* ... from the perspective w (draw_program) */
static ID3D11InputLayout   *s_layout;
static ID3D11Buffer        *s_cb;
static ID3D11Buffer        *s_vb;
static UINT                 s_vb_bytes;
static UINT                 s_vb_pos;   /* next free byte (vb_put) */
static ID3D11RasterizerState *s_rs;
static int                  s_failed, s_ready;
static uint32_t             s_frame = 1;

/* ---- Shaders ------------------------------------------------------------ */

static const char s_hlsl[] =
"cbuffer C : register(b0) {\n"
"  float4 vp;   /* 2/w, -2/h, 1/zscale, 0 */\n"
"  float4 tx;   /* 1/tex_w, 1/tex_h, alpha_ref (0..255), alpha_func (0..7) */\n"
"  float4 wd;   /* W-buffer: clip min, 1/(clip max - clip min) */\n"
"};\n"
"struct VI { float3 p : POSITION; float rhw : TEXCOORD1; float4 c : COLOR; float2 uv : TEXCOORD0; };\n"
"struct VO { float4 p : SV_Position; float4 c : COLOR; float2 uv : TEXCOORD0;\n"
"            noperspective float rw : TEXCOORD6; };\n"
"Texture2D t0 : register(t0); SamplerState s0 : register(s0);\n"
"VO vs(VI i) {\n"
"  VO o; float w = 1.0 / max(i.rhw, 1e-20);\n"
"  float x = i.p.x * vp.x - 1.0, y = 1.0 + i.p.y * vp.y, z = saturate(i.p.z * vp.z);\n"
"  o.p = float4(x * w, y * w, z * w, w); o.c = i.c; o.uv = i.uv * tx.xy; o.rw = i.rhw; return o;\n"
"}\n"
/* 1/w is linear across the screen, so interpolated without perspective it
 * gives each pixel its own w (wbuf_on). */
"float wdepth(float rw) { return saturate((1.0 / max(rw, 1e-30) - wd.x) * wd.y); }\n"
"void atest(float a) {\n"
"  int f = (int)tx.w; float v = round(saturate(a) * 255.0), r = tx.z; bool ok;\n"
"  if (f == 0) ok = false; else if (f == 1) ok = v < r; else if (f == 2) ok = v == r;\n"
"  else if (f == 3) ok = v <= r; else if (f == 4) ok = v > r; else if (f == 5) ok = v != r;\n"
"  else if (f == 6) ok = v >= r; else ok = true;\n"
"  if (!ok) discard;\n"
"}\n"
"float4 ps_tex(VO i) : SV_Target { float4 c = t0.Sample(s0, i.uv) * i.c; atest(c.a); return c; }\n"
"float4 ps_col(VO i) : SV_Target { atest(i.c.a); return i.c; }\n"
"float4 ps_tex_w(VO i, out float d : SV_Depth) : SV_Target { d = wdepth(i.rw); return ps_tex(i); }\n"
"float4 ps_col_w(VO i, out float d : SV_Depth) : SV_Target { d = wdepth(i.rw); return ps_col(i); }\n";

typedef struct { float vp[4]; float tx[4]; float wd[4]; } Consts;

/* The full pixel pipeline: a line-for-line port of what the CPU rasteriser
 * does per pixel when the register combiners are in use -- xf_rows and
 * rc_stage_fetch in nv2a_pb_exec.c, nv2a_rc_eval in nv2a_combiner.c (which in
 * turn follow xemu's psh.c). Keep the two in step: when the C changes, this
 * changes the same way. */
static const char s_hlsl_full[] =
"cbuffer C : register(b0) { float4 vp; float4 tx; float4 wd; };\n"
"cbuffer F : register(b1) {\n"
"  uint4  cw[8];      /* color_icw, alpha_icw, color_ocw, alpha_ocw */\n"
"  float4 fac0[8];\n"
"  float4 fac1[8];\n"
"  uint4  fin;        /* final0, final1, control, stage_program */\n"
"  float4 fc0, fc1;   /* final_c0, final_c1 */\n"
"  float4 fogc;       /* fog colour rgb */\n"
"  uint4  stg;        /* clip_plane_mode, stage_valid, cube mask */\n"
"  float4 tsc[4];     /* per stage: u, v scale to [0,1] */\n"
"};\n"
"struct VI { float3 p : POSITION; float rhw : TEXCOORD8; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
"            float fog : TEXCOORD9; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1;\n"
"            float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
"struct VO { float4 p : SV_Position; noperspective float4 d0 : COLOR0;\n"
"            noperspective float4 d1 : COLOR1; noperspective float fog : TEXCOORD9;\n"
"            float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
"            noperspective float rw : TEXCOORD6; };\n"
"Texture2D tx2[4] : register(t0); TextureCube txc[4] : register(t4); SamplerState sm[4] : register(s0);\n"
"VO vs(VI i) {\n"
"  VO o; float w = 1.0 / max(i.rhw, 1e-20);\n"
"  float x = i.p.x * vp.x - 1.0, y = 1.0 + i.p.y * vp.y, z = saturate(i.p.z * vp.z);\n"
"  o.p = float4(x * w, y * w, z * w, w); o.d0 = i.d0; o.d1 = i.d1; o.fog = i.fog;\n"
"  o.t0 = i.t0; o.t1 = i.t1; o.t2 = i.t2; o.t3 = i.t3; o.rw = i.rhw; return o;\n"
"}\n"
/* rc_texel: a stage with no usable texture reads 1. */
"float4 texel2d(uint s, float2 uv) {\n"
"  if (!((stg.y >> s) & 1)) return float4(1, 1, 1, 1);\n"
"  return tx2[s].Sample(sm[s], uv * tsc[s].xy);\n"
"}\n"
/* rc_stage_fetch */
"float4 fetch(uint s, float4 c) {\n"
"  uint mode = (fin.w >> (s * 5)) & 0x1F;\n"
"  if (mode == 0) return float4(0, 0, 0, 1);\n"
"  if (mode == 1 || mode == 2) { float q = c.w != 0.0 ? c.w : 1.0; return texel2d(s, c.xy / q); }\n"
"  if (mode == 3) {\n"
"    if (!((stg.z >> s) & 1)) return texel2d(s, c.xy);\n"
"    if (!((stg.y >> s) & 1)) return float4(1, 1, 1, 1);\n"
"    return txc[s].Sample(sm[s], c.xyz);\n"
"  }\n"
"  if (mode == 4) return saturate(c);\n"
"  if (mode == 5) {\n"
"    [unroll] for (uint j = 0; j < 4; j++) {\n"
"      bool ge = ((stg.x >> (s * 4 + j)) & 1) != 0;\n"
"      if (ge ? c[j] >= 0.0 : c[j] < 0.0) discard;\n"
"    }\n"
"    return float4(0, 0, 0, 0);\n"
"  }\n"
"  return float4(0, 0, 0, 0);\n"
"}\n"
/* nv2a_rc_eval */
"float map_in(float x, uint b) {\n"
"  float p = max(x, 0.0); uint m = (b >> 5) & 7;\n"
"  if (m == 0) return p; if (m == 1) return 1.0 - saturate(x); if (m == 2) return 2.0 * p - 1.0;\n"
"  if (m == 3) return -2.0 * p + 1.0; if (m == 4) return p - 0.5; if (m == 5) return -p + 0.5;\n"
"  if (m == 6) return x; return -x;\n"
"}\n"
"float map_out(float x, uint m) {\n"
"  if (m == 0x08) return x - 0.5; if (m == 0x10) return x * 2.0; if (m == 0x18) return (x - 0.5) * 2.0;\n"
"  if (m == 0x20) return x * 4.0; if (m == 0x30) return x * 0.5; return x;\n"
"}\n"
"static float4 R[16];\n"
"float3 read_rgb(uint b) {\n"
"  float4 r = R[b & 0xF]; float3 v = (b & 0x10) ? r.aaa : r.rgb;\n"
"  return float3(map_in(v.x, b), map_in(v.y, b), map_in(v.z, b));\n"
"}\n"
"float read_alpha(uint b) { float4 r = R[b & 0xF]; return map_in((b & 0x10) ? r.a : r.b, b); }\n"
"float4 ps(VO i) : SV_Target {\n"
"  float4 t[4];\n"
"  t[0] = fetch(0, i.t0); t[1] = fetch(1, i.t1); t[2] = fetch(2, i.t2); t[3] = fetch(3, i.t3);\n"
"  uint n = min(fin.z & 0xFF, 8), flags = fin.z >> 8;\n"
"  [unroll] for (uint k = 0; k < 16; k++) R[k] = float4(0, 0, 0, 0);\n"
"  R[3] = float4(fogc.rgb, i.fog); R[4] = i.d0; R[5] = i.d1;\n"
"  R[8] = t[0]; R[9] = t[1]; R[10] = t[2]; R[11] = t[3];\n"
"  R[12].a = (fin.w & 0x1F) ? t[0].a : 1.0;\n"
"  for (uint s = 0; s < n; s++) {\n"
"    uint icw = cw[s].x, aicw = cw[s].y, ocw = cw[s].z, aocw = cw[s].w;\n"
"    uint fl = ocw >> 12, afl = aocw >> 12;\n"
"    R[1] = fac0[(flags & 0x010) ? s : 0]; R[2] = fac1[(flags & 0x100) ? s : 0];\n"
"    bool mux = (flags & 1) ? R[12].a >= 0.5 : (((int)(R[12].a * 255.0)) & 1) != 0;\n"
"    float3 A = read_rgb(icw >> 24), B = read_rgb(icw >> 16), C = read_rgb(icw >> 8), D = read_rgb(icw);\n"
"    float aA = read_alpha(aicw >> 24), aB = read_alpha(aicw >> 16), aC = read_alpha(aicw >> 8), aD = read_alpha(aicw);\n"
"    float dab = dot(A, B), dcd = dot(C, D);\n"
"    float3 pab = (fl & 2) ? dab.xxx : A * B, pcd = (fl & 1) ? dcd.xxx : C * D;\n"
"    float3 sum = (fl & 4) ? (mux ? pcd : pab) : pab + pcd;\n"
"    uint mo = fl & 0x38, amo = afl & 0x38;\n"
"    float3 ab = clamp(float3(map_out(pab.x, mo), map_out(pab.y, mo), map_out(pab.z, mo)), -1.0, 1.0);\n"
"    float3 cd = clamp(float3(map_out(pcd.x, mo), map_out(pcd.y, mo), map_out(pcd.z, mo)), -1.0, 1.0);\n"
"    float3 ms = clamp(float3(map_out(sum.x, mo), map_out(sum.y, mo), map_out(sum.z, mo)), -1.0, 1.0);\n"
"    float apab = aA * aB, apcd = aC * aD, asum = (afl & 4) ? (mux ? apcd : apab) : apab + apcd;\n"
"    float aab = clamp(map_out(apab, amo), -1.0, 1.0), acd = clamp(map_out(apcd, amo), -1.0, 1.0);\n"
"    float ams = clamp(map_out(asum, amo), -1.0, 1.0);\n"
"    uint d;\n"
"    d = (ocw >> 4) & 0xF; if (d) { R[d].rgb = ab; if (fl & 0x80) R[d].a = ab.z; }\n"
"    d = ocw & 0xF;        if (d) { R[d].rgb = cd; if (fl & 0x40) R[d].a = cd.z; }\n"
"    d = (ocw >> 8) & 0xF; if (d) R[d].rgb = ms;\n"
"    d = (aocw >> 4) & 0xF; if (d) R[d].a = aab;\n"
"    d = aocw & 0xF;        if (d) R[d].a = acd;\n"
"    d = (aocw >> 8) & 0xF; if (d) R[d].a = ams;\n"
"    R[0] = float4(0, 0, 0, 0);\n"
"  }\n"
"  float4 o;\n"
"  if (fin.x || fin.y) {\n"
"    uint f0 = fin.x, f1 = fin.y, ff = f1 & 0xFF;\n"
"    R[1] = fc0; R[2] = fc1;\n"
"    float3 a = (ff & 0x40) ? 1.0 - R[5].rgb : R[5].rgb;\n"
"    float3 b = (ff & 0x20) ? 1.0 - R[12].rgb : R[12].rgb;\n"
"    R[14] = float4((ff & 0x80) ? saturate(a + b) : a + b, 0.0);\n"
"    float3 E = read_rgb(f1 >> 24), F = read_rgb(f1 >> 16);\n"
"    R[15] = float4(E * F, 0.0);\n"
"    float3 A = read_rgb(f0 >> 24), B = read_rgb(f0 >> 16), C = read_rgb(f0 >> 8), D = read_rgb(f0);\n"
"    o = float4(D + C * (1.0 - A) + B * A, read_alpha(f1 >> 8));\n"
"  } else {\n"
"    o = R[12];\n"
"  }\n"
"  o = saturate(o);\n"
"  int af = (int)tx.w; float v = round(o.a * 255.0), r = tx.z; bool ok;\n"
"  if (af == 0) ok = false; else if (af == 1) ok = v < r; else if (af == 2) ok = v == r;\n"
"  else if (af == 3) ok = v <= r; else if (af == 4) ok = v > r; else if (af == 5) ok = v != r;\n"
"  else if (af == 6) ok = v >= r; else ok = true;\n"
"  if (!ok) discard;\n"
"  return o;\n"
"}\n"
"float4 ps_w(VO i, out float d : SV_Depth) : SV_Target {\n"
"  d = saturate((1.0 / max(i.rw, 1e-30) - wd.x) * wd.y); return ps(i);\n"
"}\n"
/* The same for a vertex program run here (draw_program): w itself, through
 * perspective-correct interpolation, which is exact at the vertices the GPU's
 * clipper makes as well -- where the CPU path recomputes 1/w after its clip. */
"float4 ps_wv(VO i, float wv : TEXCOORD7, out float d : SV_Depth) : SV_Target {\n"
"  d = saturate((wv - wd.x) * wd.y); return ps(i);\n"
"}\n";

/* Colours and fog between a triangle's corners. Kept linear on the screen, a
 * corner at the eye (w near 0, x in the tens of thousands of pixels, three
 * times that on a large surface) leaves them to the GPU's clipper and the
 * road's nearest triangle shows fog colour for a frame. Interpolated with the
 * perspective, as texture coordinates are, they are exact at the clipped
 * corners. RECOMP_GPUBE_PERSP_COLOR=0 keeps them linear. The modifiers decide
 * which values share a register, so prog_hlsl's VO has to carry the same. */
static int persp_color(void)
{
    const char *e = getenv("RECOMP_GPUBE_PERSP_COLOR");
    return !(e && e[0] == '0');
}

static const char *hlsl_full(void)
{
    static char src[sizeof s_hlsl_full];
    static const char *const f[] = { "noperspective float4 d0", "noperspective float4 d1",
                                     "noperspective float fog" };
    int i;

    if (!persp_color())
        return s_hlsl_full;
    if (!src[0]) {
        memcpy(src, s_hlsl_full, sizeof src);
        for (i = 0; i < 3; i++) {
            char *q = strstr(src, f[i]);
            if (q)
                memset(q, ' ', 13);
        }
    }
    return src;
}

typedef struct {
    uint32_t cw[8][4];
    float    fac0[8][4], fac1[8][4];
    uint32_t fin[4];
    float    fc0[4], fc1[4];
    float    fogc[4];
    uint32_t stg[4];
    float    tsc[4][4];
} FullConsts;

static ID3D11VertexShader *s_vs_full;
static ID3D11PixelShader  *s_ps_full;
static ID3D11InputLayout  *s_layout_full;
static ID3D11Buffer       *s_cb_full;

static ID3DBlob *compile_src(const char *src, const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = D3DCompile(src, strlen(src), "nv2a_backend", NULL, NULL,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            &code, &err);
    if (FAILED(hr)) {
        fprintf(stderr, "[GPUBE] shader %s: %s\n", entry,
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "failed");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    return code;
}

static ID3DBlob *compile(const char *entry, const char *target)
{
    return compile_src(s_hlsl, entry, target);
}

static int device_up(void)
{
    static const D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0 };
    ID3DBlob *vsb, *pstb, *pscb;
    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT,       0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,  0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;

    if (s_ready)
        return 1;
    if (s_failed)
        return 0;
    s_failed = 1;               /* until everything below has worked */
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, want, 1,
                                 D3D11_SDK_VERSION, &s_dev, NULL, &s_ctx))) {
        fprintf(stderr, "[GPUBE] no D3D11 device\n");
        return 0;
    }
    vsb = compile("vs", "vs_5_0");
    pstb = compile("ps_tex", "ps_5_0");
    pscb = compile("ps_col", "ps_5_0");
    if (!vsb || !pstb || !pscb)
        return 0;
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(pstb),
                                   ID3D10Blob_GetBufferSize(pstb), NULL, &s_ps_tex);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(pscb),
                                   ID3D10Blob_GetBufferSize(pscb), NULL, &s_ps_col);
    ID3D11Device_CreateInputLayout(s_dev, il, 4, ID3D10Blob_GetBufferPointer(vsb),
                                   ID3D10Blob_GetBufferSize(vsb), &s_layout);
    ID3D10Blob_Release(vsb); ID3D10Blob_Release(pstb); ID3D10Blob_Release(pscb);

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(Consts);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb);

    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    rd.ScissorEnable = TRUE;        /* the window clip (raster_clip) */
    ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_rs);

    {
        D3D11_INPUT_ELEMENT_DESC fl[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,   D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 8, DXGI_FORMAT_R32_FLOAT,          0, 12,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 9, DXGI_FORMAT_R32_FLOAT,          0, 48,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 64,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 80,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 96,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 112, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        ID3DBlob *fvs = compile_src(hlsl_full(), "vs", "vs_5_0");
        ID3DBlob *fps = compile_src(hlsl_full(), "ps", "ps_5_0");
        if (!fvs || !fps)
            return 0;
        ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(fvs),
                                        ID3D10Blob_GetBufferSize(fvs), NULL, &s_vs_full);
        ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(fps),
                                       ID3D10Blob_GetBufferSize(fps), NULL, &s_ps_full);
        ID3D11Device_CreateInputLayout(s_dev, fl, 9, ID3D10Blob_GetBufferPointer(fvs),
                                       ID3D10Blob_GetBufferSize(fvs), &s_layout_full);
        ID3D10Blob_Release(fvs); ID3D10Blob_Release(fps);
        bd.ByteWidth = (sizeof(FullConsts) + 15) & ~15u;
        ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb_full);
    }
    {
        /* The W-buffer variants (wbuf_on); without them depth stays z. */
        struct { const char *src, *entry; ID3D11PixelShader **ps; } w[] = {
            { s_hlsl, "ps_tex_w", &s_ps_tex_w }, { s_hlsl, "ps_col_w", &s_ps_col_w },
            { hlsl_full(), "ps_w", &s_ps_full_w }, { hlsl_full(), "ps_wv", &s_ps_full_wv },
        };
        int j;
        for (j = 0; j < 4; j++) {
            ID3DBlob *b = compile_src(w[j].src, w[j].entry, "ps_5_0");
            if (!b)
                continue;
            ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(b),
                                           ID3D10Blob_GetBufferSize(b), NULL, w[j].ps);
            ID3D10Blob_Release(b);
        }
    }

    if (!s_vs || !s_ps_tex || !s_ps_col || !s_layout || !s_cb || !s_rs
            || !s_vs_full || !s_ps_full || !s_layout_full || !s_cb_full)
        return 0;
    s_failed = 0;
    s_ready = 1;
    fprintf(stderr, "[GPUBE] D3D11 back end up\n");
    return 1;
}

/* ---- Colour surfaces ---------------------------------------------------- */

typedef struct {
    uint32_t va, pitch, bpp, w, h;
    ID3D11Texture2D *tex, *staging;
    ID3D11RenderTargetView *rtv;
    ID3D11ShaderResourceView *srv;  /* sampling it in place (rt_samples_as) */
    int dirty;                  /* GPU holds drawing guest memory lacks */
    int pending;                /* ... copied to staging, not to guest memory yet */
    uint32_t frame;             /* s_frame it was last made current in */
    int drawn;                  /* drawn into since the last flip (shadow) */
    int fresh;                  /* created, not read in from guest memory yet */
    int swz;                    /* guest memory holds it swizzled (rt_swz_copy) */
    uint32_t last_use;
    ID3D11Texture2D *relay;     /* rt_relayout_gpu's target, same size and format */
    ID3D11RenderTargetView *relay_rtv;
    /* Drawn fx times as wide and fy times as high (rt_scale_xy), tw x th
     * pixels, big when that is not the title's size: tex is that large
     * surface, lo the title's size, which is what guest memory gets and
     * gives. mem is what was last written to guest memory, mem_ok while it is
     * valid. */
    float fx, fy;
    uint32_t tw, th;
    int big;
    uint32_t ax, ay;            /* anti-aliasing samples left out (rt_drop_aa) */
    ID3D11Texture2D *lo;
    ID3D11RenderTargetView *lo_rtv;
    ID3D11ShaderResourceView *lo_srv;
    uint8_t *mem;
    int mem_ok;
} Rt;

typedef struct {
    uint32_t va, w, h;
    ID3D11Texture2D *tex;
    ID3D11DepthStencilView *dsv;
    uint32_t last_use;
} Ds;

#define MAX_RT 16
#define MAX_DS 8
static Rt  s_rt[MAX_RT];
static Ds  s_ds[MAX_DS];
static Rt *s_cur_rt;
static Ds *s_cur_ds;
static uint32_t s_use;

static void tex_invalidate_range(uint32_t va, uint32_t bytes);
static int guest_range_ok(uint32_t va, uint32_t bytes);

/* Shadow mode: after the first frame that differs, every clear and draw of the
 * next frame is logged with its inputs, so a difference can be traced to it. */
static int s_log_draws;
static uint32_t s_log_seq;

/* Bytes the surface covers in guest memory. */
static uint32_t rt_bytes(const Rt *r)
{
    return r->swz ? r->w * r->h * r->bpp : r->pitch * r->h;
}

/* A swizzled surface (Nv2aSurface.swizzled, render-to-texture) is drawn
 * row-major on the GPU and swizzled at the guest-memory boundary, with the
 * masks the rasteriser and the texture sampler use (d3d8_swizzle.h): pixel
 * (x, y) at element deposit(x, mask_x) | deposit(y, mask_y). Copies between
 * guest memory and a row-major image `lin` of lin_pitch bytes per row. */
static uint32_t s_rxo[4096], s_ryo[4096];

static void rt_swz_copy(const Rt *r, uint8_t *lin, uint32_t lin_pitch, int to_guest)
{
    uint8_t *g = GUEST(r->va);
    uint32_t mx, my, x, y;

    xbox_swizzle_masks(r->w, r->h, &mx, &my);
    for (x = 0; x < r->w; x++) s_rxo[x] = swizzle_deposit(x, mx);
    for (y = 0; y < r->h; y++) s_ryo[y] = swizzle_deposit(y, my);
    for (y = 0; y < r->h; y++) {
        uint8_t *row = lin + (size_t)y * lin_pitch;
        for (x = 0; x < r->w; x++) {
            uint8_t *gp = g + (size_t)(s_rxo[x] | s_ryo[y]) * r->bpp;
            if (to_guest)
                memcpy(gp, row + (size_t)x * r->bpp, r->bpp);
            else
                memcpy(row + (size_t)x * r->bpp, gp, r->bpp);
        }
    }
}

/* Reorder a row-major w x h image between swizzled and row-major order:
 * to_swz moves pixel (x, y) to element deposit(x) | deposit(y), else back. */
static void swz_permute(const uint8_t *src, uint8_t *dst, uint32_t w, uint32_t h,
                        uint32_t bpp, int to_swz)
{
    uint32_t mx, my, x, y;

    xbox_swizzle_masks(w, h, &mx, &my);
    for (x = 0; x < w; x++) s_rxo[x] = swizzle_deposit(x, mx);
    for (y = 0; y < h; y++) s_ryo[y] = swizzle_deposit(y, my);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            size_t lin = ((size_t)y * w + x) * bpp;
            size_t swz = (size_t)(s_rxo[x] | s_ryo[y]) * bpp;
            memcpy(dst + (to_swz ? swz : lin), src + (to_swz ? lin : swz), bpp);
        }
}

/* The same memory addressed in the other layout. Nightfire clears its 256x256
 * reflection maps while SURFACE_FORMAT still says linear and then draws them
 * swizzled; the CPU rasteriser writes both into the one memory, so this keeps
 * one surface per address and moves its pixels to where the new layout finds
 * them -- read as the old layout, written as the new. Only when a row is
 * w * bpp bytes in both, so the two cover the same bytes. */
static void rt_complete(Rt *r);

/* The same reordering on the GPU, swz_permute as a pixel shader: target pixel
 * (x, y) of the new layout reads the old image at the pixel that holds the
 * same bytes. Exact for either format (a UNORM value read and written back is
 * the value). The CPU way read the surface back and waited for the GPU each
 * time; Nightfire changes the layout of its reflection maps several times a
 * frame (cleared linear, drawn swizzled), and those waits were a fifth of the
 * GPU thread's time in the driving missions. RECOMP_GPUBE_GPU_RELAYOUT=0 keeps
 * the CPU way; RECOMP_GPUBE_RELAYOUT_CHECK=1 does both for the first 64 and
 * reports any pixel that differs. */
static const char s_relayout_hlsl[] =
    "cbuffer R : register(b0) { uint rw, rh, rmx, rmy, rswz, r0, r1, r2; };\n"
    "Texture2D<float4> rsrc : register(t0);\n"
    "float4 rvs(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 p = float2((id << 1) & 2, id & 2);\n"
    "    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n"
    "uint pdep(uint v, uint m) {\n"
    "    uint r = 0, bit = 1;\n"
    "    [loop] while (m != 0) { uint low = m & (~m + 1); if (v & bit) r |= low;\n"
    "                            m &= m - 1; bit <<= 1; }\n"
    "    return r;\n"
    "}\n"
    "uint pext(uint e, uint m) {\n"
    "    uint r = 0, bit = 1;\n"
    "    [loop] while (m != 0) { uint low = m & (~m + 1); if (e & low) r |= bit;\n"
    "                            m &= m - 1; bit <<= 1; }\n"
    "    return r;\n"
    "}\n"
    "float4 rps(float4 pos : SV_Position) : SV_Target {\n"
    "    uint x = (uint)pos.x, y = (uint)pos.y;\n"
    "    uint2 s;\n"
    "    if (rswz != 0) { uint e = pdep(x, rmx) | pdep(y, rmy); s = uint2(e % rw, e / rw); }\n"
    "    else           { uint e = y * rw + x; s = uint2(pext(e, rmx), pext(e, rmy)); }\n"
    "    return rsrc.Load(int3(s, 0));\n"
    "}\n";

static ID3D11VertexShader    *s_relay_vs;
static ID3D11PixelShader     *s_relay_ps;
static ID3D11Buffer          *s_relay_cb;
static ID3D11RasterizerState *s_relay_rs;
static int                    s_relay_ok = -1;   /* -1 not tried, 0 off/failed, 1 ready */

static int relayout_gpu_ready(void)
{
    ID3DBlob *vsb, *psb;
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;
    const char *e;

    if (s_relay_ok >= 0)
        return s_relay_ok;
    s_relay_ok = 0;
    e = getenv("RECOMP_GPUBE_GPU_RELAYOUT");
    if (e && e[0] == '0')
        return 0;
    vsb = compile_src(s_relayout_hlsl, "rvs", "vs_5_0");
    psb = compile_src(s_relayout_hlsl, "rps", "ps_5_0");
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 32;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (vsb && psb
            && SUCCEEDED(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb),
                             ID3D10Blob_GetBufferSize(vsb), NULL, &s_relay_vs))
            && SUCCEEDED(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb),
                             ID3D10Blob_GetBufferSize(psb), NULL, &s_relay_ps))
            && SUCCEEDED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_relay_cb))
            && SUCCEEDED(ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_relay_rs)))
        s_relay_ok = 1;
    if (vsb) ID3D10Blob_Release(vsb);
    if (psb) ID3D10Blob_Release(psb);
    fprintf(stderr, "[GPUBE] surface relayout on the %s\n", s_relay_ok ? "GPU" : "CPU");
    return s_relay_ok;
}

/* rt_relayout without the round trip through the CPU; 0 if it cannot. */
static int rt_relayout_gpu(Rt *r, int swz)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_VIEWPORT vp, saved_vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11ShaderResourceView *none = NULL;
    uint32_t cb[8];

    if (!relayout_gpu_ready())
        return 0;
    if (!r->srv && FAILED(ID3D11Device_CreateShaderResourceView(
            s_dev, (ID3D11Resource *)r->tex, NULL, &r->srv)))
        return 0;
    if (!r->relay) {
        ID3D11Texture2D_GetDesc(r->tex, &td);
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->relay))
                || FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)r->relay,
                                                              NULL, &r->relay_rtv))) {
            if (r->relay) ID3D11Texture2D_Release(r->relay);
            r->relay = NULL;
            return 0;
        }
    }

    /* A copy queued for memory holds the old layout, which memory would now
     * read wrongly; the surface itself holds the same bytes in the new one,
     * so it is what goes to memory, later, like any drawing. Not when bind is
     * about to read the surface in from memory (first use this frame): then
     * memory has to hold it now, as on the CPU way. */
    if (r->pending && r->frame != s_frame)
        rt_complete(r);
    if (r->pending) {
        r->pending = 0;
        r->dirty = 1;
    }

    memset(cb, 0, sizeof cb);
    cb[0] = r->w;
    cb[1] = r->h;
    xbox_swizzle_masks(r->w, r->h, &cb[2], &cb[3]);
    cb[4] = (uint32_t)swz;
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_relay_cb, 0, NULL, cb, 0, 0);

    ID3D11DeviceContext_RSGetViewports(s_ctx, &nvp, saved_vp);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &r->relay_rtv, NULL);
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)r->w; vp.Height = (float)r->h;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(s_ctx, s_relay_rs);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_relay_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_relay_ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_relay_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &r->srv);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)r->tex, (ID3D11Resource *)r->relay);

    /* Every draw sets its own shaders and states; the target and viewport
     * are bind's, which may be under way, so they are put back. */
    if (s_cur_rt)
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_cur_rt->rtv,
                                               s_cur_ds ? s_cur_ds->dsv : NULL);
    else
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    if (nvp)
        ID3D11DeviceContext_RSSetViewports(s_ctx, nvp, saved_vp);
    r->swz = swz;
    return 1;
}

/* Higher resolution. Every linear colour surface, and the depth buffer that
 * goes with it, is drawn fx times as wide and fy times as high: the viewport
 * is that much larger, so the title's surface pixels land on the large
 * surface unchanged, only finer. Guest memory keeps the title's size -- the
 * title reads its surfaces, copies them, samples them as textures -- so a
 * large surface goes to memory filtered down (rt_queue: each title pixel the
 * area-weighted mean of what covers it) and comes from memory blown up
 * (rt_upload), except when memory still holds what it last got from the
 * surface: then the large surface already has it. The window shows the large
 * surface the title flipped to (be_window_frame).
 *
 * RECOMP_GPUBE_SIZE=<w>x<h> sets the size the shown 640x480 is drawn at -- a
 * monitor's resolution (src/main.c from nightfire.ini Resolution, the
 * launcher's list): 1920x1080 gives fx 3, fy 2.25. With widescreen the title
 * draws its world squeezed into 640x480 for 16:9, so a 16:9 size draws every
 * column the monitor shows. Else RECOMP_GPUBE_SCALE=<n> (2-8), the older
 * steps: n x n, and with widescreen the width n times 16:9 over 4:3, rounded
 * (2560x1440 at 3); n = 1 stays 640x480. RECOMP_GPUBE_SCALE_X=<n> sets the
 * width's step by hand.
 *
 * Swizzled surfaces (render to texture) stay at the title's size, and so does
 * everything in shadow mode, which compares against the CPU rasteriser. The
 * caller sets it only for the enhanced game (src/main.c). */
static int rt_drop_aa(void);

static void rt_scale_xy(float *out_x, float *out_y)
{
    static float fx = -1.0f, fy = -1.0f;
    if (fy < 0.0f) {
        const char *e = getenv("RECOMP_GPUBE_SCALE"), *ex = getenv("RECOMP_GPUBE_SCALE_X");
        const char *a = getenv("RECOMP_DISPLAY_ASPECT"), *sz = getenv("RECOMP_GPUBE_SIZE");
        float ax = 0.0f, ay = 0.0f;
        unsigned sw = 0, sh = 0;
        int sy;

        if (nv2a_backend_shadow()) {
            fx = fy = 1.0f;
        } else if (sz && sscanf(sz, "%u x %u", &sw, &sh) == 2 && sw >= 320 && sh >= 240
                   && sw <= 7680 && sh <= 4320) {
            fx = (float)sw / 640.0f;
            fy = (float)sh / 480.0f;
#ifdef NIGHTFIRE_DRIVING
            /* The driving image's surface is twice as wide (anti-aliasing):
             * above 1440 lines it gets 1/sqrt(2) of the height's factor, not
             * less than 3, for about the main game's pixels -- 3840x2160
             * draws its 1280x480 at 1536 lines, which the window shows at
             * 2160. Not when the samples are left out (rt_drop_aa). */
            if (fy > 3.0f && !rt_drop_aa()) {
                float r = (fy * 0.71f > 3.0f ? fy * 0.71f : 3.0f) / fy;
                fx *= r;
                fy *= r;
            }
#endif
        } else {
            sy = e ? atoi(e) : 1;
            if (sy < 1 || sy > 8)
                sy = 1;
#ifdef NIGHTFIRE_DRIVING
            /* The driving image's surface is twice as wide (anti-aliasing), so
             * the same n would be twice the pixels of the main game: 5120x1920
             * at 4, more than the GPU holds at 50 fps in Paris. Each axis gets
             * 1/sqrt(2) of n, rounded up, for about the main game's pixels --
             * unless the samples are left out (rt_drop_aa). */
            if (!rt_drop_aa())
                sy = (sy * 71 + 99) / 100;
#endif
            fy = (float)sy;
            fx = fy;
            if (sy > 1 && a && sscanf(a, "%f:%f", &ax, &ay) == 2 && ax > 0.0f && ay > 0.0f
                    && ax / ay > 4.0f / 3.0f + 0.01f)
                fx = (float)lrintf((float)sy * (ax / ay) / (4.0f / 3.0f));
            if (ex && atoi(ex) >= 1 && atoi(ex) <= 16 && sy > 1)
                fx = (float)atoi(ex);
        }
        if (fx != 1.0f || fy != 1.0f)
            fprintf(stderr, "[GPUBE] drawing at %.3fx the title's width and %.3fx its height\n",
                    fx, fy);
    }
    *out_x = fx;
    *out_y = fy;
}

/* RECOMP_GPUBE_NO_AA=1 (the enhanced game, nightfire.ini DrivingAA = 0):
 * a surface the title draws anti-aliased -- the driving image's 1280x480,
 * two samples a pixel, which the window averages -- is drawn at the shown
 * size instead, its factors divided by the samples, and shown as it is. At
 * a monitor's size the samples smoothed little and cost half the GPU; the
 * driving missions draw native 4K that way at what 2560x1440 cost before.
 * The title still draws to its 1280x480: only fewer pixels make it up. */
static int rt_drop_aa(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GPUBE_NO_AA");
        on = e && e[0] == '1' && !nv2a_backend_shadow();
        if (on)
            fprintf(stderr, "[GPUBE] anti-aliased surfaces drawn at the shown size\n");
    }
    return on;
}

/* Drawn at another size than the title's. */
static int rt_scaled(void)
{
    float fx, fy;
    rt_scale_xy(&fx, &fy);
    return fx != 1.0f || fy != 1.0f;
}

/* sf: large pixels per title pixel; ssz: the source's size in pixels. */
static const char s_scale_hlsl[] =
    "cbuffer S : register(b0) { float2 sf; uint2 ssz; };\n"
    "Texture2D<float4> ssrc : register(t0);\n"
    "float4 svs(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 p = float2((id << 1) & 2, id & 2);\n"
    "    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n"
    /* the title's pixel: the mean of what was drawn over it, each large pixel
     * weighted by how much of it lies inside (all 1 at a whole factor) */
    "float4 sdown(float4 pos : SV_Position) : SV_Target {\n"
    "    float2 p = floor(pos.xy), s0 = p * sf, s1 = (p + 1.0) * sf;\n"
    "    int2 i0 = (int2)floor(s0), i1 = min((int2)ceil(s1) - 1, (int2)ssz - 1);\n"
    "    float4 a = 0; float n = 0;\n"
    "    for (int y = i0.y; y <= i1.y; y++) {\n"
    "        float wy = min((float)y + 1.0, s1.y) - max((float)y, s0.y);\n"
    "        for (int x = i0.x; x <= i1.x; x++) {\n"
    "            float w = (min((float)x + 1.0, s1.x) - max((float)x, s0.x)) * wy;\n"
    "            a += ssrc.Load(int3(x, y, 0)) * w; n += w;\n"
    "        }\n"
    "    }\n"
    "    return a / max(n, 1e-6);\n"
    "}\n"
    /* ... and back: each large pixel gets the title pixel it lies in */
    "float4 sup(float4 pos : SV_Position) : SV_Target {\n"
    "    int2 q = min((int2)floor(pos.xy / sf), (int2)ssz - 1);\n"
    "    return ssrc.Load(int3(q, 0));\n"
    "}\n";

static ID3D11VertexShader    *s_scl_vs;
static ID3D11PixelShader     *s_scl_down, *s_scl_up;
static ID3D11Buffer          *s_scl_cb;
static ID3D11RasterizerState *s_scl_rs;
static int                    s_scl_ok = -1;   /* -1 not tried, 0 failed, 1 ready */

static int scale_ready(void)
{
    ID3DBlob *vsb, *dnb, *upb;
    D3D11_BUFFER_DESC bd;
    D3D11_RASTERIZER_DESC rd;

    if (s_scl_ok >= 0)
        return s_scl_ok;
    s_scl_ok = 0;
    vsb = compile_src(s_scale_hlsl, "svs", "vs_5_0");
    dnb = compile_src(s_scale_hlsl, "sdown", "ps_5_0");
    upb = compile_src(s_scale_hlsl, "sup", "ps_5_0");
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (vsb && dnb && upb
            && SUCCEEDED(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb),
                             ID3D10Blob_GetBufferSize(vsb), NULL, &s_scl_vs))
            && SUCCEEDED(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(dnb),
                             ID3D10Blob_GetBufferSize(dnb), NULL, &s_scl_down))
            && SUCCEEDED(ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(upb),
                             ID3D10Blob_GetBufferSize(upb), NULL, &s_scl_up))
            && SUCCEEDED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_scl_cb))
            && SUCCEEDED(ID3D11Device_CreateRasterizerState(s_dev, &rd, &s_scl_rs)))
        s_scl_ok = 1;
    if (vsb) ID3D10Blob_Release(vsb);
    if (dnb) ID3D10Blob_Release(dnb);
    if (upb) ID3D10Blob_Release(upb);
    if (!s_scl_ok)
        fprintf(stderr, "[GPUBE] scaling shaders failed: drawing at the title's resolution\n");
    return s_scl_ok;
}

/* ps over all of dst (w x h) reading src (sw x sh, f large pixels per title
 * pixel), then bind's target and viewport
 * back, as rt_relayout_gpu does. */
static void scale_pass(ID3D11PixelShader *ps, ID3D11ShaderResourceView *src,
                       ID3D11RenderTargetView *dst, uint32_t w, uint32_t h,
                       float fx, float fy, uint32_t sw, uint32_t sh)
{
    D3D11_VIEWPORT vp, saved_vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT nvp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11ShaderResourceView *none = NULL;
    uint32_t cb[4];

    memcpy(&cb[0], &fx, 4);
    memcpy(&cb[1], &fy, 4);
    cb[2] = sw;
    cb[3] = sh;

    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)s_scl_cb, 0, NULL, cb, 0, 0);
    ID3D11DeviceContext_RSGetViewports(s_ctx, &nvp, saved_vp);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &dst, NULL);
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)w; vp.Height = (float)h;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(s_ctx, s_scl_rs);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_scl_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_scl_cb);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &src);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
    if (s_cur_rt)
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &s_cur_rt->rtv,
                                               s_cur_ds ? s_cur_ds->dsv : NULL);
    else
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    if (nvp)
        ID3D11DeviceContext_RSSetViewports(s_ctx, nvp, saved_vp);
}

/* The surface's pixels, row-major, read back from the GPU (blocking). */
static int rt_read(Rt *r, uint8_t *out)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t y, row = r->w * r->bpp;

    if (!r->staging) {
        ID3D11Texture2D_GetDesc(r->tex, &td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->staging)))
            return 0;
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)r->staging,
                                     (ID3D11Resource *)r->tex);
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)r->staging, 0,
                                       D3D11_MAP_READ, 0, &m)))
        return 0;
    for (y = 0; y < r->h; y++)
        memcpy(out + (size_t)y * row, (const uint8_t *)m.pData + (size_t)y * m.RowPitch, row);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)r->staging, 0);
    return 1;
}

static void rt_relayout(Rt *r, int swz)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_MAPPED_SUBRESOURCE m;
    size_t n = (size_t)r->w * r->h * r->bpp;
    uint8_t *cur, *nxt;
    uint32_t y, row = r->w * r->bpp;

    {
        static int check = -1, checked, bad;
        if (check < 0) {
            const char *e = getenv("RECOMP_GPUBE_RELAYOUT_CHECK");
            check = e && e[0] && e[0] != '0';
        }
        if (check && checked < 64 && relayout_gpu_ready()) {
            /* Both ways on the same pixels; a staging copy queued for memory
             * is in the way of rt_read, so it is completed first. */
            uint8_t *before = (uint8_t *)malloc(n), *want = (uint8_t *)malloc(n),
                    *got = (uint8_t *)malloc(n);
            size_t i, diff = 0;
            rt_complete(r);
            if (before && want && got && rt_read(r, before)) {
                swz_permute(before, want, r->w, r->h, r->bpp, !swz);
                if (rt_relayout_gpu(r, swz) && rt_read(r, got)) {
                    for (i = 0; i < n; i += r->bpp)
                        if (memcmp(want + i, got + i, r->bpp))
                            diff++;
                    checked++;
                    if (diff)
                        bad++;
                    fprintf(stderr, "[GPUBE-RELAYOUT] %08X %ux%u %ubpp -> %s: %zu pixels "
                                    "differ (%d of %d checks bad)\n", r->va, r->w, r->h,
                            r->bpp * 8, swz ? "swizzled" : "linear", diff, bad, checked);
                    free(before); free(want); free(got);
                    return;
                }
            }
            free(before); free(want); free(got);
        }
    }
    if (rt_relayout_gpu(r, swz))
        return;

    cur = (uint8_t *)malloc(n);
    nxt = (uint8_t *)malloc(n);
    rt_complete(r);             /* lands in memory in the old layout */
    r->swz = swz;
    if (!cur || !nxt) {
        free(cur); free(nxt);
        return;
    }
    if (!r->staging) {
        ID3D11Texture2D_GetDesc(r->tex, &td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->staging))) {
            free(cur); free(nxt);
            return;
        }
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)r->staging,
                                     (ID3D11Resource *)r->tex);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)r->staging, 0,
                                          D3D11_MAP_READ, 0, &m))) {
        for (y = 0; y < r->h; y++)
            memcpy(cur + (size_t)y * row, (const uint8_t *)m.pData + (size_t)y * m.RowPitch, row);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)r->staging, 0);
        /* What memory holds under the old layout, then read under the new:
         * old swizzled -> memory is swz(cur) -> new linear reads it as is;
         * old linear -> memory is cur -> new swizzled reads unswz(cur). */
        swz_permute(cur, nxt, r->w, r->h, r->bpp, !swz);
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)r->tex, 0, NULL,
                                              nxt, row, 0);
    }
    free(cur);
    free(nxt);
}

/* Writing a surface back to guest memory is two steps: a copy into its staging
 * texture, which the GPU makes in its own time, and reading that copy out,
 * which waits for the GPU to get there. Leaving a surface used to do both at
 * once -- eight times a frame in the driving missions, which render to
 * texture -- and each wait held this thread until the GPU had caught up,
 * ~3.5 ms a frame. Leaving a surface now only queues the copy (rt_queue); the
 * copies are read out (rt_complete) where guest memory has to hold them: at
 * the flip, before a semaphore release (be_sync), and when a texture reads the
 * surface's memory and cannot sample the surface itself (tex_get). */
/* A scaled surface goes to memory only when something asks for it there: a
 * texture that cannot sample the surface, or the surface being replaced. Its
 * averaging pass, copy and read-out every frame cost ~3 fps in the driving
 * missions at 4x, for memory nothing read: the window takes the frame from
 * the surface, and drawing goes on from the surface too (rt_upload).
 * RECOMP_GPUBE_LAZY_WB=0 writes it back as a 1x surface is. */
static int s_wb_force;
static int lazy_wb(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GPUBE_LAZY_WB");
        on = !(e && e[0] == '0');
    }
    return on;
}

static void rt_queue(Rt *r)
{
    if (!r->dirty)
        return;
    if (lazy_wb() && r->big && !s_wb_force)
        return;
    /* Shadow mode: guest memory holds the CPU rasteriser's frame, the one
     * being compared against. Never write over it. */
    if (nv2a_backend_shadow()) {
        r->dirty = 0;
        return;
    }
    if (!r->staging) {
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D_GetDesc(r->big ? r->lo : r->tex, &td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->staging)))
            return;
    }
    if (r->big) {
        if (!r->srv && FAILED(ID3D11Device_CreateShaderResourceView(
                s_dev, (ID3D11Resource *)r->tex, NULL, &r->srv)))
            return;
        scale_pass(s_scl_down, r->srv, r->lo_rtv, r->w, r->h, r->fx, r->fy, r->tw, r->th);
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)r->staging,
                                     (ID3D11Resource *)(r->big ? r->lo : r->tex));
    r->dirty = 0;
    r->pending = 1;
}

static void rt_complete(Rt *r)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t y, row = r->w * r->bpp;

    if (!r->pending)
        return;
    r->pending = 0;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)r->staging, 0,
                                       D3D11_MAP_READ, 0, &m)))
        return;
    if (r->swz)
        rt_swz_copy(r, (uint8_t *)m.pData, m.RowPitch, 1);
    else
        for (y = 0; y < r->h; y++)
            memcpy(GUEST(r->va + y * r->pitch), (const uint8_t *)m.pData + y * m.RowPitch, row);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)r->staging, 0);
    tex_invalidate_range(r->va, rt_bytes(r));
    if (r->mem) {
        memcpy(r->mem, GUEST(r->va), (size_t)r->pitch * r->h);
        r->mem_ok = 1;
    }
}

static void rt_download(Rt *r)
{
    s_wb_force = 1;
    rt_queue(r);
    s_wb_force = 0;
    rt_complete(r);
}

/* Every queued copy, out to guest memory. */
static void rt_complete_all(void)
{
    uint32_t i;
    for (i = 0; i < MAX_RT; i++)
        if (s_rt[i].tex)
            rt_complete(&s_rt[i]);
}

static void rt_upload(Rt *r)
{
    if (r->big) {
        /* Memory still holds what the surface last wrote there: the large
         * surface has the same picture, sharper. */
        if (r->mem_ok && !memcmp(GUEST(r->va), r->mem, (size_t)r->pitch * r->h))
            return;
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)r->lo, 0, NULL,
                                              GUEST(r->va), r->pitch, 0);
        scale_pass(s_scl_up, r->lo_srv, r->rtv, r->tw, r->th, r->fx, r->fy, r->w, r->h);
        r->mem_ok = 0;
        return;
    }
    if (r->swz) {
        uint8_t *lin = (uint8_t *)malloc((size_t)r->w * r->h * r->bpp);
        if (!lin)
            return;
        rt_swz_copy(r, lin, r->w * r->bpp, 0);
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)r->tex, 0, NULL,
                                              lin, r->w * r->bpp, 0);
        free(lin);
        return;
    }
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)r->tex, 0, NULL,
                                          GUEST(r->va), r->pitch, 0);
}

static void rt_free(Rt *r)
{
    if (r->srv) ID3D11ShaderResourceView_Release(r->srv);
    if (r->relay_rtv) ID3D11RenderTargetView_Release(r->relay_rtv);
    if (r->relay) ID3D11Texture2D_Release(r->relay);
    if (r->rtv) ID3D11RenderTargetView_Release(r->rtv);
    if (r->tex) ID3D11Texture2D_Release(r->tex);
    if (r->staging) ID3D11Texture2D_Release(r->staging);
    if (r->lo_srv) ID3D11ShaderResourceView_Release(r->lo_srv);
    if (r->lo_rtv) ID3D11RenderTargetView_Release(r->lo_rtv);
    if (r->lo) ID3D11Texture2D_Release(r->lo);
    free(r->mem);
    memset(r, 0, sizeof *r);
}

static Rt *rt_get(const Nv2aSurface *s)
{
    Rt *r, *victim = &s_rt[0];
    D3D11_TEXTURE2D_DESC td;
    uint32_t w, h, pitch, i, tw, th, ax, ay;
    float fx, fy;
    int swz = SURF_SWZ(s);

    if (s->bytes_per_pixel != 4 && s->bytes_per_pixel != 2)
        return NULL;
    /* A swizzled surface's size is the format's power-of-two square, not
     * pitch / bpp. */
    w = swz ? SURF_SWZ_W(s) : s->pitch / s->bytes_per_pixel;
    h = swz ? SURF_SWZ_H(s) : s->height;
    pitch = swz ? w * s->bytes_per_pixel : s->pitch;
    if (!w || w > 4096 || !h || h > 4096)
        return NULL;
    if (!guest_range_ok(s->color_va, pitch * h))     /* see guest_range_ok */
        return NULL;
    for (i = 0; i < MAX_RT; i++) {
        r = &s_rt[i];
        if (r->tex && r->va == s->color_va && r->pitch == pitch
                && r->bpp == s->bytes_per_pixel && r->h == h && r->swz == swz)
            return r;
        /* The same bytes in the other layout (rt_relayout). A large surface
         * goes through guest memory instead: written back, and the new one
         * reads it in. */
        if (r->tex && r->va == s->color_va && r->bpp == s->bytes_per_pixel
                && r->w == w && r->h == h && r->swz != swz
                && r->pitch == w * r->bpp && pitch == w * r->bpp) {
            if (r->big) {
                victim = r;
                break;
            }
            rt_relayout(r, swz);
            return r;
        }
        if (!r->tex || r->last_use < victim->last_use)
            victim = r;
    }
    r = victim;
    if (r->tex) {
        if (r == s_cur_rt)
            s_cur_rt = NULL;
        rt_download(r);
        rt_free(r);
    }
    ax = ay = 1;
    if (swz) {
        fx = fy = 1.0f;
    } else {
        rt_scale_xy(&fx, &fy);
        if (rt_drop_aa() && (s->aa_sx > 1 || s->aa_sy > 1)) {
            ax = s->aa_sx > 1 ? s->aa_sx : 1;
            ay = s->aa_sy > 1 ? s->aa_sy : 1;
            fx /= (float)ax;
            fy /= (float)ay;
        }
    }
    tw = (uint32_t)lrintf((float)w * fx);
    th = (uint32_t)lrintf((float)h * fy);
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    if ((tw != w || th != h) && (tw > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
                                 || th > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
                                 || !scale_ready())) {
        fx = fy = 1.0f;
        tw = w;
        th = h;
        ax = ay = 1;
    }
    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = s->bytes_per_pixel == 4 ? DXGI_FORMAT_B8G8R8A8_UNORM
                                        : DXGI_FORMAT_B5G6R5_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if ((tw != w || th != h)
            && (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->lo))
                || FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)r->lo,
                                                              NULL, &r->lo_rtv))
                || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)r->lo,
                                                                NULL, &r->lo_srv))
                || !(r->mem = (uint8_t *)malloc((size_t)pitch * h)))) {
        rt_free(r);
        return NULL;
    }
    td.Width = tw;
    td.Height = th;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->tex))
            || FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)r->tex,
                                                          NULL, &r->rtv))) {
        rt_free(r);
        return NULL;
    }
    r->fx = fx;
    r->fy = fy;
    r->tw = tw;
    r->th = th;
    r->big = tw != w || th != h;
    r->ax = ax;
    r->ay = ay;
    r->va = s->color_va;
    r->pitch = pitch;
    r->bpp = s->bytes_per_pixel;
    r->w = w;
    r->h = h;
    r->swz = swz;
    r->fresh = 1;
    r->frame = s_frame - 1;     /* not current this frame */
    fprintf(stderr, "[GPUBE] surface %08X..%08X %ux%u %ubpp%s", r->va,
            r->va + rt_bytes(r), r->w, r->h, r->bpp * 8, swz ? " swizzled" : "");
    if (r->big)
        fprintf(stderr, ", drawn at %ux%u", tw, th);
    fprintf(stderr, "\n");
    return r;
}

static Ds *ds_get(uint32_t va, uint32_t w, uint32_t h)
{
    Ds *d, *victim = &s_ds[0];
    D3D11_TEXTURE2D_DESC td;
    uint32_t i;

    for (i = 0; i < MAX_DS; i++) {
        d = &s_ds[i];
        if (d->tex && d->va == va && d->w == w && d->h == h)
            return d;
        if (!d->tex || d->last_use < victim->last_use)
            victim = d;
    }
    d = victim;
    if (d->dsv) ID3D11DepthStencilView_Release(d->dsv);
    if (d->tex) ID3D11Texture2D_Release(d->tex);
    memset(d, 0, sizeof *d);
    if (d == s_cur_ds)
        s_cur_ds = NULL;
    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &d->tex))
            || FAILED(ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)d->tex,
                                                          NULL, &d->dsv))) {
        if (d->tex) ID3D11Texture2D_Release(d->tex);
        memset(d, 0, sizeof *d);
        return NULL;
    }
    d->va = va;
    d->w = w;
    d->h = h;
    /* A fresh depth buffer reads as far, like a cleared one. */
    ID3D11DeviceContext_ClearDepthStencilView(s_ctx, d->dsv,
            D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    return d;
}

/* Make s (and its depth surface) the current target. Leaving a surface writes
 * it back (queued, rt_queue); arriving at one reads it in, the first time in a
 * frame. Coming back to it later in the frame, the surface already holds what
 * memory would -- its own drawing, whose copy may not have reached memory. */
static int bind(const Nv2aSurface *s, const Nv2aRenderState *rs)
{
    Rt *r = rt_get(s);
    Ds *d = NULL;
    D3D11_VIEWPORT vp;

    if (!r)
        return 0;
    if (rs && rs->zeta_va)
        d = ds_get(rs->zeta_va, r->tw, r->th);
    if (r != s_cur_rt) {
        if (s_cur_rt)
            rt_queue(s_cur_rt);
        /* Shadow mode keeps the surface's own drawing from frame to frame
         * instead of reading the rasteriser's back in, so the comparison sees
         * this back end's frame and nothing of the other one. */
        if (nv2a_backend_shadow() ? r->fresh : r->frame != s_frame) {
            rt_complete(r);     /* memory first, should a copy be queued */
            rt_upload(r);
        }
        r->fresh = 0;
        r->frame = s_frame;
        s_cur_rt = r;
        s_cur_ds = NULL;            /* force the views to be set below */
    }
    r->last_use = ++s_use;
    if (d)
        d->last_use = s_use;
    if (d != s_cur_ds || !d) {
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &r->rtv, d ? d->dsv : NULL);
        s_cur_ds = d;
    }
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = (float)r->tw; vp.Height = (float)r->th;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    return 1;
}

/* ---- Textures ----------------------------------------------------------- */

typedef struct {
    uint32_t off, w, h, pitch, color, palette, cube, levels;
    uint64_t hash;
    uint32_t checked_frame, last_use;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} TexE;

#define MAX_TEX 512
static TexE s_tex[MAX_TEX];
static uint32_t *s_decode;
static size_t s_decode_n;

/* RECOMP_GPUBE_TEXSTATS=1: every 100 flips, which textures were decoded again
 * and why -- new to the cache, or changed in guest memory -- and what the
 * decoding cost. For finding what keeps the NV2A thread in the texture
 * decoder; changes nothing that is drawn. */
typedef struct { uint32_t off, w, h, color, levels, fresh, changed; double ms; } TexStat;
static uint32_t tex_levels(const Nv2aTexture *t);
#define MAX_TEXSTAT 128
static TexStat s_texstat[MAX_TEXSTAT];
static int s_texstat_n, s_texstat_on = -1;
static uint32_t s_texstat_lookups, s_texstat_hashes, s_texstat_flips;
static double s_texstat_hash_ms;

static int texstat_on(void)
{
    if (s_texstat_on < 0)
        s_texstat_on = getenv("RECOMP_GPUBE_TEXSTATS") != NULL;
    return s_texstat_on;
}

static double texstat_now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void texstat_note(const Nv2aTexture *t, int fresh, double ms)
{
    int i;
    for (i = 0; i < s_texstat_n; i++)
        if (s_texstat[i].off == t->offset && s_texstat[i].w == t->width
                && s_texstat[i].h == t->height && s_texstat[i].color == t->color)
            break;
    if (i == s_texstat_n) {
        if (s_texstat_n == MAX_TEXSTAT)
            return;
        memset(&s_texstat[i], 0, sizeof s_texstat[i]);
        s_texstat[i].off = t->offset; s_texstat[i].w = t->width;
        s_texstat[i].h = t->height; s_texstat[i].color = t->color;
        s_texstat[i].levels = tex_levels(t);
        s_texstat_n++;
    }
    if (fresh) s_texstat[i].fresh++; else s_texstat[i].changed++;
    s_texstat[i].ms += ms;
}

static int texstat_by_ms(const void *a, const void *b)
{
    double d = ((const TexStat *)b)->ms - ((const TexStat *)a)->ms;
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

static void texstat_report(void)
{
    double total = 0;
    uint32_t n = 0, mipped = 0;
    int i;

    if (++s_texstat_flips < 100)
        return;
    for (i = 0; i < s_texstat_n; i++) {
        total += s_texstat[i].ms;
        n += s_texstat[i].fresh + s_texstat[i].changed;
        mipped += s_texstat[i].levels > 1;
    }
    qsort(s_texstat, s_texstat_n, sizeof s_texstat[0], texstat_by_ms);
    fprintf(stderr, "[GPUBE-TEX] %u flips: %u lookups, %u hashed (%.1f ms), %u decoded "
                    "(%.1f ms, %.2f ms/flip), %d distinct, %u with mip levels\n",
            s_texstat_flips, s_texstat_lookups, s_texstat_hashes, s_texstat_hash_ms,
            n, total, total / s_texstat_flips, s_texstat_n, mipped);
    for (i = 0; i < s_texstat_n && i < 12; i++)
        fprintf(stderr, "[GPUBE-TEX]   %08X %4ux%-4u fmt %02X  mips %2u  new %3u  changed %3u"
                        "  %7.1f ms\n",
                s_texstat[i].off, s_texstat[i].w, s_texstat[i].h, s_texstat[i].color,
                s_texstat[i].levels, s_texstat[i].fresh, s_texstat[i].changed, s_texstat[i].ms);
    fflush(stderr);
    s_texstat_n = 0;
    s_texstat_flips = s_texstat_lookups = s_texstat_hashes = 0;
    s_texstat_hash_ms = 0;
}

static void tex_invalidate_range(uint32_t va, uint32_t bytes)
{
    uint32_t i;
    for (i = 0; i < MAX_TEX; i++)
        if (s_tex[i].tex && s_tex[i].off >= va && s_tex[i].off < va + bytes)
            s_tex[i].hash = 0, s_tex[i].checked_frame = 0;
}

static uint32_t texel_bytes(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1F:
        return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11:
    case 0x1C: case 0x1D:
        return 2;
    default:
        return 4;
    }
}

/* Swizzled and compressed textures take their size from the format and are
 * addressed in [0,1]; linear ones have a pitch and are addressed in texels.
 * Decided by the format, as the CPU rasteriser does (tex_size_from_format in
 * rc_texel), not by pitch: the pitch register keeps whatever the last linear
 * texture set, so a swizzled font page drawn after a 640-wide linear
 * background arrives with pitch 1280 -- and was scaled as if linear, every
 * glyph sampling a single texel. */
static int tex_normalized(const Nv2aTexture *t)
{
    return d3d8_format_is_swizzled(t->color) || d3d8_format_dxt_block_bytes(t->color);
}

/* The pitch that means something: 0 for a swizzled or compressed texture. */
static uint32_t tex_pitch(const Nv2aTexture *t)
{
    return tex_normalized(t) ? 0 : t->pitch;
}

/* Bytes of a w x h level of a swizzled or compressed texture. */
static uint32_t level_bytes(uint32_t color, uint32_t w, uint32_t h)
{
    if (color == 0x0C)                                      /* DXT1 */
        return ((w + 3) / 4) * ((h + 3) / 4) * 8;
    if (color == 0x0E || color == 0x0F)                     /* DXT3/5 */
        return ((w + 3) / 4) * ((h + 3) / 4) * 16;
    return w * h * texel_bytes(color);
}

/* Bytes of level 0 in guest memory. */
static uint32_t tex_bytes(const Nv2aTexture *t)
{
    if (tex_pitch(t))
        return t->pitch * t->height;
    return level_bytes(t->color, t->width, t->height);
}

/* RECOMP_GPUBE_MIPMAPS=0: level 0 only, sampled with one filter for
 * minifying and magnifying, as before the mip chain was uploaded. */
static int mips_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GPUBE_MIPMAPS");
        on = !(e && e[0] == '0');
    }
    return on;
}

static uint32_t mip_dim(uint32_t d, uint32_t level)
{
    return (d >> level) ? (d >> level) : 1;
}

/* Levels to upload: the format's MIPMAP_LEVELS for a swizzled or compressed
 * texture (each level swizzled on its own, packed after the one before, as
 * the cube face stride counts them), capped at a full chain. A linear
 * texture has level 0 only. Without the chain every distant floor and wall
 * sampled its full-size level: grainy, and shimmering as the camera moved,
 * where the console filters the title's own smaller levels. */
static uint32_t tex_levels(const Nv2aTexture *t)
{
    uint32_t n = 1, m = t->width > t->height ? t->width : t->height;
    if (!mips_on() || !tex_normalized(t) || t->levels <= 1)
        return 1;
    while ((m >> n) && n < t->levels)
        n++;
    return n;
}

/* Bytes of every uploaded level, for the range and change checks. */
static uint32_t tex_bytes_all(const Nv2aTexture *t)
{
    uint32_t l, n = tex_levels(t), bytes = 0;
    if (n == 1)
        return tex_bytes(t);
    for (l = 0; l < n; l++)
        bytes += level_bytes(t->color, mip_dim(t->width, l), mip_dim(t->height, l));
    return bytes;
}

/* FNV-1a over the texture's bytes; past 256 KB, over 8192 evenly spaced
 * 8-byte words, which catches a re-upload without reading every byte. */
static uint64_t hash_bytes(uint64_t h, uint32_t va, uint32_t bytes)
{
    const uint8_t *p = GUEST(va);
    uint32_t n = bytes / 8, step = n > 32768 ? n / 8192 : 1, i;

    for (i = 0; i < n; i += step) {
        uint64_t v;
        memcpy(&v, p + (size_t)i * 8, 8);
        h = (h ^ v) * 1099511628211ull;
    }
    return h;
}

static uint64_t tex_hash(const Nv2aTexture *t, uint32_t face_stride)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t f, faces = t->cube ? 6 : 1;

    for (f = 0; f < faces; f++)
        h = hash_bytes(h, t->offset + f * face_stride, tex_bytes_all(t));
    if (t->palette)
        h = hash_bytes(h, t->palette, 1024);
    return h ? h : 1;
}

/* Whole-texture decode for the common formats.
 *
 * nv2a_backend_decode_texture goes through the CPU rasteriser's sampler once
 * per texel -- border, wrap, format and a bit-by-bit swizzle deposit for each
 * one. In Driving, where a dozen textures are rewritten every frame (render
 * targets, the 2x-wide back buffer), that was 44 ms a frame on the NV2A
 * thread the title waits on.
 *
 * This produces the same texels from the same helpers (d3d8_swizzle.h, which
 * the sampler uses): the swizzle offset is deposit(x, mask_x) | deposit(y,
 * mask_y), so it is two small tables per texture instead of a loop per texel,
 * and the channel expansions are tables of d3d8_expand_channel. Formats not
 * listed here still go through the sampler. RECOMP_GPUBE_TEXCHECK=1 decodes
 * every texture both ways and reports any texel that differs.
 *
 * Returns 1 decoded, 0 not handled here, -1 the sampler would fail too (a
 * palette format without a palette). */
static uint32_t s_xo[4096], s_yo[4096];
static uint8_t s_e4[16], s_e5[32], s_e6[64];

static int fast_decode(const Nv2aTexture *t, uint32_t *out)
{
    const uint8_t *base = GUEST(t->offset);
    const uint32_t *pal = NULL;
    uint32_t w = t->width, h = t->height, fmt = t->color, x, y;
    int swz;

    if (!s_e6[63]) {
        for (x = 0; x < 16; x++) s_e4[x] = (uint8_t)d3d8_expand_channel(x, 4);
        for (x = 0; x < 32; x++) s_e5[x] = (uint8_t)d3d8_expand_channel(x, 5);
        for (x = 0; x < 64; x++) s_e6[x] = (uint8_t)d3d8_expand_channel(x, 6);
    }
    if (t->cube)
        return 0;
    if (d3d8_format_dxt_block_bytes(fmt)) {
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
                d3d8_dxt_decode_texel(base, fmt, x, y, w, &out[(size_t)y * w + x]);
        return 1;
    }

    /* The swizzled formats decode as their linear twins (the sampler's
     * linear_twin); 0x0B, the palette format, is only ever swizzled. */
    swz = d3d8_format_is_swizzled(fmt);
    if (swz) {
        switch (fmt) {
        case 0x02: fmt = 0x10; break;
        case 0x03: fmt = 0x1C; break;
        case 0x04: fmt = 0x1D; break;
        case 0x05: fmt = 0x11; break;
        case 0x06: fmt = 0x12; break;
        case 0x07: fmt = 0x1E; break;
        case 0x0B: break;
        default:   return 0;
        }
    } else if (fmt != 0x10 && fmt != 0x11 && fmt != 0x12 && fmt != 0x1C
               && fmt != 0x1D && fmt != 0x1E) {
        return 0;
    }
    if (fmt == 0x0B) {
        if (!t->palette)
            return -1;
        pal = (const uint32_t *)GUEST(t->palette);
    }
    if (swz) {
        uint32_t mx, my;
        xbox_swizzle_masks(w, h, &mx, &my);
        for (x = 0; x < w; x++) s_xo[x] = swizzle_deposit(x, mx);
        for (y = 0; y < h; y++) s_yo[y] = swizzle_deposit(y, my);
    }

    for (y = 0; y < h; y++) {
        const uint8_t *row = swz ? base : base + (size_t)y * t->pitch;
        uint32_t *o = out + (size_t)y * w;
        for (x = 0; x < w; x++) {
            uint32_t i = swz ? (s_xo[x] | s_yo[y]) : x, v;
            switch (fmt) {
            case 0x12:                                      /* A8R8G8B8 */
                o[x] = ((const uint32_t *)row)[i];
                break;
            case 0x1E:                                      /* X8R8G8B8 */
                o[x] = ((const uint32_t *)row)[i] | 0xFF000000u;
                break;
            case 0x11:                                      /* R5G6B5 */
                v = ((const uint16_t *)row)[i];
                o[x] = 0xFF000000u | ((uint32_t)s_e5[(v >> 11) & 0x1F] << 16)
                     | ((uint32_t)s_e6[(v >> 5) & 0x3F] << 8) | s_e5[v & 0x1F];
                break;
            case 0x10:                                      /* A1R5G5B5 */
            case 0x1C:                                      /* X1R5G5B5 */
                v = ((const uint16_t *)row)[i];
                o[x] = (fmt == 0x1C || (v & 0x8000u) ? 0xFF000000u : 0u)
                     | ((uint32_t)s_e5[(v >> 10) & 0x1F] << 16)
                     | ((uint32_t)s_e5[(v >> 5) & 0x1F] << 8) | s_e5[v & 0x1F];
                break;
            case 0x1D:                                      /* A4R4G4B4 */
                v = ((const uint16_t *)row)[i];
                o[x] = ((uint32_t)s_e4[(v >> 12) & 0xF] << 24)
                     | ((uint32_t)s_e4[(v >> 8) & 0xF] << 16)
                     | ((uint32_t)s_e4[(v >> 4) & 0xF] << 8) | s_e4[v & 0xF];
                break;
            default:                                        /* 0x0B, P8 */
                o[x] = pal[row[i]];
                break;
            }
        }
    }
    return 1;
}

static uint32_t *s_check;
static size_t s_check_n;
static uint32_t s_check_tex, s_check_bad;

static int decode_into(const Nv2aTexture *t)
{
    static int check = -1;
    size_t n = (size_t)t->width * t->height;
    int r;

    if (s_decode_n < n) {
        free(s_decode);
        s_decode_n = n;
        s_decode = (uint32_t *)malloc(s_decode_n * 4);
        if (!s_decode) {
            s_decode_n = 0;
            return 0;
        }
    }
    r = fast_decode(t, s_decode);
    if (r < 0)
        return 0;
    if (!r)
        return nv2a_backend_decode_texture(t, s_decode);

    if (check < 0)
        check = getenv("RECOMP_GPUBE_TEXCHECK") != NULL;
    if (check) {
        size_t i;
        if (s_check_n < n) {
            free(s_check);
            s_check_n = n;
            s_check = (uint32_t *)malloc(n * 4);
            if (!s_check) {
                s_check_n = 0;
                return 1;
            }
        }
        s_check_tex++;
        if (nv2a_backend_decode_texture(t, s_check) && memcmp(s_check, s_decode, n * 4)) {
            for (i = 0; i < n && s_check[i] == s_decode[i]; i++)
                ;
            if (s_check_bad++ < 32)
                fprintf(stderr, "[GPUBE-TEXCHECK] MISMATCH %08X %ux%u fmt %02X pitch %u: "
                                "texel (%u,%u) fast %08X sampler %08X\n",
                        t->offset, t->width, t->height, t->color, t->pitch,
                        (unsigned)(i % t->width), (unsigned)(i / t->width),
                        s_decode[i], s_check[i]);
            memcpy(s_decode, s_check, n * 4);     /* draw what the sampler gives */
        }
    }
    return 1;
}

/* Whether [va, va + bytes) is guest memory: the 64 MB of RAM or its view in
 * the contiguous window at 0x80000000. A texture descriptor the executor
 * decoded from a desynchronised pushbuffer can name any address, and hashing
 * or decoding it then faulted the whole process (seen in the driving
 * missions); such a texture is skipped instead, as the executor's own reads
 * are bounds-checked. */
static int guest_range_ok(uint32_t va, uint32_t bytes)
{
    const uint32_t ram = 64u * 1024u * 1024u;
    uint32_t base = va >= 0x80000000u ? 0x80000000u : 0u;
    uint32_t off = va - base;
    return bytes <= ram && off <= ram - bytes;
}

static int tex_range_ok(const Nv2aTexture *t)
{
    uint32_t bytes = tex_bytes_all(t), faces = t->cube ? 6u : 1u;
    if (t->cube) {
        uint32_t stride = nv2a_backend_cube_face_stride(t);
        if (!guest_range_ok(t->offset, stride * 5u + bytes))
            return 0;
    } else if (!guest_range_ok(t->offset, bytes * faces)) {
        return 0;
    }
    return !t->palette || guest_range_ok(t->palette, 1024);
}

/* Whether texture t reads surface r exactly as it is: the same bytes, layout
 * and size, in A8R8G8B8, whose decode (fast_decode) copies texels unchanged
 * into the same B8G8R8A8 a surface is drawn in. Not the current target (D3D11
 * cannot sample what it draws into), and only a surface made current this
 * frame, which therefore holds memory's contents plus its own drawing. Any
 * other view of a surface's memory -- Nightfire samples a linear 16-bit one as
 * a swizzled texture -- reads guest memory, as before. */
static int rt_samples_as(const Rt *r, const Nv2aTexture *t)
{
    if (r == s_cur_rt || r->frame != s_frame || nv2a_backend_shadow())
        return 0;
    if (t->cube || t->offset != r->va || t->width != r->w || t->height != r->h
            || r->bpp != 4)
        return 0;
    return (t->color == 0x06 && r->swz)                         /* swizzled */
        || (t->color == 0x12 && !r->swz && t->pitch == r->pitch); /* linear */
}

/* A texture over a surface's memory: the surface itself when that reads the
 * same (rt_samples_as), else NULL after its queued copy, if any, has reached
 * memory for the texture to read. */
static ID3D11ShaderResourceView *tex_from_surface(const Nv2aTexture *t)
{
    uint32_t i, lo = t->offset, hi;

    hi = lo + (t->cube ? 6u * nv2a_backend_cube_face_stride(t) : tex_bytes_all(t));
    for (i = 0; i < MAX_RT; i++) {
        Rt *r = &s_rt[i];
        if (!r->tex || r->va >= hi || r->va + rt_bytes(r) <= lo)
            continue;
        if (rt_samples_as(r, t)) {
            if (!r->srv && FAILED(ID3D11Device_CreateShaderResourceView(
                    s_dev, (ID3D11Resource *)r->tex, NULL, &r->srv)))
                return NULL;
            return r->srv;
        }
        if (r->big)
            rt_download(r);
        else
            rt_complete(r);
    }
    return NULL;
}

static ID3D11ShaderResourceView *tex_get(const Nv2aTexture *t)
{
    TexE *e = NULL, *victim = &s_tex[0];
    uint32_t i, stride = 0, cube = t->cube ? 1 : 0, levels = tex_levels(t);
    uint64_t h;
    int fresh;
    double t0 = 0;

    if (!t->width || !t->height || t->width > 4096 || t->height > 4096)
        return NULL;
    if (!tex_range_ok(t))
        return NULL;
    {
        ID3D11ShaderResourceView *v = tex_from_surface(t);
        if (v)
            return v;
    }
    if (texstat_on())
        s_texstat_lookups++;
    if (cube && t->width != t->height)
        cube = 0;
    for (i = 0; i < MAX_TEX; i++) {
        TexE *c = &s_tex[i];
        if (c->tex && c->off == t->offset && c->w == t->width && c->h == t->height
                && c->pitch == tex_pitch(t) && c->color == t->color
                && c->palette == t->palette && c->cube == cube && c->levels == levels) {
            e = c;
            break;
        }
        if (!c->tex || c->last_use < victim->last_use)
            victim = c;
    }
    if (e && e->checked_frame == s_frame) {
        e->last_use = ++s_use;
        return e->srv;
    }
    if (cube)
        stride = nv2a_backend_cube_face_stride(t);
    if (texstat_on())
        t0 = texstat_now_ms();
    {
        Nv2aTexture k = *t;
        k.cube = cube;
        h = tex_hash(&k, stride);
    }
    if (texstat_on()) {
        s_texstat_hashes++;
        s_texstat_hash_ms += texstat_now_ms() - t0;
    }
    if (e && e->hash == h) {
        e->checked_frame = s_frame;
        e->last_use = ++s_use;
        return e->srv;
    }
    fresh = !e;
    if (texstat_on())
        t0 = texstat_now_ms();
    if (!e) {
        D3D11_TEXTURE2D_DESC td;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        e = victim;
        if (e->srv) ID3D11ShaderResourceView_Release(e->srv);
        if (e->tex) ID3D11Texture2D_Release(e->tex);
        memset(e, 0, sizeof *e);
        memset(&td, 0, sizeof td);
        td.Width = t->width;
        td.Height = t->height;
        td.MipLevels = levels;
        td.ArraySize = cube ? 6 : 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
        memset(&sd, 0, sizeof sd);
        sd.Format = td.Format;
        if (cube) {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            sd.TextureCube.MipLevels = levels;
        } else {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = levels;
        }
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &e->tex))
                || FAILED(ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)e->tex,
                                                                &sd, &e->srv))) {
            if (e->tex) ID3D11Texture2D_Release(e->tex);
            memset(e, 0, sizeof *e);
            return NULL;
        }
        e->off = t->offset; e->w = t->width; e->h = t->height;
        e->pitch = tex_pitch(t); e->color = t->color;
        e->palette = t->palette; e->cube = cube; e->levels = levels;
    }
    {
        /* Each face, then each level after the one before it; subresource
         * level + face * levels (D3D11CalcSubresource). */
        uint32_t f, l;
        for (f = 0; f < (cube ? 6u : 1u); f++) {
            Nv2aTexture lv = *t;
            lv.offset = t->offset + f * stride;
            lv.cube = 0;
            for (l = 0; l < levels; l++) {
                lv.width = mip_dim(t->width, l);
                lv.height = mip_dim(t->height, l);
                if (!decode_into(&lv))
                    return NULL;
                ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)e->tex,
                                                      l + f * levels, NULL, s_decode,
                                                      lv.width * 4, 0);
                lv.offset += level_bytes(t->color, lv.width, lv.height);
            }
        }
    }
    e->hash = h;
    e->checked_frame = s_frame;
    e->last_use = ++s_use;
    if (texstat_on())
        texstat_note(t, fresh, texstat_now_ms() - t0);
    return e->srv;
}

/* ---- State objects ------------------------------------------------------ */

static D3D11_BLEND blend_factor(uint32_t gl)
{
    switch (gl) {
    case 0:      return D3D11_BLEND_ZERO;
    case 1:      return D3D11_BLEND_ONE;
    case 0x300:  return D3D11_BLEND_SRC_COLOR;
    case 0x301:  return D3D11_BLEND_INV_SRC_COLOR;
    case 0x302:  return D3D11_BLEND_SRC_ALPHA;
    case 0x303:  return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x304:  return D3D11_BLEND_DEST_ALPHA;
    case 0x305:  return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x306:  return D3D11_BLEND_DEST_COLOR;
    case 0x307:  return D3D11_BLEND_INV_DEST_COLOR;
    case 0x308:  return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return D3D11_BLEND_ONE;
    }
}

/* The alpha half of the blend may not name a colour factor. */
static D3D11_BLEND alpha_factor(D3D11_BLEND b)
{
    switch (b) {
    case D3D11_BLEND_SRC_COLOR:      return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR:  return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR:     return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
    default:                         return b;
    }
}

static D3D11_BLEND_OP blend_op(uint32_t gl)
{
    switch (gl) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

static D3D11_COMPARISON_FUNC cmp_func(uint32_t gl)
{
    return (gl >= 0x200 && gl <= 0x207) ? (D3D11_COMPARISON_FUNC)(gl - 0x200 + 1)
                                        : D3D11_COMPARISON_ALWAYS;
}

static D3D11_STENCIL_OP stencil_op(uint32_t gl)
{
    switch (gl) {
    case 0:      return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

static UINT8 write_mask(uint32_t m)
{
    return (UINT8)(((m & 0x00010000u) ? D3D11_COLOR_WRITE_ENABLE_RED : 0)
                 | ((m & 0x00000100u) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0)
                 | ((m & 0x00000001u) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0)
                 | ((m & 0x01000000u) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0));
}

/* Small caches keyed by the descriptor bytes: a frame uses a handful of
 * distinct states, and creating one per draw would dominate. */
#define STATE_CACHE 256
typedef struct { D3D11_BLEND_DESC d; ID3D11BlendState *s; } BlendE;
typedef struct { D3D11_DEPTH_STENCIL_DESC d; ID3D11DepthStencilState *s; } DepthE;
typedef struct { D3D11_SAMPLER_DESC d; ID3D11SamplerState *s; } SampE;
static BlendE s_blend[STATE_CACHE]; static int s_nblend;
static DepthE s_depth[STATE_CACHE]; static int s_ndepth;
static SampE  s_samp[STATE_CACHE];  static int s_nsamp;

static ID3D11BlendState *blend_state(const Nv2aRenderState *rs)
{
    D3D11_BLEND_DESC d;
    D3D11_RENDER_TARGET_BLEND_DESC *t = &d.RenderTarget[0];
    int i;

    memset(&d, 0, sizeof d);
    t->BlendEnable = rs->blend_enable ? TRUE : FALSE;
    t->SrcBlend = blend_factor(rs->blend_src);
    t->DestBlend = blend_factor(rs->blend_dst);
    t->BlendOp = blend_op(rs->blend_eq);
    t->SrcBlendAlpha = alpha_factor(t->SrcBlend);
    t->DestBlendAlpha = alpha_factor(t->DestBlend);
    t->BlendOpAlpha = t->BlendOp;
    t->RenderTargetWriteMask = write_mask(rs->color_mask);
    if (!t->BlendEnable) {
        t->SrcBlend = t->SrcBlendAlpha = D3D11_BLEND_ONE;
        t->DestBlend = t->DestBlendAlpha = D3D11_BLEND_ZERO;
        t->BlendOp = t->BlendOpAlpha = D3D11_BLEND_OP_ADD;
    }
    for (i = 0; i < s_nblend; i++)
        if (!memcmp(&s_blend[i].d, &d, sizeof d))
            return s_blend[i].s;
    if (s_nblend == STATE_CACHE)
        return s_blend[0].s;
    s_blend[s_nblend].d = d;
    if (FAILED(ID3D11Device_CreateBlendState(s_dev, &d, &s_blend[s_nblend].s)))
        return NULL;
    return s_blend[s_nblend++].s;
}

static ID3D11DepthStencilState *depth_state(const Nv2aRenderState *rs, int have_ds)
{
    D3D11_DEPTH_STENCIL_DESC d;
    int i;

    memset(&d, 0, sizeof d);
    d.DepthEnable = (have_ds && rs->depth_test_enable) ? TRUE : FALSE;
    d.DepthWriteMask = rs->depth_write ? D3D11_DEPTH_WRITE_MASK_ALL
                                       : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = cmp_func(rs->depth_func);
    d.StencilEnable = (have_ds && rs->stencil_enable) ? TRUE : FALSE;
    d.StencilReadMask = (UINT8)rs->stencil_func_mask;
    d.StencilWriteMask = (UINT8)rs->stencil_write_mask;
    d.FrontFace.StencilFunc = cmp_func(rs->stencil_func);
    d.FrontFace.StencilFailOp = stencil_op(rs->stencil_fail);
    d.FrontFace.StencilDepthFailOp = stencil_op(rs->stencil_zfail);
    d.FrontFace.StencilPassOp = stencil_op(rs->stencil_zpass);
    d.BackFace = d.FrontFace;
    if (!d.DepthEnable)
        d.DepthFunc = D3D11_COMPARISON_ALWAYS;
    for (i = 0; i < s_ndepth; i++)
        if (!memcmp(&s_depth[i].d, &d, sizeof d))
            return s_depth[i].s;
    if (s_ndepth == STATE_CACHE)
        return s_depth[0].s;
    s_depth[s_ndepth].d = d;
    if (FAILED(ID3D11Device_CreateDepthStencilState(s_dev, &d, &s_depth[s_ndepth].s)))
        return NULL;
    return s_depth[s_ndepth++].s;
}

static D3D11_TEXTURE_ADDRESS_MODE addr_mode(uint32_t nv)
{
    switch (nv) {
    case 1:  return D3D11_TEXTURE_ADDRESS_WRAP;
    case 2:  return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 4:  return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_CLAMP;
    }
}

/* RECOMP_GPUBE_ANISO=n (2..16): anisotropic filtering for what the title
 * filters with tent and mip levels, so a floor seen at a flat angle stays
 * sharp into the distance. The console has none here. The caller sets it only
 * for the enhanced game (src/main.c). */
static uint32_t aniso(void)
{
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("RECOMP_GPUBE_ANISO");
        n = e ? atoi(e) : 1;
        if (n < 2 || nv2a_backend_shadow())
            n = 1;
        if (n > 16)
            n = 16;
        if (n > 1)
            fprintf(stderr, "[GPUBE] anisotropic filtering %dx\n", n);
    }
    return (uint32_t)n;
}

static ID3D11SamplerState *sampler(const Nv2aTexture *t)
{
    D3D11_SAMPLER_DESC d;
    /* Without the mip chain (RECOMP_GPUBE_MIPMAPS=0) as rc_texel does it:
     * tent (bilinear) for MAG 2, or MIN 2/4/6; nearest otherwise. */
    uint32_t mag = (t->filter >> 24) & 0xF, min = (t->filter >> 16) & 0xFF;
    int i;

    memset(&d, 0, sizeof d);
    if (mips_on()) {
        /* As the title set them. MIN: 1 box and 2 tent at LOD 0; 3/4 box/tent
         * from the nearest level, 5/6 between the two nearest; 7 convolution
         * at LOD 0, taken as tent. MAG: 1 box, 2 tent, 4 convolution (tent).
         * The bias is MIPMAP_LOD_BIAS, signed 5.8 fixed point in bits 0-12. */
        int min_lin = min == 2 || min == 4 || min == 6 || min == 7;
        int mag_lin = mag == 2 || mag == 4;
        int mip_lin = min == 5 || min == 6;
        d.Filter = (D3D11_FILTER)((min_lin ? 0x10 : 0) | (mag_lin ? 0x4 : 0)
                                  | (mip_lin ? 0x1 : 0));
        d.MipLODBias = (float)((int32_t)(t->filter << 19) >> 19) / 256.0f;
        d.MaxLOD = (min >= 3 && min <= 6) ? D3D11_FLOAT32_MAX : 0.0f;
        if (aniso() > 1 && (min == 4 || min == 6) && mag_lin) {
            d.Filter = D3D11_FILTER_ANISOTROPIC;
            d.MaxAnisotropy = aniso();
        }
    } else {
        d.Filter = (mag == 2 || min == 2 || min == 4 || min == 6)
                 ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
        d.MaxLOD = D3D11_FLOAT32_MAX;
    }
    d.AddressU = addr_mode(t->addr_u);
    d.AddressV = addr_mode(t->addr_v);
    d.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    d.ComparisonFunc = D3D11_COMPARISON_NEVER;
    for (i = 0; i < s_nsamp; i++)
        if (!memcmp(&s_samp[i].d, &d, sizeof d))
            return s_samp[i].s;
    if (s_nsamp == STATE_CACHE)
        return s_samp[0].s;
    s_samp[s_nsamp].d = d;
    if (FAILED(ID3D11Device_CreateSamplerState(s_dev, &d, &s_samp[s_nsamp].s)))
        return NULL;
    return s_samp[s_nsamp++].s;
}

/* ---- Back end calls ----------------------------------------------------- */

static float zscale(const Nv2aRenderState *rs)
{
    /* SET_CLIP_MAX holds the depth buffer's full scale: 0xFFFFFF for Z24,
     * 0xFFFF for Z16. */
    return (rs && rs->depth_max > 1.0f) ? rs->depth_max : 16777215.0f;
}

/* The rasteriser state for the title's polygon offset (SET_POLY_OFFSET_FILL_
 * ENABLE, SET_POLYGON_OFFSET_SCALE_FACTOR/_BIAS), with glPolygonOffset's
 * meaning: factor times the depth slope plus units of the depth buffer's
 * smallest step. Ours is D24 whatever the title's, so a unit is 2^-24 as on
 * the console's Z24, and 256 of them for a Z16 surface. Without it a decal on
 * a wall -- the shadow behind the barrels in The Exchange's courtyard -- sat
 * at the wall's own depth and the two took turns winning the depth test.
 * RECOMP_GPUBE_POLY_OFFSET=0 leaves it out, as before. */
static ID3D11RasterizerState *raster_state(const Nv2aRenderState *rs)
{
    static struct { INT bias; float slope; ID3D11RasterizerState *s; } c[16];
    static int n, on = -1, shown;
    D3D11_RASTERIZER_DESC rd;
    INT bias;
    float slope;
    int i;

    if (on < 0) {
        const char *e = getenv("RECOMP_GPUBE_POLY_OFFSET");
        on = !(e && e[0] == '0');
    }
    if (!on || !rs || !rs->poly_offset_fill)
        return s_rs;
    bias = (INT)lrintf(rs->poly_offset_units * (zscale(rs) > 70000.0f ? 1.0f : 256.0f));
    /* Per pixel of the drawn surface: a large one's pixel spans 1/sc of the
     * title's, and so 1/sc of the depth change the factor is applied to --
     * the larger of the two factors, so the offset never comes out short. */
    slope = rs->poly_offset_factor
          * (s_cur_rt ? (s_cur_rt->fx > s_cur_rt->fy ? s_cur_rt->fx : s_cur_rt->fy) : 1.0f);
    if (!bias && slope == 0.0f)
        return s_rs;
    for (i = 0; i < n; i++)
        if (c[i].bias == bias && c[i].slope == slope)
            return c[i].s;
    if (n == 16)
        return s_rs;
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    rd.ScissorEnable = TRUE;        /* the window clip (raster_clip) */
    rd.DepthBias = bias;
    rd.SlopeScaledDepthBias = slope;
    if (FAILED(ID3D11Device_CreateRasterizerState(s_dev, &rd, &c[n].s)))
        return s_rs;
    if (shown++ < 4)
        fprintf(stderr, "[GPUBE] polygon offset: factor %g units %g (depth bias %d)\n",
                slope, rs->poly_offset_units, bias);
    c[n].bias = bias;
    c[n].slope = slope;
    return c[n++].s;
}

/* The rasterizer state for a draw, with the scissor set to the title's window
 * clip (SET_WINDOW_CLIP, Nv2aRenderState.window_*), else to the whole surface.
 * Split-screen sets it to one player's half before each of that player's
 * passes; without it their triangles reached into the other half. In the
 * surface's pixels times its scale. RECOMP_GPUBE_WINDOW_CLIP=0 leaves it out
 * (the whole surface always). */
static ID3D11RasterizerState *raster_clip(const Nv2aRenderState *rs)
{
    static int on = -1;
    D3D11_RECT sr;
    float fx = s_cur_rt ? s_cur_rt->fx : 1.0f, fy = s_cur_rt ? s_cur_rt->fy : 1.0f;
    LONG w = s_cur_rt ? (LONG)s_cur_rt->tw : 16384;
    LONG h = s_cur_rt ? (LONG)s_cur_rt->th : 16384;

    if (on < 0) {
        const char *e = getenv("RECOMP_GPUBE_WINDOW_CLIP");
        on = !(e && e[0] == '0');
    }
    sr.left = 0; sr.top = 0; sr.right = w; sr.bottom = h;
    if (on && rs && rs->window_clip) {
        sr.left = (LONG)lrintf((float)rs->window_x0 * fx);
        sr.top = (LONG)lrintf((float)rs->window_y0 * fy);
        sr.right = (LONG)lrintf((float)rs->window_x1 * fx);
        sr.bottom = (LONG)lrintf((float)rs->window_y1 * fy);
        if (sr.right > w) sr.right = w;
        if (sr.bottom > h) sr.bottom = h;
    }
    ID3D11DeviceContext_RSSetScissorRects(s_ctx, 1, &sr);
    return raster_state(rs);
}

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    if (s_log_draws)
        fprintf(stderr, "[GPUBE-DRAW] #%u -> %08X pitch %u clip %ux%u %s  clear %X argb %08X\n",
                s_log_seq++, s->color_va, s->pitch, s->width, s->height,
                SURF_SWZ(s) ? "swizzled" : "linear", flags, argb);
    if (!device_up() || !bind(s, rs))
        return;
    if (flags & 0xF0) {
        float c[4] = { ((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f,
                       (argb & 0xFF) / 255.0f, ((argb >> 24) & 0xFF) / 255.0f };
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, s_cur_rt->rtv, c);
        s_cur_rt->dirty = s_cur_rt->drawn = 1;
    }
    if ((flags & 0x3) && s_cur_ds) {
        UINT f = ((flags & 1) ? D3D11_CLEAR_DEPTH : 0) | ((flags & 2) ? D3D11_CLEAR_STENCIL : 0);
        float depth;
        UINT8 st;
        if (zscale(rs) > 70000.0f) {
            depth = (float)(zstencil >> 8) / 16777215.0f;
            st = (UINT8)(zstencil & 0xFF);
        } else {
            depth = (float)(zstencil & 0xFFFF) / 65535.0f;
            st = 0;
        }
        ID3D11DeviceContext_ClearDepthStencilView(s_ctx, s_cur_ds->dsv, f, depth, st);
    }
}

static void set_output_state(const Nv2aRenderState *rs)
{
    float bf[4];
    bf[0] = ((rs->blend_color >> 16) & 0xFF) / 255.0f;
    bf[1] = ((rs->blend_color >> 8) & 0xFF) / 255.0f;
    bf[2] = (rs->blend_color & 0xFF) / 255.0f;
    bf[3] = ((rs->blend_color >> 24) & 0xFF) / 255.0f;
    ID3D11DeviceContext_OMSetBlendState(s_ctx, blend_state(rs), bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, depth_state(rs, s_cur_ds != NULL),
                                               rs->stencil_ref & 0xFF);
}

/* The title's W-buffer (Nv2aRenderState.z_perspective): depth from each
 * pixel's w, mapped from [SET_CLIP_MIN, SET_CLIP_MAX], as the NV2A stores it,
 * instead of the z the vertex carries. Nightfire's main game sets it
 * (SET_CONTROL0 0x00110001). Taking z made distant surfaces that nearly
 * coincide -- the snow-crusted wall behind the barrels in The Exchange's
 * courtyard and what lies on it -- trade places as the camera moved, where
 * the console's depth, linear in distance, still told them apart. Writing
 * depth from the pixel shader costs the early depth test for these draws.
 * RECOMP_GPUBE_WBUFFER=0 keeps z, as before. */
static int wbuf_on(const Nv2aRenderState *rs)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GPUBE_WBUFFER");
        on = !(e && e[0] == '0');
    }
    return on && rs && rs->z_perspective && rs->depth_max > 0.0f
        && s_ps_tex_w && s_ps_col_w && s_ps_full_w;
}

/* wd for wbuf_on. The main game sets SET_CLIP_MIN = SET_CLIP_MAX = 16777215
 * (see the near plane in nv2a_pb_exec.c), a range with nothing in it; w is
 * then mapped from [0, clip max]. The first batches' w and z ranges go to the
 * log, to see what units the title's w comes in. */
static void wbuf_fill(Consts *k, const Nv2aRenderState *rs, const Nv2aBatch *b)
{
    static int shown;
    float lo_clip;
    memset(k->wd, 0, sizeof k->wd);
    if (!wbuf_on(rs))
        return;
    lo_clip = rs->depth_max > rs->depth_min ? rs->depth_min : 0.0f;
    k->wd[0] = lo_clip;
    k->wd[1] = 1.0f / (rs->depth_max - lo_clip);
    if (shown < 8 && b->count >= 30) {
        float wlo = 1e30f, whi = 0.0f, zlo = 1e30f, zhi = -1e30f;
        uint32_t i;
        for (i = 0; i < b->count; i++) {
            float r = b->full ? b->full[i].rhw : b->vertices[i].rhw;
            float z = b->full ? b->full[i].z : b->vertices[i].z;
            float w = r > 0.0f ? 1.0f / r : 0.0f;
            if (w < wlo) wlo = w;
            if (w > whi) whi = w;
            if (z < zlo) zlo = z;
            if (z > zhi) zhi = z;
        }
        fprintf(stderr, "[GPUBE] W-buffer: clip %g..%g (mapped from %g), batch of %u: "
                        "w %g..%g  z %g..%g\n", rs->depth_min, rs->depth_max, lo_clip,
                b->count, wlo, whi, zlo, zhi);
        shown++;
    }
}

static void fill_vp_alpha(Consts *k, const Nv2aRenderState *rs)
{
    k->vp[0] = 2.0f / (float)s_cur_rt->w;
    k->vp[1] = -2.0f / (float)s_cur_rt->h;
    k->vp[2] = 1.0f / zscale(rs);
    k->vp[3] = 0.0f;
    k->tx[0] = k->tx[1] = 0.0f;
    k->tx[2] = (float)(rs->alpha_ref & 0xFF);
    k->tx[3] = rs->alpha_test_enable && rs->alpha_func >= 0x200 && rs->alpha_func <= 0x207
             ? (float)(rs->alpha_func - 0x200) : 7.0f;
}

static void upload_cb(ID3D11Buffer *cb, const void *data, size_t n)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)cb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, data, n);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)cb, 0);
    }
}

/* Draws by path since the last shadow report: 0 stage 0 textured, 1 untextured,
 * 2 register combiners, 3 a texture the simple path could not get, 4 a
 * combiner stage texture it could not get. */
static uint32_t s_path[5];

static void log_draw(const char *path, const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aRenderState *rs = b->state;
    uint32_t st;

    fprintf(stderr, "[GPUBE-DRAW] #%u -> %08X pitch %u clip %ux%u %s  ", s_log_seq, s->color_va,
            s->pitch, s->width, s->height,
            SURF_SWZ(s) ? "swizzled" : "linear");
    fprintf(stderr, "%s %u verts  alpha %u/%X/%u  blend %u %X %X eq %X  "
                    "depth %u/%X/%u  mask %08X  stencil %u",
            path, b->count, rs->alpha_test_enable, rs->alpha_func, rs->alpha_ref,
            rs->blend_enable, rs->blend_src, rs->blend_dst, rs->blend_eq,
            rs->depth_test_enable, rs->depth_func, rs->depth_write, rs->color_mask,
            rs->stencil_enable);
    if (b->full) {
        const Nv2aVertexFull *v = b->full;
        fprintf(stderr, "\n[GPUBE-DRAW]     rc: stages %u final %08X %08X program %08X  "
                        "v0 pos %.1f %.1f %.4f rhw %.4f d0 %.2f %.2f %.2f %.2f",
                b->combiner->control & 0xFF, b->combiner->final0, b->combiner->final1,
                b->combiner->stage_program, v->x, v->y, v->z, v->rhw,
                v->d0[0], v->d0[1], v->d0[2], v->d0[3]);
        for (st = 0; st < 4; st++) {
            const Nv2aTexture *t = &b->stage[st];
            if (!((b->stage_valid >> st) & 1))
                continue;
            fprintf(stderr, "\n[GPUBE-DRAW]     stage %u: %08X %ux%u fmt %02X pitch %u addr %u/%u "
                            "filter %08X  v0 tc %.4f %.4f %.4f %.4f  v1 tc %.4f %.4f",
                    st, t->offset, t->width, t->height, t->color, t->pitch, t->addr_u,
                    t->addr_v, t->filter, v[0].tc[st][0], v[0].tc[st][1], v[0].tc[st][2],
                    v[0].tc[st][3], v[1].tc[st][0], v[1].tc[st][1]);
        }
    } else if (b->texture) {
        const Nv2aTexture *t = b->texture;
        fprintf(stderr, "\n[GPUBE-DRAW]     tex %08X %ux%u fmt %02X pitch %u  v0 uv %.2f %.2f",
                t->offset, t->width, t->height, t->color, t->pitch,
                b->vertices[0].u, b->vertices[0].v);
    }
    fputc('\n', stderr);
    s_log_seq++;
}

/* The register-combiner path (Nv2aBatch.full). */
/* Append a draw's vertices to the vertex buffer and say where they start.
 * Each draw used to map the whole buffer with DISCARD, hundreds of times a
 * frame; the driver keeps only so many renamed copies of a buffer, and past
 * that it waits for the GPU -- that wait was a fifth of this thread's time in
 * the driving missions. Appending with NO_OVERWRITE never touches what a
 * queued draw still reads, and DISCARD comes only when the buffer is full. */
static int vb_put(const void *src, UINT bytes, UINT *off)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;

    if (s_vb_pos + bytes > s_vb_bytes) {
        mode = D3D11_MAP_WRITE_DISCARD;
        s_vb_pos = 0;
    }
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0, mode, 0, &m)))
        return 0;
    memcpy((uint8_t *)m.pData + s_vb_pos, src, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
    *off = s_vb_pos;
    s_vb_pos = (s_vb_pos + bytes + 15u) & ~15u;
    return 1;
}

/* The combiner setup and stage textures of a combiner batch, for the full
 * pixel shader (draw_full, draw_program). */
static void full_consts(const Nv2aBatch *b, FullConsts *pf, ID3D11ShaderResourceView *srv2[4],
                        ID3D11ShaderResourceView *srvc[4], ID3D11SamplerState *smp[4])
{
    const Nv2aCombiner *rc = b->combiner;
    FullConsts f;
    uint32_t st, i;

    memset(&f, 0, sizeof f);
    for (i = 0; i < 8; i++) {
        f.cw[i][0] = rc->color_icw[i];
        f.cw[i][1] = rc->alpha_icw[i];
        f.cw[i][2] = rc->color_ocw[i];
        f.cw[i][3] = rc->alpha_ocw[i];
        nv2a_rc_unpack(rc->factor0[i], f.fac0[i]);
        nv2a_rc_unpack(rc->factor1[i], f.fac1[i]);
    }
    f.fin[0] = rc->final0;
    f.fin[1] = rc->final1;
    f.fin[2] = rc->control;
    f.fin[3] = rc->stage_program;
    nv2a_rc_unpack(rc->final_c0, f.fc0);
    nv2a_rc_unpack(rc->final_c1, f.fc1);
    memcpy(f.fogc, b->fog_color, sizeof f.fogc);
    f.stg[0] = b->clip_plane_mode;
    for (st = 0; st < 4; st++) {
        const Nv2aTexture *t = &b->stage[st];
        ID3D11ShaderResourceView *v;
        if (!((b->stage_valid >> st) & 1))
            continue;
        v = tex_get(t);
        if (!v) {
            s_path[4]++;
            continue;
        }
        f.stg[1] |= 1u << st;
        if (t->cube && t->width == t->height) {
            f.stg[2] |= 1u << st;
            srvc[st] = v;
        } else {
            srv2[st] = v;
        }
        /* rc_texel: a swizzled or compressed texture is addressed in [0,1],
         * a linear one in texels (tex_normalized). */
        f.tsc[st][0] = tex_normalized(t) ? 1.0f : 1.0f / (float)t->width;
        f.tsc[st][1] = tex_normalized(t) ? 1.0f : 1.0f / (float)t->height;
        smp[st] = sampler(t);
    }
    *pf = f;
}

static void draw_full(const Nv2aBatch *b)
{
    const Nv2aRenderState *rs = b->state;
    FullConsts f;
    Consts k;
    ID3D11ShaderResourceView *srv2[4] = { NULL, NULL, NULL, NULL };
    ID3D11ShaderResourceView *srvc[4] = { NULL, NULL, NULL, NULL };
    ID3D11SamplerState *smp[4] = { NULL, NULL, NULL, NULL };
    UINT stride = sizeof(Nv2aVertexFull), off = 0;

    if (!vb_put(b->full, b->count * (UINT)sizeof(Nv2aVertexFull), &off))
        return;
    full_consts(b, &f, srv2, srvc, smp);
    fill_vp_alpha(&k, rs);
    wbuf_fill(&k, rs, b);
    upload_cb(s_cb, &k, sizeof k);
    upload_cb(s_cb_full, &f, sizeof f);

    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout_full);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &off);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs_full, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    {
        ID3D11Buffer *cbs[2] = { s_cb, s_cb_full };
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 2, cbs);
    }
    ID3D11DeviceContext_RSSetState(s_ctx, raster_clip(rs));
    ID3D11DeviceContext_PSSetShader(s_ctx, wbuf_on(rs) ? s_ps_full_w : s_ps_full, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, srv2);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 4, 4, srvc);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, smp);
    set_output_state(rs);
    ID3D11DeviceContext_Draw(s_ctx, b->count, 0);
    s_cur_rt->dirty = s_cur_rt->drawn = 1;
}

/* The vertex buffer, large enough for `bytes` more (vb_put). */
static int vb_reserve(UINT bytes)
{
    D3D11_BUFFER_DESC bd;

    if (bytes <= s_vb_bytes && s_vb)
        return 1;
    if (s_vb) ID3D11Buffer_Release(s_vb);
    s_vb = NULL;
    s_vb_bytes = bytes < (4u << 20) ? (4u << 20) : bytes * 2;
    s_vb_pos = s_vb_bytes;              /* first use maps with DISCARD */
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = s_vb_bytes;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vb))) {
        s_vb_bytes = 0;
        return 0;
    }
    return 1;
}

/* ---- Vertex programs on the GPU (Nv2aProgramBatch) ---------------------
 *
 * The NV2A runs a title's vertex programs in its vertex unit; the executor
 * ran them on the CPU in nv2a_vsh_interp.c. Here each program becomes a
 * vertex shader: a transcription of that interpreter's vsh_run_dec, operation
 * for operation, so the GPU computes what the CPU path computed and the two
 * can be compared value for value (RECOMP_GPUBE_VSH_CHECK). What happens after
 * the program is transform_vertex, backend_full_from_xf and the near-plane
 * clip of nv2a_pb_exec.c, then this file's full vertex shader. Every value is
 * `precise`: the compiler may neither fuse a multiply and an add nor reorder a
 * sum, as the C does neither. Programs compile once, synchronously -- a draw
 * is never skipped -- and are kept in shader_cache\ (RECOMP_GPUBE_SHADER_CACHE=0
 * keeps nothing on disk), so a program is compiled once per PC. */
typedef struct {
    float c[NV2A_VSH_CONSTANTS][4];
    float pa[4];                /* aa_sx, aa_sy, near z, clip at near z */
    float pf[4];                /* fog enable, mode, param 0, param 1 */
    float cf[4];                /* oFog before the program writes it (carry_fog) */
} ProgConsts;

typedef struct {
    uint64_t key;
    ID3D11VertexShader *vs;
    ID3D11InputLayout *il;
    ID3D11ComputeShader *cs;    /* RECOMP_GPUBE_VSH_CHECK only */
    uint32_t checks;            /* batches of it checked */
    UINT stride;
    int failed;
} Prog;

#define PROG_CACHE 1024         /* power of two */
static Prog s_prog[PROG_CACHE];
static ID3D11Buffer *s_cb_prog, *s_ib;
static ProgConsts s_prog_last;
static int s_prog_last_ok;
static UINT s_ib_bytes, s_ib_pos;
static char s_vsrc[256 * 1024];

static uint32_t vfld(const uint32_t *w, int dw, int lo, int n)
{
    return (w[dw] >> lo) & ((1u << n) - 1u);
}

typedef struct { char *p; size_t cap, len; } Txt;

static void tx(Txt *t, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (t->len >= t->cap)
        return;
    va_start(ap, fmt);
    n = vsnprintf(t->p + t->len, t->cap - t->len, fmt, ap);
    va_end(ap);
    t->len = n < 0 || (size_t)n >= t->cap - t->len ? t->cap : t->len + (size_t)n;
}

/* One source operand, as dec_src and dec_read take it. */
static void vsrc(Txt *t, const uint32_t *w, int which)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg, ci = vfld(w, 1, 13, 8);

    if (which == 0) {
        neg = vfld(w, 1, 8, 1); swz = vfld(w, 1, 0, 8);
        mux = vfld(w, 2, 26, 2); reg = vfld(w, 2, 28, 4);
    } else if (which == 1) {
        neg = vfld(w, 2, 25, 1); swz = vfld(w, 2, 17, 8);
        mux = vfld(w, 2, 11, 2); reg = vfld(w, 2, 13, 4);
    } else {
        neg = vfld(w, 2, 10, 1); swz = vfld(w, 2, 2, 8);
        mux = vfld(w, 3, 28, 2);
        reg = (vfld(w, 2, 0, 2) << 2) | vfld(w, 3, 30, 2);
    }
    tx(t, neg ? "(-(" : "((");
    if (mux == 1) {                             /* a temp; 12 is oPos, past it R0 */
        if (reg == 12) tx(t, "op");
        else           tx(t, "r%u", reg < 12 ? reg : 0u);
    } else if (mux == 2) {
        tx(t, "i.v%u", vfld(w, 1, 9, 4));
    } else if (vfld(w, 3, 1, 1)) {              /* c[ci + a0.x], out of range c[0] */
        tx(t, "cr(%u + a0)", ci);
    } else {
        tx(t, "c[%u]", ci < NV2A_VSH_CONSTANTS ? ci : 0u);
    }
    tx(t, ").%c%c%c%c)", sw[(swz >> 6) & 3], sw[(swz >> 4) & 3], sw[(swz >> 2) & 3], sw[swz & 3]);
}

static const char *vmask(uint32_t m, char out[6])
{
    int n = 0;
    out[n++] = '.';
    if (m & 8) out[n++] = 'x';
    if (m & 4) out[n++] = 'y';
    if (m & 2) out[n++] = 'z';
    if (m & 1) out[n++] = 'w';
    out[n] = 0;
    return out;
}

/* A temp register by number, as the interpreter writes it: 12 is oPos, past
 * it nothing. */
static const char *vtemp(uint32_t r, char out[8])
{
    if (r == 12) return "op";
    if (r < 12) { snprintf(out, 8, "r%u", r); return out; }
    return NULL;
}

/* The program as HLSL: prog() is vsh_run_dec, main the vertex shader, cs the
 * compute shader the check runs. 0 if it did not fit. */
static size_t prog_hlsl(const Nv2aProgramBatch *pb, char *buf, size_t cap)
{
    Txt t = { buf, cap, 0 };
    const char *np = persp_color() ? "" : "noperspective ";
    uint32_t k, a;

    { /* The NV2A's 1/16-pixel grid, as the CPU rasteriser applies it
         * (nv2a_pb_exec.c, subpixel_snap_on); RECOMP_SUBPIXEL_SNAP=0 off. */
        const char *e = getenv("RECOMP_SUBPIXEL_SNAP");
        tx(&t, "#define SNAP %d\n", !(e && e[0] == '0'));
    }
    tx(&t,
       "cbuffer C : register(b0) { float4 vp; float4 tx; float4 wd; };\n"
       "cbuffer P : register(b2) { float4 c[%u]; float4 pa; float4 pf; float4 cf; };\n"
       "struct VI {\n", NV2A_VSH_CONSTANTS);
    for (a = 0; a < NV2A_VSH_INPUTS; a++)
        if ((pb->input_mask >> a) & 1)
            tx(&t, "  float4 v%u : ATTR%u;\n", a, a);
    if (!pb->input_mask)
        tx(&t, "  uint pad_ : PADDING;\n"); /* never read; VI cannot be empty */
    tx(&t,
       "};\n"
       "struct VX { float4 pos, d0, d1, fog, t0, t1, t2, t3; };\n"
       "struct VO { float4 p : SV_Position; %sfloat4 d0 : COLOR0;\n"
       "  %sfloat4 d1 : COLOR1; %sfloat fog : TEXCOORD9;\n"
       "  float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
       "  noperspective float rw : TEXCOORD6; float wv : TEXCOORD7; float2 cd : SV_ClipDistance0; };\n"
       "float4 cr(int i) { return c[(i < 0 || i >= %u) ? 0 : i]; }\n"
       "#define INF asfloat(0x7F800000u)\n"
       /* run_ilu, case by case */
       "float rcp_(float x) { precise float r = 1.0 / x; return x == 0.0 ? INF : r; }\n"
       "float rcc_(float x) {\n"
       "  precise float f = 1.0 / x;\n"
       "  if (abs(f) < 5.42101e-20) f = f < 0.0 ? -5.42101e-20 : 5.42101e-20;\n"
       "  if (abs(f) > 1.884467e19) f = f < 0.0 ? -1.884467e19 : 1.884467e19;\n"
       "  return f;\n"
       "}\n"
       "float4 vexp(float x) { precise float fl = floor(x); return float4(exp2(fl), x - fl, exp2(x), 1.0); }\n"
       "float4 vlog(float x) {\n"
       "  precise float ax = abs(x);\n"
       "  if (ax == 0.0) return float4(-INF, 1.0, -INF, 1.0);\n"
       "  precise float e = floor(log2(ax));\n"
       "  return float4(e, ax / exp2(e), log2(ax), 1.0);\n"
       "}\n"
       /* powf: x^0 is 1 for any x, 0^p is 0 above 0 and inf below; the
        * GPU's exp2(p * log2(x)) gives NaN for 0^0. */
       "float cpow(float x, float p) {\n"
       "  if (p == 0.0) return 1.0;\n"
       "  if (x == 0.0) return p < 0.0 ? INF : 0.0;\n"
       "  return pow(x, p);\n"
       "}\n"
       "float4 vlit(float4 c) {\n"
       "  float nl = c.x > 0.0 ? c.x : 0.0, nh = c.y > 0.0 ? c.y : 0.0;\n"
       "  float p = c.w < -127.9961 ? -127.9961 : (c.w > 127.9961 ? 127.9961 : c.w);\n"
       "  return float4(1.0, nl, c.x > 0.0 ? cpow(nh, p) : 0.0, 1.0);\n"
       "}\n"
       /* the colour clamp: NaN reads as 0 */
       "float4 c01(float4 v) { return v > 1.0 ? 1.0 : (v >= 0.0 ? v : 0.0); }\n"
       /* fog_factor */
       "float fogf(float d) {\n"
       "  if (pf.x == 0.0) return 1.0;\n"
       "  uint m = (uint)pf.y; precise float f;\n"
       "  if (m == 0x800 || m == 0x802) f = pf.z + exp2(d * pf.w * 16.0) - 1.5;\n"
       "  else if (m == 0x801 || m == 0x803) f = pf.z + exp2(-d * d * pf.w * pf.w * 32.0) - 1.5;\n"
       "  else f = pf.z + d * pf.w - 1.0;\n"
       "  if (m == 0x802 || m == 0x803 || m == 0x804) f = abs(f);\n"
       "  if (isnan(f)) f = 1.0;\n"
       "  return f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);\n"
       "}\n"
       "VX prog(VI i) {\n"
       "  precise float4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0;\n"
       "  precise float4 r6 = 0, r7 = 0, r8 = 0, r9 = 0, r10 = 0, r11 = 0, op = 0;\n"
       "  precise float4 o1 = 0, o2 = 0, o3 = float4(0, 0, 0, 1), o4 = float4(0, 0, 0, 1);\n"
       "  precise float4 o5 = cf, o6 = 0, o7 = 0, o8 = 0, o9 = float4(0, 0, 0, 1);\n"
       "  precise float4 o10 = float4(0, 0, 0, 1), o11 = float4(0, 0, 0, 1), o12 = float4(0, 0, 0, 1);\n"
       "  int a0 = 0;\n", np, np, np, NV2A_VSH_CONSTANTS);

    for (k = 0; k < pb->length; k++) {
        const uint32_t *w = pb->program[k];
        uint32_t mac = vfld(w, 1, 21, 4), ilu = vfld(w, 1, 25, 3);
        uint32_t mac_mask = vfld(w, 3, 24, 4), ilu_mask = vfld(w, 3, 16, 4);
        uint32_t tdst = vfld(w, 3, 20, 4), omask = vfld(w, 3, 12, 4);
        uint32_t oaddr = vfld(w, 3, 3, 8);
        char m[6], nb[8];
        const char *d;

        tx(&t, "  { /* %u: %08X %08X %08X */\n    precise float4 A = ", k, w[1], w[2], w[3]);
        vsrc(&t, w, 0);
        tx(&t, ", B = ");
        vsrc(&t, w, 1);
        tx(&t, ", C = ");
        vsrc(&t, w, 2);
        tx(&t, ";\n    precise float4 M = 0, I = 0;\n");
        switch (mac) {                             /* run_mac */
        case 0:  break;
        case 2:  tx(&t, "    M = A * B;\n"); break;
        case 3:  tx(&t, "    M = A + C;\n"); break;
        case 4:  tx(&t, "    M = A * B; M = M + C;\n"); break;
        case 5:  tx(&t, "    { precise float s = A.x * B.x + A.y * B.y + A.z * B.z; M = s.xxxx; }\n"); break;
        case 6:  tx(&t, "    { precise float s = A.x * B.x + A.y * B.y + A.z * B.z + B.w; M = s.xxxx; }\n"); break;
        case 7:  tx(&t, "    { precise float s = A.x * B.x + A.y * B.y + A.z * B.z + A.w * B.w; M = s.xxxx; }\n"); break;
        case 8:  tx(&t, "    M = float4(1.0, A.y * B.y, A.z, B.w);\n"); break;
        case 9:  tx(&t, "    M = A < B ? A : B;\n"); break;
        case 10: tx(&t, "    M = A >= B ? A : B;\n"); break;
        case 11: tx(&t, "    M = A < B ? 1.0 : 0.0;\n"); break;
        case 12: tx(&t, "    M = A >= B ? 1.0 : 0.0;\n"); break;
        case 13: tx(&t, "    M = A; a0 = (int)floor(A.x + 0.001);\n"); break;
        default: tx(&t, "    M = A;\n"); break;   /* mov, and 14/15 as the C leaves them */
        }
        switch (ilu) {                             /* run_ilu */
        case 0:  break;
        case 2:  tx(&t, "    I = rcp_(C.x).xxxx;\n"); break;
        case 3:  tx(&t, "    I = rcc_(C.x).xxxx;\n"); break;
        case 4:  tx(&t, "    { precise float s = sqrt(abs(C.x)); s = 1.0 / s; I = s.xxxx; }\n"); break;
        case 5:  tx(&t, "    I = vexp(C.x);\n"); break;
        case 6:  tx(&t, "    I = vlog(C.x);\n"); break;
        case 7:  tx(&t, "    I = vlit(C);\n"); break;
        default: tx(&t, "    I = C;\n"); break;   /* mov */
        }
        /* The writes, in the interpreter's order: MAC temp, ILU temp (R1 when
         * paired with any MAC op), then the output. */
        if (mac && mac != 13 && mac_mask && (d = vtemp(tdst, nb)) != NULL)
            tx(&t, "    %s%s = M%s;\n", d, vmask(mac_mask, m), m);
        if (ilu && ilu_mask && (d = vtemp(mac ? 1u : tdst, nb)) != NULL)
            tx(&t, "    %s%s = I%s;\n", d, vmask(ilu_mask, m), m);
        if (omask && vfld(w, 3, 11, 1) && oaddr < 13) {   /* to c[]: never sent here */
            const char *src = vfld(w, 3, 2, 1) ? "I" : "M";
            if (oaddr == 0) tx(&t, "    op%s = %s%s;\n", vmask(omask, m), src, m);
            else            tx(&t, "    o%u%s = %s%s;\n", oaddr, vmask(omask, m), src, m);
        }
        tx(&t, "  }\n");
    }
    tx(&t,
       "  VX x;\n"
       "  x.pos = op; x.d0 = c01(o3); x.d1 = c01(o4); x.fog = o5;\n"
       "  x.t0 = o9; x.t1 = o10; x.t2 = o11; x.t3 = o12;\n"
       "  return x;\n"
       "}\n"
       /* transform_vertex, backend_full_from_xf, the full vertex shader, and
        * the two near planes as clip distances (xf_clip_dist: w = 1e-3, and
        * screen z = near z). w > 0 takes the full shader's w exactly; a w at
        * or behind the eye stays as it is for the clipper. */
       "VO main(VI i) {\n"
       "  VX x = prog(i);\n"
       "  precise float4 pos = x.pos;\n"
       "  if (asuint(pos.w) == 0u) pos.w = 1.0;\n"
       "  pos.x = pos.x * pa.x; pos.y = pos.y * pa.y;\n"
       "  precise float w = pos.w, rhw = 1.0 / w;\n"
       "  precise float wc = w > 0.0 ? 1.0 / max(rhw, 1e-20) : w;\n"
       "  if (SNAP) pos.xy = trunc(pos.xy * 16.0) / 16.0;\n"
       "  precise float sx = pos.x * vp.x - 1.0, sy = 1.0 + pos.y * vp.y, sz = saturate(pos.z * vp.z);\n"
       "  precise float zd = pos.z * w - pa.z * w;\n"
       "  VO o;\n"
       "  o.p = float4(sx * wc, sy * wc, sz * wc, wc);\n"
       "  o.d0 = x.d0; o.d1 = x.d1; o.fog = fogf(x.fog.x);\n"
       "  o.t0 = x.t0; o.t1 = x.t1; o.t2 = x.t2; o.t3 = x.t3;\n"
       "  o.rw = rhw; o.wv = w;\n"
       "  o.cd = float2(w - 1e-3, pa.w != 0.0 ? zd : 1.0);\n"
       "  if (!isfinite(o.cd.x) || !isfinite(zd)) o.cd = -1.0;   /* no position at all */\n"
       "  return o;\n"
       "}\n"
       "StructuredBuffer<float4> cin : register(t0);\n"
       "RWStructuredBuffer<VX> cout : register(u0);\n"
       "[numthreads(64, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
       "  VI i;\n");
    for (a = 0; a < NV2A_VSH_INPUTS; a++)
        if ((pb->input_mask >> a) & 1)
            tx(&t, "  i.v%u = cin[id.x * 16 + %u];\n", a, a);
    if (!pb->input_mask)
        tx(&t, "  i.pad_ = 0;\n");
    tx(&t, "  cout[id.x] = prog(i);\n}\n");
    return t.len < t.cap ? t.len : 0;
}

static uint64_t fnv64(const void *p, size_t n, uint64_t h)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--)
        h = (h ^ *b++) * 1099511628211ull;
    return h;
}

/* Compiled bytecode for one entry of src: from shader_cache\ if it is there,
 * else compiled and stored there. */
static ID3DBlob *prog_compile(const char *src, size_t len, const char *entry,
                              const char *target, uint64_t *ms)
{
    static int disk = -1;
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL1 | D3DCOMPILE_IEEE_STRICTNESS;
    char path[MAX_PATH], tmp[MAX_PATH];
    uint64_t h = fnv64(src, len, 1469598103934665603ull);
    ID3DBlob *code = NULL, *err = NULL;
    LARGE_INTEGER f, t0, t1;
    FILE *fp;

    h = fnv64(entry, strlen(entry), h);
    h = fnv64(target, strlen(target), h);
    h = fnv64(&flags, sizeof flags, h);
    if (disk < 0) {
        const char *e = getenv("RECOMP_GPUBE_SHADER_CACHE");
        disk = !(e && e[0] == '0');
        if (disk)
            CreateDirectoryA("shader_cache", NULL);
    }
    snprintf(path, sizeof path, "shader_cache\\vsh_%016llx.cso", (unsigned long long)h);
    *ms = 0;
    if (disk && (fp = fopen(path, "rb")) != NULL) {
        long n;
        fseek(fp, 0, SEEK_END);
        n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (n > 0 && SUCCEEDED(D3DCreateBlob((SIZE_T)n, &code))
                && fread(ID3D10Blob_GetBufferPointer(code), 1, (size_t)n, fp) == (size_t)n) {
            fclose(fp);
            return code;
        }
        if (code) ID3D10Blob_Release(code);
        code = NULL;
        fclose(fp);
    }
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    if (FAILED(D3DCompile(src, len, "nv2a_vsh", NULL, NULL, entry, target, flags, 0,
                          &code, &err))) {
        static int shown;
        if (shown++ < 8)
            fprintf(stderr, "[GPUBE] vertex program %s: %s\n", entry,
                    err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "failed");
        if (err) ID3D10Blob_Release(err);
        return NULL;
    }
    if (err) ID3D10Blob_Release(err);
    QueryPerformanceCounter(&t1);
    *ms = (uint64_t)((t1.QuadPart - t0.QuadPart) * 1000 / f.QuadPart) + 1;
    snprintf(tmp, sizeof tmp, "%s.%lu", path, GetCurrentThreadId());
    if (disk && (fp = fopen(tmp, "wb")) != NULL) {
        size_t n = ID3D10Blob_GetBufferSize(code);
        int ok = fwrite(ID3D10Blob_GetBufferPointer(code), 1, n, fp) == n;
        fclose(fp);
        if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING))
            DeleteFileA(tmp);
    }
    return code;
}

/* The shader for this batch's program and inputs. NULL if it cannot be built
 * (logged once per program). */
static Prog *prog_get(const Nv2aProgramBatch *pb)
{
    static uint32_t last_words[NV2A_VSH_SLOTS][4], last_len, last_mask;
    static Prog *last;
    static unsigned shown;
    D3D11_INPUT_ELEMENT_DESC el[NV2A_VSH_INPUTS];
    uint64_t key, ms = 0;
    uint32_t i, slot, n = 0;
    size_t len;
    ID3DBlob *code;
    Prog *p;

    if (last && pb->length == last_len && pb->input_mask == last_mask
            && !memcmp(last_words, pb->program, pb->length * 16))
        return last;
    key = fnv64(pb->program, pb->length * 16, 1469598103934665603ull);
    key = fnv64(&pb->input_mask, sizeof pb->input_mask, key) | 1;
    slot = (uint32_t)(key ^ (key >> 32)) & (PROG_CACHE - 1);
    for (i = 0; i < PROG_CACHE; i++) {
        p = &s_prog[(slot + i) & (PROG_CACHE - 1)];
        if (!p->key || p->key == key)
            break;
    }
    if (i == PROG_CACHE)
        return NULL;                            /* full: a thousand programs */
    if (!p->key) {
        p->key = key;
        len = prog_hlsl(pb, s_vsrc, sizeof s_vsrc);
        code = len ? prog_compile(s_vsrc, len, "main", "vs_5_0", &ms) : NULL;
        if (code) {
            for (i = 0; i < NV2A_VSH_INPUTS; i++) {
                if (!((pb->input_mask >> i) & 1))
                    continue;
                el[n].SemanticName = "ATTR";
                el[n].SemanticIndex = i;
                el[n].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                el[n].InputSlot = 0;
                el[n].AlignedByteOffset = 16 * n;
                el[n].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
                el[n].InstanceDataStepRate = 0;
                n++;
            }
            /* A program with no inputs: PADDING, declared and never read,
             * from a slot nothing is bound to (which reads 0). */
            p->stride = 16 * n;
            if (!n) {
                el[0].SemanticName = "PADDING";
                el[0].SemanticIndex = 0;
                el[0].Format = DXGI_FORMAT_R32_UINT;
                el[0].InputSlot = 1;
                el[0].AlignedByteOffset = 0;
                el[0].InputSlotClass = D3D11_INPUT_PER_INSTANCE_DATA;
                el[0].InstanceDataStepRate = 0;
                n = 1;
            }
            if (FAILED(ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                           ID3D10Blob_GetBufferSize(code), NULL, &p->vs))
                    || FAILED(ID3D11Device_CreateInputLayout(s_dev, el, n,
                           ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code),
                           &p->il)))
                p->failed = 1;
            ID3D10Blob_Release(code);
        } else {
            p->failed = 1;
        }
        if (shown++ < 64)
            fprintf(stderr, "[GPUBE] vertex program %016llX: %u instructions, inputs %04X, %s\n",
                    (unsigned long long)key, pb->length, pb->input_mask,
                    p->failed ? "FAILED" : ms ? "compiled" : "from shader_cache");
        if (ms > 50 && shown < 64)
            fprintf(stderr, "[GPUBE]   compile took %llu ms\n", (unsigned long long)ms);
    }
    if (p->failed)
        return NULL;
    memcpy(last_words, pb->program, pb->length * 16);
    last_len = pb->length;
    last_mask = pb->input_mask;
    last = p;
    return p;
}

static int ib_put(const void *src, UINT bytes, UINT *off)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;

    if (!s_ib || bytes > s_ib_bytes) {
        D3D11_BUFFER_DESC bd;
        if (s_ib) ID3D11Buffer_Release(s_ib);
        s_ib = NULL;
        s_ib_bytes = bytes < (4u << 20) ? (4u << 20) : bytes * 2;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = s_ib_bytes;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_ib))) {
            s_ib_bytes = 0;
            return 0;
        }
        s_ib_pos = s_ib_bytes;
    }
    if (s_ib_pos + bytes > s_ib_bytes) {
        mode = D3D11_MAP_WRITE_DISCARD;
        s_ib_pos = 0;
    }
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_ib, 0, mode, 0, &m)))
        return 0;
    memcpy((uint8_t *)m.pData + s_ib_pos, src, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_ib, 0);
    *off = s_ib_pos;
    s_ib_pos += (bytes + 15u) & ~15u;
    return 1;
}

/* RECOMP_GPUBE_VSH_CHECK=<n>: the first n batches of every program (default
 * 4) have up to 64 vertices run through the program twice -- by this shader on
 * the GPU (its compute twin, same prog()) and by the CPU interpreter
 * (nv2a_vsh_run) -- and every output value is compared. "Differ" counts
 * values more than 1e-6 apart relative to their size; the largest relative
 * difference is reported with them. */
static void prog_check(Prog *p, const Nv2aProgramBatch *pb)
{
    static int left = -1;
    static ID3D11Buffer *in_buf, *out_buf, *stage;
    static ID3D11ShaderResourceView *in_srv;
    static ID3D11UnorderedAccessView *out_uav;
    static float in16[64][NV2A_VSH_INPUTS][4];
    static uint64_t vals, differ, nan_mismatch;
    static double worst;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11ShaderResourceView *no_srv = NULL;
    ID3D11UnorderedAccessView *no_uav = NULL;
    uint32_t n = pb->vertex_count < 64 ? pb->vertex_count : 64, v, a, j, stride = p->stride / 16;
    uint32_t bdiff = 0;
    double bworst = 0.0;

    if (left < 0) {
        const char *e = getenv("RECOMP_GPUBE_VSH_CHECK");
        left = e ? (atoi(e) > 0 ? atoi(e) : 4) : 0;
    }
    if (!left || !n || p->checks >= (uint32_t)left)
        return;
    p->checks++;
    if (!in_buf) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = sizeof in16;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = 16;
        ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &in_buf);
        bd.ByteWidth = 64 * sizeof(Nv2aVshOutput);
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.StructureByteStride = sizeof(Nv2aVshOutput);
        ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &out_buf);
        bd.Usage = D3D11_USAGE_STAGING;
        bd.BindFlags = 0;
        bd.MiscFlags = 0;
        bd.StructureByteStride = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &stage);
        if (in_buf) ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)in_buf, NULL, &in_srv);
        if (out_buf) ID3D11Device_CreateUnorderedAccessView(s_dev, (ID3D11Resource *)out_buf, NULL, &out_uav);
    }
    if (!in_srv || !out_uav || !stage)
        return;
    if (!p->cs) {
        uint64_t ms;
        size_t len = prog_hlsl(pb, s_vsrc, sizeof s_vsrc);
        ID3DBlob *code = len ? prog_compile(s_vsrc, len, "cs", "cs_5_0", &ms) : NULL;
        if (!code)
            return;
        ID3D11Device_CreateComputeShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                         ID3D10Blob_GetBufferSize(code), NULL, &p->cs);
        ID3D10Blob_Release(code);
        if (!p->cs)
            return;
    }
    memset(in16, 0, sizeof in16);
    for (v = 0; v < n; v++) {
        const float *src = pb->inputs + (size_t)v * stride * 4;
        for (a = 0, j = 0; a < NV2A_VSH_INPUTS; a++)
            if ((pb->input_mask >> a) & 1)
                memcpy(in16[v][a], src + 4 * j++, 16);
    }
    ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)in_buf, 0, NULL, in16, 0, 0);
    ID3D11DeviceContext_CSSetShader(s_ctx, p->cs, NULL, 0);
    ID3D11DeviceContext_CSSetConstantBuffers(s_ctx, 2, 1, &s_cb_prog);
    ID3D11DeviceContext_CSSetShaderResources(s_ctx, 0, 1, &in_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(s_ctx, 0, 1, &out_uav, NULL);
    ID3D11DeviceContext_Dispatch(s_ctx, 1, 1, 1);
    ID3D11DeviceContext_CSSetShaderResources(s_ctx, 0, 1, &no_srv);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(s_ctx, 0, 1, &no_uav, NULL);
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)stage, (ID3D11Resource *)out_buf);
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)stage, 0, D3D11_MAP_READ, 0, &m)))
        return;
    for (v = 0; v < n; v++) {
        Nv2aVshOutput ref;
        const float *g = (const float *)((const Nv2aVshOutput *)m.pData + v), *c = (const float *)&ref;
        if (!nv2a_vsh_run((const float (*)[4])in16[v], &ref))
            continue;
        for (j = 0; j < sizeof ref / sizeof(float); j++) {
            double x = c[j], y = g[j], d, mag;
            vals++;
            if (x != x || y != y) {
                if ((x != x) != (y != y)) { nan_mismatch++; bdiff++; }
                continue;
            }
            if (x == y)
                continue;
            mag = fabs(x) > fabs(y) ? fabs(x) : fabs(y);
            d = isinf(x) || isinf(y) ? 1.0 : fabs(x - y) / (mag > 1e-30 ? mag : 1e-30);
            if (d > bworst) bworst = d;
            if (d > 1e-6) bdiff++;
        }
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)stage, 0);
    differ += bdiff;
    if (bworst > worst) worst = bworst;
    if (bdiff || p->checks == 1)
        fprintf(stderr, "[GPUBE-VSHCHECK] program %016llX, %u vertices: %u values differ "
                        "(largest %.3g); all checks: %llu of %llu values, %llu NaN mismatches, "
                        "largest %.3g\n", (unsigned long long)p->key, n, bdiff, bworst,
                (unsigned long long)differ, (unsigned long long)vals,
                (unsigned long long)nan_mismatch, worst);
}

/* Widescreen, as Nightfire's main game draws it for a 16:9 TV: its 3D view
 * squeezed into its 640x480, which the window stretches back
 * (RECOMP_DISPLAY_ASPECT), its menus and HUD for 4:3 all the same. Those
 * arrive here already in screen pixels: the D3D pass-through program for
 * pre-transformed vertices (5 instructions, v0 the pixel position, z 0 --
 * every menu and HUD element is drawn that way, the 3D world through
 * programs of the game's own; the driving image draws its world the same
 * way and its HUD for 16:9 already). So each frame is shown at the shape it was
 * drawn for: a frame with the world in it at 16:9, with each HUD element
 * squeezed in place by 4:3 over that (hud_inputs; a draw across the whole
 * width, a fade or a bar, keeps it, so nothing leaves the sides bare); a frame of menus
 * alone -- the main menu, the pause menu, a loading screen -- at 4:3,
 * untouched, between black bars (be_flip). Videos the window keeps at 4:3
 * itself. The caller sets the display shape only for the enhanced game
 * (src/main.c). */
static float s_wide_aspect = 4.0f / 3.0f;
static int   s_world_seen;          /* the world drawn on the shown surface this frame */

/* A menu over the world (the pause menu): its frame on screen, set each
 * frame by the title hook (manual_main.c), on 0 for none. Its words come in
 * more than one draw at times (a letter on another page of the font), and
 * each part squeezed to its own edge would come apart; inside the frame
 * every draw is squeezed about the centre, the menu as one 4:3 picture. */
static volatile int s_menu_box[5];  /* on, x0, y0, x1, y1 */

void nf_backend_menu_box(int on, int x0, int y0, int x1, int y1)
{
    s_menu_box[1] = x0;
    s_menu_box[2] = y0;
    s_menu_box[3] = x1;
    s_menu_box[4] = y1;
    s_menu_box[0] = on;
}

static float hud_squeeze(void)
{
    static float sq = -1.0f;
    if (sq < 0.0f) {
        const char *e = getenv("RECOMP_DISPLAY_ASPECT");
        float x = 0.0f, y = 0.0f;
        sq = 1.0f;
        if (e && sscanf(e, "%f:%f", &x, &y) == 2 && x > 0.0f && y > 0.0f
                && x / y > 4.0f / 3.0f + 0.01f) {
            s_wide_aspect = x / y;
            sq = (4.0f / 3.0f) / s_wide_aspect;
        }
        if (sq < 1.0f)
            fprintf(stderr, "[GPUBE] widescreen: the world at %.3f, menus at 4:3, "
                            "HUD squeezed to %.3f\n", s_wide_aspect, sq);
    }
    return sq;
}

/* The surface the title shows: 640x480, linear -- with anti-aliasing, the
 * driving image's 1280x480. */
static int shown_surface(const Nv2aSurface *s)
{
    return !SURF_SWZ(s) && s->aa_sx && s->aa_sy
        && s->width == 640 * s->aa_sx && s->height == 480 * s->aa_sy;
}

/* HUD elements that come in more than one draw. A text whose glyphs are not
 * all on one page of the font yet -- a weapon's name the first moments after
 * it is picked up -- is drawn in parts, its shadow too, and each part
 * anchored by itself (left edge, centre, right edge) came apart on the
 * screen until the text settled into one draw. Draws on the same line that
 * touch are one group with one anchor; the groups of the frame before are
 * kept, so a part takes the anchor of the whole text it belonged to then.
 * RECOMP_HUD_GROUPS=0 anchors every draw by itself, as before. */
#define HUD_GROUPS 64
typedef struct { float x0, x1, y0, y1; } HudBox;
static HudBox s_hud_cur[HUD_GROUPS], s_hud_prev[HUD_GROUPS];
static int    s_hud_ncur, s_hud_nprev;

static int hud_groups_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_HUD_GROUPS");
        on = !(e && e[0] == '0');
    }
    return on;
}

/* The same line (the vertical ranges overlap by half the smaller height)
 * and touching or overlapping across (a gap of at most 16 pixels). */
static int hud_joins(const HudBox *a, const HudBox *b)
{
    float oy = fminf(a->y1, b->y1) - fmaxf(a->y0, b->y0);
    float h = fminf(a->y1 - a->y0, b->y1 - b->y0);
    return oy >= h * 0.5f && b->x0 <= a->x1 + 16.0f && a->x0 <= b->x1 + 16.0f;
}

static void hud_merge(HudBox *g, const HudBox *b)
{
    g->x0 = fminf(g->x0, b->x0); g->x1 = fmaxf(g->x1, b->x1);
    g->y0 = fminf(g->y0, b->y0); g->y1 = fmaxf(g->y1, b->y1);
}

/* The box a draw is anchored by: its group in this frame, widened by the
 * group it joins from the frame before. */
static HudBox hud_group(const HudBox *b)
{
    HudBox g = *b;
    int i, k;

    if (!hud_groups_on())
        return g;
    for (i = 0; i < s_hud_ncur && !hud_joins(&s_hud_cur[i], b); i++) {}
    if (i < s_hud_ncur)
        hud_merge(&s_hud_cur[i], b);
    else if (s_hud_ncur < HUD_GROUPS)
        s_hud_cur[s_hud_ncur++] = *b;
    if (i < s_hud_ncur)
        g = s_hud_cur[i];
    for (k = 0; k < s_hud_nprev; k++)
        if (hud_joins(&s_hud_prev[k], &g)) {
            hud_merge(&g, &s_hud_prev[k]);
            break;
        }
    return g;
}

static void hud_groups_flip(void)
{
    memcpy(s_hud_prev, s_hud_cur, sizeof s_hud_prev);
    s_hud_nprev = s_hud_ncur;
    s_hud_ncur = 0;
}

/* pb's inputs with the HUD squeezed (hud_squeeze), or pb's own. Notes a
 * draw of the world on the shown surface. */
static const float *hud_inputs(const Nv2aSurface *s, const Nv2aProgramBatch *pb)
{
    static float *buf;
    static size_t cap;
    float sq = hud_squeeze(), x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, cx, w;
    uint32_t n = 0, m = pb->input_mask, i;
    size_t need;
    float zhud = 0.0f;
    int drv = 0;

    if (sq >= 1.0f || !shown_surface(s))
        return pb->inputs;
#ifdef NIGHTFIRE_DRIVING
    /* The driving image draws its HUD for 16:9 itself, through programs of
     * its own on 640x480 inputs, which they map onto the 16:9 picture:
     * 8 instructions at z 1 (the gun at the bottom left, the crosshair, the
     * loading screen's tiles; at another z, sprites in the world), 3 at z 0
     * (texts: the weapon's name, the ammunition) and 7 at z 1 (fades and
     * bars across the picture). The mapping is linear, so squeezing the
     * inputs squeezes what is drawn. Ultrawide shows that 16:9 HUD wider
     * still: squeezed by 16:9 over the picture's shape, each element in its
     * place as below. RECOMP_DISPLAY_ASPECT 16:9 leaves it as it is. */
    sq = (16.0f / 9.0f) / s_wide_aspect;
    if (pb->length >= 10) {
        s_world_seen = 1;
        return pb->inputs;
    }
    if (sq >= 0.999f || !s_world_seen)
        return pb->inputs;
    if (pb->length == 8 && pb->input_mask == 0x0007)
        zhud = 1.0f;
    else if (pb->length == 3 && pb->input_mask == 0x0209)
        zhud = 0.0f;
    else if (pb->length == 7 && pb->input_mask == 0x0003)
        zhud = 1.0f;
    else
        return pb->inputs;
    drv = 1;
#endif
    /* The world: a program of 10 instructions or more (the main game's
     * shortest is 11). Menus, loading screens and stills -- the driving
     * image's too, which has no HUD of this kind -- take 2 to 9. */
    if (!drv) {
        if (pb->length >= 10) {
            s_world_seen = 1;
            return pb->inputs;
        }
        if (pb->length != 5 || !(pb->input_mask & 1) || s->aa_sx != 1)
            return pb->inputs;
        if (!s_world_seen)
            return pb->inputs;          /* a menu: drawn for 4:3, shown at 4:3 */
    }
    while (m) { n += m & 1; m >>= 1; }
    for (i = 0; i < pb->vertex_count; i++) {
        const float *v = pb->inputs + (size_t)i * n * 4;
        if (v[2] != zhud) {
            if (!drv)
                s_world_seen = 1;       /* not a screen-space overlay */
            return pb->inputs;
        }
        if (v[0] < x0) x0 = v[0];
        if (v[0] > x1) x1 = v[0];
        if (v[1] < y0) y0 = v[1];
        if (v[1] > y1) y1 = v[1];
    }
    w = drv ? 640.0f : (float)s->width;
    need = (size_t)pb->vertex_count * n * 4;
    if (cap < need) {
        float *grown = (float *)realloc(buf, need * sizeof *buf);
        if (!grown)
            return pb->inputs;
        buf = grown;
        cap = need;
    }
    memcpy(buf, pb->inputs, need * sizeof *buf);
    /* On the screen's edge (a fade; the sniper scope's black sides, one draw
     * or one each, whose sight is a draw of its own, squeezed below): the
     * edge stays, the rest closes in on the centre with the sight. In the
     * driving image only across the whole width: its HUD reaches past the
     * inputs' edges (the gun at x -52), and is an element as any other. */
    if (drv ? (x0 <= 0.5f && x1 >= w - 0.5f) : (x0 <= 0.5f || x1 >= w - 0.5f)) {
        for (i = 0; i < pb->vertex_count; i++) {
            float *x = buf + (size_t)i * n * 4;
            if (*x > 0.5f && *x < w - 0.5f)
                *x = w * 0.5f + (*x - w * 0.5f) * sq;
        }
        return buf;
    }
    /* Each element keeps its place, as on the GameCube, which shows the
     * 4:3 HUD stretched: one in the left third keeps its left edge, one in
     * the right third its right edge (the ammo count and its icon stay
     * flush), one in the middle (the crosshair, a message) stays centred. */
    {
        HudBox b = { x0, x1, y0, y1 }, g = hud_group(&b);
        cx = (g.x0 + g.x1) * 0.5f;
        cx = cx < w / 3.0f ? g.x0 : cx > w * 2.0f / 3.0f ? g.x1 : w * 0.5f;
    }
    if (s_menu_box[0] && x0 >= s_menu_box[1] - 8 && x1 <= s_menu_box[3] + 8
            && y0 >= s_menu_box[2] - 8 && y1 <= s_menu_box[4] + 8)
        cx = w * 0.5f;
    for (i = 0; i < pb->vertex_count; i++)
        buf[(size_t)i * n * 4] = cx + (buf[(size_t)i * n * 4] - cx) * sq;
    return buf;
}

static void draw_program(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aProgramBatch *pb = b->program;
    const Nv2aRenderState *rs = b->state;
    ID3D11ShaderResourceView *srv2[4] = { NULL, NULL, NULL, NULL };
    ID3D11ShaderResourceView *srvc[4] = { NULL, NULL, NULL, NULL };
    ID3D11SamplerState *smp[4] = { NULL, NULL, NULL, NULL };
    ID3D11Buffer *vcb[3];
    FullConsts f;
    Consts k;
    ProgConsts pc;
    UINT voff = 0, ioff = 0, vbytes;
    Prog *p;

    if (!pb->index_count || !device_up() || !bind(s, rs))
        return;
    if (!(p = prog_get(pb)))
        return;
    vbytes = pb->vertex_count * p->stride;
    if (vbytes && (!vb_reserve(vbytes) || !vb_put(hud_inputs(s, pb), vbytes, &voff)))
        return;
    if (!ib_put(pb->indices, pb->index_count * 4u, &ioff))
        return;
    if (!s_cb_prog) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof bd);
        bd.ByteWidth = sizeof(ProgConsts);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb_prog)))
            return;
    }
    memcpy(pc.c, pb->constants, sizeof pc.c);
    pc.pa[0] = pb->aa_sx;
    pc.pa[1] = pb->aa_sy;
    pc.pa[2] = pb->near_z;
    pc.pa[3] = pb->clip_near_z ? 1.0f : 0.0f;
    pc.pf[0] = pb->fog_enable ? 1.0f : 0.0f;
    pc.pf[1] = (float)pb->fog_mode;
    pc.pf[2] = pb->fog_param[0];
    pc.pf[3] = pb->fog_param[1];
#ifdef NV2A_BACKEND_HAS_CARRY_FOG
    memcpy(pc.cf, pb->carry_fog, sizeof pc.cf);
#else
    pc.cf[0] = pc.cf[1] = pc.cf[2] = 0.0f;
    pc.cf[3] = 1.0f;
#endif
    if (!s_prog_last_ok || memcmp(&pc, &s_prog_last, sizeof pc)) {
        upload_cb(s_cb_prog, &pc, sizeof pc);
        s_prog_last = pc;
        s_prog_last_ok = 1;
    }

    full_consts(b, &f, srv2, srvc, smp);
    fill_vp_alpha(&k, rs);
    wbuf_fill(&k, rs, b);
    upload_cb(s_cb, &k, sizeof k);
    upload_cb(s_cb_full, &f, sizeof f);
    prog_check(p, pb);

    ID3D11DeviceContext_IASetInputLayout(s_ctx, p->il);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (vbytes)
        ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &p->stride, &voff);
    ID3D11DeviceContext_IASetIndexBuffer(s_ctx, s_ib, DXGI_FORMAT_R32_UINT, ioff);
    ID3D11DeviceContext_VSSetShader(s_ctx, p->vs, NULL, 0);
    vcb[0] = s_cb; vcb[1] = NULL; vcb[2] = s_cb_prog;
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 3, vcb);
    {
        ID3D11Buffer *cbs[2] = { s_cb, s_cb_full };
        ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 2, cbs);
    }
    ID3D11DeviceContext_RSSetState(s_ctx, raster_clip(rs));
    ID3D11DeviceContext_PSSetShader(s_ctx, !wbuf_on(rs) ? s_ps_full
                                    : s_ps_full_wv ? s_ps_full_wv : s_ps_full_w, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, srv2);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 4, 4, srvc);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, smp);
    set_output_state(rs);
    ID3D11DeviceContext_DrawIndexed(s_ctx, pb->index_count, 0, 0);
    s_cur_rt->dirty = s_cur_rt->drawn = 1;
}

static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aRenderState *rs = b->state;
    ID3D11ShaderResourceView *srv = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    Consts k;
    UINT bytes = b->count * (UINT)(b->full ? sizeof(Nv2aVertexFull) : sizeof(Nv2aVertex));
    UINT stride = sizeof(Nv2aVertex), off = 0;
    float bf[4];

    if (b->program) {
        draw_program(s, b);
        return;
    }
    if (!b->count || !device_up() || !bind(s, rs))
        return;

    /* Vertex buffer: grow as needed, rewritten per draw. */
    if (!vb_reserve(bytes))
        return;
    if (s_log_draws)
        log_draw(b->full ? "combiner" : b->texture ? "textured" : "untextured", s, b);
    if (b->full) {
        s_path[2]++;
        draw_full(b);
        return;
    }
    if (!vb_put(b->vertices, bytes, &off))
        return;

    if (b->texture) {
        srv = tex_get(b->texture);
        if (!srv)
            s_path[3]++;
    }
    s_path[srv ? 0 : 1]++;

    k.vp[0] = 2.0f / (float)s_cur_rt->w;
    k.vp[1] = -2.0f / (float)s_cur_rt->h;
    k.vp[2] = 1.0f / zscale(rs);
    k.vp[3] = 0.0f;
    k.tx[0] = srv ? 1.0f / (float)b->texture->width : 0.0f;
    k.tx[1] = srv ? 1.0f / (float)b->texture->height : 0.0f;
    k.tx[2] = (float)(rs->alpha_ref & 0xFF);
    k.tx[3] = rs->alpha_test_enable && rs->alpha_func >= 0x200 && rs->alpha_func <= 0x207
            ? (float)(rs->alpha_func - 0x200) : 7.0f;
    wbuf_fill(&k, rs, b);
    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &k, sizeof k);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb, 0);
    }

    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &off);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_RSSetState(s_ctx, raster_clip(rs));
    if (srv) {
        ID3D11SamplerState *smp = sampler(b->texture);
        ID3D11DeviceContext_PSSetShader(s_ctx, wbuf_on(rs) ? s_ps_tex_w : s_ps_tex, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &srv);
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &smp);
    } else {
        ID3D11DeviceContext_PSSetShader(s_ctx, wbuf_on(rs) ? s_ps_col_w : s_ps_col, NULL, 0);
    }
    bf[0] = ((rs->blend_color >> 16) & 0xFF) / 255.0f;
    bf[1] = ((rs->blend_color >> 8) & 0xFF) / 255.0f;
    bf[2] = (rs->blend_color & 0xFF) / 255.0f;
    bf[3] = ((rs->blend_color >> 24) & 0xFF) / 255.0f;
    ID3D11DeviceContext_OMSetBlendState(s_ctx, blend_state(rs), bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, depth_state(rs, s_cur_ds != NULL),
                                               rs->stencil_ref & 0xFF);
    ID3D11DeviceContext_Draw(s_ctx, b->count, 0);
    s_cur_rt->dirty = s_cur_rt->drawn = 1;
}

/* ---- Shadow comparison (RECOMP_GPUBE_SHADOW) ------------------------------
 *
 * The CPU rasteriser drew the frame into guest memory and this back end drew
 * the same input into its own surfaces. At each flip every surface drawn into
 * is read back and compared with guest memory, RGB only (alpha of an X8
 * surface means nothing). A frame that differs by more than a rounding step
 * is written to logs\compare\ as three BMPs: the CPU's, this back end's and
 * their difference (x4). Every 50 flips a summary, with the draws by path. */

static uint32_t s_cmp_frames, s_cmp_bad, s_cmp_dumps, s_cmp_worst_px;
static uint32_t s_cmp_worst_frame, s_cmp_worst_va;

static void cmp_rgb(uint32_t px, uint32_t bpp, int *r, int *g, int *b)
{
    if (bpp == 4) {
        *r = (px >> 16) & 0xFF; *g = (px >> 8) & 0xFF; *b = px & 0xFF;
    } else {
        *r = (int)d3d8_expand_channel((px >> 11) & 0x1F, 5);
        *g = (int)d3d8_expand_channel((px >> 5) & 0x3F, 6);
        *b = (int)d3d8_expand_channel(px & 0x1F, 5);
    }
}

static uint32_t cmp_px(const uint8_t *row, uint32_t x, uint32_t bpp)
{
    return bpp == 4 ? ((const uint32_t *)row)[x] : ((const uint16_t *)row)[x];
}

/* 24-bit BMP of a w x h image; pixel(x, y) returns 0x00RRGGBB. */
static void cmp_write_bmp(const char *path, uint32_t w, uint32_t h,
                          const uint8_t *base, uint32_t pitch, uint32_t bpp,
                          const uint8_t *base2, uint32_t pitch2)
{
    uint32_t row = w * 3, pad = (4 - (row & 3)) & 3, size = 54 + (row + pad) * h, x, y;
    uint8_t hdr[54], *line;
    FILE *f = fopen(path, "wb");

    if (!f)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &size, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    line = (uint8_t *)calloc(1, row + pad);
    for (y = h; y-- > 0;) {                     /* bottom-up */
        const uint8_t *a = base + (size_t)y * pitch;
        const uint8_t *b2 = base2 ? base2 + (size_t)y * pitch2 : NULL;
        for (x = 0; x < w; x++) {
            int r, g, b;
            cmp_rgb(cmp_px(a, x, bpp), bpp, &r, &g, &b);
            if (b2) {                           /* difference image */
                int r2, g2, bb2;
                cmp_rgb(cmp_px(b2, x, bpp), bpp, &r2, &g2, &bb2);
                r = abs(r - r2) * 4; g = abs(g - g2) * 4; b = abs(b - bb2) * 4;
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
            }
            if (line) {
                line[x * 3 + 0] = (uint8_t)b;
                line[x * 3 + 1] = (uint8_t)g;
                line[x * 3 + 2] = (uint8_t)r;
            }
        }
        if (line)
            fwrite(line, 1, row + pad, f);
    }
    free(line);
    fclose(f);
}

static void shadow_compare(Rt *r)
{
    D3D11_MAPPED_SUBRESOURCE m;
    const uint8_t *cpu = GUEST(r->va);
    uint32_t cpu_pitch = r->pitch;
    uint8_t *lin = NULL;
    uint32_t x, y, diff = 0, big = 0, maxd = 0;

    if (!r->staging) {
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D_GetDesc(r->tex, &td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &r->staging)))
            return;
    }
    /* The rasteriser's swizzled surface, row-major, to compare like for like. */
    if (r->swz) {
        lin = (uint8_t *)malloc((size_t)r->w * r->h * r->bpp);
        if (!lin)
            return;
        rt_swz_copy(r, lin, r->w * r->bpp, 0);
        cpu = lin;
        cpu_pitch = r->w * r->bpp;
    }
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)r->staging,
                                     (ID3D11Resource *)r->tex);
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)r->staging, 0,
                                       D3D11_MAP_READ, 0, &m))) {
        free(lin);
        return;
    }
    for (y = 0; y < r->h; y++) {
        const uint8_t *a = cpu + (size_t)y * cpu_pitch;
        const uint8_t *g = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
        for (x = 0; x < r->w; x++) {
            int r1, g1, b1, r2, g2, b2, d;
            cmp_rgb(cmp_px(a, x, r->bpp), r->bpp, &r1, &g1, &b1);
            cmp_rgb(cmp_px(g, x, r->bpp), r->bpp, &r2, &g2, &b2);
            d = abs(r1 - r2);
            if (abs(g1 - g2) > d) d = abs(g1 - g2);
            if (abs(b1 - b2) > d) d = abs(b1 - b2);
            if (d) diff++;
            if (d > 8) big++;
            if ((uint32_t)d > maxd) maxd = (uint32_t)d;
        }
    }
    s_cmp_frames++;
    if (big) {
        s_cmp_bad++;
        if (big > s_cmp_worst_px) {
            s_cmp_worst_px = big;
            s_cmp_worst_frame = s_frame;
            s_cmp_worst_va = r->va;
        }
        /* The first differing frames, then one in every 100 and every frame
         * that differs badly (an outlier is the interesting one), at most 40. */
        if (s_cmp_dumps < 40 && (s_cmp_dumps < 6 || s_frame % 100 == 0 || big > 10000)) {
            char p[160];
            CreateDirectoryA("logs", NULL);
            CreateDirectoryA("logs\\compare", NULL);
            snprintf(p, sizeof p, "logs\\compare\\f%05u_%08X_cpu.bmp", s_frame, r->va);
            cmp_write_bmp(p, r->w, r->h, cpu, cpu_pitch, r->bpp, NULL, 0);
            snprintf(p, sizeof p, "logs\\compare\\f%05u_%08X_gpu.bmp", s_frame, r->va);
            cmp_write_bmp(p, r->w, r->h, (const uint8_t *)m.pData, m.RowPitch, r->bpp, NULL, 0);
            snprintf(p, sizeof p, "logs\\compare\\f%05u_%08X_diff.bmp", s_frame, r->va);
            cmp_write_bmp(p, r->w, r->h, cpu, cpu_pitch, r->bpp,
                          (const uint8_t *)m.pData, m.RowPitch);
            fprintf(stderr, "[GPUBE-SHADOW] frame %u surface %08X %ux%u: %u px differ, "
                            "%u by more than 8, max %u -> logs\\compare\\f%05u_%08X_*.bmp\n",
                    s_frame, r->va, r->w, r->h, diff, big, maxd, s_frame, r->va);
            /* The next frame's draws, after the first difference and after
             * every tenth dump. */
            if (s_cmp_dumps % 10 == 0)
                s_log_draws = 2;
            s_cmp_dumps++;
        }
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)r->staging, 0);
    free(lin);
}

static void shadow_report(void)
{
    if (s_frame % 50)
        return;
    fprintf(stderr, "[GPUBE-SHADOW] %u surfaces compared, %u differ (worst: frame %u "
                    "surface %08X, %u px); draws: %u textured, %u untextured, %u combiner, "
                    "%u missing texture, %u missing stage texture\n",
            s_cmp_frames, s_cmp_bad, s_cmp_worst_frame, s_cmp_worst_va, s_cmp_worst_px,
            s_path[0], s_path[1], s_path[2], s_path[3], s_path[4]);
    fflush(stderr);
    s_cmp_frames = s_cmp_bad = s_cmp_worst_px = 0;
    memset(s_path, 0, sizeof s_path);
}

static void be_flip(void)
{
    uint32_t i;

    if (!s_ready)
        return;
    if (nv2a_backend_shadow()) {
        /* s_log_draws: 2 = set during this frame's comparison, log the next
         * one; 1 = that frame has been logged, stop. */
        if (s_log_draws == 1) {
            s_log_draws = 0;
            fprintf(stderr, "[GPUBE-DRAW] --- end of frame %u\n", s_frame);
        }
        for (i = 0; i < MAX_RT; i++)
            if (s_rt[i].tex && s_rt[i].drawn) {
                shadow_compare(&s_rt[i]);
                s_rt[i].drawn = 0;
            }
        if (s_log_draws == 2) {
            s_log_draws = 1;
            s_log_seq = 0;
            fprintf(stderr, "[GPUBE-DRAW] --- draws of frame %u\n", s_frame + 1);
        }
        shadow_report();
    }
    for (i = 0; i < MAX_RT; i++)
        if (s_rt[i].tex)
            rt_queue(&s_rt[i]);
    rt_complete_all();          /* one wait for the GPU, not one per surface */
    /* Read the surface in again on the next draw: between frames the CPU
     * may write it (image blits, a title clearing it by hand). */
    s_cur_rt = NULL;
    s_frame++;
    if (hud_squeeze() < 1.0f)
        xbox_FramebufferWindowSetFrameAspect(s_world_seen ? s_wide_aspect : 4.0f / 3.0f);
    s_world_seen = 0;
    hud_groups_flip();
    if (texstat_on())
        texstat_report();
    if (s_check_tex && (s_frame % 100) == 0) {
        fprintf(stderr, "[GPUBE-TEXCHECK] %u textures checked, %u mismatches\n",
                s_check_tex, s_check_bad);
        fflush(stderr);
    }
}

/* A semaphore release: what the GPU drew before it is in memory when the
 * title sees the release (D3D waits on it before it locks a surface). The
 * current target is written back when it is left or at the flip, as before. */
static void be_sync(void)
{
    if (s_ready)
        rt_complete_all();
}

/* The surface the title flipped to, at the size it was drawn (rt_scale),
 * for the window (xbox_FramebufferWindowSetGpu): copied on the GPU into one of
 * two textures shared with the window's device, which draws it from there --
 * nothing goes through the CPU. The two take turns, so the window can hold
 * the last one while the next is written; a keyed mutex (key 0 on both
 * sides) orders the two devices' use of each. Right after be_flip, so the
 * surface holds the finished frame. 0 for a surface at the title's size. */
static struct {
    ID3D11Texture2D *tex;
    IDXGIKeyedMutex *km;
    HANDLE           handle;
    UINT             w, h;
    DXGI_FORMAT      fmt;
} s_share[2];
static int s_share_next;

static void share_free(int i)
{
    if (s_share[i].km) IDXGIKeyedMutex_Release(s_share[i].km);
    if (s_share[i].tex) ID3D11Texture2D_Release(s_share[i].tex);
    memset(&s_share[i], 0, sizeof s_share[i]);
}

static int be_window_frame(uint32_t fb_va, uint32_t pitch, XboxFramebufferGpuFrame *out)
{
    D3D11_TEXTURE2D_DESC td;
    uint32_t i;
    int k = s_share_next;
    Rt *r = NULL;

    for (i = 0; i < MAX_RT; i++)
        if (s_rt[i].tex && s_rt[i].va == fb_va && s_rt[i].big && !s_rt[i].swz
                && s_rt[i].pitch == pitch) {
            r = &s_rt[i];
            break;
        }
    if (!r)
        return 0;
    ID3D11Texture2D_GetDesc(r->tex, &td);
    if (!s_share[k].tex || s_share[k].w != td.Width || s_share[k].h != td.Height
            || s_share[k].fmt != td.Format) {
        IDXGIResource *res = NULL;
        share_free(k);
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = 0;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &s_share[k].tex))
                || FAILED(ID3D11Texture2D_QueryInterface(s_share[k].tex, &IID_IDXGIKeyedMutex,
                                                         (void **)&s_share[k].km))
                || FAILED(ID3D11Texture2D_QueryInterface(s_share[k].tex, &IID_IDXGIResource,
                                                         (void **)&res))
                || FAILED(IDXGIResource_GetSharedHandle(res, &s_share[k].handle))) {
            if (res) IDXGIResource_Release(res);
            share_free(k);
            return 0;
        }
        IDXGIResource_Release(res);
        s_share[k].w = td.Width;
        s_share[k].h = td.Height;
        s_share[k].fmt = td.Format;
    }
    if (IDXGIKeyedMutex_AcquireSync(s_share[k].km, 0, 100) != S_OK)
        return 0;
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)s_share[k].tex,
                                     (ID3D11Resource *)r->tex);
    IDXGIKeyedMutex_ReleaseSync(s_share[k].km, 0);
    ID3D11DeviceContext_Flush(s_ctx);   /* on its way before the window waits for it */
    s_share_next = k ^ 1;
    out->handle = s_share[k].handle;
    /* Relative to the shown picture: a surface drawn without its samples is
     * the picture's size times the factors times the samples left out. */
    out->fx = r->fx * (float)r->ax;
    out->fy = r->fy * (float)r->ay;
    out->scale = (uint32_t)lrintf(out->fy);
    out->scale_x = (uint32_t)lrintf(out->fx);
    out->flags = (r->ax > 1 || r->ay > 1) ? 1u : 0u;
    return 1;
}

/* Vertex programs run here unless RECOMP_GPU_VSH=0 (the executor asks). */
static const Nv2aBackend s_backend = { be_clear, be_draw, be_flip, be_sync,
                                       NV2A_BACKEND_CAP_PROGRAM };

void nightfire_gpu_backend_register(void)
{
    nv2a_backend_register(&s_backend);
    if (rt_scaled())
        xbox_FramebufferWindowSetGpu(be_window_frame);
    fprintf(stderr, "[GPUBE] D3D11 back end registered\n");
}
