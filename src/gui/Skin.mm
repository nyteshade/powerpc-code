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

// One stitch, centred on `c` and running along (ux, uy).
//
// Built from four passes, because that is what separates thread from a dash:
// the hole the thread passes through, a shadow cast into it, the thread itself
// leaning across the seam, and a highlight along the lit top edge. The lean
// alternates so consecutive stitches slope opposite ways, which is what a
// two-needle saddle stitch actually does.
void draw_stitch(NSPoint c, CGFloat ux, CGFloat uy, int index) {
  // Deterministic per-stitch variation. Hand stitching is not a ruler, and a
  // perfectly uniform seam is the thing that reads as printed.
  unsigned h = static_cast<unsigned>(index) * 2654435761u;
  h ^= h >> 15;
  CGFloat jitter = static_cast<CGFloat>(h & 0xFF) / 255.0;        // 0..1
  CGFloat len = 5.6 + jitter * 0.9;
  // Consistent lean, not alternating. Seen from one face a hand saddle stitch
  // slants the same way all along the seam -- it is the *gaps* that slant the
  // other way. Alternating it produces a zigzag, which is a machine stitch.
  CGFloat lean = 0.30 + (jitter - 0.5) * 0.09;

  CGFloat ca = cos(lean), sa = sin(lean);
  CGFloat rx = ux * ca - uy * sa;
  CGFloat ry = ux * sa + uy * ca;

  CGFloat half = len * 0.5;
  NSPoint a = NSMakePoint(c.x - rx * half, c.y - ry * half);
  NSPoint b = NSMakePoint(c.x + rx * half, c.y + ry * half);

  // The perforation: darkest right where the thread enters the hide.
  [[NSColor colorWithCalibratedWhite:0.0 alpha:0.50] set];
  for (int k = 0; k < 2; k++) {
    NSPoint e = k ? b : a;
    NSRectFillUsingOperation(NSMakeRect(e.x - 1.0, e.y - 1.0, 2.0, 2.0),
                             NSCompositeSourceOver);
  }

  NSBezierPath *thread = [NSBezierPath bezierPath];
  [thread moveToPoint:a];
  [thread lineToPoint:b];
  [thread setLineCapStyle:NSRoundLineCapStyle];

  // Shadow, offset down and right away from the light.
  NSAffineTransform *down = [NSAffineTransform transform];
  [down translateXBy:0.7 yBy:-0.7];
  NSBezierPath *shade = [down transformBezierPath:thread];
  [shade setLineCapStyle:NSRoundLineCapStyle];
  [shade setLineWidth:2.2];
  [[NSColor colorWithCalibratedWhite:0.0 alpha:0.34] set];
  [shade stroke];

  // The thread. Waxed linen against oxblood is a muted tan, not cream -- the
  // brighter it is the more it reads as a drawn line.
  [thread setLineWidth:1.9];
  [[NSColor colorWithCalibratedRed:0.78 green:0.70 blue:0.55
                             alpha:0.62 + jitter * 0.10] set];
  [thread stroke];

  // A finer highlight along the top, which is what gives it roundness.
  NSAffineTransform *up = [NSAffineTransform transform];
  [up translateXBy:-0.3 yBy:0.4];
  NSBezierPath *lit = [up transformBezierPath:thread];
  [lit setLineCapStyle:NSRoundLineCapStyle];
  [lit setLineWidth:0.8];
  [[NSColor colorWithCalibratedRed:0.95 green:0.90 blue:0.78 alpha:0.34] set];
  [lit stroke];
}

// GCC has no blocks, so the pixel filler is an ordinary function pointer.
typedef void (*FillFn)(unsigned char *px, int w, int h);

NSImage *make_tile(int size, FillFn fill) {
  NSBitmapImageRep *rep = [[[NSBitmapImageRep alloc]
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

  NSImage *img = [[[NSImage alloc] initWithSize:NSMakeSize(size, size)] autorelease];
  [img addRepresentation:rep];

  return img;
}

void fill_leather(unsigned char *px, int w, int h) {
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

void fill_paper(unsigned char *px, int w, int h) {
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

// Retained on purpose: these are process-lifetime singletons, created once and
// never released. Wrapping the call also keeps the declarations inside the
// column budget.
NSColor *rgba(CGFloat r, CGFloat g, CGFloat b, CGFloat a) {
  return [[NSColor colorWithCalibratedRed:r green:g blue:b alpha:a] retain];
}

} // namespace

@implementation PPSkin

// 128 tiles seamlessly at the noise periods used above, and is small enough to
// generate in a few milliseconds even on this hardware.
+ (NSImage *)leatherTile {
  static NSImage *tile = nil;
  if (!tile) tile = [make_tile(kTileSize, fill_leather) retain];

  return tile;
}

+ (NSImage *)paperTile {
  static NSImage *tile = nil;
  if (!tile) tile = [make_tile(kTileSize, fill_paper) retain];

  return tile;
}

+ (NSColor *)leatherColor {
  static NSColor *c = nil;
  if (!c) c = [[NSColor colorWithPatternImage:[PPSkin leatherTile]] retain];

  return c;
}

+ (NSColor *)darkLeatherColor {
  static NSColor *c = nil;
  if (!c) c = rgba(0.16, 0.085, 0.075, 1.0);

  return c;
}

+ (NSColor *)paperColor {
  static NSColor *c = nil;
  if (!c) c = [[NSColor colorWithPatternImage:[PPSkin paperTile]] retain];

  return c;
}

+ (NSColor *)metalColor {
  static NSColor *c = nil;
  if (!c) c = [[NSColor colorWithCalibratedWhite:0.82 alpha:1.0] retain];

  return c;
}

+ (NSColor *)inkColor {
  static NSColor *c = nil;
  if (!c) c = rgba(0.11, 0.12, 0.19, 1.0);

  return c;
}
+ (NSColor *)fadedInkColor {
  static NSColor *c = nil;
  if (!c) c = rgba(0.36, 0.35, 0.40, 1.0);

  return c;
}
+ (NSColor *)redInkColor {
  static NSColor *c = nil;
  if (!c) c = rgba(0.60, 0.13, 0.11, 1.0);

  return c;
}
+ (NSColor *)marginRuleColor {
  static NSColor *c = nil;
  if (!c) c = rgba(0.78, 0.42, 0.42, 0.55);

  return c;
}
+ (NSColor *)ruleColor {
  static NSColor *c = nil;
  if (!c) c = rgba(0.55, 0.62, 0.72, 0.30);

  return c;
}

+ (NSBezierPath *)roundedRectPath:(NSRect)rect radius:(CGFloat)radius {
  // 10.5 has no +bezierPathWithRoundedRect:, so build it by hand.
  NSBezierPath *p = [NSBezierPath bezierPath];
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

+ (void)drawStitchLineFrom:(NSPoint)a to:(NSPoint)b {
  CGFloat dx = b.x - a.x, dy = b.y - a.y;
  CGFloat len = sqrt(dx * dx + dy * dy);
  if (len < 4.0) return;

  CGFloat ux = dx / len, uy = dy / len;
  const CGFloat period = 8.5;
  int index = 0;

  // Start half a period in so the run does not begin flush against a corner.
  for (CGFloat pos = period * 0.5; pos < len; pos += period) {
    draw_stitch(NSMakePoint(a.x + ux * pos, a.y + uy * pos), ux, uy, index++);
  }
}

+ (void)drawStitchingInRect:(NSRect)rect inset:(CGFloat)inset {
  NSRect r = NSInsetRect(rect, inset, inset);
  if (NSWidth(r) < 24.0 || NSHeight(r) < 24.0) return;

  // Walk the path and place each stitch individually, rather than stroking a
  // dashed line. A dash pattern gives evenly spaced rectangles parallel to the
  // edge, which is exactly what a dashed border looks like and nothing like
  // thread. Real saddle stitching leans, and each stitch pulls the leather
  // slightly where it passes through.
  NSBezierPath *flat =
      [[PPSkin roundedRectPath:r radius:6.0] bezierPathByFlatteningPath];

  const CGFloat period = 8.5;      // centre to centre along the seam
  CGFloat carry = 0.0;
  int index = 0;

  NSPoint pts[3];
  NSPoint cur = NSZeroPoint, first = NSZeroPoint;
  BOOL started = NO;

  for (NSInteger i = 0; i < [flat elementCount]; i++) {
    NSBezierPathElement e = [flat elementAtIndex:i associatedPoints:pts];
    NSPoint next;

    if (e == NSMoveToBezierPathElement) {
      cur = first = pts[0];
      started = YES;
      continue;
    }

    else if (e == NSLineToBezierPathElement) { next = pts[0]; }

    else if (e == NSClosePathBezierPathElement) { next = first; }

    else { continue; }

    if (!started) { cur = next; started = YES; continue; }

    CGFloat dx = next.x - cur.x, dy = next.y - cur.y;
    CGFloat seg = sqrt(dx * dx + dy * dy);
    if (seg > 0.01) {
      CGFloat ux = dx / seg, uy = dy / seg;
      for (CGFloat pos = carry; pos < seg; pos += period) {
        draw_stitch(NSMakePoint(cur.x + ux * pos, cur.y + uy * pos), ux, uy,
                    index++);
      }
      carry = fmod(carry - seg, period);
      if (carry < 0) carry += period;
    }

    cur = next;
  }
}

+ (void)drawRecessedWellInRect:(NSRect)rect radius:(CGFloat)radius {
  NSBezierPath *p = [PPSkin roundedRectPath:rect radius:radius];

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
  NSBezierPath *p = [PPSkin roundedRectPath:rect radius:radius];

  // Shadow cast beneath.
  [NSGraphicsContext saveGraphicsState];
  for (int i = 3; i >= 1; i--) {
    NSRect s = NSOffsetRect(rect, 0, i);
    NSBezierPath *sp = [PPSkin roundedRectPath:s radius:radius];
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
  NSMutableParagraphStyle *ps =
      [[[NSMutableParagraphStyle alloc] init] autorelease];
  [ps setAlignment:NSCenterTextAlignment];

  // Blocked into the hide with a hot tool: the impression is pressed *into* the
  // surface, so it is dark where it faces the light and catches a highlight on
  // the far lip. Three passes -- a soft shadow spread into the leather around
  // the letter, the dark impression above it, a light catch below, then the
  // gold. One offset copy on its own reads as a drop shadow rather than
  // something stamped.
  NSDictionary *spread = [NSDictionary dictionaryWithObjectsAndKeys:
      font, NSFontAttributeName,
      [NSColor colorWithCalibratedWhite:0.0 alpha:0.30], NSForegroundColorAttributeName,
      ps, NSParagraphStyleAttributeName, nil];
  [text drawInRect:NSOffsetRect(rect, 0, -2) withAttributes:spread];
  [text drawInRect:NSOffsetRect(rect, 1, -1) withAttributes:spread];
  [text drawInRect:NSOffsetRect(rect, -1, -1) withAttributes:spread];

  NSDictionary *shade = [NSDictionary dictionaryWithObjectsAndKeys:
      font, NSFontAttributeName,
      [NSColor colorWithCalibratedWhite:0.0 alpha:0.62], NSForegroundColorAttributeName,
      ps, NSParagraphStyleAttributeName, nil];
  [text drawInRect:NSOffsetRect(rect, 0, -1) withAttributes:shade];

  NSDictionary *lip = [NSDictionary dictionaryWithObjectsAndKeys:
      font, NSFontAttributeName,
      [NSColor colorWithCalibratedWhite:1.0 alpha:0.16], NSForegroundColorAttributeName,
      ps, NSParagraphStyleAttributeName, nil];
  [text drawInRect:NSOffsetRect(rect, 0, 1) withAttributes:lip];

  NSDictionary *face = [NSDictionary dictionaryWithObjectsAndKeys:
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
  if ((self = [super initWithFrame:f])) {
    ruled = YES;
    rulePitch = 16.0;
    rulePhase = 8.0;
    marginX = 52.0;
  }

  return self;
}

- (void)setRuled:(BOOL)r { ruled = r; [self setNeedsDisplay:YES]; }

- (void)setRulePitch:(CGFloat)pitch phase:(CGFloat)phase {
  if (pitch >= 4.0) { rulePitch = pitch; }
  rulePhase = phase;
  [self setNeedsDisplay:YES];
}

- (void)setMarginX:(CGFloat)x {
  marginX = x;
  [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty {
  (void)dirty;
  NSRect b = [self bounds];

  [[PPSkin paperColor] set];
  NSRectFill(b);

  if (ruled) {
    // Ruled downward from the top, because that is where the text starts and
    // the rules have to agree with it: phase is the drop from the top of the
    // view to the first line, pitch is one line of body text. Rules on a
    // half-pixel so they stay hairlines instead of blurring across two rows.
    [[PPSkin ruleColor] set];
    for (CGFloat d = rulePhase; d < NSHeight(b); d += rulePitch) {
      CGFloat y = floor(NSMaxY(b) - d) + 0.5;
      if (y < NSMinY(b)) break;

      NSRectFillUsingOperation(NSMakeRect(0, y, NSWidth(b), 1),
                               NSCompositeSourceOver);
    }

    if (marginX > 0.0) {
      [[PPSkin marginRuleColor] set];
      NSRectFillUsingOperation(NSMakeRect(marginX, 0, 1, NSHeight(b)),
                               NSCompositeSourceOver);
    }
  }

  [PPSkin drawVignetteInRect:b strength:0.30];
}

- (BOOL)isOpaque { return YES; }

@end
