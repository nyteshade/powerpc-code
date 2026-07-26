#include "jobs.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace ppcode {

namespace {

std::string status_path(int id) {
    return JobManager::jobs_dir() + "/" + std::to_string(id) + ".status";
}
std::string meta_path(int id) {
    return JobManager::jobs_dir() + "/" + std::to_string(id) + ".json";
}
std::string log_path_for(int id) {
    return JobManager::jobs_dir() + "/" + std::to_string(id) + ".log";
}

bool pid_alive(pid_t pid) {
    if (pid <= 0) return false;
    // Signal 0 tests for existence and permission without delivering anything.
    if (kill(pid, 0) == 0) return true;
    return errno == EPERM;   // exists but owned by someone else
}

std::string human_duration(int64_t secs) {
    if (secs < 0) secs = 0;
    char buf[64];
    if (secs < 60)
        std::snprintf(buf, sizeof(buf), "%llds", static_cast<long long>(secs));
    else if (secs < 3600)
        std::snprintf(buf, sizeof(buf), "%lldm%02llds",
                      static_cast<long long>(secs / 60),
                      static_cast<long long>(secs % 60));
    else if (secs < 86400)
        std::snprintf(buf, sizeof(buf), "%lldh%02lldm",
                      static_cast<long long>(secs / 3600),
                      static_cast<long long>((secs % 3600) / 60));
    else
        std::snprintf(buf, sizeof(buf), "%lldd%02lldh",
                      static_cast<long long>(secs / 86400),
                      static_cast<long long>((secs % 86400) / 3600));
    return buf;
}

} // namespace

std::string Job::elapsed() const {
    int64_t end = finished_at > 0 ? finished_at
                                  : static_cast<int64_t>(std::time(nullptr));
    return human_duration(end - started_at);
}

json Job::to_json() const {
    return json{{"id", id},
                {"command", command},
                {"cwd", cwd},
                {"pid", static_cast<int64_t>(pid)},
                {"log_path", log_path},
                {"started_at", started_at},
                {"finished_at", finished_at},
                {"exit_code", exit_code}};
}

Job Job::from_json(const json& j) {
    Job job;
    job.id = static_cast<int>(jint(j, "id"));
    job.command = jstr(j, "command");
    job.cwd = jstr(j, "cwd");
    job.pid = static_cast<pid_t>(jint(j, "pid"));
    job.log_path = jstr(j, "log_path");
    job.started_at = jint(j, "started_at");
    job.finished_at = jint(j, "finished_at");
    job.exit_code = static_cast<int>(jint(j, "exit_code", -1));
    return job;
}

std::string JobManager::jobs_dir() {
    std::string base;
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
        base = xdg;
    else if (const char* home = std::getenv("HOME"); home && *home)
        base = std::string(home) + "/.cache";
    else
        base = "/tmp";
    return base + "/ppcode/jobs";
}

int JobManager::next_id() {
    std::error_code ec;
    fs::create_directories(jobs_dir(), ec);
    int max_id = 0;
    for (const auto& e : fs::directory_iterator(jobs_dir(), ec)) {
        std::string name = e.path().filename().string();
        if (!ends_with(name, ".json")) continue;
        int n = std::atoi(name.c_str());
        if (n > max_id) max_id = n;
    }
    return max_id + 1;
}

void JobManager::refresh(Job* j) {
    // A status file is the authoritative record: the wrapper shell writes the
    // exit code there when the command finishes. It is the only way to learn the
    // status of a process that is not our child, which is the normal case once
    // ppcode has been restarted.
    std::string text;
    if (read_file_text(status_path(j->id), &text, nullptr)) {
        std::string t = trim(text);
        if (!t.empty()) {
            j->exit_code = std::atoi(t.c_str());
            j->running = false;
            if (j->finished_at == 0) {
                std::error_code ec;
                auto ftime = fs::last_write_time(status_path(j->id), ec);
                (void)ftime;
                struct stat st;
                if (stat(status_path(j->id).c_str(), &st) == 0)
                    j->finished_at = static_cast<int64_t>(st.st_mtime);
                else
                    j->finished_at = static_cast<int64_t>(std::time(nullptr));
                // Persist so we do not re-stat forever.
                write_file_text(meta_path(j->id), j->to_json().dump(2), nullptr);
            }
            return;
        }
    }

    if (pid_alive(j->pid)) {
        j->running = true;
        return;
    }

    // No status file and no process: it died without the wrapper completing,
    // e.g. killed with SIGKILL or the machine rebooted.
    j->running = false;
    if (j->exit_code < 0) j->exit_code = -1;
}

int JobManager::start(const std::string& command, const std::string& cwd,
                      std::string* error) {
    std::error_code ec;
    fs::create_directories(jobs_dir(), ec);
    if (ec) {
        if (error) *error = "cannot create " + jobs_dir() + ": " + ec.message();
        return -1;
    }

    int id = next_id();
    Job job;
    job.id = id;
    job.command = command;
    job.cwd = cwd.empty() ? "." : cwd;
    job.log_path = log_path_for(id);
    job.started_at = static_cast<int64_t>(std::time(nullptr));

    // Truncate any stale files for this id.
    write_file_text(job.log_path, "", nullptr);
    std::remove(status_path(id).c_str());

    pid_t pid = fork();
    if (pid < 0) {
        if (error) *error = std::string("fork: ") + std::strerror(errno);
        return -1;
    }

    if (pid == 0) {
        // Detach completely: new session, no controlling terminal, so the job
        // is unaffected by ppcode exiting or by Ctrl+C in the TUI.
        setsid();

        int fd = open(job.log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) _exit(127);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);

        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }

        if (job.cwd != "." && chdir(job.cwd.c_str()) != 0) {
            std::fprintf(stderr, "ppcode: cannot chdir to %s: %s\n",
                         job.cwd.c_str(), std::strerror(errno));
            _exit(127);
        }

        // The wrapper records the exit code so a later process can read it
        // without being the parent.
        std::string wrapped = "{ " + command + "\n; } ; printf '%s' \"$?\" > " +
                              status_path(id) + ".tmp && mv " +
                              status_path(id) + ".tmp " + status_path(id);
        execl("/bin/sh", "sh", "-c", wrapped.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    job.pid = pid;
    job.running = true;

    // Do not wait for it; reparent to init by not reaping. Double-forking would
    // lose the pid, and we want it for liveness checks, so instead we simply
    // never block on this child. It becomes a zombie only if ppcode is still
    // running when it exits, and the status file covers that case.
    std::string err;
    if (!write_file_text(meta_path(id), job.to_json().dump(2) + "\n", &err)) {
        if (error) *error = "job started but metadata could not be written: " + err;
        return id;
    }
    log_line("job " + std::to_string(id) + " started (pid " +
             std::to_string(pid) + "): " + elide(command, 120));
    return id;
}

bool JobManager::get(int id, Job* out) const {
    std::string text;
    if (!read_file_text(meta_path(id), &text, nullptr)) return false;
    try {
        Job j = Job::from_json(json::parse(text));
        refresh(&j);
        *out = j;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<Job> JobManager::list() const {
    std::vector<Job> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(jobs_dir(), ec)) {
        std::string name = e.path().filename().string();
        if (!ends_with(name, ".json")) continue;
        int id = std::atoi(name.c_str());
        Job j;
        if (get(id, &j)) out.push_back(std::move(j));
    }
    std::sort(out.begin(), out.end(),
              [](const Job& a, const Job& b) { return a.id > b.id; });
    return out;
}

std::string JobManager::output(int id, size_t max_lines, size_t max_bytes,
                               std::string* error) const {
    Job j;
    if (!get(id, &j)) {
        if (error) *error = "no such job: " + std::to_string(id);
        return "";
    }
    std::string text;
    if (!read_file_text(j.log_path, &text, nullptr)) {
        if (error) *error = "no log for job " + std::to_string(id);
        return "";
    }
    if (text.empty()) return "(no output yet)";

    // Keep the tail: for a long build the recent lines are what matter.
    if (text.size() > max_bytes)
        text = "[...truncated, showing last " + std::to_string(max_bytes) +
               " bytes...]\n" + text.substr(text.size() - max_bytes);

    std::vector<std::string> lines = split(text, '\n');
    if (lines.size() > max_lines) {
        size_t drop = lines.size() - max_lines;
        std::vector<std::string> tail(lines.begin() + static_cast<long>(drop),
                                      lines.end());
        return "[..." + std::to_string(drop) + " earlier lines omitted...]\n" +
               join(tail, "\n");
    }
    return text;
}

bool JobManager::stop(int id, bool force, std::string* error) {
    Job j;
    if (!get(id, &j)) {
        if (error) *error = "no such job: " + std::to_string(id);
        return false;
    }
    if (!j.running) {
        if (error) *error = "job " + std::to_string(id) + " is not running";
        return false;
    }
    // The job is its own session leader, so its pid is also its process-group
    // id; signalling the group reaches the whole build tree.
    int sig = force ? SIGKILL : SIGTERM;
    if (kill(-j.pid, sig) != 0 && kill(j.pid, sig) != 0) {
        if (error) *error = std::string("kill: ") + std::strerror(errno);
        return false;
    }
    log_line("job " + std::to_string(id) + " sent " +
             (force ? "SIGKILL" : "SIGTERM"));
    return true;
}

int JobManager::prune(bool include_running) {
    int removed = 0;
    for (const Job& j : list()) {
        if (j.running && !include_running) continue;
        std::error_code ec;
        fs::remove(meta_path(j.id), ec);
        fs::remove(status_path(j.id), ec);
        fs::remove(j.log_path, ec);
        removed++;
    }
    return removed;
}

} // namespace ppcode
