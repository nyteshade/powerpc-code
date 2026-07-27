#import "Library.h"

#import "GuiBridge.h"
#import "Markdown.h"
#import "Skin.h"

// Declared up front: several of these are used before they are defined, and
// GCC would otherwise assume they return id and take varargs.
@interface PPLibraryController (Private)

- (void)buildPanel;

- (void)filterChanged:(id)sender;

- (void)removeSelected:(id)sender;

- (void)clearEverything:(id)sender;

- (void)updateSummary;

- (NSArray *)visibleDocuments;

@end

@implementation PPLibraryController

- (id)initWithBridge:(PPBridge *)b {
  if (!(self = [super init])) return nil;

  bridge = [b retain];
  filter = [@"" retain];
  documents = [[NSArray array] retain];

  return self;
}

- (void)dealloc {
  [bridge release];
  [filter release];
  [documents release];
  [panel release];
  [super dealloc];
}

// ---------------------------------------------------------------------------

- (NSArray *)visibleDocuments {
  if (![filter length]) { return documents; }

  NSMutableArray *out = [NSMutableArray array];
  NSEnumerator *e = [documents objectEnumerator];
  NSDictionary *d;
  while ((d = [e nextObject]) != nil) {
    if ([[d objectForKey:@"collection"] isEqualToString:filter])
        [out addObject:d];
  }

  return out;
}

- (void)reload {
  [documents release];
  documents = [[bridge indexedDocuments] retain];
  [table reloadData];
  [self updateSummary];
}

- (void)updateSummary {
  NSDictionary *s = [bridge indexStatistics];
  long long chunks = [[s objectForKey:@"chunks"] longLongValue];
  long long embedded = [[s objectForKey:@"embedded"] longLongValue];

  NSString *text = [NSString stringWithFormat:
      PPUTF8("%lu documents \xC2\xB7 %lld chunks"),
      (unsigned long)[[self visibleDocuments] count], chunks];

  // Only mention embeddings once there are some; until then it is a detail
  // about a feature that does not exist yet from the user's point of view.
  if (embedded > 0) {
    text = [text stringByAppendingFormat:PPUTF8(" \xC2\xB7 %lld embedded"), embedded];
  }

  [summary setStringValue:text];
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

  NSRect frame = NSMakeRect(0, 0, 620, 420);
  panel = [[NSWindow alloc]
      initWithContentRect:frame
                styleMask:NSTitledWindowMask | NSClosableWindowMask |
                          NSMiniaturizableWindowMask | NSResizableWindowMask
                  backing:NSBackingStoreBuffered
                    defer:NO];
  [panel setTitle:@"Indexed Content"];
  [panel setFrameAutosaveName:@"PPCodeLibrary"];
  [panel setMinSize:NSMakeSize(460, 280)];

  NSView *content = [panel contentView];

  // --- filter -------------------------------------------------------------
  NSTextField *filterLabel = [[[NSTextField alloc]
      initWithFrame:NSMakeRect(20, 380, 50, 18)] autorelease];
  [filterLabel setStringValue:@"Show:"];
  [filterLabel setBezeled:NO];
  [filterLabel setDrawsBackground:NO];
  [filterLabel setEditable:NO];
  [filterLabel setSelectable:NO];
  [filterLabel setFont:[NSFont systemFontOfSize:11.0]];
  [filterLabel setAutoresizingMask:NSViewMinYMargin];
  [content addSubview:filterLabel];

  filterPopup = [[[NSPopUpButton alloc]
      initWithFrame:NSMakeRect(72, 376, 200, 24)] autorelease];
  [filterPopup addItemWithTitle:@"Everything"];
  [filterPopup addItemWithTitle:@"Conversations"];
  [filterPopup addItemWithTitle:@"Platform notes"];
  [filterPopup addItemWithTitle:@"Reference documents"];
  [filterPopup setTarget:self];
  [filterPopup setAction:@selector(filterChanged:)];
  [filterPopup setAutoresizingMask:NSViewMinYMargin];
  [content addSubview:filterPopup];

  // --- the list -----------------------------------------------------------
  NSScrollView *scroll = [[[NSScrollView alloc]
      initWithFrame:NSMakeRect(20, 60, 580, 306)] autorelease];
  [scroll setHasVerticalScroller:YES];
  [scroll setBorderType:NSBezelBorder];
  [scroll setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

  table = [[[NSTableView alloc]
      initWithFrame:NSMakeRect(0, 0, 578, 306)] autorelease];

  struct { NSString *ident; NSString *title; CGFloat width; } cols[] = {
      {@"displayName", @"Document",   320},
      {@"collection",  @"Collection", 150},
      {@"chunks",      @"Chunks",      70},
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
  [table setUsesAlternatingRowBackgroundColors:YES];
  [table setRowHeight:18.0];
  [table setAllowsMultipleSelection:YES];
  [scroll setDocumentView:table];
  [content addSubview:scroll];

  // --- footer -------------------------------------------------------------
  summary = [[[NSTextField alloc]
      initWithFrame:NSMakeRect(20, 22, 300, 18)] autorelease];
  [summary setBezeled:NO];
  [summary setDrawsBackground:NO];
  [summary setEditable:NO];
  [summary setSelectable:YES];
  [summary setFont:[NSFont systemFontOfSize:10.0]];
  [summary setTextColor:[NSColor darkGrayColor]];
  [summary setAutoresizingMask:NSViewMaxYMargin];
  [content addSubview:summary];

  removeButton = [[[NSButton alloc]
      initWithFrame:NSMakeRect(600 - 90, 16, 90, 28)] autorelease];
  [removeButton setTitle:@"Remove"];
  [removeButton setBezelStyle:NSRoundedBezelStyle];
  [removeButton setFont:[NSFont systemFontOfSize:11.0]];
  [removeButton setTarget:self];
  [removeButton setAction:@selector(removeSelected:)];
  [removeButton setAutoresizingMask:NSViewMinXMargin | NSViewMaxYMargin];
  [removeButton setEnabled:NO];
  [content addSubview:removeButton];

  NSButton *clear = [[[NSButton alloc]
      initWithFrame:NSMakeRect(600 - 90 - 8 - 110, 16, 110, 28)] autorelease];
  [clear setTitle:PPUTF8("Empty Index\xE2\x80\xA6")];
  [clear setBezelStyle:NSRoundedBezelStyle];
  [clear setFont:[NSFont systemFontOfSize:11.0]];
  [clear setTarget:self];
  [clear setAction:@selector(clearEverything:)];
  [clear setAutoresizingMask:NSViewMinXMargin | NSViewMaxYMargin];
  [content addSubview:clear];

  [panel center];
}

// ---------------------------------------------------------------------------

- (void)filterChanged:(id)sender {
  [filter release];

  switch ([filterPopup indexOfSelectedItem]) {

    // Conversations
    case 1:
      filter = [@"conversations" retain];
      break;

    // The platform notes shipped with ppcode
    case 2:
      filter = [@"knowledge" retain];
      break;

    // Books and articles added by hand
    case 3:
      filter = [@"reference" retain];
      break;

    // Everything
    default:
      filter = [@"" retain];
      break;
  }

  [table reloadData];
  [self updateSummary];
}

- (void)removeSelected:(id)sender {
  NSArray *visible = [self visibleDocuments];
  NSIndexSet *rows = [table selectedRowIndexes];
  if ([rows count] == 0) { return; }

  NSAlert *alert = [[[NSAlert alloc] init] autorelease];
  [alert setMessageText:
      [NSString stringWithFormat:@"Remove %lu item%@ from the index?",
          (unsigned long)[rows count], [rows count] == 1 ? @"" : @"s"]];
  [alert setInformativeText:
      @"Only the index entry is removed. Conversations and files on disk are "
       "left alone, and can be indexed again."];
  [alert addButtonWithTitle:@"Remove"];
  [alert addButtonWithTitle:@"Cancel"];
  [alert setAlertStyle:NSWarningAlertStyle];
  if ([alert runModal] != NSAlertFirstButtonReturn) { return; }

  // Collected before removing anything: the indexes refer to the list as it is
  // now, and removing shifts everything after each one.
  NSMutableArray *ids = [NSMutableArray array];
  NSUInteger row = [rows firstIndex];
  while (row != NSNotFound) {
    if (row < [visible count])
        [ids addObject:[[visible objectAtIndex:row] objectForKey:@"docId"]];
    row = [rows indexGreaterThanIndex:row];
  }

  NSEnumerator *e = [ids objectEnumerator];
  NSString *docId;
  while ((docId = [e nextObject]) != nil) [bridge removeIndexedDocument:docId];

  [self reload];
}

- (void)clearEverything:(id)sender {
  NSAlert *alert = [[[NSAlert alloc] init] autorelease];
  [alert setMessageText:@"Empty the whole index?"];
  [alert setInformativeText:
      @"Everything indexed is removed. Nothing else is deleted: conversations "
       "and documents stay on disk, and the index can be rebuilt from them."];
  [alert addButtonWithTitle:@"Empty"];
  [alert addButtonWithTitle:@"Cancel"];
  [alert setAlertStyle:NSCriticalAlertStyle];
  if ([alert runModal] != NSAlertFirstButtonReturn) { return; }

  [bridge clearIndex];
  [self reload];
}

// --- table ------------------------------------------------------------------

- (NSInteger)numberOfRowsInTableView:(NSTableView *)t {
  return (NSInteger)[[self visibleDocuments] count];
}

- (id)tableView:(NSTableView *)t
    objectValueForTableColumn:(NSTableColumn *)c
                          row:(NSInteger)row {
  NSArray *visible = [self visibleDocuments];
  if (row < 0 || row >= (NSInteger)[visible count]) { return @""; }

  NSDictionary *d = [visible objectAtIndex:(NSUInteger)row];
  NSString *ident = [c identifier];

  if ([ident isEqualToString:@"chunks"]) {
    return [[d objectForKey:@"chunks"] stringValue];
  }

  if ([ident isEqualToString:@"collection"]) {
    NSString *raw = [d objectForKey:@"collection"];
    if ([raw isEqualToString:@"conversations"]) { return @"Conversation"; }
    if ([raw isEqualToString:@"knowledge"]) { return @"Platform note"; }
    if ([raw isEqualToString:@"reference"]) { return @"Reference"; }

    return raw;
  }

  return [d objectForKey:ident];
}

- (void)tableViewSelectionDidChange:(NSNotification *)note {
  [removeButton setEnabled:([[table selectedRowIndexes] count] > 0)];
}

@end
