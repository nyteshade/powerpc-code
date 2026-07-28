#import "Providers.h"

#import "GuiBridge.h"
#import "Markdown.h"

// The same small helpers the Settings panel uses. They are duplicated rather
// than shared because they are four lines each and a header holding four lines
// of layout sugar is a worse trade than the repetition.
static NSTextField *MakeLabel(NSString *text, NSRect frame, BOOL bold) {
  NSTextField *l = [[[NSTextField alloc] initWithFrame:frame] autorelease];
  [l setStringValue:text];
  [l setBezeled:NO];
  [l setDrawsBackground:NO];
  [l setEditable:NO];
  [l setSelectable:NO];
  [l setAlignment:NSRightTextAlignment];
  [l setFont:bold ? [NSFont boldSystemFontOfSize:11.0]
                  : [NSFont systemFontOfSize:11.0]];

  return l;
}

static NSTextField *MakeNote(NSString *text, NSRect frame) {
  NSTextField *l = [[[NSTextField alloc] initWithFrame:frame] autorelease];
  [l setStringValue:text];
  [l setBezeled:NO];
  [l setDrawsBackground:NO];
  [l setEditable:NO];
  [l setSelectable:YES];
  [l setAlignment:NSLeftTextAlignment];
  [l setFont:[NSFont systemFontOfSize:10.0]];
  [l setTextColor:[NSColor darkGrayColor]];
  [[l cell] setWraps:YES];
  [[l cell] setScrollable:NO];

  return l;
}

// Reserve the next row down the pane and return its bottom edge. The same
// device as the Settings panel, and for the same reason: Cocoa rectangles grow
// upward, so a top-down layout has to move the cursor by a row's full height
// before placing anything in it. Every clipped note in the first cut of this
// window was a rectangle whose height was guessed.
static CGFloat NextRow(CGFloat *y, CGFloat height, CGFloat gap) {
  *y -= (height + gap);

  return *y;
}

// Vertical nudge so a short label sits centred against a taller control.
static CGFloat Centred(CGFloat rowBottom, CGFloat rowHeight, CGFloat itemHeight) {
  return rowBottom + (rowHeight - itemHeight) / 2.0;
}

static const CGFloat kLineH = 13.0;      // one line of 10pt note text
static const CGFloat kFieldH = 22.0;
static const CGFloat kLabelH = 18.0;

static NSButton *MakePush(NSString *title, NSRect frame, id target, SEL action) {
  NSButton *b = [[[NSButton alloc] initWithFrame:frame] autorelease];
  [b setTitle:title];
  [b setBezelStyle:NSRoundedBezelStyle];
  [b setFont:[NSFont systemFontOfSize:11.0]];
  [b setTarget:target];
  [b setAction:action];

  return b;
}

// Declared up front: GCC would otherwise assume these return id and take
// varargs, which silently miscompiles a BOOL return.
@interface PPProvidersController (Private)

- (void)buildPanel;

- (NSString *)selectedProviderId;

- (NSDictionary *)selectedProvider;

- (void)refreshDetail;

- (void)addressChanged:(id)sender;

- (void)saveKey:(id)sender;

- (void)useSelected:(id)sender;

@end

@implementation PPProvidersController

- (id)initWithBridge:(PPBridge *)b {
  if (!(self = [super init])) return nil;

  bridge = [b retain];
  providers = [[NSArray array] retain];

  return self;
}

- (void)dealloc {
  [bridge release];
  [providers release];
  [panel release];
  [super dealloc];
}

- (id)delegate { return delegate; }

// Not retained, as everywhere else here: the delegate is the application
// controller, which outlives this window.
- (void)setDelegate:(id)d { delegate = d; }

// ---------------------------------------------------------------------------

- (NSDictionary *)selectedProvider {
  NSInteger row = [table selectedRow];
  if (row < 0 || row >= (NSInteger)[providers count]) { return nil; }

  return [providers objectAtIndex:(NSUInteger)row];
}

- (NSString *)selectedProviderId {
  return [[self selectedProvider] objectForKey:@"id"];
}

- (void)reload {
  // The selection is by id, not by row: reloading must not move the pane onto
  // a different provider because the list happened to come back in another
  // order.
  NSString *wanted = [self selectedProviderId];
  if (!wanted) { wanted = [bridge providerId]; }

  [providers release];
  providers = [[bridge availableProviders] retain];
  [table reloadData];

  NSUInteger i;
  for (i = 0; i < [providers count]; i++) {
    NSDictionary *p = [providers objectAtIndex:i];

    if ([[p objectForKey:@"id"] isEqualToString:wanted]) {
      [table selectRowIndexes:[NSIndexSet indexSetWithIndex:i]
         byExtendingSelection:NO];
      break;
    }
  }

  [self refreshDetail];
}

- (void)refreshDetail {
  NSDictionary *p = [self selectedProvider];
  if (!p) {
    [heading setStringValue:@""];
    [addressField setStringValue:@""];
    [keyNote setStringValue:@""];
    [useButton setEnabled:NO];

    return;
  }

  NSString *pid = [p objectForKey:@"id"];
  BOOL inUse = [pid isEqualToString:[bridge providerId]];
  BOOL hasKey = [[p objectForKey:@"hasKey"] boolValue];
  BOOL needsKey = [[p objectForKey:@"needsKey"] boolValue];
  NSString *source = [p objectForKey:@"keySource"];

  [heading setStringValue:[p objectForKey:@"name"]];
  [addressField setStringValue:[bridge baseURLForProvider:pid]];

  [useButton setTitle:inUse ? @"In Use" : @"Use This Provider"];
  [useButton setEnabled:!inUse];

  // The key line has to answer the question someone actually has, which is
  // never "is there a key" but "why is the one I just typed not being used".
  // Naming the source answers both.
  if (hasKey) {
    NSString *where = [source length] ? source : @"the configuration";
    NSString *text = [NSString stringWithFormat:@"A key is set, read from %@.",
                               where];

    if ([source hasPrefix:@"environment"]) {
      text = [text stringByAppendingString:
          PPUTF8(" An exported variable wins over a saved one \xE2\x80\x94 "
                 "unset it in the shell if you want the key below used "
                 "instead.")];
    }

    [keyNote setStringValue:text];
    [[keyNote cell] setTextColor:[NSColor darkGrayColor]];
  }

  else if (needsKey) {
    [keyNote setStringValue:
        PPUTF8("No key \xE2\x80\x94 this provider cannot answer until one is "
               "saved.")];
    [[keyNote cell] setTextColor:[NSColor redColor]];
  }

  else {
    [keyNote setStringValue:
        @"No key needed. Save one only if this server sits behind something "
         "that wants authenticating."];
    [[keyNote cell] setTextColor:[NSColor darkGrayColor]];
  }
}

- (NSWindow *)panelWindow {
  [self buildPanel];
  [self reload];

  return panel;
}

- (void)showWindow {
  [self buildPanel];
  [self reload];
  [panel makeKeyAndOrderFront:nil];
}

- (void)buildPanel {
  if (panel) { return; }

  NSRect frame = NSMakeRect(0, 0, 580, 420);
  panel = [[NSWindow alloc]
      initWithContentRect:frame
                styleMask:NSTitledWindowMask | NSClosableWindowMask
                  backing:NSBackingStoreBuffered
                    defer:NO];
  [panel setTitle:@"Providers"];
  [panel setFrameAutosaveName:@"PPCodeProviders"];

  NSView *content = [panel contentView];

  // --- the list -------------------------------------------------------------
  NSScrollView *scroll = [[[NSScrollView alloc]
      initWithFrame:NSMakeRect(20, 60, 180, 336)] autorelease];
  [scroll setHasVerticalScroller:YES];
  [scroll setBorderType:NSBezelBorder];

  table = [[[NSTableView alloc]
      initWithFrame:NSMakeRect(0, 0, 178, 336)] autorelease];

  struct { NSString *ident; NSString *title; CGFloat width; } cols[] = {
      {@"name",  @"Provider", 112},
      {@"state", @"",          50},
  };
  for (unsigned i = 0; i < sizeof(cols) / sizeof(cols[0]); i++) {
    NSTableColumn *c =
        [[[NSTableColumn alloc] initWithIdentifier:cols[i].ident] autorelease];
    [[c headerCell] setStringValue:cols[i].title];
    [c setWidth:cols[i].width];
    [c setEditable:NO];
    [table addTableColumn:c];
  }

  [table setDataSource:self];
  [table setDelegate:self];
  [table setRowHeight:18.0];
  [table setAllowsMultipleSelection:NO];
  [table setTarget:self];
  [table setDoubleAction:@selector(useSelected:)];
  [scroll setDocumentView:table];
  [content addSubview:scroll];

  // --- the detail pane ------------------------------------------------------
  //
  // Laid out top-down from the same line the list starts at, with the notes
  // given the height their longest text actually needs: the key note has to
  // hold four lines, because the case it exists for -- a key coming from an
  // environment variable rather than the field below it -- is the wordiest.
  CGFloat y = 396;
  CGFloat r;
  const CGFloat kDetailX = 216, kFieldX = 286, kFieldW = 274;

  r = NextRow(&y, 20, 0);
  heading = [[[NSTextField alloc]
      initWithFrame:NSMakeRect(kDetailX, r, 344, 20)] autorelease];
  [heading setBezeled:NO];
  [heading setDrawsBackground:NO];
  [heading setEditable:NO];
  [heading setSelectable:NO];
  [heading setFont:[NSFont boldSystemFontOfSize:13.0]];
  [content addSubview:heading];

  r = NextRow(&y, kFieldH, 12);
  [content addSubview:MakeLabel(@"Address:",
                                NSMakeRect(kDetailX, Centred(r, kFieldH, kLabelH),
                                           64, kLabelH),
                                NO)];
  addressField = [[[NSTextField alloc]
      initWithFrame:NSMakeRect(kFieldX, r, kFieldW, kFieldH)] autorelease];
  [addressField setTarget:self];
  [addressField setAction:@selector(addressChanged:)];
  // Clicking away is as much a commit as pressing Return. Without this, an
  // address typed and then abandoned for the Use button was silently lost.
  [[addressField cell] setSendsActionOnEndEditing:YES];
  [content addSubview:addressField];

  r = NextRow(&y, 3 * kLineH, 4);
  addressNote = MakeNote(
      @"Blank restores the built-in address. LM Studio cannot run on this "
       "machine, so its address is always another one on the network.",
      NSMakeRect(kFieldX, r, kFieldW, 3 * kLineH));
  [content addSubview:addressNote];

  r = NextRow(&y, kFieldH, 12);
  [content addSubview:MakeLabel(@"Key:",
                                NSMakeRect(kDetailX, Centred(r, kFieldH, kLabelH),
                                           64, kLabelH),
                                NO)];
  keyField = [[[NSSecureTextField alloc]
      initWithFrame:NSMakeRect(kFieldX, r, 190, kFieldH)] autorelease];
  [keyField setTarget:self];
  [keyField setAction:@selector(saveKey:)];
  [content addSubview:keyField];

  [content addSubview:
      MakePush(@"Save", NSMakeRect(482, r - 2, 78, 26), self, @selector(saveKey:))];

  r = NextRow(&y, 4 * kLineH, 6);
  keyNote = MakeNote(@"", NSMakeRect(kFieldX, r, kFieldW, 4 * kLineH));
  [content addSubview:keyNote];

  r = NextRow(&y, 3 * kLineH, 10);
  [content addSubview:MakeNote(
      @"A saved key goes to ~/.local/keys, readable only by you. It is the "
       "same file the command line tool reads.",
      NSMakeRect(kFieldX, r, kFieldW, 3 * kLineH))];

  // --- buttons --------------------------------------------------------------
  //
  // Deliberately not the default button: Return belongs to whichever field is
  // being edited, and making a provider switch the thing Return does while
  // someone is typing an address is a trap.
  useButton = MakePush(@"Use This Provider", NSMakeRect(400, 16, 160, 28),
                       self, @selector(useSelected:));
  [content addSubview:useButton];

  [content addSubview:
      MakePush(@"Close", NSMakeRect(308, 16, 84, 28), panel,
               @selector(performClose:))];

  [panel center];
}

// ---------------------------------------------------------------------------

- (void)addressChanged:(id)sender {
  NSString *pid = [self selectedProviderId];
  if (!pid) { return; }

  if (![bridge setBaseURL:[addressField stringValue] forProvider:pid]) {
    NSAlert *a = [[[NSAlert alloc] init] autorelease];
    [a setMessageText:@"Could not save the address."];
    [a setInformativeText:@"A turn is running. Finish it and try again."];
    [a runModal];
  }

  [self reload];
}

- (void)saveKey:(id)sender {
  NSString *pid = [self selectedProviderId];
  if (!pid) { return; }

  NSString *key = [keyField stringValue];
  if (![key length]) { return; }

  NSString *err = nil;
  if (![bridge saveKey:key forProvider:pid error:&err]) {
    NSAlert *a = [[[NSAlert alloc] init] autorelease];
    [a setMessageText:@"Could not save the key."];
    [a setInformativeText:err ? err : @"Unknown error."];
    [a runModal];

    return;
  }

  // Cleared on success. The field is a place to put a key, not a place to read
  // one back: what is in force is described by the note underneath.
  [keyField setStringValue:@""];
  [self reload];
}

- (void)useSelected:(id)sender {
  NSString *pid = [self selectedProviderId];
  if (!pid || [pid isEqualToString:[bridge providerId]]) { return; }

  if (![bridge setProviderId:pid]) {
    NSAlert *a = [[[NSAlert alloc] init] autorelease];
    [a setMessageText:@"Could not switch provider."];
    [a setInformativeText:
        @"A turn is running, or the provider is not one ppcode knows. Finish "
         "the turn and try again."];
    [a runModal];

    return;
  }

  [self reload];

  // The model list belongs to the old service and means nothing to the new one,
  // so whoever is showing it has to be told.
  if ([delegate respondsToSelector:@selector(providersDidChangeProvider)])
      [delegate providersDidChangeProvider];
}

// --- table ------------------------------------------------------------------

- (NSInteger)numberOfRowsInTableView:(NSTableView *)t {
  return (NSInteger)[providers count];
}

- (id)tableView:(NSTableView *)t
    objectValueForTableColumn:(NSTableColumn *)c
                          row:(NSInteger)row {
  if (row < 0 || row >= (NSInteger)[providers count]) { return @""; }

  NSDictionary *p = [providers objectAtIndex:(NSUInteger)row];
  if ([[c identifier] isEqualToString:@"state"]) {
    return [[p objectForKey:@"id"] isEqualToString:[bridge providerId]]
        ? @"in use"
        : @"";
  }

  return [p objectForKey:@"name"];
}

- (void)tableViewSelectionDidChange:(NSNotification *)note {
  [self refreshDetail];
}

@end
