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

// API key. A GUI application launched from the Finder does not inherit the
// shell environment, so the key usually has to come from the config file or be
// entered in the interface.
- (BOOL)hasApiKey;
- (NSString *)configPath;
- (BOOL)saveApiKey:(NSString *)key error:(NSString **)err;

// Whole-config access, so the settings window does not need an accessor per
// field. Values map JSON <-> Foundation objects.
- (NSDictionary *)configDictionary;
- (BOOL)saveConfigDictionary:(NSDictionary *)d error:(NSString **)err;
- (void)reloadConfig;

// Installing the command line tool. The CLI is carried inside the bundle so the
// application can be the thing that installs and updates it.
- (NSString *)bundledCLIPath;
- (NSString *)cliVersionAt:(NSString *)path;
- (BOOL)installCLIToDirectory:(NSString *)dir error:(NSString **)err;

// Is a MacPorts port installed, and install one (a long build -- the caller
// must warn).
- (BOOL)isPortInstalled:(NSString *)port;
- (NSString *)macportsPrefix;

- (NSString *)modelId;
- (void)setModelId:(NSString *)mid;

// The system message currently in force. Exposed so --check can prove that
// changing the model actually rebuilds it, which is otherwise invisible.
- (NSString *)systemPrompt;
- (NSArray *)availableModels;      // array of NSDictionary
- (NSArray *)favouriteModelIds;

- (NSString *)workingDirectory;
// Refused, returning NO, while a turn is running or if the path is not a
// directory. The working directory is per conversation and is saved with it.
- (BOOL)setWorkingDirectory:(NSString *)dir;

- (BOOL)modelSupportsImages;

// Conversation history for the transcript, as an array of dictionaries with
// keys: role, text.
- (NSArray *)transcript;
- (void)newConversation;

// Saved sessions: array of dictionaries with id, title, age, messages, cost.
- (NSArray *)savedSessions;
- (BOOL)loadSessionWithId:(NSString *)sessionId;

// Managing saved conversations. All refuse while a turn is running rather than
// pulling a file out from under the worker thread.
//
// `current` tells the caller the session it just operated on was the one loaded,
// so the interface can start a fresh conversation instead of leaving a
// transcript on screen that no longer has a file behind it.
- (BOOL)deleteSessionWithId:(NSString *)sessionId
                  wasLoaded:(BOOL *)current
                      error:(NSString **)err;

// Moves the file into an "archive" subdirectory, so it stops appearing in the
// list without being destroyed.
- (BOOL)archiveSessionWithId:(NSString *)sessionId
                   wasLoaded:(BOOL *)current
                       error:(NSString **)err;

// One JSON object per line: the interchange format for a conversation.
- (BOOL)exportSessionWithId:(NSString *)sessionId
                     toPath:(NSString *)path
                      error:(NSString **)err;

// Returns how many were removed, or -1 if a turn is running.
- (NSInteger)deleteAllSessions;

- (long long)totalTokens;
- (double)totalCost;

@end
