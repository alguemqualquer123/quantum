// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

// doriax-resource-server - local asset service for the doriax editor
// (GameMaker-style resource server). Standalone: standard library + platform
// sockets only; no engine or editor includes.
//
// Usage:
//   doriax-resource-server --project <dir> [--port 0] [--once] [--verbose]
//                          [--token <hex>] [--parent <pid>]

#include "ResourceServer.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

void printUsage(const char* argv0) {
    std::printf(
        "doriax-resource-server - local doriax asset index service\n"
        "Usage: %s --project <dir> [--port 0] [--once] [--verbose] [--token <hex>] [--parent <pid>]\n"
        "  --project <dir>  Project directory to watch (assets/ lua/ scenes/ scripts/).\n"
        "  --port <n>       TCP port on 127.0.0.1; 0 (default) picks an ephemeral port\n"
        "                   and prints 'doriax-resource-server: listening on 127.0.0.1:<port>'.\n"
        "  --once           Scan once, print a summary, exit 0 (no socket).\n"
        "  --verbose        Log scans and requests to stderr.\n"
        "  --token <hex>    Require {\"token\":\"<hex>\"} in every request.\n"
        "  --parent <pid>   Editor process id, used only by the tray menu's\n"
        "                   'Close server and editor'. Without a tray (POSIX) it is ignored.\n"
        "The server keeps running after the editor closes and lives in the Windows\n"
        "notification area; its menu has 'Close server'.\n"
        "Protocol: one newline-delimited JSON request/response per TCP connection:\n"
        "  {\"cmd\":\"ping\"} | {\"cmd\":\"status\"} | {\"cmd\":\"index\"}\n"
        "  {\"cmd\":\"entry\",\"path\":\"assets/sprite.png\"} | {\"cmd\":\"changes\",\"since\":<scanId>}\n",
        argv0);
}

bool needsValue(const std::string& flag) {
    return flag == "--project" || flag == "--port" || flag == "--token" || flag == "--parent";
}

}

int main(int argc, char** argv) {
    doriax::rs::ServerOptions options;
    bool haveProject = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (a == "--once") {
            options.once = true;
        } else if (a == "--verbose") {
            options.verbose = true;
        } else if (needsValue(a)) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "doriax-resource-server: %s requires a value\n", a.c_str());
                return 1;
            }
            std::string value = argv[++i];
            if (a == "--project") {
                options.projectDir = doriax::rs::fs::path(value);
                haveProject = true;
            } else if (a == "--port") {
                options.port = std::atoi(value.c_str());
                if (options.port < 0 || options.port > 65535) {
                    std::fprintf(stderr, "doriax-resource-server: bad --port '%s'\n", value.c_str());
                    return 1;
                }
            } else if (a == "--parent") {
                char* end = nullptr;
                const unsigned long long pid = std::strtoull(value.c_str(), &end, 10);
                if (end == value.c_str() || (end && *end != '\0') || pid == 0) {
                    std::fprintf(stderr, "doriax-resource-server: bad --parent '%s'\n",
                                 value.c_str());
                    return 1;
                }
                options.parentPid = pid;
            } else { // --token
                options.token = value;
            }
        } else {
            std::fprintf(stderr, "doriax-resource-server: unknown argument '%s'\n", a.c_str());
            printUsage(argv[0]);
            return 1;
        }
    }

    if (!haveProject) {
        std::fprintf(stderr, "doriax-resource-server: --project <dir> is required\n");
        printUsage(argv[0]);
        return 1;
    }

    std::error_code ec;
    if (!doriax::rs::fs::is_directory(options.projectDir, ec)) {
        std::fprintf(stderr, "doriax-resource-server: project directory not found: %s\n",
                     options.projectDir.generic_u8string().c_str());
        return 1;
    }

    return doriax::rs::runResourceServer(options);
}
