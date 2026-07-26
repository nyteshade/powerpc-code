#import "GuiBridge.h"

#include "agent.hpp"
#include "appledocs.hpp"
#include "attach.hpp"
#include "builderr.hpp"
#include "config.hpp"
#include "envinfo.hpp"
#include "http.hpp"
#include "job.hpp"
#include "jobs.hpp"
#include "macgui.hpp"
#include "mcp.hpp"
#include "openrouter.hpp"
#include "session.hpp"
#include "subagent.hpp"
#include "sysprompt.hpp"
#include "tools.hpp"
#include "webtools.hpp"
#include "xcodeproj.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

using namespace ppcode;

namespace {

NSString *Str(const std::string &s) {
    NSString *r = [NSString stringWithUTF8String:s.c_str()];
    // A tool can emit bytes that are not valid UTF-8; do not return nil into
    // AppKit, which would show as a silent blank.
    return r ? r : @"(unprintable output)";
}

std::string Cpp(NSString *s) {
    if (!s) return "";
    const char *c = [s UTF8String];
    return c ? std::string(c) : std::string();
}

// GCC 15's Objective-C++ front end hits an internal compiler error --
// objc_eh_runtime_type, objc-next-runtime-abi-01.cc:2798 -- on *any* explicit
// C++ catch clause in the translation unit, because emitting the catch type
// goes through the NeXT-runtime exception path. Implicit cleanups from
// std::string and friends are fine; it is the catch handler that breaks it.
//
// So this file contains no try/catch at all, and uses the non-throwing form of
// json::parse instead. Anything that genuinely needs to catch belongs in a
// plain .cpp compiled as C++ rather than Objective-C++.
bool LoadSessionInto(ppcode::Agent *agent, const std::string &text,
                     std::string *err) {
    ppcode::json j = ppcode::json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        if (err) *err = "the session file is not valid JSON";
        return false;
    }
    return agent->from_json(j, err);
}

} // namespace

// The C++ state, forward-declared in the header.
struct BridgeState {
    Config cfg;
    std::unique_ptr<Client> client;
    ToolRegistry tools;
    TodoStore todos;
    JobManager jobs;
    mcp::Manager mcp;
    std::unique_ptr<Agent> agent;
    ModelCatalog catalog;

    std::thread worker;
    std::atomic<bool> busy{false};
    std::atomic<bool> cancel{false};

    // Approval crosses threads in the opposite direction to events: the worker
    // blocks here while the main thread runs a sheet.
    std::mutex approve_mu;
    std::condition_variable approve_cv;
    bool approve_pending = false;
    bool approve_done = false;
    bool approve_result = false;

    std::mutex usage_mu;
    Usage subagent_usage;
    std::mutex subagent_approve_mu;

    // Everything the transcript needs, kept here so the view can be rebuilt.
    std::vector<std::pair<std::string, std::string>> transcript;  // role, text
    std::mutex transcript_mu;
};

@implementation PPBridge

- (id)delegate { return delegate; }
- (void)setDelegate:(id)d { delegate = d; }

- (id)init {
    if (!(self = [super init])) return nil;
    st = new BridgeState();

    http::global_init();

    std::vector<std::string> warnings;
    st->cfg = Config::load("", &warnings);
    if (st->cfg.api_key.empty()) {
        // Surfaced by the controller as an alert; the app is useless without it.
        NSLog(@"ppcode: no OPENROUTER_AI_API_KEY in the environment");
    }

    st->client.reset(new Client(st->cfg));

    st->tools.add_builtins();
    st->tools.add_extra_builtins(&st->todos);
    add_job_tools(st->tools, st->jobs);
    web::add_tools(st->tools, web::SearchConfig::from_env());
    xcode::add_tools(st->tools);
    builderr::add_tools(st->tools);
    if (appledocs::available()) appledocs::add_tools(st->tools);

    if (!st->cfg.mcp_servers.empty())
        st->mcp.connect_all(st->cfg.mcp_servers, st->tools,
                            [](const std::string &m) { NSLog(@"%s", m.c_str()); });

    st->agent.reset(new Agent(*st->client, st->tools, st->cfg));

    [self rebuildSystemPrompt];
    return self;
}

- (void)dealloc {
    if (st) {
        st->cancel.store(true);
        if (st->worker.joinable()) st->worker.join();
        st->mcp.disconnect_all();
        delete st;
    }
    http::global_cleanup();
    [super dealloc];
}

// Assemble the system message for the current model, and register the tools
// whose descriptions depend on it.
- (void)rebuildSystemPrompt {
    envinfo::Probe probe = envinfo::probe(false);
    st->catalog.load(*st->client, nullptr);
    const ModelInfo *mi = st->catalog.find(st->cfg.model);

    sysprompt::Inputs si;
    si.cfg = &st->cfg;
    si.probe = &probe;
    si.cwd = st->agent->cwd();
    si.model_id = st->cfg.model;
    si.context_tokens = mi ? mi->context_length : ModelCatalog::kUnknownContext;
    si.model_supports_images = mi ? mi->supports_images : false;
    si.tool_names = st->tools.names();

    sysprompt::Result sp = sysprompt::build(si);
    st->agent->set_system_prompt(sp.text);
    st->agent->set_context_limit(si.context_tokens);

    if (macgui::screencapture_available())
        macgui::add_tools(st->tools, si.model_supports_images);

    static bool agents_registered = false;
    if (!agents_registered) {
        agents_registered = true;
        subagent::Host host;
        host.client = st->client.get();
        host.config = &st->cfg;
        host.parent_tools = &st->tools;
        host.cwd = st->agent->cwd();
        host.base_system = sp.text;
        host.usage_mutex = &st->usage_mu;
        host.shared_usage = &st->subagent_usage;
        host.approve_mutex = &st->subagent_approve_mu;
        subagent::add_tools(st->tools, host,
                            subagent::load_definitions(nullptr));
    }

    if (st->agent->session_path().empty())
        st->agent->set_session_path(session::path_for(session::new_id()));
}

// ---------------------------------------------------------------------------
// Marshalling to the main thread
// ---------------------------------------------------------------------------

- (void)mainText:(NSString *)s   { [delegate bridgeDidReceiveText:s]; }
- (void)mainStatus:(NSString *)s { [delegate bridgeDidUpdateStatus:s]; }
- (void)mainError:(NSString *)s  { [delegate bridgeDidError:s]; }

- (void)mainToolStart:(NSArray *)pair {
    [delegate bridgeDidStartTool:[pair objectAtIndex:0]
                          detail:[pair objectAtIndex:1]];
}

- (void)mainToolDone:(NSArray *)triple {
    [delegate bridgeDidFinishTool:[triple objectAtIndex:0]
                           result:[triple objectAtIndex:1]
                          isError:[[triple objectAtIndex:2] boolValue]];
}

- (void)mainFinished:(NSArray *)pair {
    [delegate bridgeDidFinishTurnWithTokens:[[pair objectAtIndex:0] longLongValue]
                                       cost:[[pair objectAtIndex:1] doubleValue]];
}

// Runs on the main thread; asks the delegate and wakes the worker.
- (void)mainApprove:(NSArray *)triple {
    BOOL allowed = [delegate bridgeShouldAllowTool:[triple objectAtIndex:0]
                                             title:[triple objectAtIndex:1]
                                            detail:[triple objectAtIndex:2]];
    {
        std::lock_guard<std::mutex> lk(st->approve_mu);
        st->approve_result = allowed ? true : false;
        st->approve_done = true;
    }
    st->approve_cv.notify_all();
}

- (void)appendTranscriptRole:(const std::string &)role text:(const std::string &)text {
    std::lock_guard<std::mutex> lk(st->transcript_mu);
    if (!st->transcript.empty() && st->transcript.back().first == role &&
        role == "assistant") {
        st->transcript.back().second += text;
    } else {
        st->transcript.push_back({role, text});
    }
}

// ---------------------------------------------------------------------------

- (BOOL)isBusy { return st->busy.load() ? YES : NO; }
- (void)cancel { st->cancel.store(true); }

- (void)steer:(NSString *)text {
    st->agent->queue_steering(Cpp(text));
}

- (void)sendMessage:(NSString *)text attachments:(NSArray *)paths {
    if (st->busy.load()) { [self steer:text]; return; }

    std::string body = Cpp(text);
    std::vector<std::string> files;
    // GCC's Objective-C++ frontend does not implement fast enumeration
    // (for/in), so iterate explicitly. This applies throughout the GUI.
    NSEnumerator *pe = [paths objectEnumerator];
    NSString *p;
    while ((p = [pe nextObject]) != nil) files.push_back(Cpp(p));

    [self appendTranscriptRole:"user" text:body];

    st->busy.store(true);
    st->cancel.store(false);

    // Build the message on this thread; loading an image can take a moment but
    // is bounded, and doing it here keeps the worker simple.
    const ModelInfo *mi = st->catalog.find(st->cfg.model);
    bool vision = mi ? mi->supports_images : false;

    Message um;
    if (files.empty()) {
        um = Message::user(body);
    } else {
        um.role = "user";
        um.parts.push_back(ContentPart::make_text(body));
        for (const std::string &f : files) {
            attach::Loaded l = attach::load(f, "auto", "auto", vision,
                                            st->agent->cwd());
            if (l.ok) um.parts.push_back(l.part);
            else um.parts.push_back(ContentPart::make_text(
                     "[could not attach " + f + ": " + l.error + "]"));
        }
    }

    if (st->worker.joinable()) st->worker.join();
    PPBridge *me = self;
    st->worker = std::thread([me, um]() { [me runTurn:um]; });

    [delegate bridgeDidStart];
}

- (void)runTurn:(const Message &)um {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

    Agent::Events ev;
    PPBridge *me = self;

    ev.on_text = [me, self](const std::string &d) {
        [self appendTranscriptRole:"assistant" text:d];
        [me performSelectorOnMainThread:@selector(mainText:)
                             withObject:Str(d)
                          waitUntilDone:NO];
    };
    ev.on_status = [me](const std::string &s) {
        [me performSelectorOnMainThread:@selector(mainStatus:)
                             withObject:Str(s)
                          waitUntilDone:NO];
    };
    ev.on_error = [me](const std::string &s) {
        [me performSelectorOnMainThread:@selector(mainError:)
                             withObject:Str(s)
                          waitUntilDone:NO];
    };
    ev.on_tool_start = [me](const ToolCall &tc) {
        NSArray *pair = [NSArray arrayWithObjects:Str(tc.name),
                                                  Str(json_preview(tc.arguments, 200)),
                                                  nil];
        [me performSelectorOnMainThread:@selector(mainToolStart:)
                             withObject:pair
                          waitUntilDone:NO];
    };
    ev.on_tool_done = [me](const ToolCall &tc, const ToolResult &tr) {
        NSArray *triple = [NSArray arrayWithObjects:
                              Str(tc.name), Str(tr.content),
                              [NSNumber numberWithBool:tr.is_error ? YES : NO], nil];
        [me performSelectorOnMainThread:@selector(mainToolDone:)
                             withObject:triple
                          waitUntilDone:NO];
    };
    ev.approve = [me, self](const std::string &name, ToolKind,
                            const ToolPreview &pv) -> bool {
        if (st->cfg.yolo) return true;
        {
            std::lock_guard<std::mutex> lk(st->approve_mu);
            st->approve_done = false;
            st->approve_result = false;
        }
        NSArray *triple = [NSArray arrayWithObjects:Str(name), Str(pv.title),
                                                    Str(pv.detail), nil];
        [me performSelectorOnMainThread:@selector(mainApprove:)
                             withObject:triple
                          waitUntilDone:NO];

        std::unique_lock<std::mutex> lk(st->approve_mu);
        st->approve_cv.wait(lk, [self] {
            return st->approve_done || st->cancel.load();
        });
        return st->approve_done && st->approve_result;
    };

    Agent::RunResult r = st->agent->run(um, ev, &st->cancel);

    st->busy.store(false);
    NSArray *pair = [NSArray arrayWithObjects:
                        [NSNumber numberWithLongLong:
                            static_cast<long long>(st->agent->session_usage().total_tokens)],
                        [NSNumber numberWithDouble:st->agent->session_usage().cost], nil];
    [me performSelectorOnMainThread:@selector(mainFinished:)
                         withObject:pair
                      waitUntilDone:NO];
    (void)r;
    [pool release];
}

// ---------------------------------------------------------------------------

- (NSString *)modelId { return Str(st->cfg.model); }

- (void)setModelId:(NSString *)mid {
    st->cfg.model = Cpp(mid);
    st->client->set_model(st->cfg.model);
    [self rebuildSystemPrompt];
}

- (BOOL)modelSupportsImages {
    const ModelInfo *mi = st->catalog.find(st->cfg.model);
    return (mi && mi->supports_images) ? YES : NO;
}

- (NSArray *)availableModels {
    st->catalog.load(*st->client, nullptr);
    NSMutableArray *out = [NSMutableArray array];
    for (const ModelInfo *m : st->catalog.search("", 500)) {
        NSMutableDictionary *d = [NSMutableDictionary dictionary];
        [d setObject:Str(m->id) forKey:@"id"];
        [d setObject:Str(m->name) forKey:@"name"];
        [d setObject:[NSNumber numberWithLongLong:m->context_length] forKey:@"context"];
        [d setObject:[NSNumber numberWithDouble:m->prompt_cost * 1e6] forKey:@"promptCost"];
        [d setObject:[NSNumber numberWithDouble:m->completion_cost * 1e6]
              forKey:@"completionCost"];
        [d setObject:[NSNumber numberWithBool:m->supports_tools ? YES : NO]
              forKey:@"tools"];
        [d setObject:[NSNumber numberWithBool:m->supports_images ? YES : NO]
              forKey:@"vision"];
        [out addObject:d];
    }
    return out;
}

- (NSArray *)favouriteModelIds {
    NSMutableArray *out = [NSMutableArray array];
    for (const std::string &id_ : favorite_models()) [out addObject:Str(id_)];
    return out;
}

- (NSString *)workingDirectory { return Str(st->agent->cwd()); }

- (void)setWorkingDirectory:(NSString *)dir {
    std::string d = Cpp(dir);
    if (chdir(d.c_str()) == 0) {
        st->agent->set_cwd(d);
        [self rebuildSystemPrompt];
    }
}

- (NSArray *)transcript {
    std::lock_guard<std::mutex> lk(st->transcript_mu);
    NSMutableArray *out = [NSMutableArray array];
    for (const auto &e : st->transcript) {
        NSMutableDictionary *d = [NSMutableDictionary dictionary];
        [d setObject:Str(e.first) forKey:@"role"];
        [d setObject:Str(e.second) forKey:@"text"];
        [out addObject:d];
    }
    return out;
}

- (void)newConversation {
    st->agent->reset();
    [self rebuildSystemPrompt];
    st->agent->set_session_path(session::path_for(session::new_id()));
    std::lock_guard<std::mutex> lk(st->transcript_mu);
    st->transcript.clear();
}

- (NSArray *)savedSessions {
    NSMutableArray *out = [NSMutableArray array];
    for (const session::Meta &m : session::list(60)) {
        NSMutableDictionary *d = [NSMutableDictionary dictionary];
        [d setObject:Str(m.id) forKey:@"id"];
        [d setObject:Str(m.title.empty() ? "(untitled)" : m.title) forKey:@"title"];
        [d setObject:Str(m.age()) forKey:@"age"];
        [d setObject:Str(m.cwd) forKey:@"cwd"];
        [d setObject:[NSNumber numberWithInt:m.message_count] forKey:@"messages"];
        [d setObject:[NSNumber numberWithDouble:m.cost] forKey:@"cost"];
        [out addObject:d];
    }
    return out;
}

- (BOOL)loadSessionWithId:(NSString *)sessionId {
    std::string path = session::path_for(Cpp(sessionId));
    std::string text, err;
    if (!read_file_text(path, &text, &err)) return NO;
    if (!LoadSessionInto(st->agent.get(), text, &err)) return NO;
    [self rebuildSystemPrompt];
    st->agent->set_session_path(path);

    // Rebuild the visible transcript from the restored history.
    std::lock_guard<std::mutex> lk(st->transcript_mu);
    st->transcript.clear();
    for (const Message &m : st->agent->history()) {
        if (m.role == "system" || m.role == "tool") continue;
        std::string t = m.display_text();
        if (trim(t).empty()) continue;
        st->transcript.push_back({m.role, t});
    }
    return YES;
}

- (long long)totalTokens {
    return static_cast<long long>(st->agent->session_usage().total_tokens);
}
- (double)totalCost { return st->agent->session_usage().cost; }

@end
