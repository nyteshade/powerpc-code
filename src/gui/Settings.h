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

    // Keys
    NSSecureTextField *openrouterKey;
    NSTextField *keyStatus;
    NSSecureTextField *tavilyKey;
    NSSecureTextField *braveKey;

    // Model
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

    // Tools
    NSTextField *cliStatus;
    NSPopUpButton *cliLocation;
    NSTextField *portStatus;
}

- (id)initWithBridge:(PPBridge *)b;
- (void)showWindow;

@end
