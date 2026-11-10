// LensBrowserModel.cpp — see LensBrowserModel.h.
#include "LensBrowserModel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "LensKernel.h"

namespace lc {

const char* const kCategoryAll = "All Lenses";
const char* const kCategoryPresets = "Character Presets";
const char* const kCategoryMine = "My Lenses";

static const int kUHDHalfDiag = 2203;   // px, for "CA at UHD corner"

static bool lessNoCase(const std::string& a, const std::string& b)
{
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](unsigned char x, unsigned char y) {
        return std::tolower(x) < std::tolower(y);
    });
}

// ---- facts ---------------------------------------------------------------------------------------

static void rhoPoly(const Profile& src, const float a[5], float out[5])
{
    float s = src.calib.halfDiagMm / src.calib.halfShortMm, f = 1.0f;
    for (int k = 0; k < 5; k++) { out[k] = a[k] * f; f *= s; }
}

LensFacts lensFacts(const Profile& p, float aperture)
{
    LensFacts f;
    std::set<float> focals;
    for (const DistEntry& d : p.dist) if (d.focal > 0) focals.insert(d.focal);
    for (const TcaEntry& t : p.tca) if (t.focal > 0) focals.insert(t.focal);
    for (const VigEntry& v : p.vig) if (v.focal > 0) focals.insert(v.focal);
    if (p.measured)
    {
        for (const DistEntry& d : p.measured->dist) if (d.focal > 0) focals.insert(d.focal);
    }
    char buf[64];
    if (!focals.empty())
    {
        if (*focals.begin() == *focals.rbegin()) snprintf(buf, sizeof buf, "%gmm", *focals.begin());
        else snprintf(buf, sizeof buf, "%g\xE2\x80\x93%gmm", *focals.begin(), *focals.rbegin());   // en dash
        f.focal = buf;
    }
    if (p.maxAperture > 0)
    {
        snprintf(buf, sizeof buf, "%sf/%g", f.focal.empty() ? "" : "  ", p.maxAperture);
        f.focal += buf;
    }

    std::vector<std::string> t;
    if (const Profile* s = p.distSource())
    {
        t.push_back("Dist");
        float a[5], r[5];
        if (sampleDistortion(*s, 0, a))
        {
            rhoPoly(*s, a, r);
            f.distPct = (polyEval(r, 1.0f) - 1.0f) * 100.0f;
            float sign0 = 0;
            for (int i = 1; i <= 20; i++)
            {
                float v = polyEval(r, i / 20.0f) - 1.0f;
                if (std::fabs(v) < 1e-5f) continue;
                if (sign0 == 0) sign0 = v;
                else if (v * sign0 < 0) f.mustache = true;
            }
        }
    }
    if (const Profile* s = p.tcaSource())
    {
        t.push_back("CA");
        float rr[5], bb[5], r2[5], b2[5];
        if (sampleTca(*s, 0, rr, bb))
        {
            rhoPoly(*s, rr, r2);
            rhoPoly(*s, bb, b2);
            float sep = std::max(std::fabs(polyEval(r2, 1.0f) - 1.0f), std::fabs(polyEval(b2, 1.0f) - 1.0f));
            f.caPx = sep * kUHDHalfDiag;
        }
    }
    if (const Profile* s = p.vigSource())
    {
        t.push_back("Vig");
        float k[3];
        if (sampleVignetting(*s, 0, aperture, k))
        {
            float v = std::max(1.0f + k[0] + k[1] + k[2], 1e-3f);
            f.hasVig = true;
            f.vigStops = std::log2(v);
        }
    }
    if (p.hasCharacter) t.push_back("Bokeh");
    for (size_t i = 0; i < t.size(); i++) f.traits += (i ? " \xC2\xB7 " : "") + t[i];   // middle dot
    return f;
}

std::string lensSubtitle(const Profile& p)
{
    std::string sub = p.source == "lensfun" ? "Measured (Lensfun)"
                    : p.source == "builtin" ? "Character preset"
                    : p.source == "custom"  ? "Custom lens (your sliders)"
                                            : "Saved in My Lenses";
    if (!p.mount.empty()) sub += " \xC2\xB7 " + p.mount;
    if (p.measured) sub += "\nGeometry measured from " + p.measured->name;
    return sub;
}

std::string lensFactsText(const Profile& p, const LensFacts& f, float aperture)
{
    char buf[256];
    std::string out;
    if (p.distSource())
    {
        snprintf(buf, sizeof buf, "Distortion: %s %.2f%% at the corner\n",
                 f.mustache ? "moustache," : (f.distPct < 0 ? "barrel," : "pincushion,"), std::fabs(f.distPct));
        out += buf;
    }
    else
        out += "Distortion: none in profile\n";
    if (p.tcaSource()) { snprintf(buf, sizeof buf, "Lateral CA: %.1f px at a UHD corner\n", f.caPx); out += buf; }
    else out += "Lateral CA: none in profile\n";
    if (f.hasVig) { snprintf(buf, sizeof buf, "Vignetting at f/%g: %.1f stops at the corner\n", aperture, f.vigStops); out += buf; }
    else out += "Vignetting: none in profile\n";
    if (p.hasCharacter)
    {
        const Character& c = p.character;
        std::string iris = "round iris";
        if (c.blades >= 3) { snprintf(buf, sizeof buf, "%.0f-blade iris", c.blades); iris = buf; }
        snprintf(buf, sizeof buf, "Bokeh: %s, cat-eye %.2f, swirl %.2f, glow %.2f", iris.c_str(), c.catEye, c.swirl, c.glow);
        out += buf;
    }
    else
        out += "Bokeh: measured data only, so the iris is round. Pick a character preset in Mix & Match for bokeh character.";
    return out;
}

// ---- preview geometry ----------------------------------------------------------------------------

PreviewGrid previewGrid(const Profile& p, float x, float y, float w, float h)
{
    PreviewGrid g;
    float poly[5] = {1, 0, 0, 0, 0};
    if (const Profile* s = p.distSource())
    {
        float a[5];
        if (sampleDistortion(*s, 0, a))
        {
            rhoPoly(*s, a, poly);
            float maxd = 0;
            for (int i = 0; i <= 20; i++) maxd = std::max(maxd, std::fabs(polyEval(poly, i / 20.0f) - 1.0f));
            if (maxd > 1e-6f) g.exaggerate = std::min(60.0f, std::max(1.0f, 0.06f / maxd));
            for (int k = 0; k < 5; k++) poly[k] = (k == 0 ? 1.0f : 0.0f) + (poly[k] - (k == 0 ? 1.0f : 0.0f)) * g.exaggerate;
        }
    }
    const float cx = x + w * 0.5f, cy = y + h * 0.5f, hd = std::hypot(w, h) * 0.5f;
    auto warp = [&](float gx, float gy) {
        float dx = gx - cx, dy = gy - cy, r = std::hypot(dx, dy) / hd;
        float k = polyEval(poly, r);
        return std::make_pair(cx + dx * k, cy + dy * k);
    };
    for (int i = 0; i <= 12; i++)
    {
        float gx = x + w * i / 12.0f, gy = y + h * i / 12.0f;
        std::vector<std::pair<float, float>> v, hz;
        for (int j = 0; j <= 40; j++)
        {
            v.push_back(warp(gx, y + h * j / 40.0f));
            hz.push_back(warp(x + w * j / 40.0f, gy));
        }
        g.lines.push_back(v);
        g.lines.push_back(hz);
    }
    return g;
}

std::vector<BokehDot> previewBokeh(const Character& c, bool edge)
{
    std::vector<BokehDot> dots;
    float r = edge ? 0.9f : 0.0f;
    float rdx = 0.7071f, rdy = -0.7071f;   // toward the top-right corner
    float tdx = -rdy, tdy = rdx;
    float seg = c.blades >= 3 ? 2.0f * LC_PI / c.blades : 0.0f;
    float rot = c.rotation * LC_PI / 180.0f;
    float ceX = rdx * c.catEye * r * 1.1f, ceY = rdy * c.catEye * r * 1.1f;
    float tStretch = 1.0f + c.swirl * r * r * 0.8f, rSquash = 1.0f / (1.0f + c.swirl * r * r * 0.3f);
    float rim = std::max(-1.0f, std::min(1.0f, c.rim));
    const int N = 1400;
    dots.reserve(N);
    for (int i = 0; i < N; i++)
    {
        float rn = std::sqrt((i + 0.5f) / N), th = i * LC_GOLDEN, shape = 1.0f;
        if (seg > 0)
        {
            float a = std::fmod(th - rot + 64.0f * LC_PI, seg) - 0.5f * seg;
            shape = (std::cos(LC_PI / c.blades) / std::cos(a)) * (1.0f - c.curve) + c.curve;
        }
        float ax = std::cos(th) * rn * shape, ay = std::sin(th) * rn * shape;
        float ddx = ax + ceX, ddy = ay + ceY;
        if (std::sqrt(ddx * ddx + ddy * ddy) / shape > 1.0f) continue;
        float orad = (ax * rdx + ay * rdy) * rSquash, otan = (ax * tdx + ay * tdy) * tStretch;
        float ox = orad * rdx + otan * tdx, oy = (orad * rdy + otan * tdy) * c.squeeze;
        float w = rim >= 0 ? 1.0f + rim * 4.0f * rn * rn * rn * rn : 1.0f + rim * rn * rn;
        w *= 1.0f + c.onion * 0.6f * std::cos(rn * 38.0f);
        dots.push_back({ox, oy, std::max(0.05f, std::min(1.0f, 0.22f * w))});
    }
    return dots;
}

// ---- software preview ----------------------------------------------------------------------------

namespace {

struct Canvas
{
    int W, H;
    std::vector<float> rgb;
    float clipX0, clipY0, clipX1, clipY1;

    Canvas(int w, int h) : W(w), H(h), rgb((size_t)w * h * 3, 0.0f), clipX0(0), clipY0(0), clipX1((float)w), clipY1((float)h) {}

    void clip(float x0, float y0, float x1, float y1) { clipX0 = x0; clipY0 = y0; clipX1 = x1; clipY1 = y1; }
    void unclip() { clip(0, 0, (float)W, (float)H); }

    void blend(int x, int y, float r, float g, float b, float a)
    {
        if (x < 0 || y < 0 || x >= W || y >= H || a <= 0.0f) return;
        if (x + 1 <= clipX0 || y + 1 <= clipY0 || x >= clipX1 || y >= clipY1) return;
        a = std::min(a, 1.0f);
        float* p = &rgb[((size_t)y * W + x) * 3];
        p[0] += (r - p[0]) * a; p[1] += (g - p[1]) * a; p[2] += (b - p[2]) * a;
    }

    // Filled rectangle with partial coverage at its edges (anti-aliased)
    void rect(float x0, float y0, float x1, float y1, float r, float g, float b, float a)
    {
        x0 = std::max(x0, clipX0); y0 = std::max(y0, clipY0);
        x1 = std::min(x1, clipX1); y1 = std::min(y1, clipY1);
        if (x1 <= x0 || y1 <= y0) return;
        for (int y = (int)std::floor(y0); y < (int)std::ceil(y1); y++)
        {
            float cy = std::min(y1, (float)y + 1) - std::max(y0, (float)y);
            for (int x = (int)std::floor(x0); x < (int)std::ceil(x1); x++)
            {
                float cx = std::min(x1, (float)x + 1) - std::max(x0, (float)x);
                blend(x, y, r, g, b, a * cx * cy);
            }
        }
    }

    // About 1 px wide anti-aliased line: bilinear splats every half pixel
    void line(float xa, float ya, float xb, float yb, float r, float g, float b, float a)
    {
        float len = std::hypot(xb - xa, yb - ya);
        int n = std::max(1, (int)std::ceil(len * 2.0f));
        for (int i = 0; i <= n; i++)
        {
            if (i == n && n > 1) break;   // the next segment starts here
            float t = (float)i / n, x = xa + (xb - xa) * t - 0.5f, y = ya + (yb - ya) * t - 0.5f;
            int ix = (int)std::floor(x), iy = (int)std::floor(y);
            float fx = x - ix, fy = y - iy, w = a * 0.5f;
            blend(ix, iy, r, g, b, w * (1 - fx) * (1 - fy));
            blend(ix + 1, iy, r, g, b, w * fx * (1 - fy));
            blend(ix, iy + 1, r, g, b, w * (1 - fx) * fy);
            blend(ix + 1, iy + 1, r, g, b, w * fx * fy);
        }
    }
};

} // namespace

void renderPreview(const Profile* p, float aperture, int W, int H, float s, unsigned char* bgra, std::vector<PreviewText>& texts)
{
    texts.clear();
    Canvas cv(W, H);
    cv.rect(0, 0, (float)W, (float)H, 0.11f, 0.11f, 0.11f, 1.0f);
    const float pad = 16 * s, inner = W - 2 * pad;
    if (!p)
    {
        texts.push_back({pad, pad, inner, 40 * s, TEXT_DIM, "Select a lens to preview it."});
    }
    else
    {
        LensFacts f = lensFacts(*p, aperture);
        float y = 14 * s;
        texts.push_back({pad, y, inner, 40 * s, TEXT_TITLE, p->name});
        y += 40 * s;
        texts.push_back({pad, y, inner, 34 * s, TEXT_DIM, lensSubtitle(*p)});
        y += 40 * s;

        // Distortion grid
        float bw = inner, bh = inner * 9.0f / 16.0f;
        cv.rect(pad, y, pad + bw, y + bh, 0.16f, 0.16f, 0.16f, 1.0f);
        PreviewGrid g = previewGrid(*p, pad, y, bw, bh);
        cv.clip(pad, y, pad + bw, y + bh);
        for (const auto& l : g.lines)
            for (size_t j = 1; j < l.size(); j++)
                cv.line(l[j - 1].first, l[j - 1].second, l[j].first, l[j].second, 0.62f, 0.66f, 0.78f, 0.9f);
        cv.unclip();
        if (g.exaggerate > 1.01f)
        {
            char buf[48];
            snprintf(buf, sizeof buf, "shown \xC3\x97%.0f", g.exaggerate);   // multiplication sign
            texts.push_back({pad + 6 * s, y + bh - 18 * s, 120 * s, 16 * s, TEXT_SMALL, buf});
        }
        y += bh + 12 * s;

        // Bokeh at the centre and the edge
        if (p->hasCharacter)
        {
            float size = (W - 48 * s) / 2.0f;
            for (int which = 0; which < 2; which++)
            {
                float x0 = pad + which * (size + 16 * s);
                cv.rect(x0, y, x0 + size, y + size, 0.05f, 0.05f, 0.05f, 1.0f);
                cv.clip(x0, y, x0 + size, y + size);
                float mx = x0 + size * 0.5f, my = y + size * 0.5f, scale = size * 0.30f, dot = 2.2f * s;
                for (const BokehDot& d : previewBokeh(p->character, which == 1))
                {
                    float cx = mx + d.x * scale - 1 * s, cy = my + d.y * scale - 1 * s;
                    cv.rect(cx, cy, cx + dot, cy + dot, 1.0f, 0.86f, 0.62f, d.alpha);
                }
                cv.unclip();
                texts.push_back({x0 + 6 * s, y + size - 18 * s, size, 16 * s, TEXT_SMALL, which == 0 ? "Bokeh, centre" : "Bokeh, edge"});
            }
            y += size + 14 * s;
        }

        texts.push_back({pad, y, inner, std::max(0.0f, H - y - 8 * s), TEXT_BODY, lensFactsText(*p, f, aperture)});
    }

    for (size_t i = 0; i < (size_t)W * H; i++)
    {
        const float* c = &cv.rgb[i * 3];
        for (int k = 0; k < 3; k++)
        {
            float v = std::min(std::max(c[2 - k], 0.0f), 1.0f);   // B, G, R
            bgra[i * 4 + k] = (unsigned char)(v * 255.0f + 0.5f);
        }
        bgra[i * 4 + 3] = 255;
    }
}

// ---- model ---------------------------------------------------------------------------------------

BrowserModel::BrowserModel(const BrowserRequest& req) : m_Aperture(req.aperture), m_AllowNone(req.allowNone)
{
    if (req.custom) m_All.push_back(req.custom);
    for (const ProfilePtr& p : Library::get().all()) m_All.push_back(p);
    std::map<std::string, int> makers;
    for (const ProfilePtr& p : m_All)
    {
        m_Keys.push_back(normalizeForSearch(p->name + " " + p->maker + " " + p->mount));
        m_Facts.push_back(lensFacts(*p, m_Aperture));
        makers[p->maker]++;
    }

    m_Categories = {kCategoryAll, kCategoryPresets, kCategoryMine};
    std::vector<std::string> names;
    for (auto& kv : makers)
        if (kv.first != kCategoryPresets && kv.first != kCategoryMine) names.push_back(kv.first);
    std::sort(names.begin(), names.end(), lessNoCase);
    m_Categories.insert(m_Categories.end(), names.begin(), names.end());

    for (size_t c = 0; c < m_Categories.size(); c++)
    {
        size_t n = 0;
        for (size_t i = 0; i < m_All.size(); i++) n += inCategory(i, c) ? 1 : 0;
        m_CategoryCounts.push_back(n);
    }
}

bool BrowserModel::inCategory(size_t lens, size_t cat) const
{
    const Profile& p = *m_All[lens];
    const std::string& c = m_Categories[cat];
    if (c == kCategoryAll) return true;
    if (c == kCategoryPresets) return p.source == "builtin";
    if (c == kCategoryMine) return p.source == "user" || p.source == "custom";
    return p.maker == c;
}

std::string BrowserModel::categoryLabel(size_t i) const
{
    return m_Categories[i] + "  (" + std::to_string(m_CategoryCounts[i]) + ")";
}

size_t BrowserModel::categoryIndex(const std::string& name) const
{
    for (size_t i = 0; i < m_Categories.size(); i++)
        if (m_Categories[i] == name) return i;
    return 0;
}

void BrowserModel::filter(size_t cat, const std::string& query)
{
    if (cat >= m_Categories.size()) cat = 0;
    std::string q = normalizeForSearch(query);
    std::vector<std::string> words;
    for (size_t start = 0; start < q.size();)
    {
        size_t sp = q.find(' ', start);
        if (sp == std::string::npos) sp = q.size();
        words.push_back(q.substr(start, sp - start));
        start = sp + 1;
    }

    m_Rows.clear();
    for (size_t i = 0; i < m_All.size(); i++)
    {
        if (!inCategory(i, cat)) continue;
        bool ok = true;
        for (const std::string& w : words)
            if (m_Keys[i].find(w) == std::string::npos) { ok = false; break; }
        if (ok) m_Rows.push_back((int)i);
    }
    // Your lenses and presets first, then alphabetical
    auto rank = [&](int i) { const std::string& s = m_All[i]->source; return s == "custom" ? 0 : s == "user" ? 1 : s == "builtin" ? 2 : 3; };
    std::stable_sort(m_Rows.begin(), m_Rows.end(), [&](int a, int b) {
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        return lessNoCase(m_All[a]->name, m_All[b]->name);
    });
}

std::string BrowserModel::lensCell(size_t i, int column) const
{
    switch (column)
    {
        case COL_NAME: return m_All[i]->name;
        case COL_FOCAL: return m_Facts[i].focal;
        case COL_MOUNT: return m_All[i]->mount;
        default: return m_Facts[i].traits;
    }
}

std::string BrowserModel::cell(size_t row, int column) const
{
    return lensCell((size_t)m_Rows[row], column);
}

int BrowserModel::rowOf(const std::string& id) const
{
    for (size_t r = 0; r < m_Rows.size(); r++)
        if (m_All[m_Rows[r]]->id == id) return (int)r;
    return -1;
}

} // namespace lc
