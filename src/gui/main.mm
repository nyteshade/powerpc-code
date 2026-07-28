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
#import "Library.h"
#import "Markdown.h"
#import "Providers.h"
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

// Return sends; Shift-Return inserts a newline, which is what every other
// composer does and what the hands already expect. Option-Return does the same,
// because that is what this one used to do and muscle memory is muscle memory.
- (void)keyDown:(NSEvent *)event {
  if ([[event charactersIgnoringModifiers] isEqualToString:@"\r"]) {
    unsigned mods = [event modifierFlags];
    if (mods & (NSShiftKeyMask | NSAlternateKeyMask)) {
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

// Where documents are added to the search index.
//
// A drop target rather than a button, because the thing being added is a file
// and dragging it is the shortest path from "I have this book" to "it is
// searchable". It shows a progress bar in place while indexing, since the work
// takes long enough on this hardware that silence would read as a hang.
@interface PPDropWell : NSView {
  id target;
  NSProgressIndicator *bar;
  NSTextField *label;
  BOOL hovering;
  BOOL busy;
}

- (void)setDropTarget:(id)t;
- (void)beginBusyWithMessage:(NSString *)message;
- (void)setBusyMessage:(NSString *)message fraction:(double)fraction;
- (void)endBusy;

@end

// Declared because -initWithFrame: uses it before it is defined.
@interface PPDropWell (Private)

- (void)setLabelText:(NSString *)text;

@end

@implementation PPDropWell

- (id)initWithFrame:(NSRect)frame {
  if ((self = [super initWithFrame:frame])) {
    [self registerForDraggedTypes:
        [NSArray arrayWithObject:NSFilenamesPboardType]];

    label = [[[NSTextField alloc]
        initWithFrame:NSMakeRect(8, 10, NSWidth(frame) - 16, 32)] autorelease];
    [label setBezeled:NO];
    [label setDrawsBackground:NO];
    [label setEditable:NO];
    [label setSelectable:NO];
    [label setAlignment:NSCenterTextAlignment];
    [label setFont:[NSFont systemFontOfSize:10.0]];
    [label setTextColor:[NSColor colorWithCalibratedWhite:0.78 alpha:1.0]];
    [[label cell] setWraps:YES];
    [label setAutoresizingMask:NSViewWidthSizable];
    [self addSubview:label];
    [self setLabelText:PPUTF8("Drop documents here,\nor click to choose")];

    bar = [[[NSProgressIndicator alloc]
        initWithFrame:NSMakeRect(14, 44, NSWidth(frame) - 28, 12)] autorelease];
    [bar setStyle:NSProgressIndicatorBarStyle];
    [bar setControlSize:NSSmallControlSize];
    [bar setIndeterminate:NO];
    [bar setMinValue:0.0];
    [bar setMaxValue:1.0];
    [bar setAutoresizingMask:NSViewWidthSizable];
    [bar setHidden:YES];
    [self addSubview:bar];
  }

  return self;
}

- (void)setDropTarget:(id)t { target = t; }

// Light type sitting straight on the hide needs a shadow under it or it reads
// as thin and washed out against the grain.
- (void)setLabelText:(NSString *)text {
  NSShadow *shadow = [[[NSShadow alloc] init] autorelease];
  [shadow setShadowColor:[NSColor colorWithCalibratedWhite:0.0 alpha:0.9]];
  [shadow setShadowOffset:NSMakeSize(0.0, -1.0)];
  [shadow setShadowBlurRadius:2.5];

  NSMutableParagraphStyle *p =
      [[[NSMutableParagraphStyle alloc] init] autorelease];
  [p setAlignment:NSCenterTextAlignment];

  NSDictionary *attrs = [NSDictionary dictionaryWithObjectsAndKeys:
      [NSFont systemFontOfSize:10.0], NSFontAttributeName,
      [NSColor colorWithCalibratedWhite:0.88 alpha:1.0],
          NSForegroundColorAttributeName,
      shadow, NSShadowAttributeName,
      p, NSParagraphStyleAttributeName,
      nil];

  [label setAttributedStringValue:
      [[[NSAttributedString alloc] initWithString:(text ? text : @"")
                                       attributes:attrs] autorelease]];
}

- (void)beginBusyWithMessage:(NSString *)message {
  busy = YES;
  hovering = NO;
  [bar setHidden:NO];
  // Indeterminate until the first real fraction arrives: a bar sitting at zero
  // looks like nothing is happening.
  [bar setIndeterminate:YES];
  [bar startAnimation:nil];
  [self setLabelText:message ? message : @"Indexing..."];
  [self setNeedsDisplay:YES];
}

- (void)setBusyMessage:(NSString *)message fraction:(double)fraction {
  if (!busy) { [self beginBusyWithMessage:message]; }

  if (fraction >= 0.0) {
    [bar setIndeterminate:NO];
    [bar setDoubleValue:fraction];
  }

  if ([message length]) { [self setLabelText:message]; }
}

- (void)endBusy {
  busy = NO;
  [bar stopAnimation:nil];
  [bar setHidden:YES];
  [self setLabelText:PPUTF8("Drop documents here,\nor click to choose")];
  [self setNeedsDisplay:YES];
}

- (void)drawRect:(NSRect)dirty {
  (void)dirty;
  NSRect b = [self bounds];
  [PPSkin drawRecessedWellInRect:b radius:5.0];

  if (busy) { return; }

  // A dashed outline says "put something here" without needing a label to say
  // so twice. It brightens while a drag is over the view.
  NSBezierPath *p = [PPSkin roundedRectPath:NSInsetRect(b, 7.0, 7.0) radius:4.0];
  CGFloat pattern[2] = {4.0, 3.0};
  [p setLineDash:pattern count:2 phase:0.0];
  [p setLineWidth:1.0];
  [[NSColor colorWithCalibratedWhite:1.0 alpha:hovering ? 0.55 : 0.22] set];
  [p stroke];
}

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
  if (busy) return NSDragOperationNone;

  NSPasteboard *pb = [sender draggingPasteboard];
  if (![[pb types] containsObject:NSFilenamesPboardType])
      return NSDragOperationNone;

  hovering = YES;
  [self setNeedsDisplay:YES];

  return NSDragOperationCopy;
}

// Clicking is the same action as dropping. Some people reach for a file
// chooser and some drag; the target for both is the same rectangle, so it
// should accept both rather than growing a separate button.
- (void)mouseDown:(NSEvent *)event {
  if (busy) { return; }

  [target performSelector:@selector(chooseDocumentsToIndex:) withObject:self];
}

- (void)draggingExited:(id<NSDraggingInfo>)sender {
  hovering = NO;
  [self setNeedsDisplay:YES];
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
  hovering = NO;
  [self setNeedsDisplay:YES];
  if (busy) return NO;

  NSArray *files =
      [[sender draggingPasteboard] propertyListForType:NSFilenamesPboardType];
  if ([files count] == 0) return NO;

  [target performSelector:@selector(indexDroppedFiles:) withObject:files];

  return YES;
}

@end

// The header band: leather with the name blocked into it, the way a title is
// stamped on a cover.
@interface PPTitleView : PPLeatherView {
  NSString *subtitle;
}

- (void)setSubtitle:(NSString *)s;

@end

@implementation PPTitleView

- (void)setSubtitle:(NSString *)s {
  [s retain];
  [subtitle release];
  subtitle = s;
  [self setNeedsDisplay:YES];
}

- (void)dealloc { [subtitle release]; [super dealloc]; }

- (void)drawRect:(NSRect)dirty {
  [super drawRect:dirty];

  NSRect b = [self bounds];

  // The band is a separate piece of hide laid over the cover, so it is sewn on
  // along its bottom edge. Without this the seam around the window simply
  // disappeared behind the band, which looked like the stitching had been
  // forgotten along the top.
  [PPSkin drawStitchLineFrom:NSMakePoint(NSMinX(b) + 6.0, NSMinY(b) + 4.5)
                          to:NSMakePoint(NSMaxX(b) - 6.0, NSMinY(b) + 4.5)];

  NSFont *font = [NSFont fontWithName:@"Baskerville" size:16.0];
  if (!font) { font = [NSFont boldSystemFontOfSize:15.0]; }

  // Sits above the seam, not centred in the whole band.
  NSRect text = NSMakeRect(0, NSMinY(b) + 11.0, NSWidth(b), 20.0);
  [PPSkin drawEmbossedText:@"PowerPC Code"
                    inRect:text
                      font:font
                     color:[NSColor colorWithCalibratedRed:0.85
                                                     green:0.73
                                                      blue:0.46
                                                     alpha:1.0]];

  // The working directory rides along the right of the band, blocked into the
  // hide like the title. It replaced a wide Aqua capsule in the bottom strip,
  // which sat under the drop well at almost the same width and read as
  // belonging to it -- and put a bright white bar across the leather.
  if ([subtitle length]) {
    NSMutableParagraphStyle *ps =
        [[[NSMutableParagraphStyle alloc] init] autorelease];
    [ps setAlignment:NSRightTextAlignment];
    [ps setLineBreakMode:NSLineBreakByTruncatingHead];

    NSDictionary *shade = [NSDictionary dictionaryWithObjectsAndKeys:
        [NSFont systemFontOfSize:10.0], NSFontAttributeName,
        [NSColor colorWithCalibratedWhite:0.0 alpha:0.55],
            NSForegroundColorAttributeName,
        ps, NSParagraphStyleAttributeName, nil];
    NSDictionary *face = [NSDictionary dictionaryWithObjectsAndKeys:
        [NSFont systemFontOfSize:10.0], NSFontAttributeName,
        [NSColor colorWithCalibratedRed:0.72 green:0.63 blue:0.45 alpha:1.0],
            NSForegroundColorAttributeName,
        ps, NSParagraphStyleAttributeName, nil];

    NSRect r = NSMakeRect(NSMaxX(b) - 320.0, NSMinY(b) + 12.0, 300.0, 14.0);
    [subtitle drawInRect:NSOffsetRect(r, 0, -1) withAttributes:shade];
    [subtitle drawInRect:r withAttributes:face];
  }
}

@end

// ---------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------

// The marker on the one item in the model menu that is not a model. A
// represented object rather than a title comparison: a provider could one day
// publish a model called "Change Providers", and a title is not an identity.
//
// At file scope rather than inside the @implementation: GCC's Objective-C++
// front end is unreliable about C declarations between @implementation and
// @end.
static NSString * const kProvidersItem = @"ppcode.providers";

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
  PPDropWell *dropWell;
  PPTitleView *headerView;
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
  PPLibraryController *library;
  PPProvidersController *providers;
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
- (void)openSession:(id)sender;
- (void)exportSession:(id)sender;
- (void)archiveSession:(id)sender;
- (void)deleteSession:(id)sender;
- (void)clearAllConversations:(id)sender;
- (void)chooseWorkingDirectory:(id)sender;
- (void)indexDroppedFiles:(NSArray *)paths;
- (void)chooseDocumentsToIndex:(id)sender;
- (void)showLibrary:(id)sender;
- (void)showProviders:(id)sender;
- (void)providersDidChangeProvider;
- (void)reindexConversations:(id)sender;
- (void)refreshWorkingDirectory;
- (void)afterRemovingSessionWasLoaded:(BOOL)wasLoaded message:(NSString *)message;
- (void)presentProblem:(NSString *)text detail:(NSString *)detail;
- (void)setStatus:(NSString *)text;
- (void)reloadSessions;
- (void)showSettings:(id)sender;
- (void)newConversation:(id)sender;
- (void)clearTranscript;
- (void)flushStream;
- (void)buildWindow;
- (void)populateModels;
@end

@implementation PPController

// --- transcript helpers ----------------------------------------------------

// Status text sits directly on the leather with nothing behind it, so light
// type alone is thin and low-contrast. A dark shadow underneath separates it
// from the hide the way embossed lettering separates from a cover.
- (void)setStatus:(NSString *)text {
  if (!text) { text = @""; }

  NSShadow *shadow = [[[NSShadow alloc] init] autorelease];
  [shadow setShadowColor:[NSColor colorWithCalibratedWhite:0.0 alpha:0.85]];
  [shadow setShadowOffset:NSMakeSize(0.0, -1.0)];
  [shadow setShadowBlurRadius:2.5];

  NSDictionary *attrs = [NSDictionary dictionaryWithObjectsAndKeys:
      [NSFont systemFontOfSize:11.0], NSFontAttributeName,
      [NSColor colorWithCalibratedWhite:0.92 alpha:1.0],
          NSForegroundColorAttributeName,
      shadow, NSShadowAttributeName,
      nil];

  [statusField setAttributedStringValue:
      [[[NSAttributedString alloc] initWithString:text
                                       attributes:attrs] autorelease]];
}

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

  // Chrome has to sit on the same baseline grid as everything else. With no
  // paragraph style it took the font's natural line height, and one tool line
  // was enough to knock every rule below it out of step with the writing.
  NSMutableParagraphStyle *p =
      [[[NSMutableParagraphStyle alloc] init] autorelease];
  [p setMinimumLineHeight:PPMarkdownLineHeight()];
  [p setMaximumLineHeight:PPMarkdownLineHeight()];

  NSDictionary *attrs = [NSDictionary dictionaryWithObjectsAndKeys:
                            font, NSFontAttributeName,
                            color ? color : [NSColor blackColor],
                                NSForegroundColorAttributeName,
                            p, NSParagraphStyleAttributeName,
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
  // 24 rather than 18 so the seam, which runs 7 in from the window edge, has
  // clear air between it and the controls instead of nearly touching the Send
  // button.
  const CGFloat kMargin = 24.0;   // leather visible around the content
  const CGFloat kBarH = 52.0;     // bottom control strip
  const CGFloat kHeaderH = 38.0;
  const CGFloat kGap = 10.0;
  const CGFloat kSeamClear = 18.0;  // controls stay this far off the bottom seam
  const CGFloat ctlH = 26.0;        // standard control height in the bottom strip
  const CGFloat ctlY = kSeamClear;

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
  headerView = header;

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

  // Right-click on a conversation. The destructive items are last and
  // separated, so the pointer does not land on Delete by momentum.
  NSMenu *rowMenu = [[[NSMenu alloc] initWithTitle:@"Conversation"] autorelease];
  struct { NSString *title; SEL action; BOOL separatorBefore; } rowItems[] = {
      {@"Open",                    @selector(openSession:),   NO},
      {PPUTF8("Export as JSONL\xE2\x80\xA6"),
                                   @selector(exportSession:), NO},
      {@"Archive",                 @selector(archiveSession:), YES},
      {PPUTF8("Delete\xE2\x80\xA6"), @selector(deleteSession:), NO},
  };
  for (unsigned i = 0; i < sizeof(rowItems) / sizeof(rowItems[0]); i++) {
    if (rowItems[i].separatorBefore) {
      [rowMenu addItem:[NSMenuItem separatorItem]];
    }

    [[rowMenu addItemWithTitle:rowItems[i].title
                        action:rowItems[i].action
                 keyEquivalent:@""] setTarget:self];
  }
  [sessionTable setMenu:rowMenu];

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

  const CGFloat kDropH = 74.0;

  NSView *leftColumn = [[[NSView alloc]
      initWithFrame:NSMakeRect(0, 0, 220, splitHeight)] autorelease];
  [leftColumn setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

  PPWellView *listWell =
      WrapInPaperWell(listScroll,
                      NSMakeRect(0, kDropH + 6, 220, splitHeight - kDropH - 6),
                      NSViewWidthSizable | NSViewHeightSizable, NO);
  [leftColumn addSubview:listWell];

  // Pinned to the bottom of the column, so the conversation list takes the
  // growth when the window is resized.
  dropWell = [[[PPDropWell alloc]
      initWithFrame:NSMakeRect(0, 0, 220, kDropH)] autorelease];
  [dropWell setDropTarget:self];
  [dropWell setAutoresizingMask:NSViewWidthSizable | NSViewMaxYMargin];
  [leftColumn addSubview:dropWell];

  [mainSplit addSubview:leftColumn];
  [mainSplit addSubview:rightSplit];
  [mainSplit setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
  [content addSubview:mainSplit];

  // --- bottom bar ---------------------------------------------------------
  statusField = [[[NSTextField alloc]
      initWithFrame:NSMakeRect(kMargin, kSeamClear + 4, 560, 18)]
                    autorelease];
  [statusField setBezeled:NO];
  [statusField setDrawsBackground:NO];
  [statusField setEditable:NO];
  [statusField setSelectable:NO];
  [statusField setFont:[NSFont systemFontOfSize:11.0]];
  // Light type: this row sits directly on the leather, not on paper.
  [statusField setTextColor:[NSColor colorWithCalibratedWhite:0.86 alpha:1.0]];
  [self setStatus:@"Ready"];
  [content addSubview:statusField];

  // The attachment count used to be a label here. It is a row of removable
  // tokens above the composer now, which is both more informative and the only
  // way to drop one file without clearing the lot.

  // Laid out from the right edge inward at HIG spacing: 20 from the window,
  // then 8 between related controls.
  CGFloat right = 940 - kMargin;
  CGFloat sendW = 85, popupW = 190;

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
                               ctlY + (ctlH - 16) / 2, 16, 16)] autorelease];
  [spinner setStyle:NSProgressIndicatorSpinningStyle];
  [spinner setControlSize:NSSmallControlSize];
  [spinner setDisplayedWhenStopped:NO];
  [spinner setAutoresizingMask:NSViewMinXMargin];
  [content addSubview:spinner];

  [self refreshWorkingDirectory];

  [window center];
  [window makeKeyAndOrderFront:nil];
  [window makeFirstResponder:composer];
}

- (void)populateModels {
  [modelPopup removeAllItems];

  // The list below is one service's catalogue, so the way to a different one
  // belongs at the top of it rather than in a settings panel. Everything here
  // -- which models exist, which are pinned, whether any arrive at all --
  // follows from the provider, which makes this the honest place to change it.
  [modelPopup addItemWithTitle:PPUTF8("Change Providers\xE2\x80\xA6")];
  [[modelPopup lastItem] setRepresentedObject:kProvidersItem];
  [[modelPopup menu] addItem:[NSMenuItem separatorItem]];

  // Which service those models come from is otherwise invisible: two providers
  // can serve the same model id at different prices.
  [modelPopup setToolTip:[NSString stringWithFormat:@"Models from %@",
                                    [bridge providerName]]];

  NSArray *favs = [bridge favouriteModelIds];
  NSEnumerator *fe = [favs objectEnumerator];
  NSString *f;
  while ((f = [fe nextObject]) != nil) [modelPopup addItemWithTitle:f];
  [[modelPopup menu] addItem:[NSMenuItem separatorItem]];

  // The pinned models keep the order Brielle put them in -- that order is a
  // ranking, not a list. Everything after the separator is alphabetical, since
  // the catalogue arrives in whatever order OpenRouter felt like and hunting
  // for a model id in an unsorted list of a hundred is miserable.
  NSMutableArray *rest = [NSMutableArray array];
  NSEnumerator *me = [[bridge availableModels] objectEnumerator];
  NSDictionary *m;
  while ((m = [me nextObject]) != nil) {
    NSString *mid = [m objectForKey:@"id"];
    if (!mid || [favs containsObject:mid]) continue;
    if (![[m objectForKey:@"tools"] boolValue]) continue;   // useless here

    [rest addObject:mid];
  }

  [rest sortUsingSelector:@selector(caseInsensitiveCompare:)];

  NSEnumerator *re = [rest objectEnumerator];
  NSString *mid;
  int added = 0;
  while ((mid = [re nextObject]) != nil) {
    [modelPopup addItemWithTitle:mid];
    if (++added >= 120) break;
  }
  // The model in use may not be in the catalogue at all -- an id typed on the
  // command line, or one the service has since withdrawn. It still has to be
  // the item showing, or the popup claims the conversation is using something
  // it is not. Since the first item is now the providers entry, falling back to
  // index 0 would be a particularly bad lie.
  NSString *cur = [bridge modelId];
  if (![modelPopup itemWithTitle:cur] && [cur length]) {
    [modelPopup insertItemWithTitle:cur atIndex:2];
  }
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
       "attach them. Return sends, Shift-Return inserts a newline.\n\n",
      [bridge workingDirectory], [bridge modelId],
      [bridge modelSupportsImages] ? @" (can see images)" : @" (text only)"]];

  // Without a key nothing works, and an application launched from the Finder
  // does not inherit a key exported in a shell profile. Say so plainly and
  // open the place where it is set -- which is now the providers window, since
  // a key belongs to a provider rather than to the application.
  if (![bridge hasApiKey]) {
    [self append:[NSString stringWithFormat:@"No %@ key is configured.\n",
                            [bridge providerName]]
           color:[NSColor redColor] bold:YES mono:NO];
    [self appendPlain:
        @"An application launched from the Finder does not inherit your shell "
         "environment, so a key exported in .zshrc is invisible here. Set one "
         "in the window that just opened; it is written to ~/.local/keys, "
         "readable only by you, and the command line tool reads the same "
         "file.\n\n"];
    [self showProviders:nil];
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

  // The one item that is not a model. Put the selection back on the model
  // actually in use before opening the window, or the popup is left showing a
  // command as though it were the thing answering.
  if ([[[modelPopup selectedItem] representedObject]
          isEqual:kProvidersItem]) {
    [modelPopup selectItemWithTitle:[bridge modelId]];
    [self showProviders:nil];

    return;
  }

  // Mid-turn the worker thread is reading the config and the system prompt, so
  // the change is refused rather than raced. Put the popup back where it was.
  if ([bridge isBusy]) {
    [modelPopup selectItemWithTitle:[bridge modelId]];
    [self setStatus:@"Finish the current turn before changing model"];

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

- (void)showLibrary:(id)sender {
  if (!library) { library = [[PPLibraryController alloc] initWithBridge:bridge]; }

  [library showWindow];
}

- (void)showProviders:(id)sender {
  if (!providers) {
    providers = [[PPProvidersController alloc] initWithBridge:bridge];
    [providers setDelegate:self];
  }

  [providers showWindow];
}

// The provider changed under us: the model list on screen is the old service's
// catalogue and every id in it is now meaningless.
- (void)providersDidChangeProvider {
  [self populateModels];
  [self appendPlain:[NSString stringWithFormat:@"\n[provider: %@, model: %@]\n\n",
                        [bridge providerName], [bridge modelId]]];
  [self setStatus:[NSString stringWithFormat:@"Now using %@",
                             [bridge providerName]]];
}

- (void)showSettings:(id)sender {
  if (!settings) {
    settings = [[PPSettingsController alloc] initWithBridge:bridge];
    [settings setDelegate:self];
  }

  [settings showWindow];
}

- (void)newConversation:(id)sender {
  if ([bridge isBusy]) return;
  [bridge newConversation];
  [self refreshWorkingDirectory];
  [self clearTranscript];
  [self appendPlain:@"New conversation.\n\n"];
  [self reloadSessions];
}

// --- the search index -------------------------------------------------------

- (void)indexDroppedFiles:(NSArray *)paths {
  if ([bridge isIndexing]) {
    [self setStatus:@"Already indexing"];

    return;
  }

  [dropWell beginBusyWithMessage:@"Reading..."];

  if (![bridge indexPaths:paths
           intoCollection:PPUTF8("reference")
                 delegate:self]) {
    [dropWell endBusy];
    [self presentProblem:@"Could not start indexing." detail:nil];
  }
}

- (void)chooseDocumentsToIndex:(id)sender {
  if ([bridge isIndexing]) {
    [self setStatus:@"Already indexing"];

    return;
  }

  NSOpenPanel *panel = [NSOpenPanel openPanel];
  [panel setCanChooseFiles:YES];
  [panel setCanChooseDirectories:YES];
  [panel setAllowsMultipleSelection:YES];
  [panel setTitle:@"Add to Search Index"];
  [panel setPrompt:@"Index"];

  if ([panel runModalForDirectory:NSHomeDirectory() file:nil] != NSOKButton) {
    return;
  }

  [self indexDroppedFiles:[panel filenames]];
}

- (void)reindexConversations:(id)sender {
  if ([bridge isIndexing]) {
    [self setStatus:@"Already indexing"];

    return;
  }

  [dropWell beginBusyWithMessage:@"Re-indexing conversations..."];
  if (![bridge reindexConversationsWithDelegate:self]) {
    [dropWell endBusy];
  }
}

// --- PPIndexDelegate, always on the main thread ------------------------------

- (void)indexDidProgress:(NSString *)message fraction:(double)fraction {
  [dropWell setBusyMessage:message fraction:fraction];
}

- (void)indexDidFinish:(NSString *)summary added:(NSInteger)documents {
  [dropWell endBusy];
  [self setStatus:summary];

  // The library window, if open, is now showing a stale list.
  if (library) { [library reload]; }
}

// --- working directory ------------------------------------------------------

// Shows the tail of the path, since the interesting part of
// /Users/brie/Desktop/Sample is "Desktop/Sample" and the button is not wide
// enough for the rest. The full path is the tooltip.
- (void)refreshWorkingDirectory {
  NSString *dir = [bridge workingDirectory];
  if (![dir length]) { dir = @"(none)"; }

  NSString *shown = [dir stringByAbbreviatingWithTildeInPath];
  NSArray *parts = [shown pathComponents];
  if ([parts count] > 3) {
    shown = [NSString stringWithFormat:PPUTF8("\xE2\x80\xA6/%@/%@"),
                [parts objectAtIndex:[parts count] - 2],
                [parts lastObject]];
  }

  [headerView setSubtitle:shown];

  // The proxy icon in the title bar, which is where a Mac application has
  // always said what it is pointed at -- and it is command-clickable.
  [window setRepresentedFilename:dir];
}

- (void)chooseWorkingDirectory:(id)sender {
  if ([bridge isBusy]) {
    [self setStatus:@"Finish the current turn before changing directory"];

    return;
  }

  NSOpenPanel *panel = [NSOpenPanel openPanel];
  [panel setCanChooseFiles:NO];
  [panel setCanChooseDirectories:YES];
  [panel setAllowsMultipleSelection:NO];
  [panel setCanCreateDirectories:YES];
  [panel setTitle:@"Working Directory"];
  [panel setPrompt:@"Use"];

  if ([panel runModalForDirectory:[bridge workingDirectory] file:nil] != NSOKButton) {
    return;
  }

  NSString *chosen = [panel filename];
  if (![bridge setWorkingDirectory:chosen]) {
    [self presentProblem:@"Could not use that directory."
                  detail:@"It may have been removed, or a turn is still running."];

    return;
  }

  [self refreshWorkingDirectory];
  [self appendPlain:[NSString stringWithFormat:@"\n[working directory: %@]\n\n",
                        chosen]];
}

// --- conversation management ------------------------------------------------

// The row the context menu was opened on, or the selected row when the action
// came from the menu bar. -1 when neither applies.
- (NSInteger)targetSessionRow {
  NSInteger row = [sessionTable clickedRow];
  if (row < 0) { row = [sessionTable selectedRow]; }
  if (row < 0 || row >= (NSInteger)[sessions count]) { return -1; }

  return row;
}

- (NSDictionary *)targetSession {
  NSInteger row = [self targetSessionRow];

  return row < 0 ? nil : [sessions objectAtIndex:(NSUInteger)row];
}

- (void)openSession:(id)sender {
  NSInteger row = [self targetSessionRow];
  if (row < 0) { return; }

  [sessionTable selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row]
            byExtendingSelection:NO];
}

- (void)exportSession:(id)sender {
  NSDictionary *s = [self targetSession];
  if (!s) { return; }

  NSSavePanel *panel = [NSSavePanel savePanel];
  [panel setRequiredFileType:@"jsonl"];
  [panel setTitle:@"Export Conversation"];

  NSString *suggested =
      [[s objectForKey:@"title"] stringByReplacingOccurrencesOfString:@"/"
                                                           withString:@"-"];
  if (![suggested length]) { suggested = @"conversation"; }

  if ([panel runModalForDirectory:NSHomeDirectory() file:suggested] != NSOKButton) {
    return;
  }

  NSString *err = nil;
  if ([bridge exportSessionWithId:[s objectForKey:@"id"]
                           toPath:[panel filename]
                            error:&err]) {
    [self setStatus:[NSString stringWithFormat:@"Exported to %@",
                        [[panel filename] lastPathComponent]]];
  }

  else {
    [self presentProblem:@"Could not export that conversation." detail:err];
  }
}

- (void)archiveSession:(id)sender {
  NSDictionary *s = [self targetSession];
  if (!s) { return; }

  BOOL wasLoaded = NO;
  NSString *err = nil;
  if (![bridge archiveSessionWithId:[s objectForKey:@"id"]
                          wasLoaded:&wasLoaded
                              error:&err]) {
    [self presentProblem:@"Could not archive that conversation." detail:err];

    return;
  }

  [self afterRemovingSessionWasLoaded:wasLoaded
                              message:@"Archived. The file is still in the "
                                       "sessions folder, under archive."];
}

- (void)deleteSession:(id)sender {
  NSDictionary *s = [self targetSession];
  if (!s) { return; }

  NSAlert *alert = [[[NSAlert alloc] init] autorelease];
  [alert setMessageText:[NSString stringWithFormat:@"Delete %@?",
                            [s objectForKey:@"title"]]];
  [alert setInformativeText:@"The conversation is moved to the Trash, so it can "
                             "still be recovered from there. Archive keeps it in "
                             "the sessions folder instead."];
  [alert addButtonWithTitle:@"Delete"];
  [alert addButtonWithTitle:@"Cancel"];
  [alert setAlertStyle:NSWarningAlertStyle];
  if ([alert runModal] != NSAlertFirstButtonReturn) { return; }

  BOOL wasLoaded = NO;
  NSString *err = nil;
  if (![bridge deleteSessionWithId:[s objectForKey:@"id"]
                         wasLoaded:&wasLoaded
                             error:&err]) {
    [self presentProblem:@"Could not delete that conversation." detail:err];

    return;
  }

  [self afterRemovingSessionWasLoaded:wasLoaded message:@"Conversation moved to the Trash."];
}

- (void)clearAllConversations:(id)sender {
  NSAlert *alert = [[[NSAlert alloc] init] autorelease];
  [alert setMessageText:@"Delete every saved conversation?"];
  [alert setInformativeText:
      [NSString stringWithFormat:@"All %lu conversations are moved to the Trash. "
                                  "They can be recovered from there until it is "
                                  "emptied.",
                                 (unsigned long)[sessions count]]];
  [alert addButtonWithTitle:@"Delete All"];
  [alert addButtonWithTitle:@"Cancel"];
  [alert setAlertStyle:NSCriticalAlertStyle];
  if ([alert runModal] != NSAlertFirstButtonReturn) { return; }

  NSInteger n = [bridge deleteAllSessions];
  if (n < 0) {
    [self presentProblem:@"Finish the current turn first." detail:nil];

    return;
  }

  [self afterRemovingSessionWasLoaded:YES
                              message:[NSString stringWithFormat:
                                          @"Moved %ld conversations to the Trash.", (long)n]];
}

// Shared tail: the list is stale either way, and if the conversation on screen
// no longer has a file behind it, start a fresh one rather than leaving a
// transcript that cannot be saved back.
- (void)afterRemovingSessionWasLoaded:(BOOL)wasLoaded message:(NSString *)message {
  [self reloadSessions];

  if (wasLoaded) {
    [bridge newConversation];
    [self clearTranscript];
  }

  [self setStatus:message];
}

- (void)presentProblem:(NSString *)text detail:(NSString *)detail {
  NSAlert *alert = [[[NSAlert alloc] init] autorelease];
  [alert setMessageText:text];
  if ([detail length]) { [alert setInformativeText:detail]; }
  [alert addButtonWithTitle:@"OK"];
  [alert setAlertStyle:NSWarningAlertStyle];
  [alert runModal];
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

  // The directory is saved with the conversation, so restoring one restores
  // where its work was happening.
  [self refreshWorkingDirectory];
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
  [self setStatus:PPUTF8("Thinking\xE2\x80\xA6")];
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
  [self setStatus:
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
  [self setStatus:status];
}

- (void)bridgeDidError:(NSString *)message {
  [self flushStream];
  [self append:[NSString stringWithFormat:@"\n%@\n", message]
         color:[NSColor redColor] bold:YES mono:NO];
}

- (void)bridgeDidFinishTurnWithTokens:(long long)tokens cost:(double)cost {
  [self flushStream];
  [spinner stopAnimation:nil];
  [self setStatus:
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

  // CFBundleName, not the process name. The executable is still called ppcode
  // -- it is a unix binary and should be -- but the application is "PowerPC
  // Code", and the About/Hide/Quit items have to say so. Falls back to the
  // process name when running as a bare executable outside a bundle.
  NSString *appName =
      [[[NSBundle mainBundle] infoDictionary] objectForKey:@"CFBundleName"];
  if (![appName length]) {
    appName = [[NSProcessInfo processInfo] processName];
  }

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
  [[fileMenu addItemWithTitle:PPUTF8("Working Directory\xE2\x80\xA6")
                       action:@selector(chooseWorkingDirectory:)
                keyEquivalent:@"d"] setTarget:target];
  [fileMenu addItem:[NSMenuItem separatorItem]];
  [[fileMenu addItemWithTitle:PPUTF8("Export Conversation as JSONL\xE2\x80\xA6")
                       action:@selector(exportSession:)
                keyEquivalent:@""] setTarget:target];
  [[fileMenu addItemWithTitle:PPUTF8("Delete All Conversations\xE2\x80\xA6")
                       action:@selector(clearAllConversations:)
                keyEquivalent:@""] setTarget:target];
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
  [windowMenu addItem:[NSMenuItem separatorItem]];
  [[windowMenu addItemWithTitle:@"Providers"
                         action:@selector(showProviders:)
                  keyEquivalent:@""] setTarget:target];
  [[windowMenu addItemWithTitle:@"Indexed Content"
                         action:@selector(showLibrary:)
                  keyEquivalent:@"l"] setTarget:target];
  [[windowMenu addItemWithTitle:PPUTF8("Re-index Conversations\xE2\x80\xA6")
                         action:@selector(reindexConversations:)
                  keyEquivalent:@""] setTarget:target];
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

  // The icon. A malformed .icns does not raise anything -- the Finder and the
  // Dock quietly show a generic application instead -- so ask ImageIO whether
  // it can actually decode the file, and at which sizes.
  {
    NSDictionary *info = [[NSBundle mainBundle] infoDictionary];
    NSString *named = [info objectForKey:@"CFBundleIconFile"];
    NSString *icon = [named length]
        ? [[NSBundle mainBundle] pathForResource:named ofType:@"icns"]
        : nil;

    // The trap that cost an afternoon: LaunchServices resolves
    // CFBundleIconFile as written before appending .icns. Resources also holds
    // the command line tool, so naming the icon "ppcode" pointed the Finder at
    // a Mach-O executable, which it could not read as an icon and would not
    // fall back from. Everything else looked correct -- valid icns, valid
    // plist, right key -- and the application still showed a generic icon.
    if ([named length]) {
      NSString *collision =
          [[[NSBundle mainBundle] resourcePath]
              stringByAppendingPathComponent:named];
      BOOL clash = [[NSFileManager defaultManager] fileExistsAtPath:collision];
      printf("  %-4s nothing shadows the icon name '%s'\n",
             clash ? "FAIL" : "ok", [named UTF8String]);
      if (clash) failures++;
    }

    if (!icon) {
      // Running as a bare executable rather than from a bundle: nothing to say.
      printf("  --   not running from a bundle, icon not checked\n");
    }

    else {
      NSImage *img = [[[NSImage alloc] initWithContentsOfFile:icon] autorelease];
      NSArray *reps = img ? [img representations] : nil;
      printf("  %-4s bundle icon decodes (%lu representations)\n",
             [reps count] > 0 ? "ok" : "FAIL", (unsigned long)[reps count]);
      if ([reps count] == 0) failures++;

      NSEnumerator *re = [reps objectEnumerator];
      NSImageRep *rep;
      BOOL small = NO, large = NO;
      while ((rep = [re nextObject]) != nil) {
        NSInteger w = [rep pixelsWide];
        if (w <= 32) small = YES;
        if (w >= 128) large = YES;
      }
      // Both ends matter: the Dock uses the large sizes, the Finder list and
      // the menu bar use the small ones.
      printf("  %-4s icon has both small and large sizes\n",
             (small && large) ? "ok" : "FAIL");
      if (!(small && large)) failures++;
    }
  }

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

  // The providers window and the way in. The entry point is an item in the
  // model menu that is not a model, so the thing worth asserting is that it is
  // distinguishable from one: a title match would pass even if the marker that
  // -modelChanged: actually tests were missing, and the failure that produces
  // is silent -- picking it would set the model to "Change Providers...".
  {
    NSMenuItem *first =
        [modelPopup numberOfItems] > 0 ? [modelPopup itemAtIndex:0] : nil;
    BOOL marked = [[first representedObject] isEqual:kProvidersItem];
    printf("  %-4s model menu opens with the providers item\n",
           marked ? "ok" : "FAIL");
    if (!marked) failures++;

    NSArray *known = [bridge availableProviders];
    int inUse = 0;
    NSEnumerator *pe = [known objectEnumerator];
    NSDictionary *p;
    while ((p = [pe nextObject]) != nil)
      if ([[p objectForKey:@"id"] isEqualToString:[bridge providerId]]) inUse++;

    printf("  %-4s %d providers, exactly one in use\n",
           (([known count] > 1) && inUse == 1) ? "ok" : "FAIL",
           (int)[known count]);
    if (!(([known count] > 1) && inUse == 1)) failures++;

    if (!providers) {
      providers = [[PPProvidersController alloc] initWithBridge:bridge];
      [providers setDelegate:self];
    }

    NSMutableDictionary *ptally = [NSMutableDictionary dictionary];
    countViews([[providers panelWindow] contentView], ptally);

    struct { const char *cls; const char *what; } wanted[] = {
      {"NSTableView",       "provider list"},
      {"NSSecureTextField", "key field"},
      {"NSButton",          "switch button"},
    };
    for (unsigned i = 0; i < sizeof(wanted) / sizeof(wanted[0]); i++) {
      NSNumber *have = [ptally objectForKey:
          [NSString stringWithUTF8String:wanted[i].cls]];
      BOOL ok = (have && [have intValue] > 0);
      printf("  %-4s providers window has a %s\n", ok ? "ok" : "FAIL",
             wanted[i].what);
      if (!ok) failures++;
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

  // The library window, which otherwise only exists once someone opens it.
  {
    if (!library) {
      library = [[PPLibraryController alloc] initWithBridge:bridge];
    }

    NSWindow *lib = [library panelWindow];
    NSString *path = [dir stringByAppendingPathComponent:@"library.png"];
    if (writeViewPNG([lib contentView], path)) {
      printf("  ok   library.png\n");
    }

    else {
      printf("  FAIL library.png\n");
      failures++;
    }
  }

  // The providers window, for the same reason: it is only ever on screen
  // because someone opened it.
  {
    if (!providers) {
      providers = [[PPProvidersController alloc] initWithBridge:bridge];
      [providers setDelegate:self];
    }

    NSWindow *pw = [providers panelWindow];
    NSString *path = [dir stringByAppendingPathComponent:@"providers.png"];
    if (writeViewPNG([pw contentView], path)) {
      printf("  ok   providers.png\n");
    }

    else {
      printf("  FAIL providers.png\n");
      failures++;
    }
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
