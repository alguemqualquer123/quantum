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

#ifdef _WIN32
    // WIN32_LEAN_AND_MEAN comes from the target (see the .cmake); this file uses
    // no sockets, so plain <windows.h> is safe here.
    #include <windows.h>
#endif

namespace {

// Double-clicked from Explorer this tool gets a console of its own that is
// destroyed when it exits, so its error text flashes past and reads as "it does
// not open". Wait for a reader only in that case: a shell shares the console
// with the child (two or more attached processes) and a server the editor
// spawns has none at all, so scripted runs stay non-blocking.
#ifdef _WIN32
void waitForReaderIfWeOwnTheConsole() {
    DWORD attached[2] = {0, 0};
    if (GetConsoleProcessList(&attached[0], 2) != 1) return;
    std::atexit([] {
        std::fputs("\nPress Enter to close this window.\n", stdout);
        std::fflush(stdout);
        std::getchar();
    });
}
#else
void waitForReaderIfWeOwnTheConsole() {}
#endif

void printUsage(const char* argv0) {
    std::printf(
        "doriax-resource-server - local doriax asset index service\n"
        "Usage: %s --project <dir> [--port 0] [--once] [--verbose] [--token <hex>]\n"
        "                 [--parent <pid>] [--window | --no-window]\n"
        "  --project <dir>  Project directory to watch (assets/ lua/ scenes/ scripts/).\n"
        "  --port <n>       TCP port on 127.0.0.1; 0 (default) picks an ephemeral port\n"
        "                   and prints 'doriax-resource-server: listening on 127.0.0.1:<port>'.\n"
        "  --once           Scan once, print a summary, exit 0 (no socket).\n"
        "  --verbose        Log scans and requests to stderr.\n"
        "  --token <hex>    Require {\"token\":\"<hex>\"} in every request.\n"
        "  --parent <pid>   Editor process id, used only by the tray menu's\n"
        "                   'Close server and editor'. Without a tray (POSIX) it is ignored.\n"
        "  --window         Open the connections window (clients, requests, details).\n"
        "  --no-window      Never open it, even when launched by hand.\n"
        "With no --parent the window opens by default, because a server started by\n"
        "hand is the user's own; one the editor spawns stays in the notification\n"
        "area. Closing the window hides it - the service keeps serving - and a\n"
        "click on the tray icon brings it back.\n"
        "The server keeps running after the editor closes and lives in the Windows\n"
        "notification area; its menu has 'Close server'.\n"
        "Protocol: one newline-delimited JSON request/response per TCP connection:\n"
        "  {\"cmd\":\"ping\"} | {\"cmd\":\"status\"} | {\"cmd\":\"index\"}\n"
        "  {\"cmd\":\"entry\",\"path\":\"assets/sprite.png\"} | {\"cmd\":\"changes\",\"since\":<scanId>}\n"
        "  {\"cmd\":\"connections\"}  who is connected and what they asked for\n",
        argv0);
}

bool needsValue(const std::string& flag) {
    return flag == "--project" || flag == "--port" || flag == "--token" || flag == "--parent";
}

}

int main(int argc, char** argv) {
    waitForReaderIfWeOwnTheConsole();

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
        } else if (a == "--window") {
            options.wantWindow = true;
        } else if (a == "--no-window") {
            options.noWindow = true;
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

    if (options.wantWindow && options.noWindow) {
        std::fprintf(stderr, "doriax-resource-server: --window and --no-window are exclusive\n");
        return 1;
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
