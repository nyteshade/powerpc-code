#import "GuiBridge.h"

#include "agent.hpp"
#include "appledocs.hpp"
#include "attach.hpp"
#include "builderr.hpp"
#include "bundler.hpp"
#include "config.hpp"
#include "envinfo.hpp"
#include "http.hpp"
#include "job.hpp"
#include "jobs.hpp"
#include "macgui.hpp"
#include "mcp.hpp"
#include "openrouter.hpp"
#include "rag.hpp"
#include "session.hpp"
#include "subagent.hpp"
#include "sysprompt.hpp"
#include "tools.hpp"
#include "webtools.hpp"
#include "xcodeproj.hpp"
#include "xib.hpp"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <filesystem>
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


// Move a file to the Trash rather than unlinking it.
//
// Deleting a conversation destroys work that cannot be reconstructed -- a
// hundred-message session took a hundred exchanges to produce. The Finder has
// had an undo for this since 1984 and there is no reason not to use it: the
// conversation still leaves the application, but it is recoverable by anyone
// who realises a moment later that they meant a different one.
bool TrashFile(const std::filesystem::path &path, std::error_code *ec_out) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return true;

  const char *home = std::getenv("HOME");
  if (!home || !*home) {
    // No home to put it in; removing is still better than silently failing.
    std::filesystem::remove(path, ec);
    if (ec_out) *ec_out = ec;

    return !ec;
  }

  std::filesystem::path trash = std::filesystem::path(home) / ".Trash";
  std::filesystem::create_directories(trash, ec);

  // The Trash is flat and a name can already be taken, so disambiguate rather
  // than clobbering whatever is in there.
  std::filesystem::path dest = trash / path.filename();
  for (int n = 1; std::filesystem::exists(dest, ec) && n < 1000; n++) {
    dest = trash / (path.stem().string() + " " + std::to_string(n) +
                    path.extension().string());
  }

  std::filesystem::rename(path, dest, ec);
  if (ec) {
    // A rename fails across volumes; copy and unlink instead.
    std::error_code ec2;
    std::filesystem::copy_file(path, dest,
                               std::filesystem::copy_options::overwrite_existing,
                               ec2);
    if (ec2) { if (ec_out) *ec_out = ec2; return false; }
    std::filesystem::remove(path, ec2);
  }

  if (ec_out) ec_out->clear();

  return true;
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

  // Indexing runs on its own thread and is deliberately serialised: one
  // database, one writer, and progress that means something.
  std::thread indexer;
  std::atomic<bool> indexing{false};
};

// Declared up front so calls that appear earlier in the file are type-checked.
// Without this GCC assumes an unknown selector returns id and takes ..., which
// is exactly how a wrong argument type gets through to a crash at runtime.
@interface PPBridge (Private)

- (void)rebuildSystemPrompt;

- (void)runTurn:(const Message &)um;

@end

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
  rag::add_tools(st->tools);
  add_job_tools(st->tools, st->jobs);
  web::add_tools(st->tools, web::SearchConfig::from_env());
  xcode::add_tools(st->tools);
  builderr::add_tools(st->tools);
  xib::add_tools(st->tools);
  bundle::add_tools(st->tools);
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
    if (st->indexer.joinable()) st->indexer.join();
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
  }

  else {
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
  }

  else {
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
  // [=] rather than [me, um]: a capture list with a comma is ambiguous with
  // message-send syntax and GCC's Objective-C++ front end parses it as a comma
  // expression instead. See knowledge/60-objcpp-gcc.md.
  st->worker = std::thread([=]() { [me runTurn:um]; });

  [delegate bridgeDidStart];
}

- (void)runTurn:(const Message &)um {
  NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

  Agent::Events ev;
  PPBridge *me = self;
  // The engine state is hoisted into a plain local because GCC cannot capture
  // the implicit `self` of an Objective-C method into a C++ lambda: it compiles,
  // but warns "'self' is used uninitialized" and the captured pointer is
  // garbage. Every `st->` below would have been read through it -- on the worker
  // thread, inside the tool approval path. Referring to `state` instead means
  // nothing in these lambdas touches `self` implicitly.
  BridgeState *state = st;

  ev.on_text = [=](const std::string &d) {
    [me appendTranscriptRole:"assistant" text:d];
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
  ev.approve = [=](const std::string &name, ToolKind,
                   const ToolPreview &pv) -> bool {
    if (state->cfg.yolo) return true;
    {
      std::lock_guard<std::mutex> lk(state->approve_mu);
      state->approve_done = false;
      state->approve_result = false;
    }
    NSArray *triple = [NSArray arrayWithObjects:Str(name), Str(pv.title),
                                                Str(pv.detail), nil];
    [me performSelectorOnMainThread:@selector(mainApprove:)
                         withObject:triple
                      waitUntilDone:NO];

    std::unique_lock<std::mutex> lk(state->approve_mu);
    state->approve_cv.wait(lk, [=] {
      return state->approve_done || state->cancel.load();
    });

    return state->approve_done && state->approve_result;
  };

  Agent::RunResult r = state->agent->run(um, ev, &state->cancel);

  state->busy.store(false);
  const Usage &used = state->agent->session_usage();
  NSArray *pair = [NSArray arrayWithObjects:
                      [NSNumber numberWithLongLong:
                          static_cast<long long>(used.total_tokens)],
                      [NSNumber numberWithDouble:used.cost], nil];
  [me performSelectorOnMainThread:@selector(mainFinished:)
                       withObject:pair
                    waitUntilDone:NO];
  (void)r;
  [pool release];
}

// ---------------------------------------------------------------------------

// ---- JSON <-> Foundation -------------------------------------------------

static id JsonToObjC(const ppcode::json &j);

static NSArray *JsonArrayToObjC(const ppcode::json &j) {
  NSMutableArray *a = [NSMutableArray array];
  for (size_t i = 0; i < j.size(); i++) [a addObject:JsonToObjC(j[i])];

  return a;
}

static id JsonToObjC(const ppcode::json &j) {
  if (j.is_object()) {
    NSMutableDictionary *d = [NSMutableDictionary dictionary];
    for (auto it = j.begin(); it != j.end(); ++it)
        [d setObject:JsonToObjC(it.value())
              forKey:[NSString stringWithUTF8String:it.key().c_str()]];

    return d;
  }
  if (j.is_array())   return JsonArrayToObjC(j);
  if (j.is_boolean()) return [NSNumber numberWithBool:j.get<bool>() ? YES : NO];
  if (j.is_number_integer())

      return [NSNumber numberWithLongLong:j.get<long long>()];
  if (j.is_number())  return [NSNumber numberWithDouble:j.get<double>()];
  if (j.is_string())  return Str(j.get<std::string>());

  return [NSNull null];
}

static ppcode::json ObjCToJson(id o) {
  if (o == nil || [o isKindOfClass:[NSNull class]]) return ppcode::json();
  if ([o isKindOfClass:[NSDictionary class]]) {
    ppcode::json j = ppcode::json::object();
    NSEnumerator *e = [o keyEnumerator];
    NSString *k;
    while ((k = [e nextObject]) != nil)
        j[Cpp(k)] = ObjCToJson([o objectForKey:k]);

    return j;
  }
  if ([o isKindOfClass:[NSArray class]]) {
    ppcode::json j = ppcode::json::array();
    NSEnumerator *e = [o objectEnumerator];
    id v;
    while ((v = [e nextObject]) != nil) j.push_back(ObjCToJson(v));

    return j;
  }
  if ([o isKindOfClass:[NSNumber class]]) {
    const char *t = [o objCType];
    if (t && (t[0] == 'c' || t[0] == 'B'))

        return ppcode::json([o boolValue] ? true : false);
    if (t && (t[0] == 'd' || t[0] == 'f')) return ppcode::json([o doubleValue]);

    return ppcode::json((long long)[o longLongValue]);
  }
  if ([o isKindOfClass:[NSString class]]) return ppcode::json(Cpp(o));

  return ppcode::json();
}

- (NSDictionary *)configDictionary {
  std::string text;
  ppcode::json j = ppcode::json::object();
  if (read_file_text(st->cfg.config_path, &text, nullptr)) {
    ppcode::json parsed = ppcode::json::parse(text, nullptr, false, true);
    if (!parsed.is_discarded() && parsed.is_object()) j = parsed;
  }
  // Surface the effective values even when the file omits them, so the
  // settings window shows what is actually in force.
  if (!j.contains("model"))       j["model"] = st->cfg.model;
  if (!j.contains("max_tokens"))  j["max_tokens"] = st->cfg.max_tokens;
  if (!j.contains("max_turns"))   j["max_turns"] = st->cfg.max_turns;
  if (!j.contains("temperature")) j["temperature"] = st->cfg.temperature;
  if (!j.contains("cache_mode"))  j["cache_mode"] = st->cfg.cache_mode;
  if (!j.contains("max_cost"))    j["max_cost"] = st->cfg.max_cost;
  if (!j.contains("yolo"))        j["yolo"] = st->cfg.yolo;
  if (!j.contains("web_search"))  j["web_search"] = st->cfg.web_search;

  return (NSDictionary *)JsonToObjC(j);
}

- (BOOL)saveConfigDictionary:(NSDictionary *)d error:(NSString **)err {
  ppcode::json j = ObjCToJson(d);
  std::error_code ec;
  std::filesystem::path p(st->cfg.config_path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);

  std::string werr;
  if (!write_file_text(st->cfg.config_path, j.dump(2) + "\n", &werr)) {
    if (err) *err = Str(werr);

    return NO;
  }
  // The file holds a credential.
  std::filesystem::permissions(
      st->cfg.config_path,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace, ec);
  [self reloadConfig];

  return YES;
}

- (void)reloadConfig {
  // Same race as -setModelId:: replacing cfg wholesale while a turn is running
  // would pull it out from under the worker thread.
  if (st->busy.load()) return;

  std::vector<std::string> warn;
  std::string path = st->cfg.config_path;
  st->cfg = ppcode::Config::load(path, &warn);
  st->client->set_config(st->cfg);
  [self rebuildSystemPrompt];
}

// ---- CLI installation ------------------------------------------------------

- (NSString *)bundledCLIPath {
  // Shipped alongside the GUI inside the bundle.
  NSString *res = [[NSBundle mainBundle] resourcePath];
  if (res) {
    NSString *p = [res stringByAppendingPathComponent:@"ppcode"];
    if ([[NSFileManager defaultManager] fileExistsAtPath:p]) return p;
  }
  // Running from the build tree rather than a bundle.
  NSString *dev = @"build/ppcode";
  if ([[NSFileManager defaultManager] fileExistsAtPath:dev]) return dev;

  return nil;
}

- (NSString *)cliVersionAt:(NSString *)path {
  if (!path || ![[NSFileManager defaultManager] isExecutableFileAtPath:path])

      return nil;
  ppcode::CommandResult r =
      ppcode::run_shell(Cpp(path) + " --version 2>/dev/null", ".", 10000, 4096,
                        nullptr);
  if (r.spawn_failed || r.exit_code != 0) return nil;
  std::string out = trim(r.output);

  return out.empty() ? nil : Str(out);
}

- (BOOL)installCLIToDirectory:(NSString *)dir error:(NSString **)err {
  NSString *src = [self bundledCLIPath];
  if (!src) {
    if (err) *err = @"The command line tool was not found inside the application.";

    return NO;
  }
  std::string d = Cpp(dir);
  std::string dest = d + "/ppcode";

  // Resolve the bundle path before linking to it, so the link does not depend
  // on the application's current working directory.
  std::error_code ec;
  std::filesystem::path s = std::filesystem::absolute(Cpp(src), ec);
  if (ec) s = Cpp(src);

  std::filesystem::create_directories(d, ec);

  // A symlink, not a copy.
  //
  // The tool inside the bundle finds its libraries at
  // @executable_path/../Frameworks, and Leopard's dyld resolves
  // @executable_path through a symlink -- verified with DYLD_PRINT_LIBRARIES.
  // So a link keeps working without MacPorts, while a copy of the bare
  // executable would land somewhere with no Frameworks directory beside it and
  // fail to launch. Linking also means the tool is updated whenever the
  // application is, and replacing a link that is in use is safe where
  // overwriting a running executable is not.
  std::filesystem::remove(dest, ec);
  std::filesystem::create_symlink(s, dest, ec);
  if (ec) {
    if (err)
        *err = [NSString stringWithFormat:@"Could not link into %@: %s",
                          dir, ec.message().c_str()];

    return NO;
  }

  return YES;
}

- (NSString *)macportsPrefix {
  for (const char *p : {"/opt/local", "/usr/local"}) {
    std::string probe = std::string(p) + "/bin/port";
    std::error_code ec;
    if (std::filesystem::exists(probe, ec)) return [NSString stringWithUTF8String:p];
  }

  return nil;
}

- (BOOL)isPortInstalled:(NSString *)port {
  NSString *prefix = [self macportsPrefix];
  if (!prefix) return NO;
  std::string cmd = Cpp(prefix) + "/bin/port -q installed " + Cpp(port) +
                    " 2>/dev/null";
  ppcode::CommandResult r = ppcode::run_shell(cmd, ".", 20000, 16384, nullptr);

  return (!r.spawn_failed && !trim(r.output).empty()) ? YES : NO;
}

- (BOOL)hasApiKey { return st->cfg.api_key.empty() ? NO : YES; }
- (NSString *)configPath { return Str(st->cfg.config_path); }

- (BOOL)saveApiKey:(NSString *)key error:(NSString **)err {
  std::string k = trim(Cpp(key));
  if (k.empty()) {
    if (err) *err = @"The key is empty.";

    return NO;
  }
  // A weak sanity check, so a pasted URL or a stray word is caught here
  // rather than as an opaque 401 later.
  if (k.size() < 20 || k.find(' ') != std::string::npos) {
    if (err) *err = @"That does not look like an OpenRouter key. They start "
                     "with sk-or- and contain no spaces.";

    return NO;
  }

  st->cfg.api_key = k;
  st->client->set_api_key(k);

  // Persist it. Config::save deliberately omits the key, so write it here and
  // tighten the permissions -- this is a credential.
  std::string path = st->cfg.config_path;
  json j = json::object();
  std::string existing;
  if (read_file_text(path, &existing, nullptr)) {
    json parsed = json::parse(existing, nullptr, false, true);
    if (!parsed.is_discarded() && parsed.is_object()) j = parsed;
  }
  j["api_key"] = k;
  j["model"] = st->cfg.model;

  std::error_code ec;
  std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);

  std::string werr;
  if (!write_file_text(path, j.dump(2) + "\n", &werr)) {
    if (err) *err = Str(werr);

    return NO;
  }
  std::filesystem::permissions(
      path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
      std::filesystem::perm_options::replace, ec);

  return YES;
}

- (NSString *)modelId { return Str(st->cfg.model); }

- (void)setModelId:(NSString *)mid {
  // The worker thread reads cfg and the agent's system prompt for the whole of
  // a turn, so changing them underneath it is a data race. Callers are expected
  // to check -isBusy first; this is the backstop.
  if (st->busy.load()) return;

  st->cfg.model = Cpp(mid);
  st->client->set_model(st->cfg.model);
  [self rebuildSystemPrompt];
}

- (NSString *)systemPrompt { return Str(st->agent->system_prompt()); }

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

- (BOOL)setWorkingDirectory:(NSString *)dir {
  // Refused mid-turn. Tools read the agent's cwd for the whole of a run, and
  // chdir() below is process-wide -- moving either underneath the worker thread
  // is how a write lands somewhere nobody asked for.
  if (st->busy.load()) return NO;

  std::string d = Cpp(dir);
  std::error_code ec;
  if (!std::filesystem::is_directory(d, ec)) return NO;

  // The agent's cwd is what tools are actually given; the chdir is belt and
  // braces for anything that resolves a relative path on its own.
  if (chdir(d.c_str()) != 0) return NO;

  st->agent->set_cwd(d);
  [self rebuildSystemPrompt];

  return YES;
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

// True when `sessionId` is the conversation currently loaded.
- (BOOL)isCurrentSession:(NSString *)sessionId {
  std::string want = session::path_for(Cpp(sessionId));

  return st->agent->session_path() == want;
}

- (BOOL)deleteSessionWithId:(NSString *)sessionId
                  wasLoaded:(BOOL *)current
                      error:(NSString **)err {
  if (st->busy.load()) {
    if (err) *err = @"Finish the current turn first.";

    return NO;
  }

  if (current) *current = [self isCurrentSession:sessionId];

  std::error_code ec;
  TrashFile(session::path_for(Cpp(sessionId)), &ec);
  if (ec) {
    if (err) *err = [NSString stringWithUTF8String:ec.message().c_str()];

    return NO;
  }

  return YES;
}

- (BOOL)archiveSessionWithId:(NSString *)sessionId
                   wasLoaded:(BOOL *)current
                       error:(NSString **)err {
  if (st->busy.load()) {
    if (err) *err = @"Finish the current turn first.";

    return NO;
  }

  if (current) *current = [self isCurrentSession:sessionId];

  std::string src = session::path_for(Cpp(sessionId));
  std::filesystem::path dir =
      std::filesystem::path(session::sessions_dir()) / "archive";

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::filesystem::rename(src, dir / std::filesystem::path(src).filename(), ec);
  if (ec) {
    if (err) *err = [NSString stringWithUTF8String:ec.message().c_str()];

    return NO;
  }

  return YES;
}

- (BOOL)exportSessionWithId:(NSString *)sessionId
                     toPath:(NSString *)path
                      error:(NSString **)err {
  std::string text, e;
  if (!read_file_text(session::path_for(Cpp(sessionId)), &text, &e)) {
    if (err) *err = Str(e);

    return NO;
  }

  // Non-throwing parse: an explicit catch clause crashes GCC's Objective-C++
  // front end outright. See knowledge/60-objcpp-gcc.md.
  ppcode::json j = ppcode::json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded()) {
    if (err) *err = @"That session file is not valid JSON.";

    return NO;
  }

  // One JSON object per line. Where the session has a messages array those are
  // the lines, which is the shape anything reading JSONL will expect; otherwise
  // fall back to the whole document as a single line rather than failing.
  std::string out;
  const ppcode::json *msgs = jptr(j, "messages");
  if (msgs && msgs->is_array()) {
    for (const ppcode::json &m : *msgs) out += m.dump() + "\n";
  }

  else {
    out = j.dump() + "\n";
  }

  if (!write_file_text(Cpp(path), out, &e)) {
    if (err) *err = Str(e);

    return NO;
  }

  return YES;
}

- (NSInteger)deleteAllSessions {
  if (st->busy.load()) return -1;

  std::error_code ec;
  NSInteger n = 0;
  std::filesystem::path dir(session::sessions_dir());

  std::filesystem::directory_iterator it(dir, ec), end;
  if (ec) return 0;

  // Collected first: removing entries while iterating the directory is not
  // something to rely on.
  std::vector<std::filesystem::path> doomed;
  for (; it != end; it.increment(ec)) {
    if (ec) break;
    if (it->is_regular_file(ec) && it->path().extension() == ".json")
        doomed.push_back(it->path());
  }

  for (size_t i = 0; i < doomed.size(); i++) {
    if (TrashFile(doomed[i], &ec)) n++;
  }

  return n;
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

// ---------------------------------------------------------------------------
// The search index
// ---------------------------------------------------------------------------

@implementation PPBridge (Index)

- (BOOL)isIndexing { return st->indexing.load() ? YES : NO; }

// Progress and completion are marshalled through these, because the work
// happens on the indexing thread and AppKit must only ever be touched from the
// main one.
- (void)mainIndexProgress:(NSArray *)pair {
  id d = [pair objectAtIndex:2];
  if ([d respondsToSelector:@selector(indexDidProgress:fraction:)])
      [d indexDidProgress:[pair objectAtIndex:0]
                 fraction:[[pair objectAtIndex:1] doubleValue]];
}

- (void)mainIndexFinished:(NSArray *)triple {
  st->indexing.store(false);

  id d = [triple objectAtIndex:2];
  if ([d respondsToSelector:@selector(indexDidFinish:added:)])
      [d indexDidFinish:[triple objectAtIndex:0]
                  added:[[triple objectAtIndex:1] integerValue]];
}

- (BOOL)indexPaths:(NSArray *)paths
    intoCollection:(NSString *)collection
          delegate:(id)indexDelegate {
  if (st->indexing.load()) return NO;
  if ([paths count] == 0) return NO;

  // Copied out of Foundation before the thread starts: the caller's array is
  // not ours to keep, and NSString is not safe to read from another thread
  // while the main one may release it.
  std::vector<std::string> list;
  NSEnumerator *e = [paths objectEnumerator];
  NSString *p;
  while ((p = [e nextObject]) != nil) list.push_back(Cpp(p));

  std::string coll = Cpp(collection);
  if (coll.empty()) coll = rag::kReference;

  if (st->indexer.joinable()) st->indexer.join();
  st->indexing.store(true);

  PPBridge *me = self;
  id del = indexDelegate;
  st->indexer = std::thread([=]() {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

    vec::Store store;
    std::string err;
    int documents = 0;
    std::string summary;

    if (!store.open(vec::Store::default_path(), &err)) {
      summary = "Could not open the index: " + err;
    }

    else {
      size_t done = 0;
      for (size_t i = 0; i < list.size(); i++) {
        rag::IndexStats stt = rag::index_path(
            store, list[i], coll,
            [&](const std::string &m) {
              double frac = list.size() ? (double)(done) / (double)list.size() : 0.0;
              NSArray *pair = [NSArray arrayWithObjects:
                                  Str(m), [NSNumber numberWithDouble:frac],
                                  del, nil];
              [me performSelectorOnMainThread:@selector(mainIndexProgress:)
                                   withObject:pair
                                waitUntilDone:NO];
            });
        documents += stt.documents;
        done++;
        if (!stt.error.empty() && summary.empty()) summary = stt.error;
      }

      int64_t chunks = 0, embedded = 0;
      store.stats(&chunks, &embedded, nullptr);
      if (summary.empty()) {
        summary = "Indexed " + std::to_string(documents) + " document" +
                  (documents == 1 ? "" : "s") + "; " +
                  std::to_string(chunks) + " chunks in the index.";
      }
    }

    NSArray *triple = [NSArray arrayWithObjects:
                          Str(summary),
                          [NSNumber numberWithInteger:documents],
                          del, nil];
    [me performSelectorOnMainThread:@selector(mainIndexFinished:)
                         withObject:triple
                      waitUntilDone:NO];
    [pool release];
  });

  return YES;
}

- (BOOL)reindexConversationsWithDelegate:(id)indexDelegate {
  if (st->indexing.load()) return NO;

  if (st->indexer.joinable()) st->indexer.join();
  st->indexing.store(true);

  PPBridge *me = self;
  id del = indexDelegate;
  st->indexer = std::thread([=]() {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

    vec::Store store;
    std::string err;
    std::string summary;
    int documents = 0;

    if (!store.open(vec::Store::default_path(), &err)) {
      summary = "Could not open the index: " + err;
    }

    else {
      rag::IndexStats stt = rag::index_all_sessions(
          store, [&](const std::string &m) {
            NSArray *pair = [NSArray arrayWithObjects:
                                Str(m), [NSNumber numberWithDouble:-1.0], del, nil];
            [me performSelectorOnMainThread:@selector(mainIndexProgress:)
                                 withObject:pair
                              waitUntilDone:NO];
          });
      documents = stt.documents;
      summary = "Re-indexed " + std::to_string(stt.documents) +
                " conversations; " + std::to_string(stt.chunks) + " chunks.";
    }

    NSArray *triple = [NSArray arrayWithObjects:
                          Str(summary),
                          [NSNumber numberWithInteger:documents], del, nil];
    [me performSelectorOnMainThread:@selector(mainIndexFinished:)
                         withObject:triple
                      waitUntilDone:NO];
    [pool release];
  });

  return YES;
}

- (NSArray *)indexedDocuments {
  NSMutableArray *out = [NSMutableArray array];

  vec::Store store;
  std::string err;
  if (!store.open(vec::Store::default_path(), &err)) return out;

  std::vector<vec::Store::Document> docs = store.list_documents();
  for (size_t i = 0; i < docs.size(); i++) {
    NSMutableDictionary *d = [NSMutableDictionary dictionary];
    [d setObject:Str(docs[i].doc_id) forKey:@"docId"];
    [d setObject:Str(docs[i].collection) forKey:@"collection"];
    [d setObject:[NSNumber numberWithLongLong:docs[i].chunks] forKey:@"chunks"];
    [d setObject:[NSNumber numberWithLongLong:docs[i].embedded]
          forKey:@"embedded"];

    // A conversation id means nothing on screen; a file path is mostly
    // directory. Show the part that identifies it.
    NSString *full = Str(docs[i].doc_id);
    NSString *shown = full;
    if ([full rangeOfString:@"/"].location != NSNotFound)
        shown = [full lastPathComponent];
    [d setObject:shown forKey:@"displayName"];

    [out addObject:d];
  }

  return out;
}

- (NSDictionary *)indexStatistics {
  vec::Store store;
  std::string err;
  int64_t chunks = 0, embedded = 0;
  if (store.open(vec::Store::default_path(), &err))
      store.stats(&chunks, &embedded, nullptr);

  return [NSDictionary dictionaryWithObjectsAndKeys:
             [NSNumber numberWithLongLong:chunks], @"chunks",
             [NSNumber numberWithLongLong:embedded], @"embedded",
             Str(vec::Store::default_path()), @"path",
             nil];
}

- (BOOL)removeIndexedDocument:(NSString *)docId {
  if (st->indexing.load()) return NO;

  vec::Store store;
  std::string err;
  if (!store.open(vec::Store::default_path(), &err)) return NO;

  return store.forget_document(Cpp(docId), &err) ? YES : NO;
}

- (BOOL)clearIndex {
  if (st->indexing.load()) return NO;

  // Removed document by document rather than by deleting the file, so anything
  // holding the database open keeps working.
  vec::Store store;
  std::string err;
  if (!store.open(vec::Store::default_path(), &err)) return NO;

  std::vector<vec::Store::Document> docs = store.list_documents();
  for (size_t i = 0; i < docs.size(); i++)
      store.forget_document(docs[i].doc_id, &err);

  return YES;
}

@end
