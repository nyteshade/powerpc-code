// Providers.h -- choosing and configuring the service behind the model list.
//
// This was a row in the Settings window, which was the wrong place twice over.
// A provider is not a preference: switching it replaces the entire model
// catalogue, so it belongs next to the model picker rather than three tabs deep
// in a panel you open to change something else. And a provider needs
// configuring -- an address for the one that lives on another machine, a key for
// the ones that charge -- which is more than a popup can carry.
//
// So: a window of its own, opened from the top of the model menu. The list on
// the left is every provider ppcode knows; the pane on the right configures
// whichever is selected, whether or not it is the one in use. Switching is a
// separate, deliberate act -- selecting a row to fix its address must not move
// the whole application onto it.
#pragma once

#import <Cocoa/Cocoa.h>

@class PPBridge;

// Informal, like PPBridge's own delegate: on 10.5 an @protocol with optional
// methods is more ceremony than a respondsToSelector: check. Declared on
// NSObject rather than left to be discovered, because GCC types an unknown
// selector as returning id and taking varargs -- which for a void method
// happens to work and is therefore a warning that would go on being ignored.
@interface NSObject (PPProvidersDelegate)

// The service in use is now a different one, so the model list is stale.
- (void)providersDidChangeProvider;

@end

// No protocol conformance list on the class either: NSTableViewDataSource and
// NSTableViewDelegate are informal protocols here.
@interface PPProvidersController : NSObject {
  NSWindow *panel;
  PPBridge *bridge;
  id delegate;

  NSArray *providers;          // dictionaries from -availableProviders
  NSTableView *table;

  // The detail pane, which describes the selected row rather than the provider
  // in use. These are the two that differ until Use is pressed.
  NSTextField *heading;
  NSTextField *addressField;
  NSTextField *addressNote;
  NSSecureTextField *keyField;
  NSTextField *keyNote;
  NSButton *useButton;
  NSButton *removeButton;

  // The sheet for adding one. Built on first use and kept, like the panel.
  NSWindow *addPanel;
  NSTextField *addName;
  NSTextField *addId;
  NSTextField *addURL;
  NSTextField *addModel;
  NSButton *addNeedsKey;
  NSTextField *addStatus;
}

- (id)initWithBridge:(PPBridge *)b;

- (id)delegate;

- (void)setDelegate:(id)d;

- (void)showWindow;

// Re-read the providers and repaint. Cheap, and the truth can change from
// elsewhere -- a key exported into the environment, or --provider on a CLI run.
- (void)reload;

// Built and populated but not ordered front, so --shot can render it offscreen.
- (NSWindow *)panelWindow;

@end
