// selftest.cpp — checks the library, mixer and kernel outside Resolve.
//
//  1. Library loads and lens search finds the expected profiles.
//  2. Overdrive never produces an invalid (folding) warp, even at absurd settings.
//  3. CPU and the GPU (Metal on macOS, CUDA where built with it) render the same test chart
//     to within float tolerance.
//  4. Writes before/after PPM images to the output directory for a visual check.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../src/LensBrowser.h"
#include "../src/LensBrowserModel.h"
#include "../src/LensKernel.h"
#include "../src/Mixer.h"
#include "../src/Platform.h"
#include "../src/Profiles.h"

#if defined(__APPLE__)
int LensCharacterRunMetalTest(const lc::LCParams& p_Params, const float* p_In, float* p_Out, double* p_GpuMs);
static const char* const kGpu = "Metal";
#elif defined(LC_HAVE_CUDA)
int LensCharacterRunCudaTest(const lc::LCParams& p_Params, const float* p_In, float* p_Out, double* p_GpuMs);
static const char* const kGpu = "CUDA";
#else
static const char* const kGpu = nullptr;   // CPU-only build
#endif

static std::vector<float> renderCPU(const lc::LCParams& p_Params, const std::vector<float>& src);

// Renders on the GPU. Returns 0 on success. In a CPU-only build, renders on the CPU instead and
// returns 0, so the checks below still run (the CPU-vs-GPU comparison is skipped).
static int runGpu(const lc::LCParams& p_Params, const float* p_In, float* p_Out, double* p_GpuMs = nullptr)
{
#if defined(__APPLE__)
    return LensCharacterRunMetalTest(p_Params, p_In, p_Out, p_GpuMs);
#elif defined(LC_HAVE_CUDA)
    return LensCharacterRunCudaTest(p_Params, p_In, p_Out, p_GpuMs);
#else
    lc::LCParams P = p_Params;
    P.rowStride = P.width * 4;
    std::vector<float> in(p_In, p_In + (size_t)P.width * P.height * 4);
    std::vector<float> out = renderCPU(P, in);
    std::copy(out.begin(), out.end(), p_Out);
    if (p_GpuMs) *p_GpuMs = 0.0;
    return 0;
#endif
}

static int s_Failures = 0;
#define CHECK(cond, ...) do { if (cond) printf("  ok    " __VA_ARGS__); else { printf("  FAIL  " __VA_ARGS__); s_Failures++; } printf("\n"); } while (0)

// Test chart: grey gradient, grid lines (distortion), point lights (bokeh), a white bar (CA).
static std::vector<float> makeChart(int w, int h)
{
    std::vector<float> img((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            float* p = &img[((size_t)y * w + x) * 4];
            float v = 0.12f + 0.1f * (float)x / w;
            bool grid = (x % (w / 16) < 2) || (y % (h / 9) < 2);
            if (grid) v = 0.55f;
            p[0] = v; p[1] = v; p[2] = v * 1.05f; p[3] = 1.0f;
        }
    // point lights
    for (int j = 1; j < 6; j++)
        for (int i = 1; i < 10; i++)
        {
            int cx = i * w / 10, cy = j * h / 6;
            for (int dy = -2; dy <= 2; dy++)
                for (int dx = -2; dx <= 2; dx++)
                {
                    float* p = &img[((size_t)(cy + dy) * w + (cx + dx)) * 4];
                    p[0] = 1.0f; p[1] = 0.92f; p[2] = 0.75f;
                }
        }
    return img;
}

static void writePPM(const std::string& path, const std::vector<float>& img, int w, int h)
{
    FILE* f = lc::platform::openFile(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--)   // OFX images are bottom-up
        for (int x = 0; x < w; x++)
            for (int c = 0; c < 3; c++)
            {
                float v = img[((size_t)y * w + x) * 4 + c];
                v = v < 0 ? 0 : v > 1 ? 1 : v;
                fputc((int)(v * 255.0f + 0.5f), f);
            }
    fclose(f);
}

static std::vector<float> renderCPU(const lc::LCParams& p_Params, const std::vector<float>& src)
{
    lc::LCParams P = p_Params;
    P.decodeOnFetch = 1;
    std::vector<float> out(src.size());
    std::vector<float> mips((size_t)P.mipFloats + 4);
    lc::buildMips(P, src.data(), mips.data());
    for (int y = 0; y < P.height; y++)
        for (int x = 0; x < P.width; x++)
        {
            lc::LCColor c = lc::shade(P, src.data(), mips.data(), x, y);
            float* d = &out[((size_t)y * P.width + x) * 4];
            d[0] = c.r; d[1] = c.g; d[2] = c.b; d[3] = c.a;
        }
    return out;
}

int main(int argc, char** argv)
{
    std::string outDir = argc > 1 ? argv[1] : ".";
    lc::Library& lib = lc::Library::get();

    printf("Library\n");
    CHECK(lib.size() > 1000, "%zu profiles loaded", lib.size());
    const char* queries[] = {"Helios 44", "Helios 44-2 58mm", "Super-Takumar 50", "Sigma 85 Art", "Petzval", "Noctilux"};
    for (const char* q : queries)
    {
        lc::ProfilePtr p = lib.find(q);
        CHECK(p != nullptr, "\"%s\" -> %s", q, p ? p->summary().c_str() : "(none)");
    }
    lc::ProfilePtr helios = lib.findById("builtin:Helios 44-2 58mm (character)");
    CHECK(helios && helios->measured, "Helios character preset takes geometry from %s",
          helios && helios->measured ? helios->measured->name.c_str() : "(none)");

    const int W = 960, H = 540;
    std::vector<float> chart = makeChart(W, H);

    lc::FrameSettings s;
    s.width = W; s.height = H; s.rowStride = W * 4;
    s.encoding = lc::ENC_GAMMA24;
    s.aperture = 2.0f;
    lc::Slot* slots[4] = {&s.distortion, &s.ca, &s.vignetting, &s.character};
    for (lc::Slot* sl : slots) sl->a = helios;

    printf("Overdrive limits\n");
    for (float od : {1.0f, 10.0f, 100.0f, 10000.0f})
    {
        s.distortion.overdrive = od;
        s.ca.overdrive = od;
        lc::LCParams P = lc::buildParams(s);
        bool monotonic = true;
        float prev = -1.0f;
        for (int i = 0; i <= 200; i++)
        {
            float r = 1.25f * i / 200.0f;
            float d = r * lc::polyEval(P.dist, r);
            if (d <= prev) monotonic = false;
            prev = d;
        }
        float caShift = std::fabs(lc::polyEval(P.tcaR, 1.0f) - 1.0f);
        CHECK(monotonic && caShift <= 0.0401f, "overdrive %6.0f: warp one-to-one, d2 = %+.5f, CA corner shift = %.4f (budget 0.04)",
              od, P.dist[2], caShift);
    }

    printf("Render\n");
    s.distortion.overdrive = 40.0f;   // real primes are subtle; push them so the test image shows it
    s.ca.overdrive = 60.0f;
    s.vignetting.overdrive = 1.0f;
    s.character.overdrive = 1.0f;
    s.defocus = 4.0f;
    s.focusMode = lc::FOCUS_RADIAL;
    s.samples = 96;
    lc::LCParams P = lc::buildParams(s);
    printf("  params: distOn=%d tcaOn=%d vigOn=%d fit=%.4f catEye=%.2f swirl=%.2f blades=%.0f\n",
           P.distOn, P.tcaOn, P.vigOn, P.fit, P.catEye, P.swirl, P.blades);

    auto t0 = std::chrono::steady_clock::now();
    std::vector<float> cpu = renderCPU(P, chart);
    auto t1 = std::chrono::steady_clock::now();
    std::vector<float> gpu(chart.size());
    int rc = runGpu(P, chart.data(), gpu.data());
    if (kGpu) CHECK(rc == 0, "%s kernel compiled and ran (rc=%d)", kGpu, rc);
    else printf("  skip  no GPU backend in this build: GPU checks below use the CPU\n");
    if (rc == 0 && kGpu)
    {
        // Nearest-sample lookups can land on the other side of a pixel boundary when CPU and GPU
        // round differently (e.g. x86 without fused multiply-add), so judge by how many values
        // differ visibly rather than by the single worst value.
        double maxDiff = 0.0, sumDiff = 0.0;
        size_t visible = 0;
        for (size_t i = 0; i < cpu.size(); i++)
        {
            double d = std::fabs(cpu[i] - gpu[i]);
            maxDiff = std::max(maxDiff, d);
            sumDiff += d;
            if (d > 0.01) visible++;
        }
        double frac = (double)visible / cpu.size();
        CHECK(sumDiff / cpu.size() < 1e-4 && frac < 1e-4, "CPU vs %s: mean |diff| %.2e, max %.2e, %zu of %zu values differ by > 0.01",
              kGpu, sumDiff / cpu.size(), maxDiff, visible, cpu.size());
    }
    printf("  CPU single-thread 960x540: %.0f ms\n", std::chrono::duration<double, std::milli>(t1 - t0).count());

    // GPU timing at UHD: the old approach against each quality mode
    if (kGpu && rc == 0)
    {
        const int W4 = 3840, H4 = 2160;
        std::vector<float> big = makeChart(W4, H4), bigOut(big.size());
        lc::FrameSettings s4 = s;
        s4.width = W4; s4.height = H4; s4.rowStride = W4 * 4;
        for (lc::Slot* sl : {&s4.distortion, &s4.ca, &s4.vignetting, &s4.character}) sl->overdrive = 1.0f;
        auto timeIt = [&](const lc::LCParams& P4) {
            double best = 1e9, ms = 0;
            for (int i = 0; i < 4; i++) { runGpu(P4, big.data(), bigOut.data(), &ms); best = std::min(best, ms); }
            return best;
        };
        printf("  %s time per UHD frame, Helios, Sharp Centre (best of 4):\n", kGpu);
        for (float defocus : {3.0f, 8.0f})
        {
            s4.defocus = defocus;
            s4.quality = lc::QUALITY_FULL;
            lc::LCParams full = lc::buildParams(s4);
            lc::LCParams old = full;
            old.decodeOnFetch = 1; old.adaptiveSamples = 0; old.glowSamples = 24;
            s4.quality = lc::QUALITY_FAST;
            lc::LCParams fast = lc::buildParams(s4);
            s4.quality = lc::QUALITY_GEOMETRY;
            lc::LCParams geo = lc::buildParams(s4);
            double tOld = timeIt(old), tFull = timeIt(full), tFast = timeIt(fast), tGeo = timeIt(geo);
            printf("    defocus %.0f:  v0.3 %.1f ms   Full %.1f ms   Fast %.1f ms   Geometry Only %.1f ms\n", defocus, tOld, tFull, tFast, tGeo);
        }
    }

    printf("Aperture\n");
    {
        lc::FrameSettings a = s;
        a.maxAperture = 2.0f;
        a.aperture = 2.0f;
        lc::LCParams wide = lc::buildParams(a);
        a.aperture = 4.0f;
        lc::LCParams f4 = lc::buildParams(a);
        a.aperture = 11.0f;
        lc::LCParams f11 = lc::buildParams(a);
        CHECK(std::fabs(f4.defocusPx / wide.defocusPx - 0.5f) < 1e-4f, "f/4 on an f/2 lens halves the blur (%.1f px -> %.1f px)", wide.defocusPx, f4.defocusPx);
        CHECK(f4.catEye < wide.catEye && f11.catEye < 0.05f, "cat-eye fades when stopping down (%.2f -> %.2f -> %.2f)", wide.catEye, f4.catEye, f11.catEye);
        CHECK(f4.curve < wide.curve, "iris blades show more when stopped down (curvature %.2f -> %.2f)", wide.curve, f4.curve);
    }

    printf("Sharp Area Softness\n");
    {
        lc::FrameSettings f = s;   // Sharp Centre, Defocus 4
        lc::LCParams sharp = lc::buildParams(f);
        f.focusFloor = 0.5f;
        lc::LCParams soft = lc::buildParams(f);
        std::vector<float> a = renderCPU(sharp, chart), c = renderCPU(soft, chart);
        auto meanDiff = [&](int x0, int y0, int n) {
            double d = 0;
            for (int y = y0; y < y0 + n; y++)
                for (int x = x0; x < x0 + n; x++)
                    for (int ch = 0; ch < 3; ch++) d += std::fabs(a[((size_t)y * W + x) * 4 + ch] - c[((size_t)y * W + x) * 4 + ch]);
            return d / (n * n * 3);
        };
        double centre = meanDiff(W / 2 - 40, H / 2 - 40, 80), corner = meanDiff(0, 0, 40);
        CHECK(centre > 1e-3 && corner < 1e-6, "softens the sharp centre (mean change %.4f) and leaves the defocused corners as they were (%.1e)",
              centre, corner);
    }

    // Small scene-referred lights at large defocus. With point samples only, each light
    // showed up as a disc of separate dots (one per bokeh sample); the pyramid fills it in.
    printf("Bokeh from small bright lights (DaVinci Intermediate)\n");
    {
        std::vector<float> scene((size_t)W * H * 4);
        for (size_t i = 0; i < (size_t)W * H; i++) { scene[i*4+0] = 0.33f; scene[i*4+1] = 0.35f; scene[i*4+2] = 0.37f; scene[i*4+3] = 1.0f; }
        // Same 3×3 light (≈39× white) at different offsets on the pixel grid
        const int lx[3] = {W / 2 - 150, W / 2, W / 2 + 151}, ly[3] = {H / 2, H / 2 + 1, H / 2 - 1};
        for (int n = 0; n < 3; n++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                {
                    float* p = &scene[((size_t)(ly[n] + dy) * W + lx[n] + dx) * 4];
                    p[0] = 0.90f; p[1] = 0.89f; p[2] = 0.86f;
                }
        lc::FrameSettings b = s;
        b.encoding = lc::ENC_DAVINCI_INTERMEDIATE;
        b.focusMode = lc::FOCUS_UNIFORM;
        b.defocus = 20.0f;   // the slider's maximum
        b.samples = 96;
        b.glowScale = 0.0f;   // the bokeh alone; glow adds a bright core over each light
        for (lc::Slot* sl : {&b.distortion, &b.ca, &b.vignetting, &b.character}) sl->overdrive = 1.0f;

        // Evenness inside each disc: coefficient of variation of linear luma over its inner 60%
        auto measure = [&](const std::vector<float>& img, float radius, double* cvMax, double* meanSpread) {
            double means[3], worst = 0;
            for (int n = 0; n < 3; n++)
            {
                double sum = 0, sum2 = 0;
                int count = 0, R = (int)(radius * 0.6f);
                for (int y = ly[n] - R; y <= ly[n] + R; y++)
                    for (int x = lx[n] - R; x <= lx[n] + R; x++)
                    {
                        if ((x - lx[n]) * (x - lx[n]) + (y - ly[n]) * (y - ly[n]) > R * R) continue;
                        const float* p = &img[((size_t)y * W + x) * 4];
                        double v = lc::luma(lc::decode(b.encoding, p[0]), lc::decode(b.encoding, p[1]), lc::decode(b.encoding, p[2]));
                        sum += v; sum2 += v * v; count++;
                    }
                double mean = sum / count, var = std::max(0.0, sum2 / count - mean * mean);
                means[n] = mean;
                worst = std::max(worst, std::sqrt(var) / mean);
            }
            double lo = std::min(means[0], std::min(means[1], means[2])), hi = std::max(means[0], std::max(means[1], means[2]));
            *cvMax = worst;
            *meanSpread = (hi - lo) / lo;
        };

        // Helios: evenness with real character (cat-eye, swirl, 8 blades). Plain lens (all
        // traits off): the three lights differ only in where they sit on the pixel grid, so
        // their discs must match.
        for (int plain = 0; plain < 2; plain++)
            for (float boost : {3.0f, 0.0f})
            {
                lc::FrameSettings c = b;
                c.hlBoost = boost;
                if (plain)
                    for (lc::Slot* sl : {&c.distortion, &c.ca, &c.vignetting, &c.character}) sl->overdrive = 0.0f;
                lc::LCParams Pb = lc::buildParams(c);
                lc::LCParams Pold = Pb;
                Pold.mipLevels = 0;   // the old point sampling, for comparison
                std::vector<float> gpuB(scene.size());
                int rcB = runGpu(Pb, scene.data(), gpuB.data());
                std::vector<float> cpuB = renderCPU(Pb, scene), oldB = renderCPU(Pold, scene);
                double cvNew, spreadNew, cvOld, spreadOld, cvGpu, spreadGpu;
                measure(cpuB, Pb.defocusPx, &cvNew, &spreadNew);
                measure(oldB, Pb.defocusPx, &cvOld, &spreadOld);
                measure(gpuB, Pb.defocusPx, &cvGpu, &spreadGpu);
                const char* lens = plain ? "plain lens" : "Helios";
                CHECK(rcB == 0 && cvNew < 0.10 && cvGpu < 0.10 && cvNew < cvOld * 0.25,
                      "%s, Highlight Boost %.0f: variation inside the disc %.1f%% CPU, %.1f%% %s (point samples: %.1f%%)",
                      lens, boost, cvNew * 100, cvGpu * 100, kGpu ? kGpu : "CPU", cvOld * 100);
                if (plain)
                    CHECK(spreadNew < 0.03 && spreadGpu < 0.03,
                          "%s, Highlight Boost %.0f: same light at three pixel offsets, discs within %.1f%% (point samples: %.1f%%)",
                          lens, boost, std::max(spreadNew, spreadGpu) * 100, spreadOld * 100);
                if (!plain && boost == 0.0f)
                {
                    writePPM(outDir + "/bokeh_lights_old.ppm", oldB, W, H);
                    writePPM(outDir + "/bokeh_lights_new.ppm", gpuB, W, H);
                }
            }

        // Glow alone (no blur): the halo around each light must not depend on its pixel offset.
        // Gamma 2.4 still gives highlights extra weight in the glow; scene-referred input does not.
        for (int enc : {lc::ENC_DAVINCI_INTERMEDIATE, lc::ENC_GAMMA24})
        {
            lc::FrameSettings c = b;
            c.encoding = enc;
            c.defocus = 0.0f;
            for (lc::Slot* sl : {&c.distortion, &c.ca, &c.vignetting, &c.character}) sl->overdrive = 0.0f;
            lc::LCParams Pg = lc::buildParams(c);
            Pg.glow = 0.5f;
            lc::planMips(Pg, LC_MAX_MIPS);
            std::vector<float> gpuG(scene.size());
            int rcG = runGpu(Pg, scene.data(), gpuG.data());
            std::vector<float> cpuG = renderCPU(Pg, scene);
            auto halo = [&](const std::vector<float>& img, int n) {
                double e = 0;
                for (int y = ly[n] - 12; y <= ly[n] + 12; y++)
                    for (int x = lx[n] - 12; x <= lx[n] + 12; x++)
                    {
                        if (std::abs(x - lx[n]) <= 1 && std::abs(y - ly[n]) <= 1) continue;   // the light itself
                        const float* p = &img[((size_t)y * W + x) * 4];
                        e += lc::luma(lc::decode(c.encoding, p[0]), lc::decode(c.encoding, p[1]), lc::decode(c.encoding, p[2]));
                    }
                return e;
            };
            double worst = 0;
            for (const std::vector<float>* img : {&cpuG, &gpuG})
            {
                double e0 = halo(*img, 0), e1 = halo(*img, 1), e2 = halo(*img, 2);
                double lo = std::min(e0, std::min(e1, e2)), hi = std::max(e0, std::max(e1, e2));
                worst = std::max(worst, (hi - lo) / lo);
            }
            CHECK(rcG == 0 && worst < 0.02, "glow, %s: same light at three pixel offsets, halos within %.2f%%",
                  enc == lc::ENC_GAMMA24 ? "Gamma 2.4" : "DaVinci Intermediate", worst * 100);
        }
    }

    printf("Lens browser\n");
    {
        lc::BrowserRequest req;
        req.title = "Choose Lens";
        req.aperture = 2.0f;
        lc::BrowserModel m(req);
        m.filter(m.categoryIndex(lc::kCategoryAll), "");
        CHECK(m.rowCount() == lib.size() && m.categoryCount() > 10, "%zu lenses listed under %zu categories", m.rowCount(), m.categoryCount());
        m.filter(m.categoryIndex(lc::kCategoryPresets), "");
        bool presetsOk = m.rowCount() > 5;
        for (size_t r = 0; r < m.rowCount(); r++) presetsOk = presetsOk && m.rowProfile(r)->source == "builtin";
        CHECK(presetsOk, "%s: %zu built-in presets", lc::kCategoryPresets, m.rowCount());
        m.filter(m.categoryIndex("Sigma"), "85");
        bool sigmaOk = m.rowCount() > 0;
        for (size_t r = 0; r < m.rowCount(); r++)
            sigmaOk = sigmaOk && m.rowProfile(r)->maker == "Sigma" && m.cell(r, lc::COL_NAME).find("85") != std::string::npos;
        CHECK(sigmaOk, "Sigma + \"85\": %zu lenses, first \"%s\"", m.rowCount(), m.rowCount() ? m.cell(0, lc::COL_NAME).c_str() : "");
        m.filter(0, "helios 44");
        int row = m.rowOf("builtin:Helios 44-2 58mm (character)");
        CHECK(row == 0 && m.cell(0, lc::COL_DATA).find("Bokeh") != std::string::npos, "\"helios 44\" lists the preset first (row %d, %s, %s)",
              row, m.rowCount() ? m.cell(0, lc::COL_FOCAL).c_str() : "", m.rowCount() ? m.cell(0, lc::COL_DATA).c_str() : "");

        // The Windows browser's preview is drawn in software; render it here too for a visual check
        const int PW = 300, PH = 700;
        std::vector<unsigned char> bgra((size_t)PW * PH * 4);
        std::vector<lc::PreviewText> texts;
        lc::renderPreview(helios.get(), 2.0f, PW, PH, 1.0f, bgra.data(), texts);
        std::vector<float> img((size_t)PW * PH * 4);
        for (int y = 0; y < PH; y++)   // writePPM expects bottom-up RGB
            for (int x = 0; x < PW; x++)
                for (int c = 0; c < 3; c++)
                    img[((size_t)(PH - 1 - y) * PW + x) * 4 + c] = bgra[((size_t)y * PW + x) * 4 + 2 - c] / 255.0f;
        writePPM(outDir + "/browser_preview_sw.ppm", img, PW, PH);
        CHECK(texts.size() == 6, "software preview: %zu text blocks, written to %s/browser_preview_sw.ppm", texts.size(), outDir.c_str());
    }
#ifdef __APPLE__
    {
        lc::BrowserRequest req;
        req.title = "Choose Lens";
        req.aperture = 2.0f;
        bool a = lc::snapshotLensBrowser(req, "All Lenses", "", "builtin:Helios 44-2 58mm (character)", outDir + "/browser_all.png");
        bool b = lc::snapshotLensBrowser(req, "Character Presets", "", "builtin:Petzval 85mm (character)", outDir + "/browser_presets.png");
        bool c = lc::snapshotLensBrowser(req, "Sigma", "85", "", outDir + "/browser_search.png");
        CHECK(a && b && c, "browser rendered offscreen to %s/browser_*.png", outDir.c_str());
    }
#endif

    // Real-lens settings, no defocus: vintage presets should still show soft, swirly corners
    {
        lc::FrameSettings e = s;
        for (lc::Slot* sl : {&e.distortion, &e.ca, &e.vignetting, &e.character}) sl->overdrive = 1.0f;
        e.defocus = 0.0f;
        for (const char* id : {"builtin:Helios 44-2 58mm (character)", "builtin:Petzval 85mm (character)"})
        {
            lc::ProfilePtr lens = lib.findById(id);
            for (lc::Slot* sl : {&e.distortion, &e.ca, &e.vignetting, &e.character}) sl->a = lens;
            std::vector<float> out(chart.size());
            runGpu(lc::buildParams(e), chart.data(), out.data());
            std::string name = std::string(id).find("Petzval") != std::string::npos ? "petzval" : "helios";
            writePPM(outDir + "/edges_" + name + "_defocus0.ppm", out, W, H);
        }
    }

    writePPM(outDir + "/selftest_before.ppm", chart, W, H);
    writePPM(outDir + "/selftest_after_cpu.ppm", cpu, W, H);
    if (rc == 0 && kGpu) writePPM(outDir + "/selftest_after_gpu.ppm", gpu, W, H);

    printf(s_Failures ? "\n%d check(s) failed\n" : "\nAll checks passed\n", s_Failures);
    return s_Failures ? 1 : 0;
}
