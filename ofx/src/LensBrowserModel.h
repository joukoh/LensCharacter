// LensBrowserModel.h — everything the "Choose Lens…" window shows, minus the drawing: the
// category sidebar, search filtering, list columns, lens facts and the preview's geometry.
// Shared by the Cocoa window (LensBrowser.mm) and the Win32 one (LensBrowserWin.cpp).
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "LensBrowser.h"
#include "Profiles.h"

namespace lc {

extern const char* const kCategoryAll;       // "All Lenses"
extern const char* const kCategoryPresets;   // "Character Presets"
extern const char* const kCategoryMine;      // "My Lenses"

// Per-lens facts shown in the list and the preview
struct LensFacts
{
    std::string focal;     // "58mm  f/2", "24–70mm"
    std::string traits;    // "Dist · CA · Vig · Bokeh"
    float distPct = 0;     // radial displacement at the corner, %
    bool mustache = false;
    float caPx = 0;        // channel separation at a UHD corner
    bool hasVig = false;
    float vigStops = 0;    // at the requested aperture
};

LensFacts lensFacts(const Profile& p, float aperture);

// Preview text: the line under the name, and the numbers at the bottom (lines separated by \n)
std::string lensSubtitle(const Profile& p);
std::string lensFactsText(const Profile& p, const LensFacts& f, float aperture);

// Distortion grid for a box of w × h at (x, y): 13 + 13 polylines, already warped, exaggerated
// so subtle lenses are visible. `exaggerate` is the factor used (1 = true to the lens).
struct PreviewGrid
{
    std::vector<std::vector<std::pair<float, float>>> lines;
    float exaggerate = 1.0f;
};
PreviewGrid previewGrid(const Profile& p, float x, float y, float w, float h);

// The bokeh preview: the kernel's own sampling of the aperture, at the frame centre (edge =
// false) or near the corner. Offsets are in units of the disc radius, y down.
struct BokehDot { float x, y, alpha; };
std::vector<BokehDot> previewBokeh(const Character& c, bool edge);

// The whole preview panel drawn in software, for platforms without a vector drawing API at
// hand (the Win32 window): background, distortion grid and bokeh into `bgra` (top-down rows,
// W × H × 4 bytes, B G R A), and the text it needs drawn on top, with where it goes. `scale`
// is the display scale (1 = 96 dpi). p may be null (nothing selected).
enum { TEXT_TITLE = 0, TEXT_BODY, TEXT_DIM, TEXT_SMALL };
struct PreviewText { float x, y, w, h; int style; std::string text; };
void renderPreview(const Profile* p, float aperture, int W, int H, float scale,
                   unsigned char* bgra, std::vector<PreviewText>& texts);

// Column of the lens list
enum { COL_NAME = 0, COL_FOCAL, COL_MOUNT, COL_DATA, COL_COUNT };

class BrowserModel
{
public:
    explicit BrowserModel(const BrowserRequest& p_Request);

    // Sidebar
    size_t categoryCount() const { return m_Categories.size(); }
    const std::string& category(size_t i) const { return m_Categories[i]; }
    std::string categoryLabel(size_t i) const;            // "Zeiss  (42)"
    size_t categoryIndex(const std::string& name) const;  // 0 (All Lenses) when unknown

    // Filtered list: lenses in category `cat` whose name, maker or mount contain every word of `query`
    void filter(size_t cat, const std::string& query);
    size_t rowCount() const { return m_Rows.size(); }
    const ProfilePtr& rowProfile(size_t row) const { return m_All[m_Rows[row]]; }
    const LensFacts& rowFacts(size_t row) const { return m_Facts[m_Rows[row]]; }
    std::string cell(size_t row, int column) const;
    int rowOf(const std::string& id) const;                // -1 when not in the current filter

    // Every lens, unfiltered (for sizing columns)
    size_t lensCount() const { return m_All.size(); }
    std::string lensCell(size_t i, int column) const;

    float aperture() const { return m_Aperture; }
    bool allowNone() const { return m_AllowNone; }

private:
    bool inCategory(size_t lens, size_t cat) const;

    std::vector<ProfilePtr> m_All;
    std::vector<std::string> m_Keys;       // normalized search text per lens
    std::vector<LensFacts> m_Facts;
    std::vector<std::string> m_Categories;
    std::vector<size_t> m_CategoryCounts;
    std::vector<int> m_Rows;               // indices into m_All for the current filter
    float m_Aperture;
    bool m_AllowNone;
};

} // namespace lc
