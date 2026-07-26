// Skin.h -- the skeuomorphic surfaces.
//
// Leopard's visual language is physical: things are made of materials, lit from
// above, and stacked with real depth. This draws that vocabulary -- tooled
// leather, aged paper, brushed metal, stitching, bevels -- so the interface
// reads as an object rather than a diagram of one.
//
// Every texture is generated procedurally rather than shipped as an image.
// Three reasons: nothing has to be licensed or bundled, a tile can be produced
// at exactly the size wanted, and on this hardware a 128-pixel tile computed
// once at launch and then tiled by the window server is far cheaper than
// decompressing and blitting photographs.
#pragma once

#import <Cocoa/Cocoa.h>

@interface PPSkin : NSObject

// Cached tiles. Generated on first use, then reused for the process lifetime.
+ (NSColor *)leatherColor;        // deep oxblood, tooled grain
+ (NSColor *)darkLeatherColor;    // the sidebar, a shade deeper
+ (NSColor *)paperColor;          // aged cream writing paper
+ (NSColor *)metalColor;          // brushed aluminium

+ (NSImage *)leatherTile;
+ (NSImage *)paperTile;

// Ink colours that sit correctly on the paper.
+ (NSColor *)inkColor;            // near-black with a hint of blue
+ (NSColor *)fadedInkColor;
+ (NSColor *)redInkColor;
+ (NSColor *)marginRuleColor;     // the red margin line
+ (NSColor *)ruleColor;           // faint horizontal ruling

// Drawing helpers. All assume the current context is flipped the way an
// NSView's drawRect: gives it.

// A double row of saddle stitching inset from the edge of `rect`.
+ (void)drawStitchingInRect:(NSRect)rect inset:(CGFloat)inset;

// A recessed well: dark inner shadow at the top, light catch at the bottom.
// This is what makes a text area look pressed into the leather.
+ (void)drawRecessedWellInRect:(NSRect)rect radius:(CGFloat)radius;

// A raised panel: light top edge, shadow beneath.
+ (void)drawRaisedPanelInRect:(NSRect)rect radius:(CGFloat)radius;

// A soft vignette, darkening the edges the way a lit surface falls off.
+ (void)drawVignetteInRect:(NSRect)rect strength:(CGFloat)strength;

// The gold-blocked title lettering used on a spine or cover.
+ (void)drawEmbossedText:(NSString *)text
                  inRect:(NSRect)rect
                    font:(NSFont *)font
                   color:(NSColor *)color;

// A rounded-rectangle path, since 10.5 has no bezierPathWithRoundedRect:.
+ (NSBezierPath *)roundedRectPath:(NSRect)rect radius:(CGFloat)radius;

@end

// A view that fills itself with leather and stitches its edges.
@interface PPLeatherView : NSView {
    BOOL dark;
    BOOL stitched;
}
- (void)setDark:(BOOL)d;
- (void)setStitched:(BOOL)s;
@end

// A view that fills itself with ruled writing paper.
@interface PPPaperView : NSView {
    BOOL ruled;
}
- (void)setRuled:(BOOL)r;
@end
