// main.mm -- ppcode's Cocoa front end.
//
// Built the way a Leopard-era application should be: a unified titlebar-and-
// toolbar window, a source list on the left, a split view, sheets rather than
// modal dialogs, and drag-and-drop into the composer. No flat design -- the
// stock Aqua controls already carry the correct look, and hand-drawn
// replacements would be both more work and visibly wrong for this platform.
//
// The interface is built in code rather than a nib because the whole thing has
// to be constructible from a headless build; there is no Interface Builder in
// the loop.
#import <Cocoa/Cocoa.h>

#import "GuiBridge.h"
#import "Markdown.h"
#import "Settings.h"
#import "Skin.h"

#include "mdparse.hpp"

#include <string>
#include <string.h>

// ---------------------------------------------------------------------------
// A text view written on paper.
//
// The paper is drawn by the text view rather than by a view behind it, for two
// reasons. The rules have to line up with the writing, and the text view is the
// only thing that knows where its lines are -- a backdrop cannot follow, because
// the text scrolls over it and the alignment would survive exactly one
// scrollwheel click. And anchoring the pattern phase to the view rather than the
// window is what stops the tile seams reappearing as the view scrolls.
// ---------------------------------------------------------------------------

@interface PPPaperTextView : NSTextView {
  BOOL ruled;
}

- (void)setRuled:(BOOL)r;

@end

@implementation PPPaperTextView

- (id)initWithFrame:(NSRect)f {
  if ((self = [super initWithFrame:f])) { ruled = NO; }

  return self;
}

- (void)setRuled:(BOOL)r {
  ruled = r;
  [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty {
  NSRect b = [self bounds];

  // Pattern phase in window coordinates, so the tile is pinned to this view and
  // scrolling does not shuffle it.
  [[NSGraphicsContext currentContext]
      setPatternPhase:[self convertPoint:NSZeroPoint toView:nil]];
  [[PPSkin paperColor] set];
  NSRectFill(dirty);

  if (ruled) {
    // One rule per line of body text, placed just under the baseline so the
    // writing sits on the line. Ruling on the line *height* instead put the
    // rule through the middle of the letters. A text view is flipped, so this
    // counts downward from the top inset.
    CGFloat pitch = PPMarkdownLineHeight();
    CGFloat top = [self textContainerInset].height + PPMarkdownBaselineOffset() + 1.0;

    [[PPSkin ruleColor] set];
    for (CGFloat y = top; y < NSMaxY(b); y += pitch) {
      NSRect line = NSMakeRect(NSMinX(b), floor(y) + 0.5, NSWidth(b), 1.0);
      if (NSIntersectsRect(line, dirty)) {
        NSRectFillUsingOperation(line, NSCompositeSourceOver);
      }
    }

    // The margin sits just left of where the text begins.
    CGFloat mx = floor([self textContainerInset].width - 10.0) + 0.5;
    NSRect margin = NSMakeRect(mx, NSMinY(b), 1.0, NSHeight(b));
    if (mx > NSMinX(b) && NSIntersectsRect(margin, dirty)) {
      [[PPSkin marginRuleColor] set];
      NSRectFillUsingOperation(margin, NSCompositeSourceOver);
    }
  }

  [super drawRect:dirty];
}

- (BOOL)isOpaque { return YES; }

@end

// ---------------------------------------------------------------------------
// A text view that accepts dropped files and images.
// ---------------------------------------------------------------------------

@interface PPComposer : PPPaperTextView {
  NSMutableArray *attachments;
  id dropTarget;
}
- (NSArray *)attachments;
- (void)clearAttachments;
- (void)removeAttachmentAtIndex:(NSUInteger)i;
- (void)addAttachment:(NSString *)path;
- (void)setDropTarget:(id)t;
@end

@implementation PPComposer

- (id)initWithFrame:(NSRect)frame {
  if ((self = [super initWithFrame:frame])) {
    attachments = [[NSMutableArray alloc] init];
    [self registerForDraggedTypes:
        [NSArray arrayWithObjects:NSFilenamesPboardType, NSTIFFPboardType, nil]];
  }

  return self;
}

- (void)dealloc { [attachments release]; [super dealloc]; }

- (NSArray *)attachments { return attachments; }
- (void)clearAttachments { [attachments removeAllObjects]; }

- (void)removeAttachmentAtIndex:(NSUInteger)i {
  if (i < [attachments count]) { [attachments removeObjectAtIndex:i]; }
}

// Only --shot uses this; a real attachment arrives by drag and drop.
- (void)addAttachment:(NSString *)path { [attachments addObject:path]; }

- (void)setDropTarget:(id)t { dropTarget = t; }

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
  NSPasteboard *pb = [sender draggingPasteboard];
  if ([[pb types] containsObject:NSFilenamesPboardType]) return NSDragOperationCopy;
  if ([[pb types] containsObject:NSTIFFPboardType]) return NSDragOperationCopy;

  return NSDragOperationNone;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
  NSPasteboard *pb = [sender draggingPasteboard];

  if ([[pb types] containsObject:NSFilenamesPboardType]) {
    NSArray *files = [pb propertyListForType:NSFilenamesPboardType];
    NSEnumerator *fe = [files objectEnumerator];
    NSString *f;
    while ((f = [fe nextObject]) != nil) [attachments addObject:f];
    [dropTarget performSelector:@selector(attachmentsChanged)];

    return YES;
  }
  // An image dragged from another application arrives as data rather than a
  // path, so write it somewhere the engine can read.
  if ([[pb types] containsObject:NSTIFFPboardType]) {
    NSData *tiff = [pb dataForType:NSTIFFPboardType];
    NSBitmapImageRep *rep = [NSBitmapImageRep imageRepWithData:tiff];
    NSData *png = [rep representationUsingType:NSPNGFileType properties:nil];
    long stamp = (long)[NSDate timeIntervalSinceReferenceDate];
    NSString *path =
        [NSString stringWithFormat:@"/tmp/ppcode-drop-%ld.png", stamp];
    if ([png writeToFile:path atomically:YES]) {
      [attachments addObject:path];
      [dropTarget performSelector:@selector(attachmentsChanged)];

      return YES;
    }
  }

  return NO;
}

// Return sends; Option-Return inserts a newline, matching the terminal front end.
- (void)keyDown:(NSEvent *)event {
  if ([[event charactersIgnoringModifiers] isEqualToString:@"\r"]) {
    if ([event modifierFlags] & NSAlternateKeyMask) {
      [self insertNewline:self];

      return;
    }
    [dropTarget performSelector:@selector(send:) withObject:self];

    return;
  }
  [super keyDown:event];
}

@end

// ---------------------------------------------------------------------------
// Skin containers
//
// Skin.mm draws the materials; these two put them where the interface needs
// them. Both are here rather than in Skin.mm because they are about this
// window's arrangement, not about the surfaces themselves.
// ---------------------------------------------------------------------------

// Holds a scroll view pressed into the leather. The content is inset so the
// well's shadow is visible around it.
@interface PPWellView : NSView
@end

@implementation PPWellView

- (void)drawRect:(NSRect)dirty {
  (void)dirty;
  [PPSkin drawRecessedWellInRect:[self bounds] radius:5.0];
}

@end

// Put `scroll` in a well, over a sheet of paper.
//
// The paper is a view behind the scroll view rather than the text view's own
// background colour. A pattern NSColor on a scrolling NSTextView is drawn with
// whatever pattern phase each partial redraw happens to have, so the tile seams
// show up as a grid of rectangles across the page. A view draws the tile
// against its own bounds every time, so it is seamless -- and the paper then
// stays put while the text moves over it, which is the right way round.
static PPWellView *WrapInPaperWell(NSScrollView *scroll, NSRect frame,
                                   unsigned autoresize, BOOL ruled) {
  PPWellView *well = [[[PPWellView alloc] initWithFrame:frame] autorelease];
  [well setAutoresizingMask:autoresize];

  NSRect inner = NSInsetRect([well bounds], 5, 5);

  PPPaperView *paper = [[[PPPaperView alloc] initWithFrame:inner] autorelease];
  [paper setRuled:ruled];
  [paper setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
  [well addSubview:paper];

  [scroll setFrame:inner];
  [scroll setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
  [scroll setDrawsBackground:NO];
  [well addSubview:scroll];

  return well;
}

// A row of attachment tokens, each removable.
//
// This replaces a label that said "2 attachments", which told you the count and
// nothing else: not which files, and not how to drop one you added by mistake
// short of clearing the composer.
@interface PPTokenRow : NSView {
  id target;
}

- (void)setPaths:(NSArray *)paths target:(id)t action:(SEL)action;

@end

@implementation PPTokenRow

// Rebuilt rather than diffed. A handful of tokens on a machine this size is not
// worth the bookkeeping of keeping views in sync.
- (void)setPaths:(NSArray *)paths target:(id)t action:(SEL)action {
  target = t;

  NSArray *old = [[[self subviews] copy] autorelease];
  NSEnumerator *oe = [old objectEnumerator];
  NSView *v;
  while ((v = [oe nextObject]) != nil) [v removeFromSuperview];

  CGFloat x = 0.0;
  for (NSUInteger i = 0; i < [paths count]; i++) {
    NSString *path = [paths objectAtIndex:i];
    NSString *name = [path lastPathComponent];
    if ([name length] > 22) {
      name = [[name substringToIndex:20] stringByAppendingString:PPUTF8("\xE2\x80\xA6")];
    }

    // The multiplication sign, not a letter x: it is the right glyph and it is
    // in every font this platform ships.
    NSString *title =
        [name stringByAppendingString:PPUTF8("  \xC3\x97")];

    NSButton *b = [[[NSButton alloc]
        initWithFrame:NSMakeRect(x, 1, 0, 20)] autorelease];
    [b setTitle:title];
    [b setBezelStyle:NSRecessedBezelStyle];
    [b setFont:[NSFont systemFontOfSize:10.0]];
    [b setTag:(NSInteger)i];
    [b setTarget:target];
    [b setAction:action];
    [b setToolTip:[path stringByAppendingString:@"  (click to remove)"]];
    [b sizeToFit];

    NSRect f = [b frame];
    f.origin.x = x;
    f.origin.y = 1;
    f.size.height = 20;
    [b setFrame:f];
    [self addSubview:b];

    x += f.size.width + 5.0;
  }
}

@end

// The header band: leather with the name blocked into it, the way a title is
// stamped on a cover.
@interface PPTitleView : PPLeatherView
@end

@implementation PPTitleView

- (void)drawRect:(NSRect)dirty {
  [super drawRect:dirty];

  NSRect b = [self bounds];
  NSFont *font = [NSFont fontWithName:@"Baskerville" size:15.0];
  if (!font) { font = [NSFont boldSystemFontOfSize:14.0]; }

  NSRect text = NSMakeRect(0, (NSHeight(b) - 18.0) / 2.0, NSWidth(b), 18.0);
  [PPSkin drawEmbossedText:@"ppcode"
                    inRect:text
                      font:font
                     color:[NSColor colorWithCalibratedRed:0.83
                                                     green:0.71
                                                      blue:0.44
                                                     alpha:1.0]];
}

@end

// ---------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------

// No protocol conformance list: NSTableViewDataSource and NSTableViewDelegate
// are informal protocols on 10.5 (they were only formalised in 10.6), so
// declaring conformance does not compile here.
@interface PPController : NSObject {
  NSWindow *window;
  NSTextView *transcript;
  PPComposer *composer;
  NSTableView *sessionTable;
  NSArray *sessions;
  NSTextField *statusField;
  // The attachment strip, and the pieces whose frames it pushes around when it
  // appears and disappears.
  PPTokenRow *attachRow;
  NSScrollView *compScroll;
  NSView *compWell;
  NSPopUpButton *modelPopup;
  NSProgressIndicator *spinner;
  NSButton *sendButton;
  PPBridge *bridge;
  PPSettingsController *settings;
  BOOL streaming;

  // Streaming markdown. mdBuffer holds the part of the reply whose block has
  // not closed yet; previewStart is where its cheap plain rendering begins in
  // the text storage. See -streamDelta:.
  NSMutableString *mdBuffer;
  NSUInteger previewStart;
}
- (void)send:(id)sender;
- (void)attachmentsChanged;
- (void)removeAttachment:(id)sender;
- (void)showSettings:(id)sender;
- (void)newConversation:(id)sender;
- (void)clearTranscript;
- (void)flushStream;
- (void)buildWindow;
- (void)populateModels;
- (void)reloadSessions;
@end

@implementation PPController

// --- transcript helpers ----------------------------------------------------

- (void)clearTranscript {
  [mdBuffer release];
  mdBuffer = nil;
  previewStart = 0;
  [transcript setString:@""];
}

- (void)appendAttributed:(NSAttributedString *)as {
  if (!as || [as length] == 0) { return; }

  [[transcript textStorage] appendAttributedString:as];
  [transcript scrollRangeToVisible:NSMakeRange([[transcript string] length], 0)];
}

// Chrome: tool lines, errors, the banner. Not markdown, because none of it
// comes from the model.
- (void)append:(NSString *)text
         color:(NSColor *)color
          bold:(BOOL)bold
          mono:(BOOL)mono {
  if (!text) { return; }

  NSFont *font = mono
      ? [NSFont fontWithName:@"Monaco" size:11.0]
      : (bold ? [NSFont boldSystemFontOfSize:12.0] : [NSFont systemFontOfSize:12.0]);
  if (!font) { font = [NSFont systemFontOfSize:12.0]; }

  NSDictionary *attrs = [NSDictionary dictionaryWithObjectsAndKeys:
                            font, NSFontAttributeName,
                            color ? color : [NSColor blackColor],
                                NSForegroundColorAttributeName,
                            nil];

  [self appendAttributed:[[[NSAttributedString alloc] initWithString:text
                                                         attributes:attrs]
                             autorelease]];
}

- (void)appendPlain:(NSString *)t {
  [self append:t color:nil bold:NO mono:NO];
}

// A complete piece of markdown, rendered in one go. Used for replayed sessions
// and anywhere the whole text is already in hand.
- (void)appendMarkdown:(NSString *)text {
  if (!text) { return; }

  const char *utf8 = [text UTF8String];
  [self appendAttributed:PPAttributedFromMarkdown(utf8 ? std::string(utf8)
                                                       : std::string())];
}

// --- streaming markdown ----------------------------------------------------
//
// Re-rendering the whole reply on every delta would have a G5 re-highlighting
// the same code block hundreds of times. Instead the tail whose block has not
// closed yet is shown as plain text -- which is a pure append, so it costs
// nothing -- and only when md::complete_prefix() reports a closed block is that
// prefix replaced with its real rendering. Each block is therefore laid out
// exactly once.

- (void)replacePreviewWith:(NSAttributedString *)as {
  NSTextStorage *store = [transcript textStorage];
  if (previewStart > [store length]) { previewStart = [store length]; }

  NSRange preview = NSMakeRange(previewStart, [store length] - previewStart);
  [store deleteCharactersInRange:preview];

  if (as && [as length]) { [store appendAttributedString:as]; }
}

- (void)streamDelta:(NSString *)delta {
  if (!delta || [delta length] == 0) { return; }

  if (!mdBuffer) {
    mdBuffer = [[NSMutableString alloc] init];
    previewStart = [[transcript textStorage] length];
  }

  [mdBuffer appendString:delta];

  // The cheap part: extend the plain preview by exactly what arrived.
  [self appendAttributed:
      [[[NSAttributedString alloc] initWithString:delta
                                       attributes:PPMarkdownStreamAttributes()]
          autorelease]];

  const char *utf8 = [mdBuffer UTF8String];
  if (!utf8) { return; }

  std::string buffer(utf8);
  size_t done = ppcode::md::complete_prefix(buffer);
  if (done == 0) { return; }

  // One or more blocks closed: render them for real and keep the remainder as
  // preview.
  std::string settled = buffer.substr(0, done);
  std::string rest = buffer.substr(done);

  NSMutableAttributedString *replacement =
      [[[NSMutableAttributedString alloc] init] autorelease];
  [replacement appendAttributedString:PPAttributedFromMarkdown(settled)];
  NSUInteger settledLength = [replacement length];

  if (!rest.empty()) {
    NSString *tail = [NSString stringWithUTF8String:rest.c_str()];
    if (tail) {
      [replacement appendAttributedString:
          [[[NSAttributedString alloc] initWithString:tail
                                           attributes:PPMarkdownStreamAttributes()]
              autorelease]];
    }
  }

  [self replacePreviewWith:replacement];
  previewStart += settledLength;

  [mdBuffer setString:(rest.empty() ? @""
                                    : [NSString stringWithUTF8String:rest.c_str()])];
  [transcript scrollRangeToVisible:NSMakeRange([[transcript string] length], 0)];
}

// Render whatever is left and stop streaming. Must run before any chrome is
// written, or the tool output would land inside the buffered markdown.
- (void)flushStream {
  streaming = NO;
  if (!mdBuffer) { return; }

  if ([mdBuffer length]) {
    const char *utf8 = [mdBuffer UTF8String];
    [self replacePreviewWith:
        PPAttributedFromMarkdown(utf8 ? std::string(utf8) : std::string())];
  }

  [mdBuffer release];
  mdBuffer = nil;
  previewStart = [[transcript textStorage] length];
  [transcript scrollRangeToVisible:NSMakeRange([[transcript string] length], 0)];
}

// --- lifecycle -------------------------------------------------------------

- (void)buildWindow {
  NSRect frame = NSMakeRect(0, 0, 940, 640);
  unsigned mask = NSTitledWindowMask | NSClosableWindowMask |
                  NSMiniaturizableWindowMask | NSResizableWindowMask |
                  NSUnifiedTitleAndToolbarWindowMask;

  window = [[NSWindow alloc] initWithContentRect:frame
                                       styleMask:mask
                                         backing:NSBackingStoreBuffered
                                           defer:NO];
  [window setTitle:@"ppcode"];
  [window setFrameAutosaveName:@"PPCodeMainWindow"];   // position persists free
  [window setMinSize:NSMakeSize(640, 400)];

  // The whole window is a leather surface; the content areas are paper laid
  // on top of it. Keeping the text on paper rather than tinting it onto the
  // hide is what keeps it legible -- dark type on dark leather is the usual
  // way this style goes wrong.
  PPLeatherView *content =
      [[[PPLeatherView alloc] initWithFrame:frame] autorelease];
  [content setStitched:YES];
  [window setContentView:content];

  // Aqua metrics: a 20-point window margin, 12 between groups, 8 between
  // related controls. Beyond the HIG, the margin is what the leather is *for* --
  // with the content running to the edges there was nowhere for the stitching to
  // sit, and it was drawn underneath the panels where nobody could see it.
  const CGFloat kMargin = 18.0;   // leather visible around the content
  const CGFloat kBarH = 38.0;     // bottom control strip
  const CGFloat kHeaderH = 34.0;
  const CGFloat kGap = 10.0;

  CGFloat contentTop = 640 - kHeaderH;
  CGFloat splitBottom = kBarH + kGap;
  CGFloat splitHeight = contentTop - kGap - splitBottom;
  CGFloat splitWidth = 940 - 2 * kMargin;

  // --- header band ---------------------------------------------------------
  PPTitleView *header =
      [[[PPTitleView alloc]
          initWithFrame:NSMakeRect(0, contentTop, 940, kHeaderH)] autorelease];
  [header setStitched:NO];
  [header setDark:YES];
  [header setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
  [content addSubview:header];

  // --- left: session list -------------------------------------------------
  NSScrollView *listScroll =
      [[[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 220, 580)] autorelease];
  [listScroll setHasVerticalScroller:YES];
  [listScroll setAutoresizingMask:NSViewHeightSizable];
  [listScroll setBorderType:NSNoBorder];
  [listScroll setDrawsBackground:NO];

  sessionTable = [[[NSTableView alloc] initWithFrame:NSMakeRect(0, 0, 200, 580)]
                     autorelease];
  // Transparent so the paper behind the scroll view shows through. The
  // alternating row tint has to go with it: it paints opaque bands that would
  // hide the texture entirely.
  [sessionTable setBackgroundColor:[NSColor clearColor]];
  [sessionTable setGridStyleMask:NSTableViewGridNone];
  NSTableColumn *col =
      [[[NSTableColumn alloc] initWithIdentifier:@"title"] autorelease];
  [[col headerCell] setStringValue:@"Conversations"];
  [col setWidth:196];
  [col setResizingMask:NSTableColumnAutoresizingMask];
  [sessionTable addTableColumn:col];
  [sessionTable setColumnAutoresizingStyle:NSTableViewUniformColumnAutoresizingStyle];
  [sessionTable setDataSource:self];
  [sessionTable setDelegate:self];
  [sessionTable setUsesAlternatingRowBackgroundColors:NO];
  // Two lines of type need room for two lines of type. At 32 the second line
  // was being cut through the middle.
  [sessionTable setRowHeight:40.0];
  [sessionTable setIntercellSpacing:NSMakeSize(3.0, 4.0)];
  [listScroll setDocumentView:sessionTable];

  // --- right: transcript over composer ------------------------------------
  // The transcript is the page: aged paper, ruled, with the type sitting on
  // it. NSTextView draws its own background, so the paper pattern goes there
  // rather than into a view behind it -- one less layer to keep in sync when
  // the text scrolls.
  NSScrollView *transScroll =
      [[[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 700, 430)] autorelease];
  [transScroll setHasVerticalScroller:YES];
  [transScroll setBorderType:NSNoBorder];
  [transScroll setDrawsBackground:NO];
  [transScroll setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

  transcript = [[[PPPaperTextView alloc] initWithFrame:NSMakeRect(0, 0, 700, 430)]
                   autorelease];
  [(PPPaperTextView *)transcript setRuled:YES];
  [transcript setEditable:NO];
  [transcript setRichText:YES];
  [transcript setAutoresizingMask:NSViewWidthSizable];
  // Clear of the paper's red margin rule, which PPPaperView draws at x = 52.
  // Text running across the margin is the one thing ruled paper must not do.
  [transcript setTextContainerInset:NSMakeSize(58, 12)];
  [transcript setDrawsBackground:NO];
  [transScroll setDocumentView:transcript];

  PPWellView *transWell =
      WrapInPaperWell(transScroll, NSMakeRect(0, 0, 720, 440),
                      NSViewWidthSizable | NSViewHeightSizable, NO);

  compScroll =
      [[[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 700, 110)] autorelease];
  [compScroll setHasVerticalScroller:YES];
  [compScroll setBorderType:NSNoBorder];
  [compScroll setDrawsBackground:NO];
  [compScroll setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

  composer = [[[PPComposer alloc] initWithFrame:NSMakeRect(0, 0, 700, 110)]
                 autorelease];
  [composer setFont:[NSFont systemFontOfSize:12.0]];
  [composer setDropTarget:self];
  [composer setAutoresizingMask:NSViewWidthSizable];
  [composer setDrawsBackground:NO];
  [composer setTextContainerInset:NSMakeSize(6, 6)];
  [compScroll setDocumentView:composer];

  // The composer sits pressed into the leather, which is the affordance that
  // says "write here". Unruled: it is a note pad, not a page.
  compWell = WrapInPaperWell(compScroll, NSMakeRect(0, 0, 720, 120),
                             NSViewWidthSizable | NSViewHeightSizable, NO);

  // Sits along the top of the composer's paper. Zero height until something is
  // attached, so it costs nothing when unused; -attachmentsChanged does the
  // arithmetic.
  attachRow = [[[PPTokenRow alloc] initWithFrame:NSZeroRect] autorelease];
  [attachRow setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
  [compWell addSubview:attachRow];

  NSSplitView *rightSplit =
      [[[NSSplitView alloc] initWithFrame:NSMakeRect(220, 30, 720, 580)] autorelease];
  [rightSplit setVertical:NO];
  [rightSplit setDividerStyle:NSSplitViewDividerStyleThin];
  [rightSplit addSubview:transWell];
  [rightSplit addSubview:compWell];
  [rightSplit setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

  NSSplitView *mainSplit = [[[NSSplitView alloc]
      initWithFrame:NSMakeRect(kMargin, splitBottom, splitWidth, splitHeight)]
                                autorelease];
  [mainSplit setVertical:YES];
  [mainSplit setDividerStyle:NSSplitViewDividerStyleThin];

  PPWellView *listWell =
      WrapInPaperWell(listScroll, NSMakeRect(0, 0, 220, 580),
                      NSViewHeightSizable, NO);

  [mainSplit addSubview:listWell];
  [mainSplit addSubview:rightSplit];
  [mainSplit setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
  [content addSubview:mainSplit];

  // --- bottom bar ---------------------------------------------------------
  statusField = [[[NSTextField alloc]
      initWithFrame:NSMakeRect(kMargin, (kBarH - 18) / 2, 560, 18)]
                    autorelease];
  [statusField setBezeled:NO];
  [statusField setDrawsBackground:NO];
  [statusField setEditable:NO];
  [statusField setSelectable:NO];
  [statusField setFont:[NSFont systemFontOfSize:11.0]];
  // Light type: this row sits directly on the leather, not on paper.
  [statusField setTextColor:[NSColor colorWithCalibratedWhite:0.86 alpha:1.0]];
  [statusField setStringValue:@"Ready"];
  [content addSubview:statusField];

  // The attachment count used to be a label here. It is a row of removable
  // tokens above the composer now, which is both more informative and the only
  // way to drop one file without clearing the lot.

  // Laid out from the right edge inward at HIG spacing: 20 from the window,
  // then 8 between related controls.
  CGFloat right = 940 - kMargin;
  CGFloat sendW = 85, popupW = 190, ctlH = 26;
  CGFloat ctlY = (kBarH - ctlH) / 2;

  sendButton = [[[NSButton alloc]
      initWithFrame:NSMakeRect(right - sendW, ctlY, sendW, ctlH)] autorelease];
  [sendButton setTitle:@"Send"];
  [sendButton setBezelStyle:NSRoundedBezelStyle];   // the glossy Aqua capsule
  [sendButton setKeyEquivalent:@"\r"];              // default button, pulses
  [sendButton setTarget:self];
  [sendButton setAction:@selector(send:)];
  [sendButton setAutoresizingMask:NSViewMinXMargin];
  [content addSubview:sendButton];

  modelPopup = [[[NSPopUpButton alloc]
      initWithFrame:NSMakeRect(right - sendW - 8 - popupW, ctlY, popupW, ctlH)]
                   autorelease];
  [modelPopup setAutoresizingMask:NSViewMinXMargin];
  [modelPopup setTarget:self];
  [modelPopup setAction:@selector(modelChanged:)];
  [content addSubview:modelPopup];

  spinner = [[[NSProgressIndicator alloc]
      initWithFrame:NSMakeRect(right - sendW - 8 - popupW - 8 - 16,
                               (kBarH - 16) / 2, 16, 16)] autorelease];
  [spinner setStyle:NSProgressIndicatorSpinningStyle];
  [spinner setControlSize:NSSmallControlSize];
  [spinner setDisplayedWhenStopped:NO];
  [spinner setAutoresizingMask:NSViewMinXMargin];
  [content addSubview:spinner];

  [window center];
  [window makeKeyAndOrderFront:nil];
  [window makeFirstResponder:composer];
}

- (void)populateModels {
  [modelPopup removeAllItems];
  NSArray *favs = [bridge favouriteModelIds];
  NSEnumerator *fe = [favs objectEnumerator];
  NSString *f;
  while ((f = [fe nextObject]) != nil) [modelPopup addItemWithTitle:f];
  [[modelPopup menu] addItem:[NSMenuItem separatorItem]];

  NSArray *all = [bridge availableModels];
  NSEnumerator *me = [all objectEnumerator];
  NSDictionary *m;
  int added = 0;
  while ((m = [me nextObject]) != nil) {
    NSString *mid = [m objectForKey:@"id"];
    if ([favs containsObject:mid]) continue;
    if (![[m objectForKey:@"tools"] boolValue]) continue;   // useless here
    [modelPopup addItemWithTitle:mid];
    if (++added >= 120) break;
  }
  NSString *cur = [bridge modelId];
  if ([modelPopup itemWithTitle:cur]) [modelPopup selectItemWithTitle:cur];
}

- (void)applicationDidFinishLaunching:(NSNotification *)note {
  bridge = [[PPBridge alloc] init];
  [bridge setDelegate:self];

  [self buildWindow];
  [self populateModels];
  [self reloadSessions];

  [self append:@"ppcode\n" color:[NSColor colorWithCalibratedRed:0.1
                                                           green:0.3
                                                            blue:0.6
                                                           alpha:1.0]
          bold:YES mono:NO];
  [self appendPlain:[NSString stringWithFormat:
      @"Working in %@\nModel: %@%@\n\nDrag files or images into the composer to "
       "attach them. Return sends, Option-Return inserts a newline.\n\n",
      [bridge workingDirectory], [bridge modelId],
      [bridge modelSupportsImages] ? @" (can see images)" : @" (text only)"]];

  // Without a key nothing works, and an application launched from the Finder
  // does not inherit a key exported in a shell profile. Say so plainly and
  // open the place where it is set.
  if (![bridge hasApiKey]) {
    [self append:@"No OpenRouter key is configured.\n"
           color:[NSColor redColor] bold:YES mono:NO];
    [self appendPlain:
        @"An application launched from the Finder does not inherit your shell "
         "environment, so a key exported in .zshrc is invisible here. Set one "
         "in Settings (Command-comma); it is stored in your config file with "
         "owner-only permissions.\n\n"];
    [self showSettings:nil];
  }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app {
  return YES;
}

// --- actions ---------------------------------------------------------------

// Lay the token strip along the top of the composer's paper and give the
// composer whatever is left. The strip takes no height at all when nothing is
// attached, so the composer is full size in the usual case.
- (void)attachmentsChanged {
  NSArray *a = [composer attachments];
  [attachRow setPaths:a target:self action:@selector(removeAttachment:)];

  NSRect inner = NSInsetRect([compWell bounds], 5, 5);
  CGFloat rowHeight = [a count] ? 24.0 : 0.0;

  [attachRow setHidden:([a count] == 0)];
  [attachRow setFrame:NSMakeRect(inner.origin.x,
                                 NSMaxY(inner) - rowHeight,
                                 inner.size.width, rowHeight)];
  [compScroll setFrame:NSMakeRect(inner.origin.x, inner.origin.y,
                                  inner.size.width,
                                  inner.size.height - rowHeight)];
  [compWell setNeedsDisplay:YES];
}

- (void)removeAttachment:(id)sender {
  [composer removeAttachmentAtIndex:(NSUInteger)[sender tag]];
  [self attachmentsChanged];
}

- (void)modelChanged:(id)sender {
  NSString *mid = [modelPopup titleOfSelectedItem];
  if (!mid) return;

  // Mid-turn the worker thread is reading the config and the system prompt, so
  // the change is refused rather than raced. Put the popup back where it was.
  if ([bridge isBusy]) {
    [modelPopup selectItemWithTitle:[bridge modelId]];
    [statusField setStringValue:@"Finish the current turn before changing model"];

    return;
  }

  [bridge setModelId:mid];
  [self appendPlain:[NSString stringWithFormat:@"\n[model: %@%@]\n\n", mid,
                        [bridge modelSupportsImages] ? @", vision" : @""]];
}

- (void)send:(id)sender {
  NSString *text = [[composer string] copy];
  if ([[text stringByTrimmingCharactersInSet:
           [NSCharacterSet whitespaceAndNewlineCharacterSet]] length] == 0) {
    [text release];

    return;
  }
  NSArray *files = [[[composer attachments] copy] autorelease];

  if ([bridge isBusy]) {
    [bridge steer:text];
    [self append:@"\n[queued: " color:[NSColor grayColor] bold:NO mono:NO];
    [self append:text color:[NSColor grayColor] bold:NO mono:NO];
    [self append:@"]\n" color:[NSColor grayColor] bold:NO mono:NO];
  }

  else {
    [self append:@"\nYou\n" color:[NSColor colorWithCalibratedRed:0.0
                                                            green:0.35
                                                             blue:0.55
                                                            alpha:1.0]
            bold:YES mono:NO];
    [self appendPlain:text];
    [self appendPlain:@"\n"];
    NSEnumerator *ae = [files objectEnumerator];
    NSString *af;
    while ((af = [ae nextObject]) != nil)
        [self append:[NSString stringWithFormat:@"   attached: %@\n",
                         [af lastPathComponent]]
               color:[NSColor grayColor] bold:NO mono:NO];
    [self appendPlain:@"\n"];
    [bridge sendMessage:text attachments:files];
  }

  [composer setString:@""];
  [composer clearAttachments];
  [self attachmentsChanged];
  [text release];
}

- (void)showSettings:(id)sender {
  if (!settings) {
    settings = [[PPSettingsController alloc] initWithBridge:bridge];
  }

  [settings showWindow];
}

- (void)newConversation:(id)sender {
  if ([bridge isBusy]) return;
  [bridge newConversation];
  [self clearTranscript];
  [self appendPlain:@"New conversation.\n\n"];
  [self reloadSessions];
}

- (void)reloadSessions {
  [sessions release];
  sessions = [[bridge savedSessions] retain];
  [sessionTable reloadData];
}

// --- table -----------------------------------------------------------------

- (NSInteger)numberOfRowsInTableView:(NSTableView *)t {
  return sessions ? (NSInteger)[sessions count] : 0;
}

- (id)tableView:(NSTableView *)t
    objectValueForTableColumn:(NSTableColumn *)c
                          row:(NSInteger)row {
  NSDictionary *s = [sessions objectAtIndex:(NSUInteger)row];

  // Two lines with a clear hierarchy: the title in ink, the age and count
  // smaller and faded beneath it. As one plain string in one font the two ran
  // together and the list was hard to read at a glance.
  NSMutableParagraphStyle *p =
      [[[NSMutableParagraphStyle alloc] init] autorelease];
  [p setLineBreakMode:NSLineBreakByTruncatingTail];

  NSDictionary *titleAttrs = [NSDictionary dictionaryWithObjectsAndKeys:
      [NSFont boldSystemFontOfSize:11.0], NSFontAttributeName,
      [PPSkin inkColor], NSForegroundColorAttributeName,
      p, NSParagraphStyleAttributeName, nil];
  NSDictionary *metaAttrs = [NSDictionary dictionaryWithObjectsAndKeys:
      [NSFont systemFontOfSize:9.0], NSFontAttributeName,
      [PPSkin fadedInkColor], NSForegroundColorAttributeName,
      p, NSParagraphStyleAttributeName, nil];

  NSString *title = [s objectForKey:@"title"];
  if (![title length]) { title = @"(untitled)"; }

  NSMutableAttributedString *out =
      [[[NSMutableAttributedString alloc] init] autorelease];
  [out appendAttributedString:
      [[[NSAttributedString alloc] initWithString:title
                                       attributes:titleAttrs] autorelease]];
  [out appendAttributedString:
      [[[NSAttributedString alloc]
          initWithString:[NSString stringWithFormat:PPUTF8("\n%@ \xC2\xB7 %@ msg"),
                             [s objectForKey:@"age"],
                             [s objectForKey:@"messages"]]
              attributes:metaAttrs] autorelease]];

  return out;
}

- (void)tableViewSelectionDidChange:(NSNotification *)note {
  NSInteger row = [sessionTable selectedRow];
  if (row < 0 || [bridge isBusy]) return;
  NSDictionary *s = [sessions objectAtIndex:(NSUInteger)row];
  if (![bridge loadSessionWithId:[s objectForKey:@"id"]]) return;

  [self clearTranscript];
  NSEnumerator *te = [[bridge transcript] objectEnumerator];
  NSDictionary *m;
  while ((m = [te nextObject]) != nil) {
    NSString *role = [m objectForKey:@"role"];
    if ([role isEqualToString:@"user"]) {
      [self append:@"\nYou\n" color:[NSColor colorWithCalibratedRed:0.0
                                                              green:0.35
                                                               blue:0.55
                                                              alpha:1.0]
              bold:YES mono:NO];
    }

    else {
      [self append:@"\nppcode\n" color:[NSColor colorWithCalibratedRed:0.35
                                                                 green:0.15
                                                                  blue:0.5
                                                                 alpha:1.0]
              bold:YES mono:NO];
    }
    // The model's own turns are markdown; what the user typed is not.
    if ([role isEqualToString:@"user"]) {
      [self appendPlain:[m objectForKey:@"text"]];
      [self appendPlain:@"\n"];
    }

    else {
      [self appendMarkdown:[m objectForKey:@"text"]];
    }
  }
}

// --- bridge delegate -------------------------------------------------------

- (void)bridgeDidStart {
  streaming = NO;
  [spinner startAnimation:nil];
  [statusField setStringValue:PPUTF8("Thinking\xE2\x80\xA6")];
}

- (void)bridgeDidReceiveText:(NSString *)delta {
  if (!streaming) {
    streaming = YES;
    [self append:@"\nppcode\n" color:[NSColor colorWithCalibratedRed:0.35
                                                               green:0.15
                                                                blue:0.5
                                                               alpha:1.0]
            bold:YES mono:NO];
  }
  [self streamDelta:delta];
}

- (void)bridgeDidStartTool:(NSString *)name detail:(NSString *)detail {
  // Settle the markdown first, or this line lands inside the buffered reply.
  [self flushStream];
  [self append:[NSString stringWithFormat:PPUTF8("\n  \xE2\x97\x8F %@  %@\n"),
                   name, detail]
         color:[NSColor colorWithCalibratedRed:0.55 green:0.4 blue:0.0 alpha:1.0]
          bold:NO mono:YES];
  [statusField setStringValue:
      [NSString stringWithFormat:PPUTF8("Running %@\xE2\x80\xA6"), name]];
}

- (void)bridgeDidFinishTool:(NSString *)name
                     result:(NSString *)result
                    isError:(BOOL)isError {
  NSArray *lines = [result componentsSeparatedByString:@"\n"];
  NSUInteger show = [lines count] > 12 ? 12 : [lines count];
  NSString *body = [[lines subarrayWithRange:NSMakeRange(0, show)]
                       componentsJoinedByString:@"\n"];
  if ([lines count] > show)
      body = [body stringByAppendingFormat:PPUTF8("\n    \xE2\x80\xA6 %lu more lines"),
                   (unsigned long)([lines count] - show)];

  [self append:[NSString stringWithFormat:@"%@\n", body]
         color:isError ? [NSColor redColor] : [NSColor darkGrayColor]
          bold:NO mono:YES];
}

- (void)bridgeDidUpdateStatus:(NSString *)status {
  [statusField setStringValue:status];
}

- (void)bridgeDidError:(NSString *)message {
  [self flushStream];
  [self append:[NSString stringWithFormat:@"\n%@\n", message]
         color:[NSColor redColor] bold:YES mono:NO];
}

- (void)bridgeDidFinishTurnWithTokens:(long long)tokens cost:(double)cost {
  [self flushStream];
  [spinner stopAnimation:nil];
  [statusField setStringValue:
      [NSString stringWithFormat:
          PPUTF8("Ready \xC2\xB7 %lld tokens \xC2\xB7 $%.4f"), tokens, cost]];
  [self appendPlain:@"\n"];
  [self reloadSessions];
}

// A sheet, not an application-modal alert: this concerns one window.
- (BOOL)bridgeShouldAllowTool:(NSString *)name
                        title:(NSString *)title
                       detail:(NSString *)detail {
  NSAlert *alert = [[[NSAlert alloc] init] autorelease];
  [alert setMessageText:[NSString stringWithFormat:@"Allow %@?", name]];
  [alert setInformativeText:[NSString stringWithFormat:@"%@\n\n%@", title, detail]];
  [alert addButtonWithTitle:@"Allow"];
  [alert addButtonWithTitle:@"Deny"];
  [alert setAlertStyle:NSInformationalAlertStyle];
  NSInteger r = [alert runModal];

  return (r == NSAlertFirstButtonReturn) ? YES : NO;
}

@end

// ---------------------------------------------------------------------------

// `target` receives the application's own actions. The standard AppKit ones
// (hide:, terminate:, cut:, copy: ...) keep a nil target on purpose so they
// travel the responder chain to whatever is first responder.
//
// The application's own actions must not: with a nil target AppKit resolves
// them through -targetForAction:, finds nothing it is willing to use, disables
// the item, and a disabled item ignores its key equivalent. That is why
// Command-comma did nothing while calling -showSettings: directly worked.
static void buildMenuBar(id target) {
  // AppKit decides which menu is the application menu, and without being told
  // it synthesises an empty one of its own -- which is why the name appeared
  // twice, once bold and empty and once with the real items. -setAppleMenu: is
  // how a code-built menu bar claims that slot on this system.
  NSMenu *bar = [[[NSMenu alloc] initWithTitle:@""] autorelease];
  [NSApp setMainMenu:bar];

  NSString *appName = [[NSProcessInfo processInfo] processName];

  // --- Application ---------------------------------------------------------
  NSMenuItem *appItem = [bar addItemWithTitle:@"" action:NULL keyEquivalent:@""];
  NSMenu *appMenu = [[[NSMenu alloc] initWithTitle:appName] autorelease];

  [appMenu addItemWithTitle:[@"About " stringByAppendingString:appName]
                     action:@selector(orderFrontStandardAboutPanel:)
              keyEquivalent:@""];
  [appMenu addItem:[NSMenuItem separatorItem]];

  NSMenuItem *settingsItem =
      [appMenu addItemWithTitle:PPUTF8("Settings\xE2\x80\xA6")
                         action:@selector(showSettings:)
                  keyEquivalent:@","];
  [settingsItem setKeyEquivalentModifierMask:NSCommandKeyMask];
  [settingsItem setTarget:target];
  [appMenu addItem:[NSMenuItem separatorItem]];

  NSMenuItem *services = [appMenu addItemWithTitle:@"Services"
                                            action:NULL
                                     keyEquivalent:@""];
  NSMenu *servicesMenu = [[[NSMenu alloc] initWithTitle:@"Services"] autorelease];
  [services setSubmenu:servicesMenu];
  [NSApp setServicesMenu:servicesMenu];
  [appMenu addItem:[NSMenuItem separatorItem]];

  [appMenu addItemWithTitle:[@"Hide " stringByAppendingString:appName]
                     action:@selector(hide:)
              keyEquivalent:@"h"];
  [[appMenu addItemWithTitle:@"Hide Others"
                      action:@selector(hideOtherApplications:)
               keyEquivalent:@"h"]
      setKeyEquivalentModifierMask:NSCommandKeyMask | NSAlternateKeyMask];
  [appMenu addItemWithTitle:@"Show All"
                     action:@selector(unhideAllApplications:)
              keyEquivalent:@""];
  [appMenu addItem:[NSMenuItem separatorItem]];

  [appMenu addItemWithTitle:[@"Quit " stringByAppendingString:appName]
                     action:@selector(terminate:)
              keyEquivalent:@"q"];
  [appItem setSubmenu:appMenu];

  // This is the line that prevents the duplicate. It is not in the public
  // headers on 10.5, hence the guarded perform.
  if ([NSApp respondsToSelector:@selector(setAppleMenu:)]) {
    [NSApp performSelector:@selector(setAppleMenu:) withObject:appMenu];
  }

  // --- File ----------------------------------------------------------------
  NSMenuItem *fileItem = [bar addItemWithTitle:@"" action:NULL keyEquivalent:@""];
  NSMenu *fileMenu = [[[NSMenu alloc] initWithTitle:@"File"] autorelease];

  [[fileMenu addItemWithTitle:@"New Conversation"
                       action:@selector(newConversation:)
                keyEquivalent:@"n"] setTarget:target];
  [fileMenu addItem:[NSMenuItem separatorItem]];
  [fileMenu addItemWithTitle:@"Close"
                      action:@selector(performClose:)
               keyEquivalent:@"w"];
  [fileItem setSubmenu:fileMenu];

  // --- Edit ----------------------------------------------------------------
  NSMenuItem *editItem = [bar addItemWithTitle:@"" action:NULL keyEquivalent:@""];
  NSMenu *editMenu = [[[NSMenu alloc] initWithTitle:@"Edit"] autorelease];

  [editMenu addItemWithTitle:@"Undo" action:@selector(undo:) keyEquivalent:@"z"];
  [[editMenu addItemWithTitle:@"Redo" action:@selector(redo:) keyEquivalent:@"z"]
      setKeyEquivalentModifierMask:NSCommandKeyMask | NSShiftKeyMask];
  [editMenu addItem:[NSMenuItem separatorItem]];
  [editMenu addItemWithTitle:@"Cut" action:@selector(cut:) keyEquivalent:@"x"];
  [editMenu addItemWithTitle:@"Copy" action:@selector(copy:) keyEquivalent:@"c"];
  [editMenu addItemWithTitle:@"Paste" action:@selector(paste:) keyEquivalent:@"v"];
  [editMenu addItemWithTitle:@"Select All"
                      action:@selector(selectAll:)
               keyEquivalent:@"a"];
  [editItem setSubmenu:editMenu];

  // --- Window --------------------------------------------------------------
  NSMenuItem *windowItem = [bar addItemWithTitle:@"" action:NULL keyEquivalent:@""];
  NSMenu *windowMenu = [[[NSMenu alloc] initWithTitle:@"Window"] autorelease];

  [windowMenu addItemWithTitle:@"Minimize"
                        action:@selector(performMiniaturize:)
                 keyEquivalent:@"m"];
  [windowMenu addItemWithTitle:@"Zoom" action:@selector(performZoom:) keyEquivalent:@""];
  [windowItem setSubmenu:windowMenu];
  [NSApp setWindowsMenu:windowMenu];
}

// Walk a view tree and count what is actually there. Used by --check, because
// the machine's display is often asleep and a screenshot then proves nothing.
static void countViews(NSView *v, NSMutableDictionary *tally) {
  NSString *cls = NSStringFromClass([v class]);
  NSNumber *n = [tally objectForKey:cls];
  [tally setObject:[NSNumber numberWithInt:(n ? [n intValue] : 0) + 1] forKey:cls];
  NSEnumerator *e = [[v subviews] objectEnumerator];
  NSView *sub;
  while ((sub = [e nextObject]) != nil) countViews(sub, tally);
}

// Render a view hierarchy to a PNG without it ever reaching the screen.
//
// screencapture cannot do this job. Run over SSH it has no access to the
// console framebuffer and returns a uniformly black frame -- not an error, a
// convincing-looking black image -- whether or not the display is awake.
// -cacheDisplayInRect: draws the views into an offscreen bitmap instead, so
// this works with the display asleep, needs no root, and never touches the
// accessibility API.
static BOOL writeViewPNG(NSView *view, NSString *path) {
  if (!view) { return NO; }

  NSRect bounds = [view bounds];
  if (bounds.size.width < 1.0 || bounds.size.height < 1.0) { return NO; }

  NSBitmapImageRep *rep = [view bitmapImageRepForCachingDisplayInRect:bounds];
  if (!rep) { return NO; }

  [view cacheDisplayInRect:bounds toBitmapImageRep:rep];

  NSData *png = [rep representationUsingType:NSPNGFileType properties:nil];
  if (!png) { return NO; }

  return [png writeToFile:path atomically:YES];
}

@interface PPController (SelfCheck)
- (int)runSelfCheck;
- (int)writeShotsTo:(NSString *)dir;
- (void)installSampleTranscript;
@end

@implementation PPController (SelfCheck)

- (int)runSelfCheck {
  bridge = [[PPBridge alloc] init];
  [bridge setDelegate:self];
  [self buildWindow];
  [self populateModels];
  [self reloadSessions];

  NSMutableDictionary *tally = [NSMutableDictionary dictionary];
  countViews([window contentView], tally);

  int failures = 0;
  printf("ppcode gui self-check\n\n");
  printf("window:      %s  %.0fx%.0f\n", [[window title] UTF8String],
         [window frame].size.width, [window frame].size.height);
  printf("model:       %s%s\n", [[bridge modelId] UTF8String],
         [bridge modelSupportsImages] ? " (vision)" : "");
  printf("cwd:         %s\n", [[bridge workingDirectory] UTF8String]);
  printf("models:      %d in the popup\n", (int)[modelPopup numberOfItems]);
  printf("sessions:    %d in the list\n", (int)[sessionTable numberOfRows]);
  printf("menu bar:    %d menus\n", (int)[[NSApp mainMenu] numberOfItems]);
  printf("\nview hierarchy:\n");

  NSEnumerator *ke = [tally keyEnumerator];
  NSString *k;
  while ((k = [ke nextObject]) != nil)
      printf("  %-28s %d\n", [k UTF8String], [[tally objectForKey:k] intValue]);

  // The pieces that must exist for the interface to be usable at all.
  struct { const char *cls; const char *what; } required[] = {
    {"NSSplitView",         "split view"},
    {"NSTableView",         "conversation list"},
    {"PPComposer",          "composer with drag and drop"},
    {"NSButton",            "send button"},
    {"NSPopUpButton",       "model picker"},
    {"NSProgressIndicator", "activity spinner"},
  };
  printf("\nrequired elements:\n");
  for (unsigned i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
    NSNumber *have = [tally objectForKey:
        [NSString stringWithUTF8String:required[i].cls]];
    BOOL ok = (have && [have intValue] > 0);
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", required[i].what);
    if (!ok) failures++;
  }
  if (![composer isKindOfClass:[PPComposer class]]) {
    printf("  FAIL composer class\n");
    failures++;
  }
  if ([[composer registeredDraggedTypes] count] == 0) {
    printf("  FAIL composer accepts no dragged types\n");
    failures++;
  }

  else {
    printf("  ok   composer accepts %d dragged type(s)\n",
           (int)[[composer registeredDraggedTypes] count]);
  }
  if ([modelPopup numberOfItems] < 2) { printf("  FAIL model list empty\n"); failures++; }
  if ([[NSApp mainMenu] numberOfItems] < 3) {
    printf("  FAIL menu bar incomplete\n");
    failures++;
  }

  else {
    printf("  ok   menu bar has %d menus\n", (int)[[NSApp mainMenu] numberOfItems]);
  }

  // A menu item whose action resolves nowhere is silently disabled, and a
  // disabled item ignores its key equivalent -- which is exactly how
  // Command-comma came to do nothing. Check the wiring, not just the shape.
  NSMenu *appMenu = [[[NSApp mainMenu] itemAtIndex:0] submenu];
  NSMenuItem *settingsItem = nil;
  for (NSInteger i = 0; i < [appMenu numberOfItems]; i++) {
    NSMenuItem *it = [appMenu itemAtIndex:i];
    if ([it action] == @selector(showSettings:)) settingsItem = it;
  }
  if (!settingsItem) {
    printf("  FAIL no Settings menu item\n");
    failures++;
  }

  else {
    id t = [settingsItem target];
    BOOL wired = (t != nil) && [t respondsToSelector:@selector(showSettings:)];
    printf("  %-4s Settings item targets a responder\n", wired ? "ok" : "FAIL");
    if (!wired) failures++;

    BOOL hasKey = [[settingsItem keyEquivalent] isEqualToString:@","] &&
                  ([settingsItem keyEquivalentModifierMask] & NSCommandKeyMask);
    printf("  %-4s Settings item bound to Command-comma\n", hasKey ? "ok" : "FAIL");
    if (!hasKey) failures++;
  }

  // Non-ASCII inside an @"..." literal becomes garbage under GCC, and the
  // symptom is cosmetic enough to ship unnoticed. Every menu title must
  // survive a UTF-8 round trip.
  int badTitles = 0;
  for (NSInteger i = 0; i < [[NSApp mainMenu] numberOfItems]; i++) {
    NSMenu *sub = [[[NSApp mainMenu] itemAtIndex:i] submenu];
    for (NSInteger j = 0; j < [sub numberOfItems]; j++) {
      NSString *title = [[sub itemAtIndex:j] title];
      const char *utf8 = [title UTF8String];
      if (!utf8) { badTitles++; continue; }
      if (![title isEqualToString:[NSString stringWithUTF8String:utf8]])
          badTitles++;
    }
  }
  printf("  %-4s menu titles survive a UTF-8 round trip\n",
         badTitles == 0 ? "ok" : "FAIL");
  if (badTitles) failures++;

  // A model change has to rebuild the system message: it carries the model id,
  // the context window it was budgeted against, and whether images are usable.
  // Nothing about that is visible from the interface, so assert it here.
  {
    NSString *before = [[[bridge systemPrompt] copy] autorelease];
    NSString *original = [[[bridge modelId] copy] autorelease];

    NSString *other = nil;
    for (NSInteger i = 0; i < [modelPopup numberOfItems] && !other; i++) {
      NSString *t = [[modelPopup itemAtIndex:i] title];
      if ([t length] && ![t isEqualToString:original]) other = t;
    }

    if (!other) {
      printf("  FAIL only one model available, cannot test the rebuild\n");
      failures++;
    }

    else {
      [bridge setModelId:other];
      NSString *after = [bridge systemPrompt];

      BOOL changed = ![after isEqualToString:before];
      BOOL names = [after rangeOfString:other].location != NSNotFound;
      printf("  %-4s model change rebuilds the system prompt\n",
             changed ? "ok" : "FAIL");
      printf("  %-4s rebuilt prompt names the new model\n", names ? "ok" : "FAIL");
      if (!changed) failures++;
      if (!names) failures++;

      [bridge setModelId:original];
    }
  }

  printf("\n%s\n", failures == 0 ? "all checks passed" : "CHECKS FAILED");

  return failures == 0 ? 0 : 1;
}

// A document exercising every block the renderer knows, so a screenshot is a
// real check on §3.1 rather than a picture of an empty window.
- (void)installSampleTranscript {
  [self clearTranscript];
  [self appendMarkdown:PPUTF8(
      "# Markdown check\n"
      "\n"
      "Body text with **bold**, *italic*, ***both***, `inline_code()`, "
      "~~struck out~~ and a [link](http://openrouter.ai) plus a bare URL "
      "http://example.com/path.\n"
      "\n"
      "Identifiers such as read_file_text and MAX__VALUE must not turn italic.\n"
      "\n"
      "## Lists\n"
      "\n"
      "- first item\n"
      "- second item, long enough that it has to wrap onto another line so the "
      "hanging indent can be seen working properly\n"
      "  - nested item\n"
      "    - deeper still\n"
      "\n"
      "1. ordered one\n"
      "2. ordered two\n"
      "\n"
      "> A blockquote, which should be indented and set in italic.\n"
      "> - even a list inside it\n"
      "\n"
      "### Code\n"
      "\n"
      "```objc\n"
      "// Objective-C is the primary language here.\n"
      "@implementation PPThing\n"
      "\n"
      "- (NSString *)describe:(NSInteger)count {\n"
      "  NSString *s = [NSString stringWithFormat:@\"%ld items\", (long)count];\n"
      "\n"
      "  return s ? s : nil;\n"
      "}\n"
      "\n"
      "@end\n"
      "```\n"
      "\n"
      "| Setting | Default | Notes |\n"
      "| --- | --- | --- |\n"
      "| model | glm-5.2 | pinned |\n"
      "| caching | on | ~30% cheaper |\n"
      "\n"
      "---\n"
      "\n"
      "Final paragraph after a rule.\n")];
}

- (int)writeShotsTo:(NSString *)dir {
  // --check may already have built everything; building twice would leave two
  // windows and two bridges behind.
  if (!window) {
    bridge = [[PPBridge alloc] init];
    [bridge setDelegate:self];
    [self buildWindow];
    [self populateModels];
    [self reloadSessions];
  }

  [self installSampleTranscript];

  // Two attachments so the token strip is in the picture rather than collapsed.
  [composer addAttachment:@"/Users/brie/Desktop/Sample/AppDelegate.m"];
  [composer addAttachment:@"/tmp/a-rather-long-screenshot-name.png"];
  [self attachmentsChanged];

  [[NSFileManager defaultManager] createDirectoryAtPath:dir
                             withIntermediateDirectories:YES
                                              attributes:nil
                                                   error:NULL];

  int failures = 0;
  printf("ppcode gui offscreen shots -> %s\n\n", [dir UTF8String]);

  NSString *main = [dir stringByAppendingPathComponent:@"main-window.png"];
  if (writeViewPNG([window contentView], main)) {
    printf("  ok   main-window.png\n");
  }

  else {
    printf("  FAIL main-window.png\n");
    failures++;
  }

  // The transcript again, grown to its full laid-out height. The window only
  // ever shows one screenful, and a rendering bug is just as likely to be in
  // the part that is scrolled out of view.
  {
    NSRect saved = [transcript frame];
    NSLayoutManager *lm = [transcript layoutManager];
    NSTextContainer *tc = [transcript textContainer];
    [lm ensureLayoutForTextContainer:tc];

    NSRect used = [lm usedRectForTextContainer:tc];
    CGFloat height = used.size.height + 2 * [transcript textContainerInset].height + 8;
    [transcript setFrame:NSMakeRect(saved.origin.x, saved.origin.y,
                                    saved.size.width, height)];
    [transcript setNeedsDisplay:YES];

    NSString *full = [dir stringByAppendingPathComponent:@"transcript-full.png"];
    if (writeViewPNG(transcript, full)) {
      printf("  ok   transcript-full.png  (%.0fx%.0f)\n", saved.size.width, height);
    }

    else {
      printf("  FAIL transcript-full.png\n");
      failures++;
    }

    [transcript setFrame:saved];
  }

  // Each settings tab, which is the only way to see the layout of a pane that
  // is not frontmost.
  if (!settings) { settings = [[PPSettingsController alloc] initWithBridge:bridge]; }

  NSWindow *panel = [settings panelWindow];
  NSTabView *tabs = nil;
  NSEnumerator *se = [[[panel contentView] subviews] objectEnumerator];
  NSView *sub;
  while ((sub = [se nextObject]) != nil)
    if ([sub isKindOfClass:[NSTabView class]]) tabs = (NSTabView *)sub;

  if (!tabs) {
    printf("  FAIL settings tab view not found\n");

    return failures + 1;
  }

  for (NSInteger i = 0; i < [tabs numberOfTabViewItems]; i++) {
    NSTabViewItem *item = [tabs tabViewItemAtIndex:i];
    [tabs selectTabViewItemAtIndex:i];
    [[panel contentView] setNeedsDisplay:YES];

    NSString *name = [NSString stringWithFormat:@"settings-%@.png", [item label]];
    NSString *path = [dir stringByAppendingPathComponent:[name lowercaseString]];
    if (writeViewPNG([panel contentView], path)) {
      printf("  ok   %s\n", [[name lowercaseString] UTF8String]);
    }

    else {
      printf("  FAIL %s\n", [[name lowercaseString] UTF8String]);
      failures++;
    }
  }

  printf("\n%s\n", failures == 0 ? "shots written" : "SHOTS FAILED");

  return failures == 0 ? 0 : 1;
}

@end

int main(int argc, const char **argv) {
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

  BOOL selfCheck = NO;
  NSString *shotDir = nil;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--check") == 0) selfCheck = YES;
    else if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc)
        shotDir = [NSString stringWithUTF8String:argv[++i]];
  }

  [NSApplication sharedApplication];

  if (selfCheck || shotDir) {
    // Build everything and report, without entering the run loop. This is
    // how the interface gets verified on a machine whose display is asleep.
    PPController *c = [[PPController alloc] init];
    buildMenuBar(c);
    int rc = selfCheck ? [c runSelfCheck] : 0;
    if (shotDir && rc == 0) rc = [c writeShotsTo:shotDir];
    [pool release];

    return rc;
  }

  // -setActivationPolicy: is 10.6 and later. On 10.5 an application inside a
  // bundle is a regular, Dock-visible application already, and one run as a
  // bare executable simply has no Dock tile.
  PPController *c = [[PPController alloc] init];
  buildMenuBar(c);
  [NSApp setDelegate:c];
  [NSApp activateIgnoringOtherApps:YES];
  [NSApp run];

  [pool release];

  return 0;
}
