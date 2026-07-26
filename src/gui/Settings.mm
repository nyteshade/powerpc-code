#import "Settings.h"

#import "GuiBridge.h"
#import "Markdown.h"
#import "Skin.h"

// ---------------------------------------------------------------------------
// Small layout helpers. Aqua has real metrics -- 20px window margins, 8px
// between related controls, 12px between groups, labels right-aligned to a
// colon -- and following them is most of what makes a panel look native.
// ---------------------------------------------------------------------------

// GCC's Objective-C++ front end cannot parse the GNU elvis operator when the
// right-hand side is an Objective-C literal -- `x ?: @""` is reported as a
// stray '@'. These helpers stand in for it.
static NSString *Or(id value, NSString *fallback) {
  if (value == nil || [value isKindOfClass:[NSNull class]]) {
    return fallback;
  }

  return (NSString *)value;
}

static id OrObj(id value, id fallback) {
  if (value == nil || [value isKindOfClass:[NSNull class]]) {
    return fallback;
  }

  return value;
}

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
  // Without this a note is laid out as a single line and simply runs off the
  // right edge of the pane, which is how the longer ones came to be clipped.
  [[l cell] setWraps:YES];
  [[l cell] setScrollable:NO];

  return l;
}

// A left-aligned bold heading. MakeLabel right-aligns, which is right for a
// field label and wrong for a section title.
static NSTextField *MakeSection(NSString *text, NSRect frame) {
  NSTextField *l = MakeLabel(text, frame, YES);
  [l setAlignment:NSLeftTextAlignment];

  return l;
}

// Reserve the next row going down the pane and return its *bottom* edge.
//
// Cocoa rectangles grow upward from their origin, so a top-down layout must
// move the cursor down by the full height of a row before placing anything in
// it. Doing that by hand is what put the descriptions on top of the text
// fields: the cursor was moved 24 points but a 32-point note was then drawn
// from there, so it grew back up through the field above. Going through this
// function makes the mistake hard to repeat.
static CGFloat NextRow(CGFloat *y, CGFloat height, CGFloat gap) {
  *y -= (height + gap);

  return *y;
}

// Vertical nudge so an 18-point label sits centred against a taller control.
static CGFloat Centred(CGFloat rowBottom, CGFloat rowHeight, CGFloat itemHeight) {
  return rowBottom + (rowHeight - itemHeight) / 2.0;
}

static const CGFloat kPaneTop = 288.0;
static const CGFloat kFieldH = 22.0;
static const CGFloat kLabelH = 18.0;
static const CGFloat kPopupH = 24.0;
static const CGFloat kLineH = 14.0;     // one line of note text

static NSButton *MakeCheck(NSString *title, NSRect frame, id target, SEL action) {
  NSButton *b = [[[NSButton alloc] initWithFrame:frame] autorelease];
  [b setButtonType:NSSwitchButton];
  [b setTitle:title];
  [b setFont:[NSFont systemFontOfSize:11.0]];
  [b setTarget:target];
  [b setAction:action];

  return b;
}

static NSButton *MakePush(NSString *title, NSRect frame, id target, SEL action) {
  NSButton *b = [[[NSButton alloc] initWithFrame:frame] autorelease];
  [b setTitle:title];
  [b setBezelStyle:NSRoundedBezelStyle];   // the glossy Aqua capsule
  [b setFont:[NSFont systemFontOfSize:11.0]];
  [b setTarget:target];
  [b setAction:action];

  return b;
}

// Declared up front because the tab builders are used through performSelector:
// and -buildPanel is defined below its caller.
@interface PPSettingsController (Private)

- (void)buildPanel;

- (void)load:(id)sender;

- (NSView *)buildKeysTab;

- (NSView *)buildModelTab;

- (NSView *)buildRoutingTab;

- (NSView *)buildMcpTab;

- (NSView *)buildToolsTab;

@end

@implementation PPSettingsController

- (id)initWithBridge:(PPBridge *)b {
  if (!(self = [super init])) return nil;
  bridge = [b retain];
  mcpServers = [[NSMutableArray alloc] init];

  return self;
}

- (void)dealloc {
  [bridge release];
  [mcpServers release];
  [panel release];
  [super dealloc];
}

// ---------------------------------------------------------------------------

- (NSView *)buildKeysTab {
  NSView *v = [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 520, 300)] autorelease];
  CGFloat y = kPaneTop;
  CGFloat r;

  r = NextRow(&y, kFieldH, 0);
  [v addSubview:MakeLabel(@"OpenRouter key:",
                          NSMakeRect(20, Centred(r, kFieldH, kLabelH), 120, kLabelH),
                          YES)];
  openrouterKey = [[[NSSecureTextField alloc]
      initWithFrame:NSMakeRect(148, r, 340, kFieldH)] autorelease];
  [v addSubview:openrouterKey];

  r = NextRow(&y, 2 * kLineH, 4);
  [v addSubview:MakeNote(@"Required. Get one at openrouter.ai/keys. Stored in "
                          "your config file with owner-only permissions.",
                         NSMakeRect(148, r, 350, 2 * kLineH))];

  r = NextRow(&y, kLineH, 6);
  keyStatus = MakeNote(@"", NSMakeRect(148, r, 350, kLineH));
  [v addSubview:keyStatus];

  r = NextRow(&y, 3 * kLineH, 14);
  [v addSubview:MakeNote(@"An application launched from the Finder does not "
                          "inherit your shell environment, so a key exported in "
                          ".zshrc is not visible here. That is why it is set in "
                          "this window instead.",
                         NSMakeRect(20, r, 470, 3 * kLineH))];

  r = NextRow(&y, kLabelH, 16);
  [v addSubview:MakeSection(@"Web search keys", NSMakeRect(20, r, 200, kLabelH))];

  r = NextRow(&y, kLineH, 2);
  [v addSubview:MakeNote(@"Optional. Without one, web_search falls back to "
                          "Wikipedia and instant answers.",
                         NSMakeRect(20, r, 470, kLineH))];

  r = NextRow(&y, kFieldH, 8);
  [v addSubview:MakeLabel(@"Tavily:",
                          NSMakeRect(20, Centred(r, kFieldH, kLabelH), 120, kLabelH),
                          NO)];
  tavilyKey = [[[NSSecureTextField alloc]
      initWithFrame:NSMakeRect(148, r, 340, kFieldH)] autorelease];
  [v addSubview:tavilyKey];

  r = NextRow(&y, kFieldH, 6);
  [v addSubview:MakeLabel(@"Brave:",
                          NSMakeRect(20, Centred(r, kFieldH, kLabelH), 120, kLabelH),
                          NO)];
  braveKey = [[[NSSecureTextField alloc]
      initWithFrame:NSMakeRect(148, r, 340, kFieldH)] autorelease];
  [v addSubview:braveKey];

  return v;
}

- (NSView *)buildModelTab {
  NSView *v = [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 520, 300)] autorelease];
  CGFloat y = kPaneTop;
  CGFloat r;

  r = NextRow(&y, kPopupH, 0);
  [v addSubview:MakeLabel(@"Default model:",
                          NSMakeRect(20, Centred(r, kPopupH, kLabelH), 120, kLabelH),
                          YES)];
  defaultModel = [[[NSPopUpButton alloc]
      initWithFrame:NSMakeRect(146, r, 344, kPopupH)] autorelease];
  [v addSubview:defaultModel];

  r = NextRow(&y, 20, 12);
  [v addSubview:MakeLabel(@"Temperature:",
                          NSMakeRect(20, Centred(r, 20, kLabelH), 120, kLabelH), NO)];
  temperatureSlider = [[[NSSlider alloc]
      initWithFrame:NSMakeRect(148, r, 250, 20)] autorelease];
  [temperatureSlider setMinValue:0.0];
  [temperatureSlider setMaxValue:2.0];
  [temperatureSlider setTarget:self];
  [temperatureSlider setAction:@selector(temperatureChanged:)];
  [v addSubview:temperatureSlider];
  temperatureLabel = MakeNote(@"1.00",
                              NSMakeRect(406, Centred(r, 20, kLineH), 60, kLineH));
  [v addSubview:temperatureLabel];

  r = NextRow(&y, kFieldH, 10);
  [v addSubview:MakeLabel(@"Max tokens:",
                          NSMakeRect(20, Centred(r, kFieldH, kLabelH), 120, kLabelH),
                          NO)];
  maxTokensField = [[[NSTextField alloc]
      initWithFrame:NSMakeRect(148, r, 90, kFieldH)] autorelease];
  [v addSubview:maxTokensField];
  [v addSubview:MakeNote(@"per reply",
                         NSMakeRect(244, Centred(r, kFieldH, kLineH), 200, kLineH))];

  r = NextRow(&y, kFieldH, 8);
  [v addSubview:MakeLabel(@"Max rounds:",
                          NSMakeRect(20, Centred(r, kFieldH, kLabelH), 120, kLabelH),
                          NO)];
  maxTurnsField = [[[NSTextField alloc]
      initWithFrame:NSMakeRect(148, r, 90, kFieldH)] autorelease];
  [v addSubview:maxTurnsField];
  [v addSubview:MakeNote(@"tool calls before stopping",
                         NSMakeRect(244, Centred(r, kFieldH, kLineH), 240, kLineH))];

  r = NextRow(&y, kFieldH, 8);
  [v addSubview:MakeLabel(@"Spend limit:",
                          NSMakeRect(20, Centred(r, kFieldH, kLabelH), 120, kLabelH),
                          NO)];
  maxCostField = [[[NSTextField alloc]
      initWithFrame:NSMakeRect(148, r, 90, kFieldH)] autorelease];
  [v addSubview:maxCostField];
  [v addSubview:MakeNote(@"US dollars per session; 0 disables it",
                         NSMakeRect(244, Centred(r, kFieldH, kLineH), 250, kLineH))];

  return v;
}

- (NSView *)buildRoutingTab {
  NSView *v = [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 520, 300)] autorelease];
  CGFloat y = kPaneTop;
  CGFloat r;

  r = NextRow(&y, kPopupH, 0);
  [v addSubview:MakeLabel(@"Prefer providers by:",
                          NSMakeRect(20, Centred(r, kPopupH, kLabelH), 140, kLabelH),
                          YES)];
  providerSort = [[[NSPopUpButton alloc]
      initWithFrame:NSMakeRect(166, r, 160, kPopupH)] autorelease];
  [providerSort addItemWithTitle:@"(no preference)"];
  [providerSort addItemWithTitle:@"price"];
  [providerSort addItemWithTitle:@"throughput"];
  [providerSort addItemWithTitle:@"latency"];
  [v addSubview:providerSort];

  r = NextRow(&y, kLabelH, 10);
  allowFallbacks = MakeCheck(@"Allow falling back to other providers",
                             NSMakeRect(166, r, 320, kLabelH), nil, NULL);
  [v addSubview:allowFallbacks];

  r = NextRow(&y, kLabelH, 4);
  denyTraining = MakeCheck(@"Refuse providers that train on my data",
                           NSMakeRect(166, r, 320, kLabelH), nil, NULL);
  [v addSubview:denyTraining];

  r = NextRow(&y, kPopupH, 14);
  [v addSubview:MakeLabel(@"Reasoning effort:",
                          NSMakeRect(20, Centred(r, kPopupH, kLabelH), 140, kLabelH),
                          YES)];
  reasoningEffort = [[[NSPopUpButton alloc]
      initWithFrame:NSMakeRect(166, r, 160, kPopupH)] autorelease];
  [reasoningEffort addItemWithTitle:@"(model default)"];
  [reasoningEffort addItemWithTitle:@"minimal"];
  [reasoningEffort addItemWithTitle:@"low"];
  [reasoningEffort addItemWithTitle:@"medium"];
  [reasoningEffort addItemWithTitle:@"high"];
  [v addSubview:reasoningEffort];

  r = NextRow(&y, kLabelH, 14);
  promptCaching = MakeCheck(@"Cache the system prompt where supported",
                            NSMakeRect(166, r, 340, kLabelH), nil, NULL);
  [v addSubview:promptCaching];

  r = NextRow(&y, 2 * kLineH, 1);
  [v addSubview:MakeNote(@"Measured roughly 30% cheaper on a two-round task, "
                          "and more on longer ones.",
                         NSMakeRect(184, r, 320, 2 * kLineH))];

  r = NextRow(&y, kLabelH, 12);
  webPlugin = MakeCheck(@"Let the model search the web (OpenRouter plugin)",
                        NSMakeRect(166, r, 340, kLabelH), nil, NULL);
  [v addSubview:webPlugin];

  r = NextRow(&y, kLineH, 1);
  [v addSubview:MakeNote(@"Billed per search by OpenRouter. Needs no extra key.",
                         NSMakeRect(184, r, 320, kLineH))];

  return v;
}

- (NSView *)buildMcpTab {
  NSView *v = [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 520, 300)] autorelease];

  CGFloat y = kPaneTop;
  CGFloat r = NextRow(&y, 92, 0);

  NSScrollView *scroll = [[[NSScrollView alloc]
      initWithFrame:NSMakeRect(20, r, 470, 92)] autorelease];
  [scroll setHasVerticalScroller:YES];
  [scroll setBorderType:NSBezelBorder];

  mcpTable = [[[NSTableView alloc]
      initWithFrame:NSMakeRect(0, 0, 450, 92)] autorelease];
  NSTableColumn *c1 = [[[NSTableColumn alloc] initWithIdentifier:@"name"] autorelease];
  [[c1 headerCell] setStringValue:@"Server"];
  [c1 setWidth:120];
  NSTableColumn *c2 = [[[NSTableColumn alloc] initWithIdentifier:@"detail"] autorelease];
  [[c2 headerCell] setStringValue:@"Transport and address"];
  [c2 setWidth:320];
  [mcpTable addTableColumn:c1];
  [mcpTable addTableColumn:c2];
  [mcpTable setDataSource:self];
  [mcpTable setDelegate:self];
  [mcpTable setUsesAlternatingRowBackgroundColors:YES];
  [scroll setDocumentView:mcpTable];
  [v addSubview:scroll];

  r = NextRow(&y, kPopupH, 10);
  [v addSubview:MakeLabel(@"Name:",
                          NSMakeRect(20, Centred(r, kPopupH, kLabelH), 90, kLabelH),
                          NO)];
  mcpName = [[[NSTextField alloc]
      initWithFrame:NSMakeRect(116, Centred(r, kPopupH, kFieldH), 130, kFieldH)]
                autorelease];
  [v addSubview:mcpName];

  [v addSubview:MakeLabel(@"Transport:",
                          NSMakeRect(252, Centred(r, kPopupH, kLabelH), 70, kLabelH),
                          NO)];
  mcpTransport = [[[NSPopUpButton alloc]
      initWithFrame:NSMakeRect(328, r, 100, kPopupH)] autorelease];
  [mcpTransport addItemWithTitle:@"http"];
  [mcpTransport addItemWithTitle:@"stdio"];
  [mcpTransport setTarget:self];
  [mcpTransport setAction:@selector(transportChanged:)];
  [v addSubview:mcpTransport];

  r = NextRow(&y, kFieldH, 8);
  [v addSubview:MakeLabel(@"URL:",
                          NSMakeRect(20, Centred(r, kFieldH, kLabelH), 90, kLabelH),
                          NO)];
  mcpCommandOrUrl = [[[NSTextField alloc]
      initWithFrame:NSMakeRect(116, r, 374, kFieldH)] autorelease];
  [v addSubview:mcpCommandOrUrl];

  r = NextRow(&y, kFieldH, 8);
  [v addSubview:MakeLabel(@"Arguments:",
                          NSMakeRect(20, Centred(r, kFieldH, kLabelH), 90, kLabelH),
                          NO)];
  mcpArgs = [[[NSTextField alloc]
      initWithFrame:NSMakeRect(116, r, 374, kFieldH)] autorelease];
  [v addSubview:mcpArgs];

  r = NextRow(&y, kFieldH, 8);
  [v addSubview:MakeLabel(@"Header:",
                          NSMakeRect(20, Centred(r, kFieldH, kLabelH), 90, kLabelH),
                          NO)];
  mcpHeaderName = [[[NSTextField alloc]
      initWithFrame:NSMakeRect(116, r, 130, kFieldH)] autorelease];
  [mcpHeaderName setStringValue:@"Authorization"];
  [v addSubview:mcpHeaderName];
  mcpHeaderValue = [[[NSSecureTextField alloc]
      initWithFrame:NSMakeRect(252, r, 238, kFieldH)] autorelease];
  [[mcpHeaderValue cell] setPlaceholderString:@"Bearer ..."];
  [v addSubview:mcpHeaderValue];

  r = NextRow(&y, 2 * kLineH, 2);
  [v addSubview:MakeNote(@"For a token-protected server, leave the header name "
                          "as Authorization and paste \"Bearer <token>\".",
                         NSMakeRect(116, r, 380, 2 * kLineH))];

  r = NextRow(&y, 26, 8);
  mcpEnabled = MakeCheck(@"Enabled",
                         NSMakeRect(116, Centred(r, 26, kLabelH), 90, kLabelH),
                         nil, NULL);
  [mcpEnabled setState:NSOnState];
  [v addSubview:mcpEnabled];

  [v addSubview:MakePush(@"Add / Update", NSMakeRect(232, r, 110, 26), self,
                         @selector(addMcp:))];
  [v addSubview:MakePush(@"Remove", NSMakeRect(348, r, 80, 26), self,
                         @selector(removeMcp:))];
  [v addSubview:MakePush(@"Test", NSMakeRect(434, r, 56, 26), self,
                         @selector(testMcp:))];

  r = NextRow(&y, kLineH, 6);
  mcpStatus = MakeNote(@"", NSMakeRect(20, r, 470, kLineH));
  [v addSubview:mcpStatus];

  return v;
}

- (NSView *)buildToolsTab {
  NSView *v = [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 520, 300)] autorelease];
  CGFloat y = kPaneTop;
  CGFloat r;

  r = NextRow(&y, kLabelH, 0);
  [v addSubview:MakeSection(@"Command line tool", NSMakeRect(20, r, 200, kLabelH))];

  r = NextRow(&y, 2 * kLineH, 4);
  cliStatus = MakeNote(@"", NSMakeRect(20, r, 470, 2 * kLineH));
  [v addSubview:cliStatus];

  r = NextRow(&y, 26, 12);
  [v addSubview:MakeLabel(@"Install to:",
                          NSMakeRect(20, Centred(r, 26, kLabelH), 90, kLabelH), NO)];
  cliLocation = [[[NSPopUpButton alloc]
      initWithFrame:NSMakeRect(116, Centred(r, 26, kPopupH), 220, kPopupH)]
                    autorelease];
  [cliLocation addItemWithTitle:
      [NSHomeDirectory() stringByAppendingPathComponent:@"bin"]];
  [cliLocation addItemWithTitle:@"/usr/local/bin"];
  [v addSubview:cliLocation];
  [v addSubview:MakePush(@"Install", NSMakeRect(346, r, 90, 26), self,
                         @selector(installCLI:))];

  r = NextRow(&y, kLineH, 6);
  [v addSubview:MakeNote(@"Adds the ppcode command to that directory. Make sure "
                          "it is on your PATH.",
                         NSMakeRect(116, r, 380, kLineH))];

  r = NextRow(&y, kLabelH, 20);
  [v addSubview:MakeSection(@"MacPorts", NSMakeRect(20, r, 200, kLabelH))];

  r = NextRow(&y, 3 * kLineH, 4);
  portStatus = MakeNote(@"", NSMakeRect(20, r, 470, 3 * kLineH));
  [v addSubview:portStatus];

  return v;
}

// ---------------------------------------------------------------------------

- (NSWindow *)panelWindow {
  [self buildPanel];
  [self load:nil];

  return panel;
}

- (void)showWindow {
  [self buildPanel];
  [self load:nil];
  [panel makeKeyAndOrderFront:nil];
}

- (void)buildPanel {
  if (!panel) {
    NSRect frame = NSMakeRect(0, 0, 560, 400);
    panel = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSTitledWindowMask | NSClosableWindowMask
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [panel setTitle:@"ppcode Settings"];
    [panel setFrameAutosaveName:@"PPCodeSettings"];

    NSTabView *tabs = [[[NSTabView alloc]
        initWithFrame:NSMakeRect(12, 46, 536, 342)] autorelease];

    struct { NSString *label; SEL builder; } pages[] = {
      {@"Keys",       @selector(buildKeysTab)},
      {@"Model",      @selector(buildModelTab)},
      {@"Routing",    @selector(buildRoutingTab)},
      {@"MCP",        @selector(buildMcpTab)},
      {@"Tools",      @selector(buildToolsTab)},
    };
    for (unsigned i = 0; i < sizeof(pages) / sizeof(pages[0]); i++) {
      NSTabViewItem *item =
          [[[NSTabViewItem alloc] initWithIdentifier:pages[i].label] autorelease];
      [item setLabel:pages[i].label];
      [item setView:[self performSelector:pages[i].builder]];
      [tabs addTabViewItem:item];
    }
    [[panel contentView] addSubview:tabs];

    [[panel contentView] addSubview:
        MakePush(@"Save", NSMakeRect(456, 12, 90, 28), self, @selector(save:))];
    [[panel contentView] addSubview:
        MakePush(@"Revert", NSMakeRect(366, 12, 84, 28), self, @selector(load:))];

    [panel center];
  }
}

// ---------------------------------------------------------------------------

- (void)temperatureChanged:(id)sender {
  [temperatureLabel setStringValue:
      [NSString stringWithFormat:@"%.2f", [temperatureSlider doubleValue]]];
}

- (void)transportChanged:(id)sender {
  BOOL http = [[mcpTransport titleOfSelectedItem] isEqualToString:@"http"];
  [[mcpHeaderName cell] setEnabled:http];
  [[mcpHeaderValue cell] setEnabled:http];
  [mcpStatus setStringValue:http
      ? @"HTTP: give the server's URL. Headers are sent with every request."
      : @"stdio: give the command to run, and any arguments separated by spaces."];
}

- (void)load:(id)sender {
  NSDictionary *cfg = [bridge configDictionary];

  [openrouterKey setStringValue:Or([cfg objectForKey:@"api_key"], @"")];
  [keyStatus setStringValue:[bridge hasApiKey]
      ? @"A key is configured."
      : PPUTF8("No key yet \xE2\x80\x94 ppcode cannot talk to any model "
               "until one is set.")];
  [[keyStatus cell] setTextColor:[bridge hasApiKey] ? [NSColor darkGrayColor]
                                                    : [NSColor redColor]];

  NSDictionary *search = [cfg objectForKey:@"search_keys"];
  [tavilyKey setStringValue:Or([search objectForKey:@"tavily"], @"")];
  [braveKey setStringValue:Or([search objectForKey:@"brave"], @"")];

  // Model list: favourites first, then everything that supports tools.
  [defaultModel removeAllItems];
  NSArray *favs = [bridge favouriteModelIds];
  NSEnumerator *fe = [favs objectEnumerator];
  NSString *f;
  while ((f = [fe nextObject]) != nil) [defaultModel addItemWithTitle:f];
  [[defaultModel menu] addItem:[NSMenuItem separatorItem]];
  NSEnumerator *me = [[bridge availableModels] objectEnumerator];
  NSDictionary *m;
  int n = 0;
  while ((m = [me nextObject]) != nil) {
    NSString *mid = [m objectForKey:@"id"];
    if ([favs containsObject:mid]) continue;
    if (![[m objectForKey:@"tools"] boolValue]) continue;
    [defaultModel addItemWithTitle:mid];
    if (++n >= 120) break;
  }
  NSString *cur = [cfg objectForKey:@"model"];
  if (cur && [defaultModel itemWithTitle:cur]) [defaultModel selectItemWithTitle:cur];

  id tokens = OrObj([cfg objectForKey:@"max_tokens"], [NSNumber numberWithInt:8192]);
  id turns = OrObj([cfg objectForKey:@"max_turns"], [NSNumber numberWithInt:40]);
  id cost = OrObj([cfg objectForKey:@"max_cost"], [NSNumber numberWithInt:0]);

  [maxTokensField setStringValue:[NSString stringWithFormat:@"%@", tokens]];
  [maxTurnsField setStringValue:[NSString stringWithFormat:@"%@", turns]];
  [maxCostField setStringValue:[NSString stringWithFormat:@"%@", cost]];
  double temp = [[cfg objectForKey:@"temperature"] doubleValue];
  [temperatureSlider setDoubleValue:temp];
  [self temperatureChanged:nil];

  NSDictionary *prov = [cfg objectForKey:@"provider"];
  NSString *sort = [prov objectForKey:@"sort"];
  [providerSort selectItemWithTitle:Or(sort, @"(no preference)")];
  id af = [prov objectForKey:@"allow_fallbacks"];
  [allowFallbacks setState:(af == nil || [af boolValue]) ? NSOnState : NSOffState];
  [denyTraining setState:
      [[prov objectForKey:@"data_collection"] isEqualToString:@"deny"] ? NSOnState
                                                                       : NSOffState];

  NSDictionary *reason = [cfg objectForKey:@"reasoning"];
  NSString *effort = [reason objectForKey:@"effort"];
  [reasoningEffort selectItemWithTitle:Or(effort, @"(model default)")];

  NSString *cache = [cfg objectForKey:@"cache_mode"];
  [promptCaching setState:[cache isEqualToString:@"off"] ? NSOffState : NSOnState];
  [webPlugin setState:[[cfg objectForKey:@"web_search"] boolValue] ? NSOnState
                                                                   : NSOffState];

  [mcpServers removeAllObjects];
  NSEnumerator *se = [[cfg objectForKey:@"mcp_servers"] objectEnumerator];
  NSDictionary *s;
  while ((s = [se nextObject]) != nil)
      [mcpServers addObject:[[s mutableCopy] autorelease]];
  [mcpTable reloadData];
  [self transportChanged:nil];

  // Tools tab status.
  NSString *bundled = [bridge bundledCLIPath];
  NSString *bundledVer = bundled ? [bridge cliVersionAt:bundled] : nil;
  NSString *installed = nil;
  NSEnumerator *le = [[NSArray arrayWithObjects:
      [NSHomeDirectory() stringByAppendingPathComponent:@"bin/ppcode"],
      @"/usr/local/bin/ppcode", nil] objectEnumerator];
  NSString *cand;
  while ((cand = [le nextObject]) != nil) {
    NSString *v = [bridge cliVersionAt:cand];
    if (v) { installed = [NSString stringWithFormat:@"%@  (%@)", v, cand]; break; }
  }
  [cliStatus setStringValue:
      [NSString stringWithFormat:@"In this application: %@\nInstalled: %@",
          Or(bundledVer, @"not found"),
          Or(installed, @"not installed")]];

  NSString *prefix = [bridge macportsPrefix];
  if (!prefix) {
    [portStatus setStringValue:@"MacPorts was not found. ppcode needs its curl "
                                "and OpenSSL for network access."];
  }

  else {
    BOOL qjs = [bridge isPortInstalled:@"quickjs"];
    [portStatus setStringValue:[NSString stringWithFormat:
        PPUTF8("MacPorts at %@.\nQuickJS: %@ \xE2\x80\x94 only needed to run "
               "JavaScript MCP servers locally. HTTP and other stdio servers "
               "do not require it."),
        prefix, qjs ? @"installed" : @"not installed"]];
  }
}

- (void)save:(id)sender {
  NSMutableDictionary *cfg =
      [[[bridge configDictionary] mutableCopy] autorelease];

  NSString *key = [[openrouterKey stringValue]
      stringByTrimmingCharactersInSet:
          [NSCharacterSet whitespaceAndNewlineCharacterSet]];
  if ([key length] > 0) [cfg setObject:key forKey:@"api_key"];

  NSMutableDictionary *search = [NSMutableDictionary dictionary];
  NSString *tavily = [tavilyKey stringValue];
  NSString *brave = [braveKey stringValue];
  if ([tavily length]) { [search setObject:tavily forKey:@"tavily"]; }
  if ([brave length]) { [search setObject:brave forKey:@"brave"]; }
  [cfg setObject:search forKey:@"search_keys"];

  if ([defaultModel titleOfSelectedItem])
      [cfg setObject:[defaultModel titleOfSelectedItem] forKey:@"model"];
  [cfg setObject:[NSNumber numberWithInt:[maxTokensField intValue]] forKey:@"max_tokens"];
  [cfg setObject:[NSNumber numberWithInt:[maxTurnsField intValue]] forKey:@"max_turns"];
  [cfg setObject:[NSNumber numberWithDouble:[maxCostField doubleValue]]
          forKey:@"max_cost"];
  [cfg setObject:[NSNumber numberWithDouble:[temperatureSlider doubleValue]]
          forKey:@"temperature"];

  NSMutableDictionary *prov = [NSMutableDictionary dictionary];
  NSString *sort = [providerSort titleOfSelectedItem];
  if (![sort hasPrefix:@"("]) [prov setObject:sort forKey:@"sort"];
  [prov setObject:[NSNumber numberWithBool:[allowFallbacks state] == NSOnState]
           forKey:@"allow_fallbacks"];
  if ([denyTraining state] == NSOnState)
      [prov setObject:@"deny" forKey:@"data_collection"];
  [cfg setObject:prov forKey:@"provider"];

  NSString *effort = [reasoningEffort titleOfSelectedItem];
  if ([effort hasPrefix:@"("]) [cfg removeObjectForKey:@"reasoning"];
  else [cfg setObject:[NSDictionary dictionaryWithObject:effort forKey:@"effort"]
               forKey:@"reasoning"];

  [cfg setObject:([promptCaching state] == NSOnState ? @"auto" : @"off")
          forKey:@"cache_mode"];
  [cfg setObject:[NSNumber numberWithBool:[webPlugin state] == NSOnState]
          forKey:@"web_search"];

  [cfg setObject:mcpServers forKey:@"mcp_servers"];

  NSString *err = nil;
  if (![bridge saveConfigDictionary:cfg error:&err]) {
    NSAlert *a = [NSAlert alertWithMessageText:@"Could not save settings"
                                 defaultButton:@"OK"
                               alternateButton:nil
                                   otherButton:nil
                     informativeTextWithFormat:@"%@", Or(err, @"Unknown error.")];
    [a runModal];

    return;
  }
  [self load:nil];
  [keyStatus setStringValue:@"Saved."];
}

// ---- MCP table -------------------------------------------------------------

- (NSInteger)numberOfRowsInTableView:(NSTableView *)t {
  return (NSInteger)[mcpServers count];
}

- (id)tableView:(NSTableView *)t
    objectValueForTableColumn:(NSTableColumn *)col
                          row:(NSInteger)row {
  NSDictionary *s = [mcpServers objectAtIndex:(NSUInteger)row];
  if ([[col identifier] isEqualToString:@"name"]) {
    BOOL on = [[s objectForKey:@"enabled"] boolValue] ||
              [s objectForKey:@"enabled"] == nil;

    return [NSString stringWithFormat:@"%@%@", [s objectForKey:@"name"],
                      on ? @"" : @"  (off)"];
  }
  NSString *transport = Or([s objectForKey:@"transport"], @"stdio");
  NSString *addr = [transport isEqualToString:@"http"]
                       ? Or([s objectForKey:@"url"], @"")
                       : Or([s objectForKey:@"command"], @"");
  NSDictionary *h = [s objectForKey:@"headers"];
  // No emoji: this platform predates an emoji font, so a lock character would
  // render as a hollow box even once the literal itself is built correctly.
  NSString *auth = [h count] ? @"  [auth]" : @"";

  return [NSString stringWithFormat:@"%@  %@%@", transport, addr, auth];
}

- (void)tableViewSelectionDidChange:(NSNotification *)note {
  NSInteger row = [mcpTable selectedRow];
  if (row < 0) return;
  NSDictionary *s = [mcpServers objectAtIndex:(NSUInteger)row];

  [mcpName setStringValue:Or([s objectForKey:@"name"], @"")];
  NSString *transport = Or([s objectForKey:@"transport"], @"stdio");
  [mcpTransport selectItemWithTitle:transport];
  [mcpCommandOrUrl setStringValue:
      [transport isEqualToString:@"http"] ? Or([s objectForKey:@"url"], @"")
                                          : Or([s objectForKey:@"command"], @"")];
  NSArray *args = [s objectForKey:@"args"];
  [mcpArgs setStringValue:args ? [args componentsJoinedByString:@" "] : @""];

  NSDictionary *h = [s objectForKey:@"headers"];
  NSString *firstKey = [[h allKeys] count] ? [[h allKeys] objectAtIndex:0]
                                           : @"Authorization";
  [mcpHeaderName setStringValue:firstKey];
  [mcpHeaderValue setStringValue:Or([h objectForKey:firstKey], @"")];

  id en = [s objectForKey:@"enabled"];
  [mcpEnabled setState:(en == nil || [en boolValue]) ? NSOnState : NSOffState];
  [self transportChanged:nil];
}

- (void)addMcp:(id)sender {
  NSString *name = [[mcpName stringValue]
      stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
  if ([name length] == 0) {
    [mcpStatus setStringValue:@"Give the server a name."];

    return;
  }
  NSString *addr = [[mcpCommandOrUrl stringValue]
      stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
  if ([addr length] == 0) {
    [mcpStatus setStringValue:@"Give a URL or a command."];

    return;
  }

  NSMutableDictionary *s = [NSMutableDictionary dictionary];
  NSString *transport = [mcpTransport titleOfSelectedItem];
  [s setObject:name forKey:@"name"];
  [s setObject:transport forKey:@"transport"];
  [s setObject:[NSNumber numberWithBool:[mcpEnabled state] == NSOnState]
        forKey:@"enabled"];

  if ([transport isEqualToString:@"http"]) {
    [s setObject:addr forKey:@"url"];
    NSString *hn = [[mcpHeaderName stringValue]
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    NSString *hv = [[mcpHeaderValue stringValue]
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if ([hn length] && [hv length]) {
      // A very common paste error: the token without the scheme.
      if ([hn isEqualToString:@"Authorization"] &&
          ![hv hasPrefix:@"Bearer "] && ![hv hasPrefix:@"Basic "] &&
          ![hv hasPrefix:@"Token "]) {
        hv = [@"Bearer " stringByAppendingString:hv];
        [mcpStatus setStringValue:@"Added the \"Bearer \" prefix to the token."];
      }
      [s setObject:[NSDictionary dictionaryWithObject:hv forKey:hn]
            forKey:@"headers"];
    }
  }

  else {
    [s setObject:addr forKey:@"command"];
    NSString *args = [[mcpArgs stringValue]
        stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    if ([args length]) {
      NSArray *parts = [args componentsSeparatedByCharactersInSet:
                           [NSCharacterSet whitespaceCharacterSet]];
      NSMutableArray *clean = [NSMutableArray array];
      NSEnumerator *pe = [parts objectEnumerator];
      NSString *p;
      while ((p = [pe nextObject]) != nil)
          if ([p length]) [clean addObject:p];
      [s setObject:clean forKey:@"args"];
    }
  }

  // Replace an entry with the same name rather than adding a duplicate.
  NSUInteger existing = NSNotFound;
  for (NSUInteger i = 0; i < [mcpServers count]; i++)
      if ([[[mcpServers objectAtIndex:i] objectForKey:@"name"] isEqualToString:name])
          existing = i;
  if (existing == NSNotFound) [mcpServers addObject:s];
  else [mcpServers replaceObjectAtIndex:existing withObject:s];

  [mcpTable reloadData];
  if ([[mcpStatus stringValue] length] == 0)
      [mcpStatus setStringValue:@"Added. Choose Save to write it to the config."];
}

- (void)removeMcp:(id)sender {
  NSInteger row = [mcpTable selectedRow];
  if (row < 0) { [mcpStatus setStringValue:@"Select a server first."]; return; }
  [mcpServers removeObjectAtIndex:(NSUInteger)row];
  [mcpTable reloadData];
  [mcpStatus setStringValue:@"Removed. Choose Save to apply."];
}

- (void)testMcp:(id)sender {
  [mcpStatus setStringValue:@"Save first, then the connection is attempted at "
                             "launch and reported in the transcript."];
}

// ---- CLI -------------------------------------------------------------------

- (void)installCLI:(id)sender {
  NSString *dir = [cliLocation titleOfSelectedItem];
  NSString *err = nil;
  if ([bridge installCLIToDirectory:dir error:&err]) {
    NSAlert *a = [NSAlert alertWithMessageText:@"Installed"
                                 defaultButton:@"OK"
                               alternateButton:nil
                                   otherButton:nil
                     informativeTextWithFormat:
                         @"ppcode was installed to %@.\n\nIf that directory is "
                          "not on your PATH, add it to your shell profile.", dir];
    [a runModal];
  }

  else {
    NSAlert *a = [NSAlert alertWithMessageText:@"Could not install"
                                 defaultButton:@"OK"
                               alternateButton:nil
                                   otherButton:nil
                     informativeTextWithFormat:@"%@\n\n/usr/local/bin usually "
                          "needs administrator rights; installing into your "
                          "home directory does not.",
                          Or(err, @"Unknown error.")];
    [a runModal];
  }
  [self load:nil];
}

@end
