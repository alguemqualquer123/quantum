// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace quantum::editor::cli {

    namespace fs = std::filesystem;

    // Project Doctor: audits a project folder by reading project.yaml and its scene
    // files straight off disk (std::filesystem + yaml-cpp only). It deliberately
    // never calls Project::loadProject nor instantiates a Scene, so it still runs on
    // the broken projects the editor would crash or corrupt on - which is exactly
    // when the user needs it.

    enum class DoctorSeverity { Info, Warning, Error };

    const char* doctorSeverityName(DoctorSeverity severity);

    struct DoctorIssue {
        DoctorSeverity severity = DoctorSeverity::Info;
        std::string check;   // stable machine-readable id, e.g. "missing-scene"
        std::string file;    // project-relative display path (generic separators)
        int line = 0;        // 1-based YAML line, 0 when unknown
        std::string message;
    };

    struct DoctorResult {
        bool completed = false;  // false = the project could not be read at all
        std::string fatal;       // reason to print when !completed
        std::vector<DoctorIssue> issues;
        int errorCount = 0;
        int warningCount = 0;
        int infoCount = 0;
        int scenesAudited = 0;
        int entitiesAudited = 0;
    };

    // `projectDir` is the folder holding project.yaml. Pure file reading: every
    // failure becomes an issue row instead of an exception (the editor builds with
    // _HAS_EXCEPTIONS=0, nothing may throw toward main()).
    DoctorResult auditProject(const fs::path& projectDir);

    // Prints one line per issue plus a summary, and maps the result to a process
    // exit code: 0 clean (infos do not count), 1 warnings, 2 errors.
    int runProjectDoctorCommand(int argc, char** argv, const char* executableName);

}
