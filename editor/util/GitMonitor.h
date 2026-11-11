// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace quantum::editor {

    namespace fs = std::filesystem;

    enum class GitHeadType {
        Branch,
        Tag,
        Commit
    };

    struct GitStatus {
        bool repository = false;
        GitHeadType headType = GitHeadType::Branch;
        std::string head; // branch, tag or short commit
        std::string upstream;
        int ahead = 0;
        int behind = 0;
        int staged = 0;
        int unstaged = 0; // untracked files included
        bool operationInProgress = false; // merge, rebase or conflicts

        bool operator==(const GitStatus& other) const;
        bool operator!=(const GitStatus& other) const { return !(*this == other); }
    };

    // Staged content of an open file, what its change markers compare with
    struct GitFileBase {
        bool tracked = false;
        std::string content;
        uint64_t version = 0;
    };

    // Polls git on a worker thread. Main thread only, apart from shutdown().
    class GitMonitor {
    public:
        ~GitMonitor();

        // Every frame, an empty path or disabled clears everything
        void update(const fs::path& projectPath, bool enabled);
        // Project-relative paths of the files open in the code editor
        void setFiles(const std::vector<std::string>& files);
        void requestRefresh();
        // Unfocused, only the index and HEAD are watched
        void setFocused(bool focused);

        const GitStatus& getStatus() const { return status; }
        // nullptr until the file has been looked up
        const GitFileBase* getFileBase(const std::string& file) const;

        void shutdown();

    private:
        using Clock = std::chrono::steady_clock;

        struct Request {
            uint64_t generation = 0;
            fs::path projectPath;
            bool enabled = false;
            bool focused = true;
            std::vector<std::string> files;
            bool filesChanged = false;
            bool refresh = false;
            bool stop = false;
        };

        struct FileUpdate {
            std::string file;
            std::optional<std::string> content; // nullopt when it has no base
        };

        struct Update {
            uint64_t generation = 0;
            std::optional<GitStatus> status;
            std::vector<FileUpdate> files;
        };

        // What the worker knows of the repository
        struct Repository {
            bool found = false;
            fs::path gitDir;
            std::string prefix; // of the project in the repository
            Clock::time_point nextDiscovery;
            Clock::time_point nextStatus;
            std::string stamp; // of the index and HEAD
            std::optional<GitStatus> status;
        };

        fs::path projectPath;
        bool enabled = false;
        bool focused = true;
        std::vector<std::string> files; // sorted
        uint64_t generation = 0;
        GitStatus status;
        std::unordered_map<std::string, GitFileBase> fileBases;
        uint64_t nextVersion = 1;

        std::mutex mutex;
        std::condition_variable wake;
        Request request;
        std::atomic<bool> cancel{false};
        std::thread worker;

        void run();
        void readFiles(const fs::path& git, const fs::path& projectDir, const std::string& prefix,
                       const std::vector<std::string>& openFiles, Update& update);
        void apply(Update update);
    };

}
