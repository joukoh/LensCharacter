// LensBrowser.mm — the "Choose Lens…" popup: makers sidebar, searchable lens list, preview.
// Compiled with ARC. Class names carry an LC prefix because every plugin shares Resolve's
// Objective-C runtime.

#import <Cocoa/Cocoa.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <vector>

#include "LensBrowser.h"
#include "LensBrowserModel.h"

using namespace lc;

static NSString* NS(const std::string& s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

// ---- preview view -------------------------------------------------------------------------------

@interface LCLensPreview : NSView
@property (nonatomic) float aperture;
- (void)showProfile:(const ProfilePtr&)p;
@end

@implementation LCLensPreview
{
    ProfilePtr _profile;
}

- (BOOL)isFlipped { return YES; }

- (void)showProfile:(const ProfilePtr&)p
{
    _profile = p;
    [self setNeedsDisplay:YES];
}

static void drawText(NSString* s, NSRect r, CGFloat size, NSColor* c, BOOL bold)
{
    NSDictionary* a = @{NSFontAttributeName: bold ? [NSFont boldSystemFontOfSize:size] : [NSFont systemFontOfSize:size],
                        NSForegroundColorAttributeName: c};
    [s drawWithRect:r options:NSStringDrawingUsesLineFragmentOrigin attributes:a context:nil];
}

- (void)drawRect:(NSRect)dirty
{
    [[NSColor colorWithWhite:0.11 alpha:1] setFill];
    NSRectFill(self.bounds);
    const CGFloat W = self.bounds.size.width;
    NSColor* text = [NSColor colorWithWhite:0.92 alpha:1];
    NSColor* dim = [NSColor colorWithWhite:0.6 alpha:1];

    if (!_profile)
    {
        drawText(@"Select a lens to preview it.", NSMakeRect(16, 16, W - 32, 40), 13, dim, NO);
        return;
    }
    const Profile& p = *_profile;
    LensFacts f = lensFacts(p, self.aperture);

    CGFloat y = 14;
    drawText(NS(p.name), NSMakeRect(16, y, W - 32, 40), 14, text, YES);
    y += 40;
    drawText(NS(lensSubtitle(p)), NSMakeRect(16, y, W - 32, 34), 11, dim, NO);
    y += 40;

    // Distortion grid, exaggerated so subtle lenses are visible
    NSRect box = NSMakeRect(16, y, W - 32, (W - 32) * 9.0 / 16.0);
    [[NSColor colorWithWhite:0.16 alpha:1] setFill];
    NSRectFill(box);
    PreviewGrid g = previewGrid(p, box.origin.x, box.origin.y, box.size.width, box.size.height);
    [NSGraphicsContext saveGraphicsState];
    NSRectClip(box);
    NSBezierPath* grid = [NSBezierPath bezierPath];
    grid.lineWidth = 1.0;
    for (const auto& line : g.lines)
        for (size_t j = 0; j < line.size(); j++)
        {
            NSPoint a = NSMakePoint(line[j].first, line[j].second);
            j ? [grid lineToPoint:a] : [grid moveToPoint:a];
        }
    [[NSColor colorWithRed:0.62 green:0.66 blue:0.78 alpha:0.9] setStroke];
    [grid stroke];
    [NSGraphicsContext restoreGraphicsState];
    const float exaggerate = g.exaggerate;
    if (exaggerate > 1.01f)
        drawText([NSString stringWithFormat:@"shown ×%.0f", exaggerate], NSMakeRect(box.origin.x + 6, NSMaxY(box) - 18, 120, 16), 10, dim, NO);
    y = NSMaxY(box) + 12;

    // Bokeh: the kernel's own sampling, drawn at the frame centre and at an edge
    if (p.hasCharacter)
    {
        CGFloat size = (W - 48) / 2.0;
        for (int which = 0; which < 2; which++)
        {
            NSRect cell = NSMakeRect(16 + which * (size + 16), y, size, size);
            [[NSColor colorWithWhite:0.05 alpha:1] setFill];
            NSRectFill(cell);
            CGFloat scale = size * 0.30;
            for (const BokehDot& d : previewBokeh(p.character, which == 1))
            {
                [[NSColor colorWithRed:1.0 green:0.86 blue:0.62 alpha:d.alpha] setFill];
                NSRectFillUsingOperation(NSMakeRect(NSMidX(cell) + d.x * scale - 1, NSMidY(cell) + d.y * scale - 1, 2.2, 2.2), NSCompositingOperationSourceOver);
            }
            drawText(which == 0 ? @"Bokeh, centre" : @"Bokeh, edge", NSMakeRect(cell.origin.x + 6, NSMaxY(cell) - 18, size, 16), 10, dim, NO);
        }
        y += size + 14;
    }

    // Numbers
    drawText(NS(lensFactsText(p, f, self.aperture)), NSMakeRect(16, y, W - 32, 140), 11.5, text, NO);
}
@end

// ---- controller ---------------------------------------------------------------------------------

@interface LCLensBrowserController : NSObject <NSTableViewDataSource, NSTableViewDelegate, NSSearchFieldDelegate, NSWindowDelegate>
@property (strong) NSPanel* panel;
@property (strong) NSSearchField* search;
@property (strong) NSTableView* sidebar;
@property (strong) NSTableView* table;
@property (strong) LCLensPreview* preview;
@property (strong) NSButton* useButton;
@property (strong) NSTextField* countLabel;
@property (nonatomic) BOOL chose;
@property (nonatomic) BOOL modal;
- (instancetype)initWithRequest:(const BrowserRequest&)req;
- (std::string)selectedId;
- (void)selectCategory:(NSString*)cat search:(NSString*)text selectId:(const std::string&)pid;
@end

@implementation LCLensBrowserController
{
    std::unique_ptr<BrowserModel> _model;
    BOOL _allowNone;
    float _aperture;
    BOOL _none;   // "Use Base Lens" was chosen
}

- (instancetype)initWithRequest:(const BrowserRequest&)req
{
    if (!(self = [super init])) return nil;
    _allowNone = req.allowNone;
    _aperture = req.aperture;
    _model.reset(new BrowserModel(req));
    [self buildWindow:NS(req.title)];
    return self;
}

- (void)buildWindow:(NSString*)title
{
    const CGFloat W = 1220, H = 700;
    self.panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, W, H)
                                            styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                              backing:NSBackingStoreBuffered defer:NO];
    self.panel.title = title.length ? title : @"Choose Lens";
    self.panel.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    self.panel.minSize = NSMakeSize(1000, 560);
    self.panel.delegate = self;
    self.panel.releasedWhenClosed = NO;
    NSView* root = self.panel.contentView;

    const CGFloat side = 210, preview = 300, pad = 14, bottom = 56, top = 52;

    self.search = [[NSSearchField alloc] initWithFrame:NSMakeRect(pad + side + pad, H - top + 12, 360, 26)];
    self.search.placeholderString = @"Search lenses, makers, mounts…";
    self.search.delegate = self;
    self.search.autoresizingMask = NSViewMinYMargin;
    [root addSubview:self.search];

    self.countLabel = [NSTextField labelWithString:@""];
    self.countLabel.frame = NSMakeRect(pad + side + pad + 372, H - top + 16, 260, 18);
    self.countLabel.textColor = NSColor.secondaryLabelColor;
    self.countLabel.autoresizingMask = NSViewMinYMargin;
    [root addSubview:self.countLabel];

    // Sidebar
    NSScrollView* sideScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(pad, bottom, side, H - bottom - top + 38)];
    sideScroll.hasVerticalScroller = YES;
    sideScroll.autoresizingMask = NSViewHeightSizable;
    self.sidebar = [[NSTableView alloc] initWithFrame:sideScroll.bounds];
    NSTableColumn* sc = [[NSTableColumn alloc] initWithIdentifier:@"cat"];
    sc.width = side - 4;
    [self.sidebar addTableColumn:sc];
    self.sidebar.headerView = nil;
    self.sidebar.rowHeight = 22;
    self.sidebar.style = NSTableViewStyleSourceList;
    self.sidebar.dataSource = self;
    self.sidebar.delegate = self;
    sideScroll.documentView = self.sidebar;
    [root addSubview:sideScroll];

    // Lens list
    CGFloat listX = pad + side + pad, listW = W - listX - pad - preview - pad;
    NSScrollView* listScroll = [[NSScrollView alloc] initWithFrame:NSMakeRect(listX, bottom, listW, H - bottom - top)];
    listScroll.hasVerticalScroller = YES;
    listScroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.table = [[NSTableView alloc] initWithFrame:listScroll.bounds];
    // Size the detail columns to their widest entry (capped, so one long zoom range or mount
    // name cannot squeeze the lens names), and let the name column take the rest. Fixed widths
    // that ignored the gap between columns pushed the Data column past the right edge.
    NSDictionary* cellFont = @{NSFontAttributeName: [NSFont systemFontOfSize:NSFont.systemFontSize]};
    BrowserModel* model = _model.get();
    auto fitWidth = [&](NSString* title, CGFloat cap, int column) {
        CGFloat w = [title sizeWithAttributes:cellFont].width;
        std::set<std::string> seen;
        for (size_t i = 0; i < model->lensCount(); i++)
        {
            std::string v = model->lensCell(i, column);
            if (seen.insert(v).second) w = std::max(w, [NS(v) sizeWithAttributes:cellFont].width);
        }
        return std::ceil(std::min(w, cap)) + 8;
    };
    CGFloat focalW = fitWidth(@"Focal", 110, COL_FOCAL);
    CGFloat mountW = fitWidth(@"Mount", 90, COL_MOUNT);
    CGFloat traitsW = fitWidth(@"Data", 220, COL_DATA);
    struct { NSString* ident; NSString* title; CGFloat width; } cols[] = {
        {@"name", @"Lens", 200}, {@"focal", @"Focal", focalW}, {@"mount", @"Mount", mountW}, {@"traits", @"Data", traitsW}};
    for (auto& c : cols)
    {
        NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:c.ident];
        col.title = c.title;
        col.width = c.width;
        col.minWidth = [c.ident isEqualToString:@"name"] ? 160 : 40;
        col.resizingMask = [c.ident isEqualToString:@"name"] ? NSTableColumnAutoresizingMask : NSTableColumnUserResizingMask;
        [self.table addTableColumn:col];
    }
    self.table.columnAutoresizingStyle = NSTableViewFirstColumnOnlyAutoresizingStyle;
    self.table.allowsExpansionToolTips = YES;   // hover shows the full text of a truncated cell
    self.table.style = NSTableViewStyleFullWidth;  // keep edge-to-edge rows; Automatic switches to inset
    self.table.rowHeight = 22;
    self.table.usesAlternatingRowBackgroundColors = YES;
    self.table.dataSource = self;
    self.table.delegate = self;
    self.table.target = self;
    self.table.doubleAction = @selector(useLens:);
    listScroll.documentView = self.table;
    [root addSubview:listScroll];
    [self.table sizeToFit];   // fit all columns inside the visible list, resizing the name column

    // Preview
    self.preview = [[LCLensPreview alloc] initWithFrame:NSMakeRect(W - pad - preview, bottom, preview, H - bottom - top + 38)];
    self.preview.aperture = _aperture;
    self.preview.autoresizingMask = NSViewMinXMargin | NSViewHeightSizable;
    self.preview.wantsLayer = YES;
    self.preview.layer.cornerRadius = 6;
    [root addSubview:self.preview];

    // Buttons
    self.useButton = [NSButton buttonWithTitle:@"Use Lens" target:self action:@selector(useLens:)];
    self.useButton.frame = NSMakeRect(W - pad - 120, 14, 120, 30);
    self.useButton.keyEquivalent = @"\r";
    self.useButton.autoresizingMask = NSViewMinXMargin;
    [root addSubview:self.useButton];

    NSButton* cancel = [NSButton buttonWithTitle:@"Cancel" target:self action:@selector(cancel:)];
    cancel.frame = NSMakeRect(W - pad - 120 - 10 - 100, 14, 100, 30);
    cancel.keyEquivalent = @"\033";
    cancel.autoresizingMask = NSViewMinXMargin;
    [root addSubview:cancel];

    if (_allowNone)
    {
        NSButton* none = [NSButton buttonWithTitle:@"Use Base Lens" target:self action:@selector(useBase:)];
        none.frame = NSMakeRect(pad, 14, 140, 30);
        [root addSubview:none];
    }
}

// ---- filtering ----

- (void)refilter
{
    NSInteger catRow = self.sidebar.selectedRow;
    _model->filter(catRow >= 0 ? (size_t)catRow : 0, self.search.stringValue.UTF8String ?: "");
    [self.table reloadData];
    self.countLabel.stringValue = [NSString stringWithFormat:@"%zu lenses", _model->rowCount()];
    if (_model->rowCount() > 0 && self.table.selectedRow < 0)
        [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
    [self updatePreview];
}

- (void)updatePreview
{
    NSInteger r = self.table.selectedRow;
    [self.preview showProfile:(r >= 0 && r < (NSInteger)_model->rowCount()) ? _model->rowProfile((size_t)r) : ProfilePtr()];
    self.useButton.enabled = r >= 0;
}

- (void)selectCategory:(NSString*)cat search:(NSString*)text selectId:(const std::string&)pid
{
    size_t ci = _model->categoryIndex(cat.UTF8String ?: "");
    [self.sidebar reloadData];
    [self.sidebar selectRowIndexes:[NSIndexSet indexSetWithIndex:ci] byExtendingSelection:NO];
    self.search.stringValue = text ?: @"";
    [self refilter];
    int row = _model->rowOf(pid);
    if (row >= 0)
    {
        [self.table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row] byExtendingSelection:NO];
        [self.table scrollRowToVisible:row];
    }
    [self updatePreview];
}

- (std::string)selectedId
{
    NSInteger r = self.table.selectedRow;
    if (_none) return std::string();
    return (r >= 0 && r < (NSInteger)_model->rowCount()) ? _model->rowProfile((size_t)r)->id : std::string();
}

// ---- actions ----

- (void)finish:(BOOL)chose
{
    self.chose = chose;
    if (self.modal)
    {
        self.modal = NO;
        [NSApp stopModal];
    }
    [self.panel orderOut:nil];
}

- (void)useLens:(id)sender { if (self.table.selectedRow >= 0) [self finish:YES]; }
- (void)cancel:(id)sender { [self finish:NO]; }
- (void)useBase:(id)sender
{
    [self.table deselectAll:nil];
    _none = YES;
    [self finish:YES];
}
- (void)windowWillClose:(NSNotification*)n { if (self.modal) { self.modal = NO; self.chose = NO; [NSApp stopModal]; } }

- (void)controlTextDidChange:(NSNotification*)n { [self refilter]; }

// ---- table data ----

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tv
{
    return tv == self.sidebar ? (NSInteger)_model->categoryCount() : (NSInteger)_model->rowCount();
}

- (id)tableView:(NSTableView*)tv objectValueForTableColumn:(NSTableColumn*)col row:(NSInteger)row
{
    if (tv == self.sidebar) return NS(_model->categoryLabel((size_t)row));
    NSString* ident = col.identifier;
    int column = [ident isEqualToString:@"name"] ? COL_NAME : [ident isEqualToString:@"focal"] ? COL_FOCAL
               : [ident isEqualToString:@"mount"] ? COL_MOUNT : COL_DATA;
    return NS(_model->cell((size_t)row, column));
}

- (BOOL)tableView:(NSTableView*)tv shouldEditTableColumn:(NSTableColumn*)col row:(NSInteger)row { return NO; }

- (void)tableViewSelectionDidChange:(NSNotification*)n
{
    if (n.object == self.sidebar) [self refilter];
    else [self updatePreview];
}
@end

// ---- entry points ---------------------------------------------------------------------------------

namespace lc {

static bool runOnMain(bool (^block)(void))
{
    if ([NSThread isMainThread]) return block();
    __block bool result = false;
    dispatch_sync(dispatch_get_main_queue(), ^{ result = block(); });
    return result;
}

bool showLensBrowser(const BrowserRequest& p_Request, std::string& outId)
{
    BrowserRequest req = p_Request;
    __block std::string picked;
    bool ok = runOnMain(^bool {
        @autoreleasepool
        {
            [NSApplication sharedApplication];
            LCLensBrowserController* c = [[LCLensBrowserController alloc] initWithRequest:req];
            [c selectCategory:NS(kCategoryAll) search:@"" selectId:req.currentId];
            [c.panel center];
            c.modal = YES;
            [c.panel makeKeyAndOrderFront:nil];
            [c.panel makeFirstResponder:c.search];
            [NSApp runModalForWindow:c.panel];
            if (c.chose) picked = [c selectedId];
            return c.chose;
        }
    });
    if (ok) outId = picked;
    return ok;
}

bool snapshotLensBrowser(const BrowserRequest& p_Request, const std::string& p_Category, const std::string& p_Search,
                         const std::string& p_SelectId, const std::string& p_PngPath)
{
    @autoreleasepool
    {
        [NSApplication sharedApplication];
        LCLensBrowserController* c = [[LCLensBrowserController alloc] initWithRequest:p_Request];
        [c selectCategory:NS(p_Category) search:NS(p_Search) selectId:p_SelectId];
        NSView* v = c.panel.contentView;
        [v layoutSubtreeIfNeeded];
        [v displayIfNeeded];
        NSBitmapImageRep* rep = [v bitmapImageRepForCachingDisplayInRect:v.bounds];
        if (!rep) return false;
        [v cacheDisplayInRect:v.bounds toBitmapImageRep:rep];

        // The content view has no background of its own (on screen the window frame draws it),
        // so composite onto the panel's window colour; otherwise the dark-appearance text and
        // button titles are white on transparent and the snapshot looks blank.
        NSBitmapImageRep* out = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL
            pixelsWide:rep.pixelsWide pixelsHigh:rep.pixelsHigh bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES
            isPlanar:NO colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
        if (!out) return false;
        out.size = rep.size;
        [NSGraphicsContext saveGraphicsState];
        NSGraphicsContext.currentContext = [NSGraphicsContext graphicsContextWithBitmapImageRep:out];
        [c.panel.appearance performAsCurrentDrawingAppearance:^{
            [NSColor.windowBackgroundColor setFill];
            NSRectFill(NSMakeRect(0, 0, rep.size.width, rep.size.height));
        }];
        [rep drawInRect:NSMakeRect(0, 0, rep.size.width, rep.size.height) fromRect:NSZeroRect
              operation:NSCompositingOperationSourceOver fraction:1.0 respectFlipped:NO hints:nil];
        [NSGraphicsContext restoreGraphicsState];
        NSData* png = [out representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        return [png writeToFile:NS(p_PngPath) atomically:YES];
    }
}

} // namespace lc
