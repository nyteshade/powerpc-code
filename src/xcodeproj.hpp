// xcodeproj.hpp -- reading and modifying Xcode 3 project files.
//
// Xcode 3 stores project.pbxproj as an old-style ASCII plist (see plist.hpp), a
// flat `objects` dictionary of 24-hex-character ids pointing at typed records.
// Editing it by hand is error-prone in a specific way: a project that parses but
// has a dangling reference will open and then behave strangely, so every
// mutation here updates all of the places a reference has to appear.
#pragma once

#include "common.hpp"
#include "plist.hpp"
#include "tools.hpp"

namespace ppcode::xcode {

struct BuildConfig {
    std::string id;
    std::string name;                                  // Debug, Release
    std::vector<std::pair<std::string, std::string>> settings;
};

struct Target {
    std::string id;
    std::string name;
    std::string type;              // com.apple.product-type.application, ...
    std::string product_name;
    std::vector<std::string> source_files;
    std::vector<std::string> resource_files;
    std::vector<std::string> frameworks;
    std::vector<BuildConfig> configs;
};

class Project {
public:
    // `path` may be the .xcodeproj bundle or the project.pbxproj inside it.
    bool load(const std::string& path, std::string* error);
    bool save(std::string* error) const;

    const std::string& pbxproj_path() const { return pbxproj_path_; }
    std::string object_version() const;
    std::string compatibility_version() const;

    std::vector<Target> targets() const;
    std::vector<BuildConfig> project_configs() const;

    // Human-readable summary for the info tool.
    std::string describe() const;

    // Add a file to the project and, when `target_name` is given, to that
    // target's compile or copy-resources phase.
    bool add_file(const std::string& file_path, const std::string& target_name,
                  const std::string& group_name, std::string* error);

    // Set (or remove, when `value` is empty and `remove` is true) a build
    // setting. `target_name` empty means the project level. `config_name` empty
    // means every configuration.
    bool set_setting(const std::string& key, const std::string& value,
                     const std::string& target_name, const std::string& config_name,
                     bool remove, std::string* error);

    // Link a system framework into a target.
    bool add_framework(const std::string& framework, const std::string& target_name,
                       std::string* error);

private:
    plist::ValuePtr root_;
    plist::ValuePtr objects_;
    std::string pbxproj_path_;

    plist::ValuePtr obj(const std::string& id) const;
    plist::ValuePtr project_object() const;
    std::string new_id() const;

    // Find a target by name; empty name returns the first target.
    std::string find_target(const std::string& name) const;
    std::string phase_of_target(const std::string& target_id,
                                const std::string& isa) const;
    std::vector<std::string> config_ids(const std::string& list_id,
                                        const std::string& config_name) const;
    std::string main_group() const;
    std::string find_group(const std::string& name) const;
    std::vector<std::string> files_in_phase(const std::string& phase_id) const;
};

// File type Xcode uses for a given extension, e.g. sourcecode.c.objc.
std::string file_type_for(const std::string& path);
bool is_source_extension(const std::string& path);

// Register xcode_info / xcode_add_file / xcode_set_setting / xcode_add_framework.
void add_tools(ToolRegistry& registry);

} // namespace ppcode::xcode
