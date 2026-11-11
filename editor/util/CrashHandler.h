// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>

namespace quantum::editor {

    // Appends the uncaught exception or signal that kills the editor, plus a backtrace, to
    // a local crash sidecar. Without it the window just disappears and the user has nothing
    // to send. The sidecar is kept separate from the live editor log so rotation cannot block
    // on the logger's file handle on Windows.
    class CrashHandler {
    public:
        static void install(const std::filesystem::path& logFile);

        // Records a bounded breadcrumb that is safe to update from worker threads. The value
        // is copied into fixed storage and is therefore also available if the process crashes
        // while the editor is between normal logging calls.
        static void setLastPhase(const char* phase) noexcept;
    };

}
