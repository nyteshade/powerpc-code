// GuiBridge.h -- the seam between Cocoa and the C++ agent.
//
// The engine is C++ and knows nothing about AppKit; the interface is
// Objective-C and must never block. So the bridge owns a worker thread running
// Agent::run, and marshals every event back to the main thread, because AppKit
// is not thread-safe and drawing from the worker would corrupt the display in
// ways that look like random crashes hours later.
//
// This header is included from Objective-C++ only.
#pragma once

#import <Cocoa/Cocoa.h>

// Delegate methods are always invoked on the main thread.
@protocol PPBridgeDelegate <NSObject>
- (void)bridgeDidStart;
- (void)bridgeDidReceiveText:(NSString *)delta;
- (void)bridgeDidStartTool:(NSString *)name detail:(NSString *)detail;
- (void)bridgeDidFinishTool:(NSString *)name
                     result:(NSString *)result
                    isError:(BOOL)isError;
- (void)bridgeDidUpdateStatus:(NSString *)status;
- (void)bridgeDidError:(NSString *)message;
- (void)bridgeDidFinishTurnWithTokens:(long long)tokens cost:(double)cost;
// Return YES to allow the tool. Called on the main thread; may run a sheet.
- (BOOL)bridgeShouldAllowTool:(NSString *)name
                        title:(NSString *)title
                       detail:(NSString *)detail;
@end

// Opaque C++ state. This header is only ever included from Objective-C++, so a
// C++ forward declaration is fine and keeps the engine out of the interface.
struct BridgeState;

@interface PPBridge : NSObject {
    // Under the fragile Objective-C ABI that GCC targets here, instance
    // variables must be declared in the @interface -- declaring them in the
    // @implementation block is a non-fragile-ABI feature and is rejected.
    struct BridgeState *st;
    id delegate;
}

- (id)init;
- (id)delegate;
- (void)setDelegate:(id)d;

// Send a message, optionally with attachments (file paths; images are detected
// and carried through the multimodal path).
- (void)sendMessage:(NSString *)text attachments:(NSArray *)paths;

// Queue steering text while a turn is running; it is injected between rounds.
- (void)steer:(NSString *)text;

- (BOOL)isBusy;
- (void)cancel;

- (NSString *)modelId;
- (void)setModelId:(NSString *)mid;
- (NSArray *)availableModels;      // array of NSDictionary
- (NSArray *)favouriteModelIds;

- (NSString *)workingDirectory;
- (void)setWorkingDirectory:(NSString *)dir;

- (BOOL)modelSupportsImages;

// Conversation history for the transcript, as an array of dictionaries with
// keys: role, text.
- (NSArray *)transcript;
- (void)newConversation;

// Saved sessions: array of dictionaries with id, title, age, messages, cost.
- (NSArray *)savedSessions;
- (BOOL)loadSessionWithId:(NSString *)sessionId;

- (long long)totalTokens;
- (double)totalCost;

@end
