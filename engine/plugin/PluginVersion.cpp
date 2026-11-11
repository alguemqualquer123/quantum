// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "PluginVersion.h"

#include <cstdio>

using namespace quantum;
using namespace quantum::plugin;

namespace {

    // Accepts an optional leading "v" the way git tags are written, and trailing build metadata
    // ("1.2.3-10-gabc123") the way `git describe` produces it.
    bool readNumber(const std::string& text, size_t& cursor, int& out) {
        size_t start = cursor;
        while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') cursor++;
        if (cursor == start) return false;
        out = std::atoi(text.substr(start, cursor - start).c_str());
        return true;
    }

}

Version quantum::plugin::parseVersion(const std::string& text) {
    Version out;
    size_t cursor = 0;

    std::string core = text;
    if (!core.empty() && (core[0] == 'v' || core[0] == 'V')) core.erase(core.begin());

    // Cut the build metadata before parsing so "1.2.3-dirty" reads as 1.2.3.
    for (size_t i = 0; i < core.size(); i++) {
        const char c = core[i];
        if (c == '-' || c == '+' || c == ' ') {
            core.erase(i);
            break;
        }
    }

    int numbers[3] = { 0, 0, 0 };
    int read = 0;
    while (read < 3) {
        if (!readNumber(core, cursor, numbers[read])) break;
        read++;
        if (read < 3) {
            if (cursor >= core.size() || core[cursor] != '.') break;
            cursor++;
        }
    }

    // Anything left over was not a version separator, so the text was not a version either.
    if (read == 0) return out;
    if (cursor < core.size() && core[cursor] != '.') return out;

    out.major = numbers[0];
    out.minor = read > 1 ? numbers[1] : 0;
    out.patch = read > 2 ? numbers[2] : 0;
    out.parsed = true;
    return out;
}

int quantum::plugin::compareVersions(const Version& left, const Version& right) {
    if (left.major != right.major) return left.major < right.major ? -1 : 1;
    if (left.minor != right.minor) return left.minor < right.minor ? -1 : 1;
    if (left.patch != right.patch) return left.patch < right.patch ? -1 : 1;
    return 0;
}

std::string quantum::plugin::versionToString(const Version& version) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%d.%d.%d", version.major, version.minor, version.patch);
    return std::string(buffer);
}

Version quantum::plugin::engineVersionNumber() {
    return parseVersion(engineVersionText());
}

const char* quantum::plugin::engineVersionText() {
    return QUANTUM_ENGINE_VERSION;
}

bool quantum::plugin::isVersionCompatible(const Version& available, const Version& minimum,
                                         std::string& reason) {
    if (!minimum.parsed) {
        reason = "manifest minEngineVersion is not a version";
        return false;
    }
    if (!available.parsed) {
        reason = "engine version '" + std::string(engineVersionText()) +
                 "' is not a number; minEngineVersion check skipped";
        return true;
    }
    if (compareVersions(available, minimum) < 0) {
        reason = "plugin needs engine " + versionToString(minimum) + ", running " +
                 versionToString(available);
        return false;
    }
    reason.clear();
    return true;
}
