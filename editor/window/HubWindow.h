// (c) SR VINIX
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"

namespace quantum::editor {

    // The Quantum Hub: the launcher shown by `quantum-editor --hub`. It lists the
    // known projects and the installed editor builds, creates new projects and
    // opens projects with the chosen build by spawning a separate editor
    // process. The Hub never loads a project itself.
    class HubWindow {
    private:
        struct EditorInstall {
            std::string label;
            std::filesystem::path path;
            bool valid = false;
        };

        struct RemoteRelease {
            std::string version;
            std::string label;
            std::string url;
            std::string sha256;
            unsigned long long size = 0;
        };

        // A background install copy (build folders carry ~170 MB of engine
        // files); the hub UI keeps running and shows progress while it copies.
        struct InstallJob {
            std::thread worker;
            std::atomic<bool> running{false};
            std::atomic<bool> cancelRequested{false};
            std::atomic<int> copiedFiles{0};
            std::atomic<int> totalFiles{0};
            std::atomic<bool> success{false};
            std::mutex mutex;
            std::string error;
            std::string label;
            std::filesystem::path destination;
            bool missingResourceServer = false;
            std::atomic<int> phase{0}; // 0=scan, 1=download, 2=verify, 3=extract
        };
        std::unique_ptr<InstallJob> installJob;

        struct ReleaseJob {
            std::thread worker;
            std::atomic<bool> running{false};
            std::atomic<bool> cancelRequested{false};
            std::atomic<bool> success{false};
            std::mutex mutex;
            std::string error;
            std::vector<RemoteRelease> releases;
        };
        std::unique_ptr<ReleaseJob> releaseJob;

        enum class Tab {
            Projects,
            Installs
        };

        Tab tab = Tab::Projects;

        std::vector<EditorInstall> installs;
        bool installsScanned = false;
        // Index into installs; -1 opens with the running build.
        int selectedInstall = -1;
        std::string defaultInstallLabel;

        char releaseManifestUrl[512] = "";
        std::vector<RemoteRelease> remoteReleases;

        std::vector<std::string> templates;
        bool templatesScanned = false;
        // 0 = blank project, otherwise templates[selectedTemplate - 1].
        int selectedTemplate = 0;

        bool createDialogOpen = false;
        bool createPopupOpened = false;
        char projectName[128] = "";
        char projectLocation[512] = "";
        std::string dialogError;

        int pendingRemove = -1;
        bool removePopupOpened = false;

        std::string statusMessage;
        double statusMessageTime = 0.0;

        std::filesystem::path hubDataDir() const;
        std::filesystem::path installsDir() const;
        std::filesystem::path hubConfigPath() const;
        std::filesystem::path effectiveEditorDir() const;

        void loadConfig();
        void saveConfig();
        void scanInstalls();
        void scanTemplates();
        void setStatus(const std::string& message);

        void fetchRemoteReleases();
        void pollRemoteReleases();

        void launchProject(const std::filesystem::path& projectPath);
        void launchCreateProject(const std::filesystem::path& projectPath, const std::string& templateName);
        void openCreateDialog();
        void startInstall(const std::filesystem::path& sourceDir, const std::string& label);
        void startRemoteInstall(const RemoteRelease& release);
        void pollInstallJob();
        void removeInstall(int index);

        void drawProjectsTab();
        void drawInstallsTab();
        void drawCreateDialog();
        void drawRemoveDialog();

    public:
        HubWindow();
        ~HubWindow();

        // Draws the whole hub UI. Called once per frame from App::show() while
        // the editor runs in Hub mode.
        void show();
    };

}
