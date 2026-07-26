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
#import "Settings.h"
#import "Skin.h"

#include <string.h>

// ---------------------------------------------------------------------------
// A text view that accepts dropped files and images.
// ---------------------------------------------------------------------------

@interface PPComposer : NSTextView {
    NSMutableArray *attachments;
    id dropTarget;
}
- (NSArray *)attachments;
- (void)clearAttachments;
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
        NSString *path = [NSString stringWithFormat:@"/tmp/ppcode-drop-%ld.png",
                                                    (long)[NSDate timeIntervalSinceReferenceDate]];
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
    NSTextField *attachField;
    NSPopUpButton *modelPopup;
    NSProgressIndicator *spinner;
    NSButton *sendButton;
    PPBridge *bridge;
    PPSettingsController *settings;
    BOOL streaming;
}
- (void)send:(id)sender;
- (void)attachmentsChanged;
- (void)showSettings:(id)sender;
- (void)buildWindow;
- (void)populateModels;
- (void)reloadSessions;
@end

@implementation PPController

// --- transcript helpers ----------------------------------------------------

- (void)append:(NSString *)text
         color:(NSColor *)color
          bold:(BOOL)bold
          mono:(BOOL)mono {
    if (!text) return;
    NSFont *font = mono
        ? [NSFont fontWithName:@"Monaco" size:11.0]
        : (bold ? [NSFont boldSystemFontOfSize:12.0] : [NSFont systemFontOfSize:12.0]);
    if (!font) font = [NSFont systemFontOfSize:12.0];

    NSDictionary *attrs = [NSDictionary dictionaryWithObjectsAndKeys:
                              font, NSFontAttributeName,
                              color ? color : [NSColor blackColor],
                                  NSForegroundColorAttributeName, nil];
    NSAttributedString *as =
        [[[NSAttributedString alloc] initWithString:text attributes:attrs] autorelease];

    [[transcript textStorage] appendAttributedString:as];
    [transcript scrollRangeToVisible:NSMakeRange([[transcript string] length], 0)];
}

- (void)appendPlain:(NSString *)t { [self append:t color:nil bold:NO mono:NO]; }

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

    NSView *content = [window contentView];

    // --- left: session list -------------------------------------------------
    NSScrollView *listScroll =
        [[[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 220, 640)] autorelease];
    [listScroll setHasVerticalScroller:YES];
    [listScroll setAutoresizingMask:NSViewHeightSizable];
    [listScroll setBorderType:NSBezelBorder];

    sessionTable = [[[NSTableView alloc] initWithFrame:NSMakeRect(0, 0, 200, 640)]
                       autorelease];
    NSTableColumn *col =
        [[[NSTableColumn alloc] initWithIdentifier:@"title"] autorelease];
    [[col headerCell] setStringValue:@"Conversations"];
    [col setWidth:196];
    [sessionTable addTableColumn:col];
    [sessionTable setDataSource:self];
    [sessionTable setDelegate:self];
    [sessionTable setUsesAlternatingRowBackgroundColors:YES];
    [sessionTable setRowHeight:32.0];
    [listScroll setDocumentView:sessionTable];

    // --- right: transcript over composer ------------------------------------
    NSScrollView *transScroll =
        [[[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 700, 430)] autorelease];
    [transScroll setHasVerticalScroller:YES];
    [transScroll setBorderType:NSBezelBorder];
    [transScroll setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

    transcript = [[[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 700, 430)]
                     autorelease];
    [transcript setEditable:NO];
    [transcript setRichText:YES];
    [transcript setAutoresizingMask:NSViewWidthSizable];
    [transcript setTextContainerInset:NSMakeSize(8, 8)];
    [transScroll setDocumentView:transcript];

    NSScrollView *compScroll =
        [[[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, 700, 110)] autorelease];
    [compScroll setHasVerticalScroller:YES];
    [compScroll setBorderType:NSBezelBorder];
    [compScroll setAutoresizingMask:NSViewWidthSizable];

    composer = [[[PPComposer alloc] initWithFrame:NSMakeRect(0, 0, 700, 110)]
                   autorelease];
    [composer setFont:[NSFont systemFontOfSize:12.0]];
    [composer setDropTarget:self];
    [composer setAutoresizingMask:NSViewWidthSizable];
    [compScroll setDocumentView:composer];

    NSSplitView *rightSplit =
        [[[NSSplitView alloc] initWithFrame:NSMakeRect(220, 30, 720, 610)] autorelease];
    [rightSplit setVertical:NO];
    [rightSplit setDividerStyle:NSSplitViewDividerStyleThin];
    [rightSplit addSubview:transScroll];
    [rightSplit addSubview:compScroll];
    [rightSplit setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

    NSSplitView *mainSplit =
        [[[NSSplitView alloc] initWithFrame:NSMakeRect(0, 30, 940, 610)] autorelease];
    [mainSplit setVertical:YES];
    [mainSplit setDividerStyle:NSSplitViewDividerStyleThin];
    [mainSplit addSubview:listScroll];
    [mainSplit addSubview:rightSplit];
    [mainSplit setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
    [content addSubview:mainSplit];

    // --- bottom bar ---------------------------------------------------------
    statusField = [[[NSTextField alloc] initWithFrame:NSMakeRect(230, 6, 380, 18)]
                      autorelease];
    [statusField setBezeled:NO];
    [statusField setDrawsBackground:NO];
    [statusField setEditable:NO];
    [statusField setSelectable:NO];
    [statusField setFont:[NSFont systemFontOfSize:11.0]];
    [statusField setStringValue:@"Ready"];
    [content addSubview:statusField];

    attachField = [[[NSTextField alloc] initWithFrame:NSMakeRect(10, 6, 210, 18)]
                      autorelease];
    [attachField setBezeled:NO];
    [attachField setDrawsBackground:NO];
    [attachField setEditable:NO];
    [attachField setSelectable:NO];
    [attachField setFont:[NSFont systemFontOfSize:11.0]];
    [attachField setTextColor:[NSColor darkGrayColor]];
    [content addSubview:attachField];

    spinner = [[[NSProgressIndicator alloc] initWithFrame:NSMakeRect(618, 6, 16, 16)]
                  autorelease];
    [spinner setStyle:NSProgressIndicatorSpinningStyle];
    [spinner setControlSize:NSSmallControlSize];
    [spinner setDisplayedWhenStopped:NO];
    [content addSubview:spinner];

    modelPopup = [[[NSPopUpButton alloc] initWithFrame:NSMakeRect(645, 3, 190, 24)]
                     autorelease];
    [modelPopup setAutoresizingMask:NSViewMinXMargin];
    [modelPopup setTarget:self];
    [modelPopup setAction:@selector(modelChanged:)];
    [content addSubview:modelPopup];

    sendButton = [[[NSButton alloc] initWithFrame:NSMakeRect(845, 3, 85, 24)]
                     autorelease];
    [sendButton setTitle:@"Send"];
    [sendButton setBezelStyle:NSRoundedBezelStyle];   // the glossy Aqua capsule
    [sendButton setKeyEquivalent:@"\r"];              // default button, pulses
    [sendButton setTarget:self];
    [sendButton setAction:@selector(send:)];
    [sendButton setAutoresizingMask:NSViewMinXMargin];
    [content addSubview:sendButton];

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

- (void)attachmentsChanged {
    NSArray *a = [composer attachments];
    if ([a count] == 0) { [attachField setStringValue:@""]; return; }
    [attachField setStringValue:
        [NSString stringWithFormat:@"%lu attachment%@", (unsigned long)[a count],
                                   [a count] == 1 ? @"" : @"s"]];
}

- (void)modelChanged:(id)sender {
    NSString *mid = [modelPopup titleOfSelectedItem];
    if (!mid) return;
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
    } else {
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
    [transcript setString:@""];
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
    return [NSString stringWithFormat:@"%@\n%@ · %@ msg",
                     [s objectForKey:@"title"], [s objectForKey:@"age"],
                     [s objectForKey:@"messages"]];
}

- (void)tableViewSelectionDidChange:(NSNotification *)note {
    NSInteger row = [sessionTable selectedRow];
    if (row < 0 || [bridge isBusy]) return;
    NSDictionary *s = [sessions objectAtIndex:(NSUInteger)row];
    if (![bridge loadSessionWithId:[s objectForKey:@"id"]]) return;

    [transcript setString:@""];
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
        } else {
            [self append:@"\nppcode\n" color:[NSColor colorWithCalibratedRed:0.35
                                                                       green:0.15
                                                                        blue:0.5
                                                                       alpha:1.0]
                    bold:YES mono:NO];
        }
        [self appendPlain:[m objectForKey:@"text"]];
        [self appendPlain:@"\n"];
    }
}

// --- bridge delegate -------------------------------------------------------

- (void)bridgeDidStart {
    streaming = NO;
    [spinner startAnimation:nil];
    [statusField setStringValue:@"Thinking…"];
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
    [self appendPlain:delta];
}

- (void)bridgeDidStartTool:(NSString *)name detail:(NSString *)detail {
    streaming = NO;
    [self append:[NSString stringWithFormat:@"\n  ● %@  %@\n", name, detail]
           color:[NSColor colorWithCalibratedRed:0.55 green:0.4 blue:0.0 alpha:1.0]
            bold:NO mono:YES];
    [statusField setStringValue:[NSString stringWithFormat:@"Running %@…", name]];
}

- (void)bridgeDidFinishTool:(NSString *)name
                     result:(NSString *)result
                    isError:(BOOL)isError {
    NSArray *lines = [result componentsSeparatedByString:@"\n"];
    NSUInteger show = [lines count] > 12 ? 12 : [lines count];
    NSString *body = [[lines subarrayWithRange:NSMakeRange(0, show)]
                         componentsJoinedByString:@"\n"];
    if ([lines count] > show)
        body = [body stringByAppendingFormat:@"\n    … %lu more lines",
                     (unsigned long)([lines count] - show)];

    [self append:[NSString stringWithFormat:@"%@\n", body]
           color:isError ? [NSColor redColor] : [NSColor darkGrayColor]
            bold:NO mono:YES];
}

- (void)bridgeDidUpdateStatus:(NSString *)status {
    [statusField setStringValue:status];
}

- (void)bridgeDidError:(NSString *)message {
    [self append:[NSString stringWithFormat:@"\n%@\n", message]
           color:[NSColor redColor] bold:YES mono:NO];
}

- (void)bridgeDidFinishTurnWithTokens:(long long)tokens cost:(double)cost {
    streaming = NO;
    [spinner stopAnimation:nil];
    [statusField setStringValue:
        [NSString stringWithFormat:@"Ready · %lld tokens · $%.4f", tokens, cost]];
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

static void buildMenuBar(void) {
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

  [[appMenu addItemWithTitle:@"Settings…"
                      action:@selector(showSettings:)
               keyEquivalent:@","] setKeyEquivalentModifierMask:NSCommandKeyMask];
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

  [fileMenu addItemWithTitle:@"New Conversation"
                      action:@selector(newConversation:)
               keyEquivalent:@"n"];
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

static void buildMenuBar(void);

@interface PPController (SelfCheck)
- (int)runSelfCheck;
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
    if (![composer isKindOfClass:[PPComposer class]]) { printf("  FAIL composer class\n"); failures++; }
    if ([[composer registeredDraggedTypes] count] == 0) {
        printf("  FAIL composer accepts no dragged types\n");
        failures++;
    } else {
        printf("  ok   composer accepts %d dragged type(s)\n",
               (int)[[composer registeredDraggedTypes] count]);
    }
    if ([modelPopup numberOfItems] < 2) { printf("  FAIL model list empty\n"); failures++; }
    if ([[NSApp mainMenu] numberOfItems] < 3) {
        printf("  FAIL menu bar incomplete\n");
        failures++;
    } else {
        printf("  ok   menu bar has %d menus\n", (int)[[NSApp mainMenu] numberOfItems]);
    }

    printf("\n%s\n", failures == 0 ? "all checks passed" : "CHECKS FAILED");
    return failures == 0 ? 0 : 1;
}

@end

int main(int argc, const char **argv) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

    BOOL selfCheck = NO;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--check") == 0) selfCheck = YES;

    [NSApplication sharedApplication];

    if (selfCheck) {
        // Build everything and report, without entering the run loop. This is
        // how the interface gets verified on a machine whose display is asleep.
        buildMenuBar();
        PPController *c = [[PPController alloc] init];
        int rc = [c runSelfCheck];
        [pool release];
        return rc;
    }

    // -setActivationPolicy: is 10.6 and later. On 10.5 an application inside a
    // bundle is a regular, Dock-visible application already, and one run as a
    // bare executable simply has no Dock tile.
    buildMenuBar();

    PPController *c = [[PPController alloc] init];
    [NSApp setDelegate:c];
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp run];

    [pool release];
    return 0;
}
