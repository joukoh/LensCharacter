// LensBrowserWin.cpp — the "Choose Lens…" window on Windows: makers sidebar, searchable lens
// list and preview, built from plain Win32 and common controls (no resource file, no framework).
// What it shows comes from LensBrowserModel, shared with the macOS window; the preview's
// graphics are drawn by renderPreview() and only the text is drawn here.
//
// Keys: type to search, Up/Down move through the list while typing, Enter uses the selected
// lens, Esc cancels. Double-click also uses a lens.

#ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
  #define NOMINMAX
#endif
#ifndef _WIN32_WINNT
  #define _WIN32_WINNT 0x0601   // Windows 7 APIs; newer ones are looked up at run time
#endif
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#include "LensBrowser.h"
#include "LensBrowserModel.h"

#ifdef _MSC_VER
  #pragma comment(lib, "comctl32.lib")
  #pragma comment(lib, "gdi32.lib")
  #pragma comment(lib, "user32.lib")
  // Common Controls 6 (themed controls, the search field's hint text). Embedded in the DLL's
  // manifest and activated around the window below, whatever the host process itself uses.
  #pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

using namespace lc;

namespace {

enum { ID_SEARCH = 101, ID_SIDEBAR, ID_LIST, ID_PREVIEW, ID_COUNT, ID_BASE };

const wchar_t* const kWindowClass = L"LensCharacterLensBrowser";
const wchar_t* const kPreviewClass = L"LensCharacterLensPreview";

std::wstring wide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string utf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring windowText(HWND h)
{
    int n = GetWindowTextLengthW(h);
    std::wstring s((size_t)n + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize((size_t)n);
    return s;
}

HMODULE thisModule()
{
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&thisModule), &m);
    return m;
}

// Activates the DLL's own manifest (resource 2) for the lifetime of the object
class ManifestContext
{
public:
    ManifestContext()
    {
        static HANDLE s_Ctx = []() {
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(thisModule(), path, MAX_PATH);
            ACTCTXW a = {};
            a.cbSize = sizeof(a);
            a.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
            a.lpSource = path;
            a.hModule = thisModule();
            a.lpResourceName = MAKEINTRESOURCEW(2);
            return CreateActCtxW(&a);
        }();
        if (s_Ctx != INVALID_HANDLE_VALUE) m_Active = ActivateActCtx(s_Ctx, &m_Cookie) != FALSE;
    }
    ~ManifestContext()
    {
        if (m_Active) DeactivateActCtx(0, m_Cookie);
    }

private:
    ULONG_PTR m_Cookie = 0;
    bool m_Active = false;
};

// Display scale of a window (1 = 96 dpi). GetDpiForWindow needs Windows 10 1607, so look it up.
float dpiScale(HWND w)
{
    typedef UINT(WINAPI * DpiForWindow)(HWND);
    typedef UINT(WINAPI * DpiForSystem)(void);
    HMODULE user = GetModuleHandleW(L"user32.dll");
    if (w && user)
        if (DpiForWindow f = reinterpret_cast<DpiForWindow>(reinterpret_cast<void*>(GetProcAddress(user, "GetDpiForWindow"))))
            if (UINT d = f(w)) return d / 96.0f;
    if (user)
        if (DpiForSystem f = reinterpret_cast<DpiForSystem>(reinterpret_cast<void*>(GetProcAddress(user, "GetDpiForSystem"))))
            if (UINT d = f()) return d / 96.0f;
    HDC dc = GetDC(nullptr);
    int d = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);
    return d > 0 ? d / 96.0f : 1.0f;
}

struct Browser
{
    BrowserModel model;
    std::string title;
    HWND wnd = nullptr, search = nullptr, sidebar = nullptr, list = nullptr, preview = nullptr;
    HWND count = nullptr, useBtn = nullptr, cancelBtn = nullptr, baseBtn = nullptr;
    HFONT font = nullptr;
    HFONT textFonts[4] = {};   // preview text, by TEXT_ style
    float scale = 1.0f;
    int colWidth[COL_COUNT] = {};
    bool done = false, chose = false, none = false;

    explicit Browser(const BrowserRequest& r) : model(r), title(r.title) {}
    ~Browser() { freeFonts(); }

    int px(float v) const { return (int)std::lround(v * scale); }

    void freeFonts()
    {
        if (font) DeleteObject(font);
        for (HFONT& f : textFonts)
            if (f) { DeleteObject(f); f = nullptr; }
        font = nullptr;
    }

    HFONT makeFont(float size, bool bold) const
    {
        return CreateFontW(-px(size), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    }

    void makeFonts()
    {
        freeFonts();
        font = makeFont(12.5f, false);
        textFonts[TEXT_TITLE] = makeFont(15.0f, true);
        textFonts[TEXT_BODY] = makeFont(12.0f, false);
        textFonts[TEXT_DIM] = makeFont(11.5f, false);
        textFonts[TEXT_SMALL] = makeFont(10.5f, false);
    }

    int selectedRow() const { return list ? ListView_GetNextItem(list, -1, LVNI_SELECTED) : -1; }

    std::string selectedId() const
    {
        int r = selectedRow();
        return r >= 0 && (size_t)r < model.rowCount() ? model.rowProfile((size_t)r)->id : std::string();
    }
};

Browser* browserOf(HWND h) { return reinterpret_cast<Browser*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

// ---- list ---------------------------------------------------------------------------------------

void updatePreview(Browser& b)
{
    InvalidateRect(b.preview, nullptr, FALSE);
    EnableWindow(b.useBtn, b.selectedRow() >= 0);
}

void selectRow(Browser& b, int row)
{
    ListView_SetItemState(b.list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    if (row >= 0 && (size_t)row < b.model.rowCount())
    {
        ListView_SetItemState(b.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(b.list, row, FALSE);
    }
    updatePreview(b);
}

void refilter(Browser& b)
{
    LRESULT cat = SendMessageW(b.sidebar, LB_GETCURSEL, 0, 0);
    b.model.filter(cat == LB_ERR ? 0 : (size_t)cat, utf8(windowText(b.search)));
    ListView_SetItemCountEx(b.list, (int)b.model.rowCount(), 0);
    InvalidateRect(b.list, nullptr, TRUE);
    SetWindowTextW(b.count, wide(std::to_string(b.model.rowCount()) + " lenses").c_str());
    selectRow(b, b.model.rowCount() > 0 ? 0 : -1);   // Enter picks the best match
}

// Size the detail columns to their widest entry (capped, so one long zoom range or mount name
// cannot squeeze the lens names); the name column takes the rest (see layout()).
void fitColumns(Browser& b)
{
    HDC dc = GetDC(b.list);
    HGDIOBJ old = SelectObject(dc, b.font);
    const wchar_t* titles[COL_COUNT] = {L"Lens", L"Focal", L"Mount", L"Data"};
    const float caps[COL_COUNT] = {0, 110, 90, 220};
    for (int c = COL_FOCAL; c < COL_COUNT; c++)
    {
        SIZE sz = {};
        GetTextExtentPoint32W(dc, titles[c], (int)wcslen(titles[c]), &sz);
        int w = sz.cx;
        std::set<std::string> seen;
        for (size_t i = 0; i < b.model.lensCount(); i++)
        {
            std::string v = b.model.lensCell(i, c);
            if (!seen.insert(v).second) continue;
            std::wstring t = wide(v);
            GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
            w = std::max(w, (int)sz.cx);
        }
        b.colWidth[c] = std::min(w, b.px(caps[c])) + b.px(16);
        ListView_SetColumnWidth(b.list, c, b.colWidth[c]);
    }
    SelectObject(dc, old);
    ReleaseDC(b.list, dc);
}

void layout(Browser& b)
{
    RECT rc;
    GetClientRect(b.wnd, &rc);
    const int W = rc.right, H = rc.bottom;
    const int pad = b.px(14), side = b.px(210), prev = b.px(300), bottom = b.px(56), top = b.px(52);
    const int listX = pad + side + pad, listW = std::max(b.px(200), W - listX - pad - prev - pad);

    MoveWindow(b.search, listX, b.px(14), b.px(360), b.px(26), TRUE);
    MoveWindow(b.count, listX + b.px(372), b.px(14), b.px(260), b.px(26), TRUE);
    MoveWindow(b.sidebar, pad, pad, side, std::max(0, H - bottom - pad), TRUE);
    MoveWindow(b.list, listX, top, listW, std::max(0, H - bottom - top), TRUE);
    MoveWindow(b.preview, W - pad - prev, pad, prev, std::max(0, H - bottom - pad), TRUE);
    const int by = H - b.px(14) - b.px(30);
    MoveWindow(b.useBtn, W - pad - b.px(120), by, b.px(120), b.px(30), TRUE);
    MoveWindow(b.cancelBtn, W - pad - b.px(230), by, b.px(100), b.px(30), TRUE);
    if (b.baseBtn) MoveWindow(b.baseBtn, pad, by, b.px(140), b.px(30), TRUE);

    RECT lc;
    GetClientRect(b.list, &lc);   // excludes the scroll bar
    int rest = lc.right - b.colWidth[COL_FOCAL] - b.colWidth[COL_MOUNT] - b.colWidth[COL_DATA];
    ListView_SetColumnWidth(b.list, COL_NAME, std::max(b.px(160), rest));
}

void applyFonts(Browser& b)
{
    for (HWND h : {b.search, b.count, b.sidebar, b.list, b.useBtn, b.cancelBtn, b.baseBtn})
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(b.font), TRUE);
}

void createControls(Browser& b)
{
    HINSTANCE inst = thisModule();
    auto make = [&](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style, int id, void* param) {
        return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, b.wnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, param);
    };
    // Creation order is the Tab order
    b.search = make(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, ID_SEARCH, nullptr);
    SendMessageW(b.search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search lenses, makers, mounts…"));
    b.list = make(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                  WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_OWNERDATA, ID_LIST, nullptr);
    ListView_SetExtendedListViewStyle(b.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    b.sidebar = make(WS_EX_CLIENTEDGE, L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                     ID_SIDEBAR, nullptr);
    b.useBtn = make(0, L"BUTTON", L"Use Lens", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, nullptr);
    b.cancelBtn = make(0, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL, nullptr);
    if (b.model.allowNone()) b.baseBtn = make(0, L"BUTTON", L"Use Base Lens", WS_TABSTOP | BS_PUSHBUTTON, ID_BASE, nullptr);
    b.count = make(0, L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE, ID_COUNT, nullptr);
    b.preview = make(0, kPreviewClass, L"", 0, ID_PREVIEW, &b);

    const wchar_t* titles[COL_COUNT] = {L"Lens", L"Focal", L"Mount", L"Data"};
    for (int c = 0; c < COL_COUNT; c++)
    {
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<wchar_t*>(titles[c]);
        col.cx = b.px(100);
        col.iSubItem = c;
        ListView_InsertColumn(b.list, c, &col);
    }
    for (size_t i = 0; i < b.model.categoryCount(); i++)
        SendMessageW(b.sidebar, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wide(b.model.categoryLabel(i)).c_str()));

    applyFonts(b);
    fitColumns(b);
}

void finish(Browser& b, bool chose)
{
    b.chose = chose;
    b.done = true;
}

// ---- preview ------------------------------------------------------------------------------------

void paintPreview(Browser& b, HWND hwnd, HDC dc)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down rows
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); return; }
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    int row = b.selectedRow();
    const Profile* p = row >= 0 && (size_t)row < b.model.rowCount() ? b.model.rowProfile((size_t)row).get() : nullptr;
    std::vector<PreviewText> texts;
    renderPreview(p, b.model.aperture(), w, h, b.scale, static_cast<unsigned char*>(bits), texts);
    GdiFlush();

    SetBkMode(mem, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(mem, b.textFonts[TEXT_BODY]);
    for (const PreviewText& t : texts)
    {
        SelectObject(mem, b.textFonts[t.style]);
        SetTextColor(mem, t.style == TEXT_TITLE || t.style == TEXT_BODY ? RGB(235, 235, 235) : RGB(153, 153, 153));
        RECT r = {(LONG)t.x, (LONG)t.y, (LONG)(t.x + t.w), (LONG)(t.y + t.h)};
        std::wstring s = wide(t.text);
        DrawTextW(mem, s.c_str(), (int)s.size(), &r, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    }
    SelectObject(mem, oldFont);

    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteDC(mem);
    DeleteObject(bmp);
}

LRESULT CALLBACK previewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
        case WM_NCCREATE:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (Browser* b = browserOf(hwnd)) paintPreview(*b, hwnd, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- window ---------------------------------------------------------------------------------------

LRESULT onNotify(Browser& b, NMHDR* h)
{
    if (h->idFrom != ID_LIST) return 0;
    switch (h->code)
    {
        case LVN_GETDISPINFOW:
        {
            LVITEMW& item = reinterpret_cast<NMLVDISPINFOW*>(h)->item;
            if ((item.mask & LVIF_TEXT) && item.iItem >= 0 && (size_t)item.iItem < b.model.rowCount() && item.cchTextMax > 0)
            {
                std::wstring s = wide(b.model.cell((size_t)item.iItem, item.iSubItem));
                lstrcpynW(item.pszText, s.c_str(), item.cchTextMax);
            }
            return 0;
        }
        case LVN_ITEMCHANGED:
        case LVN_ODSTATECHANGED:
            updatePreview(b);
            return 0;
        case NM_DBLCLK:
            if (reinterpret_cast<NMITEMACTIVATE*>(h)->iItem >= 0) finish(b, true);
            return 0;
    }
    return 0;
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE)
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    Browser* b = browserOf(hwnd);
    if (!b) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg)
    {
        case WM_CREATE:
            b->wnd = hwnd;
            b->scale = dpiScale(hwnd);
            b->makeFonts();
            createControls(*b);
            return 0;
        case WM_SIZE:
            layout(*b);
            return 0;
        case WM_GETMINMAXINFO:
        {
            RECT r = {0, 0, b->px(1000), b->px(560)};
            AdjustWindowRectEx(&r, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
            return 0;
        }
        case 0x02E0:   // WM_DPICHANGED: the window moved to a display with another scale
        {
            b->scale = HIWORD(wp) / 96.0f;
            b->makeFonts();
            applyFonts(*b);
            fitColumns(*b);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            layout(*b);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case DM_GETDEFID:
            return MAKELRESULT(IDOK, DC_HASDEFID);
        case WM_NOTIFY:
            return onNotify(*b, reinterpret_cast<NMHDR*>(lp));
        case WM_COMMAND:
        {
            const int id = LOWORD(wp), code = HIWORD(wp);
            if (id == ID_SEARCH && code == EN_CHANGE) refilter(*b);
            else if (id == ID_SIDEBAR && code == LBN_SELCHANGE) refilter(*b);
            else if (id == IDOK) { if (b->selectedRow() >= 0) finish(*b, true); }
            else if (id == IDCANCEL) finish(*b, false);
            else if (id == ID_BASE) { b->none = true; finish(*b, true); }
            return 0;
        }
        case WM_CLOSE:
            finish(*b, false);
            return 0;
        case WM_DESTROY:
            b->done = true;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void registerClasses()
{
    HINSTANCE inst = thisModule();
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    if (!GetClassInfoExW(inst, kWindowClass, &wc))
    {
        wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = windowProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = kWindowClass;
        RegisterClassExW(&wc);
    }
    wc = {};
    wc.cbSize = sizeof(wc);
    if (!GetClassInfoExW(inst, kPreviewClass, &wc))
    {
        wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = previewProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        wc.lpszClassName = kPreviewClass;
        RegisterClassExW(&wc);
    }
}

// Resolve's window, when the call comes on its UI thread. A window on another thread is never
// used as the owner: disabling it from here would wait on that thread, which may be waiting on us.
HWND ownerWindow()
{
    HWND w = GetActiveWindow();
    if (!w)
    {
        HWND f = GetForegroundWindow();
        DWORD pid = 0;
        DWORD tid = f ? GetWindowThreadProcessId(f, &pid) : 0;
        if (f && pid == GetCurrentProcessId() && tid == GetCurrentThreadId()) w = f;
    }
    return w ? GetAncestor(w, GA_ROOT) : nullptr;
}

HWND createWindow(Browser& b, HWND owner)
{
    registerClasses();
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN;
    const DWORD ex = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT | (owner ? 0 : WS_EX_TOPMOST);

    // Centred on the work area of the display Resolve is on, at that display's scale
    HMONITOR mon = MonitorFromWindow(owner ? owner : GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    const RECT& work = mi.rcWork;
    b.scale = dpiScale(owner);
    RECT r = {0, 0, b.px(1220), b.px(700)};
    AdjustWindowRectEx(&r, style, FALSE, ex);
    int w = std::min<int>(r.right - r.left, work.right - work.left);
    int h = std::min<int>(r.bottom - r.top, work.bottom - work.top);
    int x = work.left + (work.right - work.left - w) / 2, y = work.top + (work.bottom - work.top - h) / 2;

    std::wstring title = wide(b.title.empty() ? std::string("Choose Lens") : b.title);
    return CreateWindowExW(ex, kWindowClass, title.c_str(), style, x, y, w, h, owner, nullptr, thisModule(), &b);
}

bool isListKey(WPARAM k) { return k == VK_UP || k == VK_DOWN || k == VK_PRIOR || k == VK_NEXT; }

// Up/Down/PgUp/PgDn typed in the search field move through the list
void moveSelection(Browser& b, WPARAM key)
{
    int n = (int)b.model.rowCount();
    if (n == 0) return;
    int page = std::max(1, ListView_GetCountPerPage(b.list) - 1);
    int r = b.selectedRow();
    int step = key == VK_UP ? -1 : key == VK_DOWN ? 1 : key == VK_PRIOR ? -page : page;
    r = r < 0 ? 0 : std::max(0, std::min(n - 1, r + step));
    selectRow(b, r);
}

} // namespace

namespace lc {

bool showLensBrowser(const BrowserRequest& p_Request, std::string& outId)
{
    ManifestContext manifest;
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    Browser b(p_Request);
    HWND owner = ownerWindow();
    HWND wnd = createWindow(b, owner);
    if (!wnd) return false;
    layout(b);

    SendMessageW(b.sidebar, LB_SETCURSEL, 0, 0);
    refilter(b);
    int row = b.model.rowOf(p_Request.currentId);
    if (row >= 0) selectRow(b, row);

    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(wnd, SW_SHOW);
    SetForegroundWindow(wnd);
    SetFocus(b.search);

    bool quit = false;
    WPARAM quitCode = 0;
    MSG msg;
    while (!b.done)
    {
        BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r == 0) { quit = true; quitCode = msg.wParam; break; }
        if (r == -1) break;
        if (msg.message == WM_KEYDOWN && msg.hwnd == b.search && isListKey(msg.wParam))
        {
            moveSelection(b, msg.wParam);
            continue;
        }
        if (!IsDialogMessageW(wnd, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    const bool chose = b.chose;
    const std::string picked = b.none ? std::string() : b.selectedId();
    if (owner) EnableWindow(owner, TRUE);   // before destroying, so Windows activates the owner
    if (IsWindow(wnd)) DestroyWindow(wnd);
    if (owner) SetForegroundWindow(owner);
    if (quit) PostQuitMessage((int)quitCode);   // hand the host's quit request back to its loop

    if (chose) outId = picked;
    return chose;
}

bool snapshotLensBrowser(const BrowserRequest&, const std::string&, const std::string&, const std::string&, const std::string&)
{
    return false;   // the self-test checks the model and the software preview on every platform instead
}

} // namespace lc
