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

// Is there a key for the provider in use? A GUI application launched from the
// Finder does not inherit the shell environment, so a key exported in a profile
// is invisible here and one usually has to be entered in the interface --
// see -saveKey:forProvider:error: below.
- (BOOL)hasApiKey;
- (NSString *)configPath;

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

// --- providers ---------------------------------------------------------
//
// Which service to talk to. The command line has --provider; without this the
// application could only ever use whatever the config file said.

// Dictionaries with: id, name, baseURL, hasKey, needsKey, keySource.
- (NSArray *)availableProviders;
- (NSString *)providerId;
- (NSString *)providerName;

// Switching reloads the model catalogue, since the previous provider's ids
// mean nothing to the new one. Returns NO while a turn is running.
- (BOOL)setProviderId:(NSString *)pid;

// The address for a provider, and a persisted override for it. LM Studio
// cannot run on a PowerPC G5, so it is always on another machine.
- (NSString *)baseURLForProvider:(NSString *)pid;
- (BOOL)setBaseURL:(NSString *)url forProvider:(NSString *)pid;

// A provider's key, written to the key file the command line already reads, so
// one entered here is the same key ppcode finds in a terminal.
- (BOOL)saveKey:(NSString *)key forProvider:(NSString *)pid error:(NSString **)err;

// Where the key in force came from: "environment (DEEPSEEK_API_KEY)", a path,
// or empty for none. An exported variable beats the file, so a key that was
// just typed and appears to do nothing has a visible explanation.
- (NSString *)keySourceForProvider:(NSString *)pid;

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

// --- the search index -------------------------------------------------------
//
// Indexing runs on its own thread: a book is thousands of chunks and doing that
// on the main thread would freeze the window for as long as it took. Progress
// arrives on the main thread through the delegate methods below.

@protocol PPIndexDelegate
- (void)indexDidProgress:(NSString *)message fraction:(double)fraction;
- (void)indexDidFinish:(NSString *)summary added:(NSInteger)documents;
@end

@interface PPBridge (Index)

// Index files or folders into a collection. Returns NO if indexing is already
// running -- one at a time, since they share one database.
- (BOOL)indexPaths:(NSArray *)paths
    intoCollection:(NSString *)collection
          delegate:(id)indexDelegate;

- (BOOL)isIndexing;

// Rebuild the conversation half from the session files.
- (BOOL)reindexConversationsWithDelegate:(id)indexDelegate;

// Everything in the index: dictionaries with docId, collection, chunks,
// embedded, age, and a displayName suitable for a table.
- (NSArray *)indexedDocuments;

// Total chunks and how many carry an embedding.
- (NSDictionary *)indexStatistics;

- (BOOL)removeIndexedDocument:(NSString *)docId;

// Empty the whole index. The conversations themselves are untouched -- this is
// derived data and can always be rebuilt.
- (BOOL)clearIndex;

@end
