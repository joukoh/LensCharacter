// LensKernel.h — the per-pixel lens simulation, shared by the CPU path and the Metal path.
//
// The CPU build #includes this file. The Metal build embeds this same text as a string
// (see Makefile) and compiles it at runtime with LC_METAL defined, so both paths run
// identical math. Keep to the subset both languages accept: scalar floats/ints, plain
// structs, no recursion, no std:: containers.

#ifndef LC_KERNEL_H
#define LC_KERNEL_H

#ifdef LC_METAL
  #define LC_CONST constant
  #define LC_SRC const device float*
  #define LC_FN inline
  // Fast trig for the sampling loops only. The library is compiled without fast math so the
  // transfer functions and NaN scrub stay exact; the loops pass these small, pre-wrapped angles.
  #define LC_FCOS(x) metal::fast::cos(x)
  #define LC_FSIN(x) metal::fast::sin(x)
  #define LC_FSQRT(x) metal::fast::sqrt(x)
#else
  #include <cmath>
  #define LC_CONST const
  #define LC_SRC const float*
  #define LC_FN static inline
  #define LC_FCOS(x) std::cos(x)
  #define LC_FSIN(x) std::sin(x)
  #define LC_FSQRT(x) std::sqrt(x)
#endif

namespace lc {

#ifndef LC_METAL
using std::sqrt; using std::cos; using std::sin; using std::floor; using std::fabs;
using std::fmax; using std::fmin; using std::pow; using std::fmod; using std::log2;
using std::exp2; using std::round;
#endif

enum { ENC_LINEAR = 0, ENC_DAVINCI_INTERMEDIATE = 1, ENC_GAMMA24 = 2 };
enum { FOCUS_UNIFORM = 0, FOCUS_RADIAL = 1, FOCUS_ALPHA = 2, FOCUS_ALPHA_INV = 3 };

#define LC_MAX_MIPS 10

// Everything the kernel needs for one frame. Layout must stay identical in C++ and MSL:
// only 4-byte scalars and arrays of them.
struct LCParams
{
    int   width, height, rowStride;   // rowStride in floats
    int   encoding;
    int   decodeOnFetch;               // 1: source is still encoded (CPU path); 0: already linear (Metal pre-pass)

    float cx, cy;                      // optical centre, pixels
    float pxToRho;                     // 1 / half-diagonal in pixels

    // Distortion, forward model in rho units: rho_d = rho_u * (d0 + d1 r + d2 r^2 + d3 r^3 + d4 r^4)
    int   distOn;
    float dist[5];
    float fit;                         // scale on source coordinates (auto-zoom for barrel)

    // Lateral CA, red/blue relative to green, same polynomial form
    int   tcaOn;
    float tcaR[5];
    float tcaB[5];

    // Relative illumination, two blended sources: gain = VA^expA * VB^expB, V = 1 + k1 r^2 + k2 r^4 + k3 r^6
    int   vigOn;
    float vigA[3]; float vigAExp;
    float vigB[3]; float vigBExp;

    // Character (bokeh shape and field behaviour)
    float blades, curve, rotation, catEye, swirl, field, rim, onion, loca;
    float glow, veil, squeeze, tint;

    // Defocus
    float defocusPx;
    float focusFloorPx;                // defocus inside the sharp area (Sharp Area Softness); 0 = fully sharp
    float edgeBlurPx;                  // field-curvature blur at the corner per unit of `field`, independent of defocus
    int   focusMode;
    float focusRadius, focusFalloff;
    float hlBoost, hlThresh;
    int   samples;                     // maximum bokeh samples; fewer are used for small blur radii
    int   adaptiveSamples;             // 1: scale sample count with blur area
    int   glowSamples;
    float hlCap;                       // highlight weight levels off at 1 + hlBoost * hlCap

    // Prefiltered sampling. A blur of radius R px read with N point samples leaves gaps of
    // about R·sqrt(π/N) px between them; a light smaller than that shows up as N separate
    // dots instead of a disc. Samples therefore read a 2× box pyramid of the linear source at
    // the level whose texel matches the gap. Level k lives at mipOffset[k-1] (floats, rows
    // tightly packed, 8 floats per texel: see mipBuildTexel) with size mipW[k-1] × mipH[k-1].
    // mipLevels 0: plain point samples.
    int   mipLevels;
    int   mipFloats;                   // total size of the pyramid buffer, in floats
    int   mipW[LC_MAX_MIPS], mipH[LC_MAX_MIPS], mipOffset[LC_MAX_MIPS];
    float mipBias;                     // added to log2(gap) when choosing the level
    float glowMipBias;                 // the same for the glow, whose halo is only a few gaps wide
};

struct LCColor { float r, g, b, a; };

#define LC_PI 3.14159265f
#define LC_TWO_PI 6.28318531f
#define LC_GOLDEN 2.39996323f
#define LC_GOLDEN_TURNS 0.381966011f   // LC_GOLDEN / 2π

LC_FN float sat(float x) { return fmin(fmax(x, 0.0f), 1.0f); }
LC_FN float mixf(float a, float b, float t) { return a + (b - a) * t; }
LC_FN float wrap01(float x) { return x - floor(x); }
LC_FN float sstep(float a, float b, float x)
{
    float t = sat((x - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}
// sstep with the reciprocal of (b - a) precomputed, for loops where a and b are constant
LC_FN float sstepK(float a, float invBA, float x)
{
    float t = sat((x - a) * invBA);
    return t * t * (3.0f - 2.0f * t);
}

// Angle of golden-angle sample i, wrapped to [0, 2π) so fast trig stays accurate
LC_FN float goldenAngle(int i)
{
    return wrap01((float)i * LC_GOLDEN_TURNS) * LC_TWO_PI;
}

// ---- transfer functions -------------------------------------------------------------
LC_FN float decode(int enc, float v)
{
    if (enc == ENC_DAVINCI_INTERMEDIATE)
        return v <= 0.02740668f ? v / 10.44426855f : exp2(v / 0.07329248f - 7.0f) - 0.0075f;
    if (enc == ENC_GAMMA24)
        return v > 0.0f ? pow(v, 2.4f) : v;
    return v;
}

LC_FN float encode(int enc, float v)
{
    if (enc == ENC_DAVINCI_INTERMEDIATE)
        return v <= 0.00262409f ? v * 10.44426855f : (log2(v + 0.0075f) + 7.0f) * 0.07329248f;
    if (enc == ENC_GAMMA24)
        return v > 0.0f ? pow(v, 1.0f / 2.4f) : v;
    return v;
}

// ---- image access (decoded to linear light) ---------------------------------------------
LC_FN float fetchN(LC_SRC src, LC_CONST LCParams& P, float x, float y, int c)
{
    int ix = (int)fmin(fmax(round(x), 0.0f), (float)(P.width - 1));
    int iy = (int)fmin(fmax(round(y), 0.0f), (float)(P.height - 1));
    float v = src[iy * P.rowStride + ix * 4 + c];
    return P.decodeOnFetch != 0 ? decode(P.encoding, v) : v;
}

LC_FN float fetchB(LC_SRC src, LC_CONST LCParams& P, float x, float y, int c)
{
    x = fmin(fmax(x, 0.0f), (float)(P.width - 1));
    y = fmin(fmax(y, 0.0f), (float)(P.height - 1));
    int x0 = (int)floor(x);
    int y0 = (int)floor(y);
    int x1 = x0 + 1 < P.width ? x0 + 1 : x0;
    int y1 = y0 + 1 < P.height ? y0 + 1 : y0;
    float fx = x - (float)x0;
    float fy = y - (float)y0;
    float a = src[y0 * P.rowStride + x0 * 4 + c];
    float b = src[y0 * P.rowStride + x1 * 4 + c];
    float d = src[y1 * P.rowStride + x0 * 4 + c];
    float e = src[y1 * P.rowStride + x1 * 4 + c];
    if (P.decodeOnFetch != 0)
    {
        a = decode(P.encoding, a); b = decode(P.encoding, b);
        d = decode(P.encoding, d); e = decode(P.encoding, e);
    }
    return mixf(mixf(a, b, fx), mixf(d, e, fx), fy);
}

// ---- prefiltered access (mip pyramid of the linear source) --------------------------------
LC_FN float luma(float r, float g, float b)
{
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

// Extra weight for a bright sample, so highlights bloom into bokeh balls. Grows as hl² for
// display-referred values, then levels off at hlCap: scene-referred lights (10–100× white)
// would otherwise outweigh the rest of the disc thousands of times over.
LC_FN float hlWeight(LC_CONST LCParams& P, float hl)
{
    float h2 = hl * hl;
    return h2 / (1.0f + h2 / P.hlCap);
}

LC_FN float bokehWeight(LC_CONST LCParams& P, float r, float g, float b)
{
    float hl = fmax(luma(r, g, b) - P.hlThresh, 0.0f) * 4.0f;
    return 1.0f + P.hlBoost * hlWeight(P, hl);
}

// The glow's own, fixed highlight emphasis. It makes up for highlights that a display
// encoding clipped at 1.0. Scene-referred input (DaVinci Intermediate, Linear) still has the
// light, so there the glow spreads it as it is: emphasis on top would turn every small glint
// into a flat, full-brightness disc the size of the halo.
LC_FN float glowWeight(LC_CONST LCParams& P, float r, float g, float b)
{
    if (P.encoding != ENC_GAMMA24) return 1.0f;
    float hl = fmax(luma(r, g, b) - 0.5f, 0.0f) * 3.0f;
    return 1.0f + 2.0f * hlWeight(P, hl);
}

enum { SET_BOKEH = 0, SET_GLOW = 1 };

LC_FN float sampleWeight(LC_CONST LCParams& P, int set, float r, float g, float b)
{
    return set == SET_GLOW ? glowWeight(P, r, g, b) : bokehWeight(P, r, g, b);
}

// Bokeh and glow are weighted averages (bright samples count more), so the pyramid stores
// weighted colour and weight, (r·w, g·w, b·w, w), once per weighting: floats 0–3 of a texel
// use the bokeh weight, 4–7 the glow weight. Averaging those is the same weighted average
// pre-summed over the texel, wherever a light falls on the pixel grid.
//
// One set of one texel of level k >= 1: the 2×2 box average of level k-1. For k = 1 the
// inputs are source pixels, decoded to linear and weighted. The GPU builds one level per
// dispatch; the CPU calls buildMips below.
LC_FN LCColor mipBuildTexel(LC_SRC src, LC_SRC mips, LC_CONST LCParams& P, int k, int x, int y, int set)
{
    int pw = k == 1 ? P.width : P.mipW[k - 2];
    int ph = k == 1 ? P.height : P.mipH[k - 2];
    int xs[2] = {2 * x, 2 * x + 1 < pw ? 2 * x + 1 : 2 * x};
    int ys[2] = {2 * y, 2 * y + 1 < ph ? 2 * y + 1 : 2 * y};
    float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++)
        {
            float v[4];
            if (k == 1)
            {
                for (int c = 0; c < 3; c++)
                {
                    float e = src[ys[j] * P.rowStride + xs[i] * 4 + c];
                    v[c] = P.decodeOnFetch != 0 ? decode(P.encoding, e) : e;
                }
                float w = sampleWeight(P, set, v[0], v[1], v[2]);
                v[0] *= w; v[1] *= w; v[2] *= w; v[3] = w;
            }
            else
                for (int c = 0; c < 4; c++) v[c] = mips[P.mipOffset[k - 2] + (ys[j] * pw + xs[i]) * 8 + set * 4 + c];
            for (int c = 0; c < 4; c++) acc[c] += v[c];
        }
    LCColor o;
    o.r = acc[0] * 0.25f; o.g = acc[1] * 0.25f; o.b = acc[2] * 0.25f; o.a = acc[3] * 0.25f;
    return o;
}

// One RGBA texel of the pyramid, read as a single vector load on the GPU
#ifdef LC_METAL
LC_FN LCColor loadTexel(LC_SRC m, int i)
{
    float4 v = *((const device float4*)(m + i));
    LCColor c;
    c.r = v.x; c.g = v.y; c.b = v.z; c.a = v.w;
    return c;
}
#else
LC_FN LCColor loadTexel(LC_SRC m, int i)
{
    LCColor c;
    c.r = m[i]; c.g = m[i + 1]; c.b = m[i + 2]; c.a = m[i + 3];
    return c;
}
#endif

// Bilinear read of one weighted set of level k >= 1, at level-0 pixel coordinates
LC_FN LCColor fetchMip(LC_SRC mips, LC_CONST LCParams& P, int k, float x, float y, int set)
{
    int w = P.mipW[k - 1], h = P.mipH[k - 1];
    float s = 1.0f / (float)(1 << k);
    float u = fmin(fmax((x + 0.5f) * s - 0.5f, 0.0f), (float)(w - 1));
    float v = fmin(fmax((y + 0.5f) * s - 0.5f, 0.0f), (float)(h - 1));
    int x0 = (int)floor(u), y0 = (int)floor(v);
    int x1 = x0 + 1 < w ? x0 + 1 : x0;
    int y1 = y0 + 1 < h ? y0 + 1 : y0;
    float fx = u - (float)x0, fy = v - (float)y0;
    LC_SRC m = mips + P.mipOffset[k - 1] + set * 4;
    LCColor a = loadTexel(m, (y0 * w + x0) * 8);
    LCColor b = loadTexel(m, (y0 * w + x1) * 8);
    LCColor d = loadTexel(m, (y1 * w + x0) * 8);
    LCColor e = loadTexel(m, (y1 * w + x1) * 8);
    LCColor o;
    o.r = mixf(mixf(a.r, b.r, fx), mixf(d.r, e.r, fx), fy);
    o.g = mixf(mixf(a.g, b.g, fx), mixf(d.g, e.g, fx), fy);
    o.b = mixf(mixf(a.b, b.b, fx), mixf(d.b, e.b, fx), fy);
    o.a = mixf(mixf(a.a, b.a, fx), mixf(d.a, e.a, fx), fy);
    return o;
}

// Which pyramid levels a pixel reads: level k0, blended by t toward k0 + 1
struct LCLod { int k0; float t; };

// True when the R and B sample positions are within a quarter texel of G at the finest pyramid level this pixel reads (level 1 when it blends up from level 0)
LC_FN bool sameSpotFor(LCLod L, float caShiftPx)
{
    int k = L.k0 > 0 ? L.k0 : 1;
    return caShiftPx < 0.25f * (float)(1 << k);
}

LC_FN LCLod lodFor(LC_CONST LCParams& P, float gapPx, float bias)
{
    LCLod L;
    L.k0 = 0; L.t = 0.0f;
    if (P.mipLevels <= 0 || gapPx <= 1e-3f) return L;
    float lod = fmin(log2(gapPx) + bias, (float)P.mipLevels);
    if (lod <= 0.0f) return L;
    L.k0 = (int)floor(lod);
    L.t = lod - (float)L.k0;
    if (L.k0 >= P.mipLevels) { L.k0 = P.mipLevels; L.t = 0.0f; }
    return L;
}

// Weighted colour (r·w, g·w, b·w) and weight w of one sample, each channel at its own
// (lateral-CA-shifted) position. The weight is read at the green position. Level 0 is read
// bilinearly: with nearest samples a blur under ~1.3 px did nothing and then jumped.
struct LCPre { float r, g, b, w; };

// sameSpot: the channels' lateral-CA shift is under a quarter texel at this pixel's level, so
// on pyramid levels all channels are read at the green position (one read instead of three).
LC_FN LCPre fetchPreLevel(LC_SRC src, LC_SRC mips, LC_CONST LCParams& P, int k,
                          float xR, float yR, float xG, float yG, float xB, float yB, int set, bool sameSpot)
{
    LCPre o;
    if (k == 0)
    {
        float sR = fetchB(src, P, xR, yR, 0);
        float sG = fetchB(src, P, xG, yG, 1);
        float sB = fetchB(src, P, xB, yB, 2);
        float w = sampleWeight(P, set, sR, sG, sB);
        o.r = sR * w; o.g = sG * w; o.b = sB * w; o.w = w;
    }
    else
    {
        LCColor g = fetchMip(mips, P, k, xG, yG, set);
        o.g = g.g; o.w = g.a;
        o.r = sameSpot ? g.r : fetchMip(mips, P, k, xR, yR, set).r;
        o.b = sameSpot ? g.b : fetchMip(mips, P, k, xB, yB, set).b;
    }
    return o;
}

LC_FN LCPre fetchPre(LC_SRC src, LC_SRC mips, LC_CONST LCParams& P, LCLod L,
                     float xR, float yR, float xG, float yG, float xB, float yB, int set, bool sameSpot)
{
    LCPre a = fetchPreLevel(src, mips, P, L.k0, xR, yR, xG, yG, xB, yB, set, sameSpot);
    if (L.t <= 0.0f) return a;
    LCPre b = fetchPreLevel(src, mips, P, L.k0 + 1, xR, yR, xG, yG, xB, yB, set, sameSpot);
    a.r = mixf(a.r, b.r, L.t); a.g = mixf(a.g, b.g, L.t);
    a.b = mixf(a.b, b.b, L.t); a.w = mixf(a.w, b.w, L.t);
    return a;
}

// ---- radial polynomials ----------------------------------------------------------------
LC_FN float polyEval(LC_CONST float* a, float r)
{
    return a[0] + r * (a[1] + r * (a[2] + r * (a[3] + r * a[4])));
}

LC_FN float polyDeriv(LC_CONST float* a, float r)
{
    return a[1] + r * (2.0f * a[2] + r * (3.0f * a[3] + r * 4.0f * a[4]));
}

// Solve u * P(u) = t for u (Newton). Used to turn the lens's forward model into a lookup.
LC_FN float polyInverse(LC_CONST float* a, float t)
{
    float u = t / fmax(a[0], 0.05f);
    for (int i = 0; i < 8; i++)
    {
        float p = polyEval(a, u);
        float g = u * p - t;
        float dg = p + u * polyDeriv(a, u);
        if (fabs(dg) < 1e-4f) break;
        u = fmax(u - g / dg, 0.0f);
    }
    return u;
}

// ---- the whole lens, one output pixel -----------------------------------------------------
LC_FN LCColor shade(LC_CONST LCParams& P, LC_SRC src, LC_SRC mips, int px, int py)
{
    float H = (float)P.height;

    // Field position of this output pixel
    float dx = (float)px - P.cx;
    float dy = (float)py - P.cy;
    float rpx = sqrt(dx * dx + dy * dy);
    float r = rpx * P.pxToRho;           // 1.0 at the frame corner
    float r2 = r * r;
    float rdx = 1.0f, rdy = 0.0f;
    if (rpx > 1e-4f) { rdx = dx / rpx; rdy = dy / rpx; }
    float tdx = -rdy, tdy = rdx;

    // Geometry: undo lateral CA per channel, then undo distortion, to find where each
    // channel's light came from in the source plate.
    float rhoR = r, rhoG = r, rhoB = r;
    if (P.tcaOn != 0)
    {
        rhoR = polyInverse(P.tcaR, r);
        rhoB = polyInverse(P.tcaB, r);
    }
    if (P.distOn != 0)
    {
        rhoR = polyInverse(P.dist, rhoR);
        rhoG = polyInverse(P.dist, rhoG);
        rhoB = polyInverse(P.dist, rhoB);
    }
    float k = P.fit / P.pxToRho;
    float xR = P.cx + rdx * rhoR * k, yR = P.cy + rdy * rhoR * k;
    float xG = P.cx + rdx * rhoG * k, yG = P.cy + rdy * rhoG * k;
    float xB = P.cx + rdx * rhoB * k, yB = P.cy + rdy * rhoB * k;
    // Lateral CA separates the channels along the radius; this is the larger of R and B's offset
    float caShift = fmax(fabs(rhoR - rhoG), fabs(rhoB - rhoG)) * k;

    float alpha = src[py * P.rowStride + px * 4 + 3];

    // Defocus amount
    float m;
    if (P.focusMode == FOCUS_UNIFORM)     m = 1.0f;
    else if (P.focusMode == FOCUS_RADIAL) m = sstep(P.focusRadius, P.focusRadius + P.focusFalloff, r);
    else if (P.focusMode == FOCUS_ALPHA)  m = sat(alpha);
    else                                  m = 1.0f - sat(alpha);
    float mField = P.field * r2 * (P.focusMode == FOCUS_UNIFORM ? 1.0f : 0.5f);
    // The sharp area takes focusFloorPx (a slight overall softness), rising to defocusPx where
    // m reaches 1. Vintage lenses go soft toward the corners even when the subject is in focus,
    // so field curvature adds its own blur (and with it cat-eye and swirl) at any defocus.
    float Rpx = mixf(P.focusFloorPx, P.defocusPx, m) + P.defocusPx * mField + P.edgeBlurPx * P.field * r2;

    // Sharp path
    float shR = fetchB(src, P, xR, yR, 0);
    float shG = fetchB(src, P, xG, yG, 1);
    float shB = fetchB(src, P, xB, yB, 2);
    float outR = shR, outG = shG, outB = shB;

    // Bokeh gather: shaped aperture kernel that changes across the field
    // Bilinear samples make the gather shrink smoothly to the sharp image as Rpx goes to 0, so
    // the blur is used from a twentieth of a pixel up (it used to fade in from 0.6 to 2 px,
    // which looked like a sharp image under a soft haze rather than a slight defocus)
    float blurMix = sstep(0.02f, 0.1f, Rpx);
    if (blurMix > 0.0f)
    {
        // A disc of radius R px needs roughly R² samples to cover it; small blurs need far
        // fewer than the maximum, which is where most of the frame sits in Sharp Centre mode.
        int N = P.samples;
        if (P.adaptiveSamples != 0)
            N = (int)fmin((float)P.samples, fmax(12.0f, Rpx * Rpx * 0.9f));
        float loca = P.loca;
        float extent = 1.0f + fabs(loca) * 0.35f;
        float eR = 1.0f + loca * 0.35f;
        float eG = 1.0f;
        float eB = 1.0f - loca * 0.35f;
        float edgeSoft = 0.05f;
        float seg = P.blades >= 3.0f ? 2.0f * LC_PI / P.blades : 0.0f;
        float rotRad = P.rotation * LC_PI / 180.0f;
        float ceShift = P.catEye * r * 1.1f;
        float ceX = rdx * ceShift, ceY = rdy * ceShift;
        float tStretch = 1.0f + P.swirl * r2 * 0.8f;
        float rSquash = 1.0f / (1.0f + P.swirl * r2 * 0.3f);
        float rim = fmin(fmax(P.rim, -1.0f), 1.0f);

        // Loop invariants, hoisted: everything below that does not depend on the sample index
        float invN = 1.0f / (float)N;
        float invSeg = seg > 0.0f ? 1.0f / seg : 0.0f;
        float bladeCos = seg > 0.0f ? cos(LC_PI / P.blades) : 1.0f;
        float segPhase = 64.0f * LC_PI - rotRad;   // same offset the original fmod used
        float invEdge = 1.0f / (-2.0f * edgeSoft);  // 1 / (b - a) for sstep(x + e, x - e, …)
        bool onionOn = P.onion != 0.0f;

        // Pyramid level that fills the gaps between samples (see LCParams::mipLevels). The
        // disc's area is stretched by swirl and squeeze, and the gap with it.
        float gap = Rpx * extent * LC_FSQRT(LC_PI * invN * tStretch * rSquash * fmax(P.squeeze, 0.05f));
        LCLod lod = lodFor(P, gap, P.mipBias);
        bool sameSpot = sameSpotFor(lod, caShift);

        float aR = 0.0f, aG = 0.0f, aB = 0.0f;
        float wR = 0.0f, wG = 0.0f, wB = 0.0f;

        for (int i = 0; i < N; i++)
        {
            float fi = (float)i + 0.5f;
            float rn = LC_FSQRT(fi * invN) * extent;
            float th = goldenAngle(i);

            float shape = 1.0f;
            if (seg > 0.0f)
            {
                float a = wrap01((th + segPhase) * invSeg) * seg - 0.5f * seg;
                float poly = bladeCos / LC_FCOS(a);
                shape = mixf(poly, 1.0f, P.curve);
            }
            float ax = LC_FCOS(th) * rn * shape;
            float ay = LC_FSIN(th) * rn * shape;

            // mechanical vignetting (cat eye)
            float ddx = ax + ceX, ddy = ay + ceY;
            float ce = sstepK(1.0f + edgeSoft, invEdge, LC_FSQRT(ddx * ddx + ddy * ddy) / shape);
            if (ce <= 0.0f) continue;

            // swirl: stretch tangentially
            float orad = (ax * rdx + ay * rdy) * rSquash;
            float otan = (ax * tdx + ay * tdy) * tStretch;
            float ox = (orad * rdx + otan * tdx) * Rpx;
            float oy = (orad * rdy + otan * tdy) * Rpx * P.squeeze;

            LCPre sp = fetchPre(src, mips, P, lod, xR + ox, yR + oy, xG + ox, yG + oy, xB + ox, yB + oy, SET_BOKEH, sameSpot);

            float onionW = onionOn ? 1.0f + P.onion * 0.6f * LC_FCOS(wrap01(rn * (38.0f / LC_TWO_PI)) * LC_TWO_PI) : 1.0f;
            float base = ce * onionW;   // the highlight weight is already in sp

            float pR = sat(rn / eR), pG = sat(rn / eG), pB = sat(rn / eB);
            float rimR = rim >= 0.0f ? 1.0f + rim * 4.0f * pR * pR * pR * pR : 1.0f + rim * pR * pR;
            float rimG = rim >= 0.0f ? 1.0f + rim * 4.0f * pG * pG * pG * pG : 1.0f + rim * pG * pG;
            float rimB = rim >= 0.0f ? 1.0f + rim * 4.0f * pB * pB * pB * pB : 1.0f + rim * pB * pB;
            float kR = sstepK(eR + edgeSoft, invEdge, rn) * rimR * base;
            float kG = sstepK(eG + edgeSoft, invEdge, rn) * rimG * base;
            float kB = sstepK(eB + edgeSoft, invEdge, rn) * rimB * base;

            aR += sp.r * kR; wR += sp.w * kR;
            aG += sp.g * kG; wG += sp.w * kG;
            aB += sp.b * kB; wB += sp.w * kB;
        }
        if (wR > 0.0f) outR = mixf(shR, aR / wR, blurMix);
        if (wG > 0.0f) outG = mixf(shG, aG / wG, blurMix);
        if (wB > 0.0f) outB = mixf(shB, aB / wB, blurMix);
    }

    // Glow (spherical aberration) and veiling glare
    if (P.glow > 0.0f || P.veil > 0.0f)
    {
        float gR = 0.0f, gG = 0.0f, gB = 0.0f, gw = 0.0f;
        float gRad = 0.012f * H;
        int G = P.glowSamples;
        float invG = 1.0f / (float)G;
        LCLod glod = lodFor(P, gRad * LC_FSQRT(LC_PI * invG), P.glowMipBias);
        bool gSameSpot = sameSpotFor(glod, caShift);
        for (int i = 0; i < G; i++)
        {
            float fi = (float)i + 0.5f;
            float rn = LC_FSQRT(fi * invG);
            float th = goldenAngle(i);
            float ox = LC_FCOS(th) * rn * gRad;
            float oy = LC_FSIN(th) * rn * gRad;
            LCPre sp = fetchPre(src, mips, P, glod, xR + ox, yR + oy, xG + ox, yG + oy, xB + ox, yB + oy, SET_GLOW, gSameSpot);
            float kk = 1.0f - rn * 0.7f;   // the highlight weight is already in sp
            gR += sp.r * kk; gG += sp.g * kk; gB += sp.b * kk; gw += sp.w * kk;
        }
        gR /= gw; gG /= gw; gB /= gw;
        float glow = sat(P.glow);
        outR = mixf(outR, fmax(outR, gR), glow);
        outG = mixf(outG, fmax(outG, gG), glow);
        outB = mixf(outB, fmax(outB, gB), glow);
        outR += P.veil * gR; outG += P.veil * gG; outB += P.veil * gB;
        float norm = 1.0f / (1.0f + P.veil * 0.5f);
        outR *= norm; outG *= norm; outB *= norm;
    }

    // Relative illumination (measured vignetting), blended in log space
    if (P.vigOn != 0)
    {
        float r4 = r2 * r2, r6 = r4 * r2;
        float vA = fmax(1.0f + P.vigA[0] * r2 + P.vigA[1] * r4 + P.vigA[2] * r6, 1e-3f);
        float vB = fmax(1.0f + P.vigB[0] * r2 + P.vigB[1] * r4 + P.vigB[2] * r6, 1e-3f);
        float v = pow(vA, P.vigAExp) * pow(vB, P.vigBExp);
        outR *= v; outG *= v; outB *= v;
    }

    // Coating tint
    outR *= 1.0f + P.tint * 0.06f;
    outG *= 1.0f + P.tint * 0.01f;
    outB *= 1.0f - P.tint * 0.14f;

    LCColor c;
    c.r = encode(P.encoding, outR);
    c.g = encode(P.encoding, outG);
    c.b = encode(P.encoding, outB);
    c.a = alpha;
    // NaN / Inf scrub
    if (!(c.r == c.r) || fabs(c.r) > 1e20f) c.r = 0.0f;
    if (!(c.g == c.g) || fabs(c.g) > 1e20f) c.g = 0.0f;
    if (!(c.b == c.b) || fabs(c.b) > 1e20f) c.b = 0.0f;
    return c;
}

#ifndef LC_METAL
// ---- host side (C++ only) ---------------------------------------------------------------------
// Lays out the pyramid for P.width × P.height: `levels` levels, stopping early at 1 × 1.
inline void planMips(LCParams& P, int levels)
{
    int w = P.width, h = P.height, off = 0, n = 0;
    levels = levels < LC_MAX_MIPS ? levels : LC_MAX_MIPS;
    while (n < levels && (w > 1 || h > 1))
    {
        w = (w + 1) / 2; h = (h + 1) / 2;
        P.mipW[n] = w; P.mipH[n] = h; P.mipOffset[n] = off;
        off += w * h * 8;
        n++;
    }
    P.mipLevels = n;
    P.mipFloats = off;
}

// Builds the whole pyramid on the CPU into `mips` (P.mipFloats floats), level by level.
inline void buildMips(const LCParams& P, const float* src, float* mips)
{
    for (int k = 1; k <= P.mipLevels; k++)
        for (int y = 0; y < P.mipH[k - 1]; y++)
            for (int x = 0; x < P.mipW[k - 1]; x++)
                for (int set = 0; set < 2; set++)
                {
                    LCColor c = mipBuildTexel(src, mips, P, k, x, y, set);
                    float* d = mips + P.mipOffset[k - 1] + (y * P.mipW[k - 1] + x) * 8 + set * 4;
                    d[0] = c.r; d[1] = c.g; d[2] = c.b; d[3] = c.a;
                }
}
#endif

} // namespace lc

#endif
