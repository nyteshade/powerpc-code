// Settings.h -- the preferences window.
//
// This is where the application earns its keep as the front door: keys, the
// default model, OpenRouter routing, MCP servers, and installing the command
// line tool. Editing JSON by hand should never be necessary.
//
// Laid out as a tabbed panel, which is the period-correct shape for preferences
// and keeps each concern on its own page rather than in one long scroll.
#pragma once

#import <Cocoa/Cocoa.h>

@class PPBridge;

@interface PPSettingsController : NSObject {
  NSWindow *panel;
  PPBridge *bridge;

  // Whoever opens the providers window on this panel's behalf -- the
  // application controller. Not retained; it owns this object.
  id delegate;

  // Search keys. The provider keys are deliberately not here: a key is part of
  // configuring a provider, and that lives in the Providers window.
  NSTextField *saveStatus;
  NSSecureTextField *tavilyKey;
  NSSecureTextField *braveKey;

  // Model
  NSTextField *providerLabel;   // which service the model list below comes from
  NSPopUpButton *defaultModel;
  NSTextField *maxTokensField;
  NSTextField *maxTurnsField;
  NSSlider *temperatureSlider;
  NSTextField *temperatureLabel;

  // OpenRouter
  NSPopUpButton *providerSort;
  NSButton *allowFallbacks;
  NSButton *denyTraining;
  NSPopUpButton *reasoningEffort;
  NSButton *promptCaching;
  NSButton *webPlugin;
  NSTextField *maxCostField;

  // MCP
  NSTableView *mcpTable;
  NSMutableArray *mcpServers;      // array of NSMutableDictionary
  NSTextField *mcpName;
  NSPopUpButton *mcpTransport;
  NSTextField *mcpCommandOrUrl;
  NSTextField *mcpArgs;
  NSTextField *mcpHeaderName;
  NSSecureTextField *mcpHeaderValue;
  NSButton *mcpEnabled;
  NSTextField *mcpStatus;
  // A test runs on its own thread; this keeps a second one from being started
  // on top of the first.
  BOOL mcpTesting;

  // Tools
  NSTextField *cliStatus;
  NSPopUpButton *cliLocation;
  NSTextField *portStatus;
}

- (id)initWithBridge:(PPBridge *)b;
- (void)showWindow;

// Sent -showProviders: when the Model tab's button is pressed. Informal, like
// PPBridge's own delegate.
- (id)delegate;
- (void)setDelegate:(id)d;

// The panel itself, so --shot can render each tab offscreen. Building it is a
// side effect of asking, which is what makes a headless screenshot possible.
- (NSWindow *)panelWindow;

@end
