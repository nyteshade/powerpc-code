// Library.h -- what is in the search index, and how to get rid of it.
//
// An index that only grows and cannot be inspected is a liability: you cannot
// tell why a search returned something, or remove a document you added by
// mistake. This is the window that makes it accountable.
#pragma once

#import <Cocoa/Cocoa.h>

@class PPBridge;

// No protocol conformance list: NSTableViewDataSource and NSTableViewDelegate
// are informal protocols on 10.5.
@interface PPLibraryController : NSObject {
  NSWindow *panel;
  PPBridge *bridge;

  NSTableView *table;
  NSArray *documents;          // dictionaries from -indexedDocuments
  NSPopUpButton *filterPopup;
  NSTextField *summary;
  NSButton *removeButton;
  NSString *filter;            // "" for everything
}

- (id)initWithBridge:(PPBridge *)b;
- (void)showWindow;

// Re-read the index. Called after indexing finishes, since the list on screen
// is stale the moment anything is added.
- (void)reload;

// The window, built and populated but not ordered front, so --shot can render
// it offscreen.
- (NSWindow *)panelWindow;

@end
