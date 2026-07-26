// jobs.hpp -- detached background commands that outlive ppcode.
//
// The bash tool kills anything that runs past its timeout, which is right for
// ordinary commands and useless for the work this machine actually does: a
// MacPorts build or a large compile here can take hours, sometimes more than a
// day. Those need to be started, left alone, and checked on later -- possibly
// from a different ppcode session entirely.
//
// So jobs are spawned in their own session (setsid), their output goes to a log
// file, and their metadata is written to disk. Nothing is held in memory that a
// later process cannot reconstruct.
#pragma once

#include "common.hpp"
#include "tools.hpp"

#include <sys/types.h>

namespace ppcode {

struct Job {
    int id = 0;
    std::string command;
    std::string cwd;
    pid_t pid = 0;
    std::string log_path;
    int64_t started_at = 0;      // unix seconds
    int64_t finished_at = 0;     // 0 while running
    int exit_code = -1;          // valid once finished
    bool running = false;

    // Human-readable elapsed time.
    std::string elapsed() const;
    json to_json() const;
    static Job from_json(const json& j);
};

class JobManager {
public:
    // Directory holding job metadata and logs.
    static std::string jobs_dir();

    // Launch `command` detached. Returns the job id, or -1 with `error` set.
    int start(const std::string& command, const std::string& cwd,
              std::string* error);

    // Look up one job, refreshing its liveness and exit status from disk.
    bool get(int id, Job* out) const;

    // Every job we know about, newest first. Refreshes status.
    std::vector<Job> list() const;

    // Last `max_lines` lines of a job's log (or the whole thing if smaller).
    std::string output(int id, size_t max_lines, size_t max_bytes,
                       std::string* error) const;

    // Signal the job's process group. Returns false if it was not running.
    bool stop(int id, bool force, std::string* error);

    // Delete metadata and log for finished jobs. Returns how many were removed.
    int prune(bool include_running);

private:
    // Refresh `j` in place: is the pid still alive, and did a status file appear?
    static void refresh(Job* j);
    static int next_id();
};

// Register run_background / job_list / job_output / job_stop. The manager must
// outlive the registry.
void add_job_tools(ToolRegistry& registry, JobManager& jobs);

} // namespace ppcode
