#import "Skin.h"

#include <cmath>
#include <cstdlib>

namespace {

// A small deterministic noise source. Deterministic so the texture is identical
// every launch -- a leather grain that reshuffles itself on each start would be
// visible and cheap-looking.
struct Rng {
    unsigned s;
    explicit Rng(unsigned seed) : s(seed ? seed : 1) {}
    unsigned next() {
        s = s * 1664525u + 1013904223u;
        return s;
    }
    double unit() { return static_cast<double>(next() & 0xFFFFFF) / 16777215.0; }
};

// Value noise with smooth interpolation: cheap, and gives the soft mottling
// that reads as hide rather than as television static.
//
// The lattice wraps at the tile edge. Without that the value at x = 0 and the
// value at x = kTileSize are unrelated, every repeat of the tile shows a hard
// edge, and a surface meant to read as one piece of leather instead reads as a
// grid of tiles -- which is exactly how it looked before. periodX and periodY
// must divide kTileSize, or the wrap lands mid-cell and the seam comes back.
const int kTileSize = 128;

// A free function rather than a lambda on purpose. A capture list is written
// with the same brackets as a message send, and GCC's Objective-C++ front end
// resolves the ambiguity the wrong way: `[nx, ny](int, int)` is parsed as an
// expression with a comma operator, and the only sign of it is
// "left operand of comma operator has no effect". An empty `[]` capture happens
// to be unambiguous, which is why the original compiled.
double lattice_at(int ix, int iy, int nx, int ny) {
    ix = ((ix % nx) + nx) % nx;
    iy = ((iy % ny) + ny) % ny;
    unsigned h = static_cast<unsigned>(ix) * 374761393u +
                 static_cast<unsigned>(iy) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;

    return static_cast<double>((h ^ (h >> 16)) & 0xFFFF) / 65535.0;
}

double smooth_noise(double x, double y, int periodX, int periodY) {
    int nx = kTileSize / (periodX > 0 ? periodX : 1);
    int ny = kTileSize / (periodY > 0 ? periodY : 1);
    if (nx < 1) nx = 1;
    if (ny < 1) ny = 1;

    double fx = x / periodX, fy = y / periodY;
    int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
    double tx = fx - x0, ty = fy - y0;
    // Smoothstep, so the lattice does not show as a grid.
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);

    double a = lattice_at(x0, y0, nx, ny), b = lattice_at(x0 + 1, y0, nx, ny);
    double c = lattice_at(x0, y0 + 1, nx, ny), d = lattice_at(x0 + 1, y0 + 1, nx, ny);
    double top = a + (b - a) * tx;
    double bottom = c + (d - c) * tx;
    return top + (bottom - top) * ty;
}

// Isotropic convenience.
double smooth_noise(double x, double y, int period) {
    return smooth_noise(x, y, period, period);
}

// GCC has no blocks, so the pixel filler is an ordinary function pointer.
typedef void (*FillFn)(unsigned char* px, int w, int h);

NSImage* make_tile(int size, FillFn fill) {
    NSBitmapImageRep* rep = [[[NSBitmapImageRep alloc]
        initWithBitmapDataPlanes:NULL
                      pixelsWide:size
                      pixelsHigh:size
                   bitsPerSample:8
                 samplesPerPixel:3
                        hasAlpha:NO
                        isPlanar:NO
                  colorSpaceName:NSDeviceRGBColorSpace
                     bytesPerRow:size * 3
                    bitsPerPixel:24] autorelease];
    fill([rep bitmapData], size, size);

    NSImage* img = [[[NSImage alloc] initWithSize:NSMakeSize(size, size)] autorelease];
    [img addRepresentation:rep];
    return img;
}

void fill_leather(unsigned char* px, int w, int h) {
        // No Rng here: the noise is hashed from the coordinates themselves, so
        // the tile is both reproducible across launches and able to wrap.
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                // Two octaves: broad hide mottling, then fine pebble grain.
                double broad = smooth_noise(x, y, 32);
                double fine  = smooth_noise(x, y, 4);
                double grain = 0.70 * broad + 0.30 * fine;

                // Oxblood: a warm dark red-brown.
                double base_r = 74, base_g = 38, base_b = 32;
                double lift = (grain - 0.5) * 34.0;

                // Occasional darker creases, as if the hide were folded.
                double crease = smooth_noise(x, y, 16, 8);
                if (crease < 0.22) lift -= (0.22 - crease) * 90.0;

                int r = static_cast<int>(base_r + lift * 1.10);
                int g = static_cast<int>(base_g + lift * 0.80);
                int b = static_cast<int>(base_b + lift * 0.70);

                px[(y * w + x) * 3 + 0] = static_cast<unsigned char>(std::max(0, std::min(255, r)));
                px[(y * w + x) * 3 + 1] = static_cast<unsigned char>(std::max(0, std::min(255, g)));
                px[(y * w + x) * 3 + 2] = static_cast<unsigned char>(std::max(0, std::min(255, b)));
            }
        }
}

void fill_paper(unsigned char* px, int w, int h) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                // Aged cream with a very fine fibre speckle. Kept subtle: paper
                // that is obviously noisy looks dirty rather than old.
                double fibre = smooth_noise(x, y, 2);
                double stain = smooth_noise(x, y, 64);
                double lift = (fibre - 0.5) * 7.0 + (stain - 0.5) * 9.0;

                int r = static_cast<int>(243 + lift);
                int g = static_cast<int>(236 + lift * 0.95);
                int b = static_cast<int>(217 + lift * 0.85);

                px[(y * w + x) * 3 + 0] = static_cast<unsigned char>(std::max(0, std::min(255, r)));
                px[(y * w + x) * 3 + 1] = static_cast<unsigned char>(std::max(0, std::min(255, g)));
                px[(y * w + x) * 3 + 2] = static_cast<unsigned char>(std::max(0, std::min(255, b)));
            }
        }
}

} // namespace

@implementation PPSkin

// 128 tiles seamlessly at the noise periods used above, and is small enough to
// generate in a few milliseconds even on this hardware.
+ (NSImage *)leatherTile {
    static NSImage* tile = nil;
    if (!tile) tile = [make_tile(kTileSize, fill_leather) retain];
    return tile;
}

+ (NSImage *)paperTile {
    static NSImage* tile = nil;
    if (!tile) tile = [make_tile(kTileSize, fill_paper) retain];
    return tile;
}

+ (NSColor *)leatherColor {
    static NSColor* c = nil;
    if (!c) c = [[NSColor colorWithPatternImage:[PPSkin leatherTile]] retain];
    return c;
}

+ (NSColor *)darkLeatherColor {
    static NSColor* c = nil;
    if (!c) c = [[NSColor colorWithCalibratedRed:0.16 green:0.085 blue:0.075 alpha:1.0] retain];
    return c;
}

+ (NSColor *)paperColor {
    static NSColor* c = nil;
    if (!c) c = [[NSColor colorWithPatternImage:[PPSkin paperTile]] retain];
    return c;
}

+ (NSColor *)metalColor {
    static NSColor* c = nil;
    if (!c) c = [[NSColor colorWithCalibratedWhite:0.82 alpha:1.0] retain];
    return c;
}

+ (NSColor *)inkColor {
    static NSColor* c = nil;
    if (!c) c = [[NSColor colorWithCalibratedRed:0.11 green:0.12 blue:0.19 alpha:1.0] retain];
    return c;
}
+ (NSColor *)fadedInkColor {
    static NSColor* c = nil;
    if (!c) c = [[NSColor colorWithCalibratedRed:0.36 green:0.35 blue:0.40 alpha:1.0] retain];
    return c;
}
+ (NSColor *)redInkColor {
    static NSColor* c = nil;
    if (!c) c = [[NSColor colorWithCalibratedRed:0.60 green:0.13 blue:0.11 alpha:1.0] retain];
    return c;
}
+ (NSColor *)marginRuleColor {
    static NSColor* c = nil;
    if (!c) c = [[NSColor colorWithCalibratedRed:0.78 green:0.42 blue:0.42 alpha:0.55] retain];
    return c;
}
+ (NSColor *)ruleColor {
    static NSColor* c = nil;
    if (!c) c = [[NSColor colorWithCalibratedRed:0.55 green:0.62 blue:0.72 alpha:0.30] retain];
    return c;
}

+ (NSBezierPath *)roundedRectPath:(NSRect)rect radius:(CGFloat)radius {
    // 10.5 has no +bezierPathWithRoundedRect:, so build it by hand.
    NSBezierPath* p = [NSBezierPath bezierPath];
    if (radius <= 0.0) {
        [p appendBezierPathWithRect:rect];
        return p;
    }
    CGFloat r = radius;
    CGFloat minX = NSMinX(rect), midX = NSMidX(rect), maxX = NSMaxX(rect);
    CGFloat minY = NSMinY(rect), midY = NSMidY(rect), maxY = NSMaxY(rect);

    [p moveToPoint:NSMakePoint(minX, midY)];
    [p appendBezierPathWithArcFromPoint:NSMakePoint(minX, minY)
                                toPoint:NSMakePoint(midX, minY)
                                 radius:r];
    [p appendBezierPathWithArcFromPoint:NSMakePoint(maxX, minY)
                                toPoint:NSMakePoint(maxX, midY)
                                 radius:r];
    [p appendBezierPathWithArcFromPoint:NSMakePoint(maxX, maxY)
                                toPoint:NSMakePoint(midX, maxY)
                                 radius:r];
    [p appendBezierPathWithArcFromPoint:NSMakePoint(minX, maxY)
                                toPoint:NSMakePoint(minX, midY)
                                 radius:r];
    [p closePath];
    return p;
}

+ (void)drawStitchingInRect:(NSRect)rect inset:(CGFloat)inset {
    NSRect r = NSInsetRect(rect, inset, inset);
    NSBezierPath* p = [PPSkin roundedRectPath:r radius:5.0];

    CGFloat pattern[2] = {5.0, 4.0};
    [p setLineDash:pattern count:2 phase:0.0];
    [p setLineWidth:2.0];

    // The dark trough the thread sits in, then the thread itself one pixel up,
    // which is what makes it read as stitching rather than a dashed line.
    [[NSColor colorWithCalibratedWhite:0.0 alpha:0.42] set];
    [p stroke];

    NSAffineTransform* up = [NSAffineTransform transform];
    [up translateXBy:0.0 yBy:-1.0];
    NSBezierPath* thread = [up transformBezierPath:p];
    [thread setLineDash:pattern count:2 phase:0.0];
    [thread setLineWidth:1.5];
    [[NSColor colorWithCalibratedRed:0.93 green:0.86 blue:0.70 alpha:0.75] set];
    [thread stroke];
}

+ (void)drawRecessedWellInRect:(NSRect)rect radius:(CGFloat)radius {
    NSBezierPath* p = [PPSkin roundedRectPath:rect radius:radius];

    [NSGraphicsContext saveGraphicsState];
    [p addClip];

    // Inner shadow along the top and left: the light comes from above, so a
    // recess is dark where it faces the light.
    for (int i = 0; i < 6; i++) {
        CGFloat a = 0.20 - i * 0.032;
        if (a <= 0) break;
        [[NSColor colorWithCalibratedWhite:0.0 alpha:a] set];
        NSRectFillUsingOperation(
            NSMakeRect(NSMinX(rect), NSMinY(rect) + i, NSWidth(rect), 1),
            NSCompositeSourceOver);
        NSRectFillUsingOperation(
            NSMakeRect(NSMinX(rect) + i, NSMinY(rect), 1, NSHeight(rect)),
            NSCompositeSourceOver);
    }
    // A catch of light along the bottom lip.
    [[NSColor colorWithCalibratedWhite:1.0 alpha:0.22] set];
    NSRectFillUsingOperation(
        NSMakeRect(NSMinX(rect), NSMaxY(rect) - 1, NSWidth(rect), 1),
        NSCompositeSourceOver);

    [NSGraphicsContext restoreGraphicsState];

    [[NSColor colorWithCalibratedWhite:0.0 alpha:0.55] set];
    [p setLineWidth:1.0];
    [p stroke];
}

+ (void)drawRaisedPanelInRect:(NSRect)rect radius:(CGFloat)radius {
    NSBezierPath* p = [PPSkin roundedRectPath:rect radius:radius];

    // Shadow cast beneath.
    [NSGraphicsContext saveGraphicsState];
    for (int i = 3; i >= 1; i--) {
        NSRect s = NSOffsetRect(rect, 0, i);
        NSBezierPath* sp = [PPSkin roundedRectPath:s radius:radius];
        [[NSColor colorWithCalibratedWhite:0.0 alpha:0.10] set];
        [sp fill];
    }
    [NSGraphicsContext restoreGraphicsState];

    [[PPSkin metalColor] set];
    [p fill];

    [NSGraphicsContext saveGraphicsState];
    [p addClip];
    [[NSColor colorWithCalibratedWhite:1.0 alpha:0.65] set];
    NSRectFillUsingOperation(NSMakeRect(NSMinX(rect), NSMinY(rect), NSWidth(rect), 1),
                             NSCompositeSourceOver);
    [NSGraphicsContext restoreGraphicsState];

    [[NSColor colorWithCalibratedWhite:0.35 alpha:0.8] set];
    [p setLineWidth:1.0];
    [p stroke];
}

+ (void)drawVignetteInRect:(NSRect)rect strength:(CGFloat)strength {
    // Cheap approximation: concentric inset frames of decreasing alpha. A real
    // radial gradient would be prettier and much slower here.
    int steps = 14;
    for (int i = 0; i < steps; i++) {
        CGFloat a = strength * (1.0 - static_cast<CGFloat>(i) / steps) / steps;
        [[NSColor colorWithCalibratedWhite:0.0 alpha:a] set];
        NSFrameRectWithWidthUsingOperation(NSInsetRect(rect, i, i), 1.0,
                                           NSCompositeSourceOver);
    }
}

+ (void)drawEmbossedText:(NSString *)text
                  inRect:(NSRect)rect
                    font:(NSFont *)font
                   color:(NSColor *)color {
    if (!text) return;
    NSMutableParagraphStyle* ps =
        [[[NSMutableParagraphStyle alloc] init] autorelease];
    [ps setAlignment:NSCenterTextAlignment];

    // Stamped into the surface: a dark impression above, the gold below.
    NSDictionary* shade = [NSDictionary dictionaryWithObjectsAndKeys:
        font, NSFontAttributeName,
        [NSColor colorWithCalibratedWhite:0.0 alpha:0.55], NSForegroundColorAttributeName,
        ps, NSParagraphStyleAttributeName, nil];
    [text drawInRect:NSOffsetRect(rect, 0, -1) withAttributes:shade];

    NSDictionary* face = [NSDictionary dictionaryWithObjectsAndKeys:
        font, NSFontAttributeName,
        color, NSForegroundColorAttributeName,
        ps, NSParagraphStyleAttributeName, nil];
    [text drawInRect:rect withAttributes:face];
}

@end

// ---------------------------------------------------------------------------

@implementation PPLeatherView

- (id)initWithFrame:(NSRect)f {
    if ((self = [super initWithFrame:f])) { dark = NO; stitched = YES; }
    return self;
}
- (void)setDark:(BOOL)d { dark = d; [self setNeedsDisplay:YES]; }
- (void)setStitched:(BOOL)s { stitched = s; [self setNeedsDisplay:YES]; }

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect b = [self bounds];

    [[PPSkin leatherColor] set];
    NSRectFill(b);

    if (dark) {
        // The sidebar is the same hide in shadow, not a different material.
        [[NSColor colorWithCalibratedWhite:0.0 alpha:0.28] set];
        NSRectFillUsingOperation(b, NSCompositeSourceOver);
    }

    [PPSkin drawVignetteInRect:b strength:0.55];
    if (stitched) [PPSkin drawStitchingInRect:b inset:7.0];
}

- (BOOL)isOpaque { return YES; }

@end

@implementation PPPaperView

- (id)initWithFrame:(NSRect)f {
    if ((self = [super initWithFrame:f])) ruled = YES;
    return self;
}
- (void)setRuled:(BOOL)r { ruled = r; [self setNeedsDisplay:YES]; }

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect b = [self bounds];

    [[PPSkin paperColor] set];
    NSRectFill(b);

    if (ruled) {
        // Faint feint ruling and a red margin, drawn beneath the text.
        [[PPSkin ruleColor] set];
        for (CGFloat y = 24.0; y < NSHeight(b); y += 16.0)
            NSRectFillUsingOperation(NSMakeRect(0, y, NSWidth(b), 1),
                                     NSCompositeSourceOver);
        [[PPSkin marginRuleColor] set];
        NSRectFillUsingOperation(NSMakeRect(52, 0, 1, NSHeight(b)),
                                 NSCompositeSourceOver);
    }
    [PPSkin drawVignetteInRect:b strength:0.30];
}

- (BOOL)isOpaque { return YES; }

@end
