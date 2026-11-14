// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "window/HubWindow.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <system_error>

#include "AppSettings.h"
#include "EditorVersion.h"
#include "Out.h"
#include "ai/HttpClient.h"
#include "imgui.h"
#include "util/FileDialogs.h"
#include "util/FileUtils.h"
#include "util/PackageZip.h"
#include "util/ProcessLaunch.h"
#include "json.hpp"
#include "util/Checksum.h"

using namespace quantum;

namespace {

#ifdef _WIN32
constexpr const char* kEditorExecutableName = "quantum-editor.exe";
constexpr const char* kResourceServerExecutableName = "quantum-resource-server.exe";
#else
constexpr const char* kEditorExecutableName = "quantum-editor";
constexpr const char* kResourceServerExecutableName = "quantum-resource-server";
#endif

// Per-run state and heavy build outputs that must not travel into an installed
// build. Everything else (executable, dlls, engine/, templates/, tools) is what
// the editor needs to run.
constexpr const char* kInstallSkipFiles[] = {
    "editor.log", "editor_layout.ini", "settings.yaml", "imgui.ini", "stderr.txt", "stdout.txt"};

constexpr const char* kInstallSkipDirs[] = {"build", ".git", ".vs"};

constexpr const char* kCreatePopupId = "New Project";
constexpr const char* kRemovePopupId = "Remove Install";
constexpr const char* kDefaultHubManifestUrl =
    "https://raw.githubusercontent.com/alguemqualquer123/quantum/main/release-manifest.json";

using Json = nlohmann::json;

std::string releasePlatform() {
#if defined(_WIN32) && defined(_M_ARM64)
    return "windows-arm64";
#elif defined(_WIN32)
    return "windows-x64";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(__APPLE__)
    return "macos-x64";
#elif defined(__aarch64__)
    return "linux-arm64";
#else
    return "linux-x64";
#endif
}

bool isAllowedRemoteUrl(const std::string& url) {
    if (url.rfind("https://", 0) == 0) {
        return true;
    }
    // Plain HTTP is useful for a local release server during development, but a
    // public Hub must use HTTPS so the manifest and archive cannot be replaced in transit.
    return url.rfind("http://127.0.0.1", 0) == 0 ||
           url.rfind("http://localhost", 0) == 0;
}

std::string safeInstallLabel(std::string label) {
    if (label.empty()) {
        label = "Quantum";
    }
    for (char& character : label) {
        const bool safe = (character >= 'a' && character <= 'z') ||
                          (character >= 'A' && character <= 'Z') ||
                          (character >= '0' && character <= '9') ||
                          character == '-' || character == '_' || character == '.';
        if (!safe) {
            character = '_';
        }
    }
    while (!label.empty() && (label.back() == '.' || label.back() == ' ')) {
        label.pop_back();
    }
    return label.empty() ? "Quantum" : label;
}

std::string jsonString(const Json& node, const char* key) {
    const auto found = node.find(key);
    return found != node.end() && found->is_string() ? found->get<std::string>() : std::string();
}

bool isInstallSkippedFile(const std::filesystem::path& path) {
    const std::string name = path.filename().string();
    for (const char* skipped : kInstallSkipFiles) {
        if (name == skipped) {
            return true;
        }
    }
    return path.extension() == ".pdb";
}

std::filesystem::path homeDirectory() {
#if defined(_WIN32)
    if (const char* profile = std::getenv("USERPROFILE"); profile && *profile) {
        return std::filesystem::path(profile);
    }
#else
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home);
    }
#endif
    return {};
}

std::string trimmed(const char* value) {
    std::string text = value ? value : "";
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// Long paths are elided in the middle so the row buttons stay visible.
std::string elideMiddle(const std::string& text, size_t maxCharacters) {
    if (text.size() <= maxCharacters || maxCharacters < 6) {
        return text;
    }
    const size_t head = (maxCharacters - 3) / 2;
    const size_t tail = maxCharacters - 3 - head;
    return text.substr(0, head) + "..." + text.substr(text.size() - tail);
}

// Long build trees (engine sources included in some builds) can exceed the
// Windows MAX_PATH limit; the extended-length prefix lifts it for copy/remove.
std::filesystem::path extendedLengthPath(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring wide = path.wstring();
    if (wide.size() < 4 || wide.compare(0, 4, L"\\\\?\\") == 0) {
        return path;
    }
    if (wide.compare(0, 2, L"\\\\") == 0) {
        return std::filesystem::path(L"\\\\?\\UNC\\" + wide.substr(2));
    }
    return std::filesystem::path(L"\\\\?\\" + wide);
#else
    return path;
#endif
}

// Counts the files an install would copy (same filters copyBuildFolder uses).
int countBuildFiles(const std::filesystem::path& sourceDirIn) {
    std::error_code ec;
    const std::filesystem::path sourceDir = extendedLengthPath(sourceDirIn);
    const std::filesystem::recursive_directory_iterator end;
    int count = 0;
    for (std::filesystem::recursive_directory_iterator it(
             sourceDir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) {
            const std::string name = it->path().filename().string();
            for (const char* skipped : kInstallSkipDirs) {
                if (name == skipped) {
                    it.disable_recursion_pending();
                    break;
                }
            }
            continue;
        }
        if (!isInstallSkippedFile(it->path())) {
            ++count;
        }
    }
    return count;
}

// Copies a build folder into an install, skipping per-run state. Runs on the
// install worker thread: cancelRequested is checked between files and
// copiedFiles feeds the hub progress bar.
bool copyBuildFolder(const std::filesystem::path& sourceDirIn,
                     const std::filesystem::path& destinationDirIn,
                     const std::atomic<bool>& cancelRequested,
                     std::atomic<int>& copiedFiles,
                     std::string& errorOut) {
    std::error_code ec;
    const std::filesystem::path sourceDir = extendedLengthPath(sourceDirIn);
    const std::filesystem::path destinationDir = extendedLengthPath(destinationDirIn);
    const std::filesystem::recursive_directory_iterator end;
    for (std::filesystem::recursive_directory_iterator it(
             sourceDir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != end; it.increment(ec)) {
        if (cancelRequested) {
            errorOut = "cancelled";
            return false;
        }
        const std::filesystem::path& source = it->path();
        if (it->is_directory(ec)) {
            const std::string name = source.filename().string();
            for (const char* skipped : kInstallSkipDirs) {
                if (name == skipped) {
                    it.disable_recursion_pending();
                    break;
                }
            }
            continue;
        }
        if (isInstallSkippedFile(source)) {
            continue;
        }

        const std::filesystem::path relative = source.lexically_relative(sourceDir);
        if (relative.empty()) {
            errorOut = source.string();
            return false;
        }

        const std::filesystem::path target = destinationDir / relative;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            errorOut = ec.message();
            return false;
        }
        std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            errorOut = target.string() + ": " + ec.message();
            return false;
        }
        copiedFiles.fetch_add(1);
    }
    if (ec) {
        errorOut = ec.message();
        return false;
    }
    return true;
}

// The folder the hub used before the fork took the Quantum name.
std::filesystem::path legacyHubDataDir() {
#if defined(_WIN32)
    if (const char* localAppData = std::getenv("LOCALAPPDATA"); localAppData && *localAppData) {
        return std::filesystem::path(localAppData) / "Quantum" / "Hub";
    }
    if (const char* appData = std::getenv("APPDATA"); appData && *appData) {
        return std::filesystem::path(appData) / "Quantum" / "Hub";
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / "Library" / "Application Support" / "Quantum" / "Hub";
    }
#else
    if (const char* dataHome = std::getenv("XDG_DATA_HOME"); dataHome && *dataHome) {
        return std::filesystem::path(dataHome) / "quantum" / "hub";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local" / "share" / "quantum" / "hub";
    }
#endif
    return {};
}

} // namespace

quantum::editor::HubWindow::HubWindow() {
    FileUtils::adoptLegacyDirectory(legacyHubDataDir(), hubDataDir());
    std::snprintf(releaseManifestUrl, sizeof(releaseManifestUrl), "%s", kDefaultHubManifestUrl);
#ifdef QUANTUM_HUB_DEFAULT_MANIFEST_URL
    std::snprintf(releaseManifestUrl, sizeof(releaseManifestUrl), "%s",
                  QUANTUM_HUB_DEFAULT_MANIFEST_URL);
#endif
    loadConfig();
}

quantum::editor::HubWindow::~HubWindow() {
    if (releaseJob && releaseJob->worker.joinable()) {
        releaseJob->cancelRequested = true;
        releaseJob->worker.join();
    }
    if (installJob && installJob->worker.joinable()) {
        // Ask the worker to stop early so closing the hub never waits for a
        // full copy to finish.
        installJob->cancelRequested = true;
        installJob->worker.join();
    }
}

std::filesystem::path quantum::editor::HubWindow::hubDataDir() const {
#if defined(_WIN32)
    if (const char* localAppData = std::getenv("LOCALAPPDATA"); localAppData && *localAppData) {
        return std::filesystem::path(localAppData) / "Quantum" / "Hub";
    }
    if (const char* appData = std::getenv("APPDATA"); appData && *appData) {
        return std::filesystem::path(appData) / "Quantum" / "Hub";
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / "Library" / "Application Support" / "Quantum" / "Hub";
    }
#else
    if (const char* dataHome = std::getenv("XDG_DATA_HOME"); dataHome && *dataHome) {
        return std::filesystem::path(dataHome) / "quantum" / "hub";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".local" / "share" / "quantum" / "hub";
    }
#endif
    return std::filesystem::current_path() / "quantum-hub";
}

std::filesystem::path quantum::editor::HubWindow::installsDir() const {
    return hubDataDir() / "installs";
}

std::filesystem::path quantum::editor::HubWindow::hubConfigPath() const {
    return hubDataDir() / "hub.yaml";
}

void quantum::editor::HubWindow::loadConfig() {
    std::ifstream input(hubConfigPath());
    if (!input.is_open()) {
        return;
    }
    std::string line;
    while (std::getline(input, line)) {
        const size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);
        if (key == "default_install" && value != "-") {
            defaultInstallLabel = value;
        } else if (key == "manifest_url") {
            std::snprintf(releaseManifestUrl, sizeof(releaseManifestUrl), "%s", value.c_str());
        }
    }
}

void quantum::editor::HubWindow::saveConfig() {
    std::error_code ec;
    std::filesystem::create_directories(hubDataDir(), ec);
    std::ofstream output(hubConfigPath(), std::ios::trunc);
    if (!output.is_open()) {
        return;
    }
    const std::string selected =
        selectedInstall >= 0 && selectedInstall < static_cast<int>(installs.size())
            ? installs[selectedInstall].label
            : std::string("-");
    output << "default_install=" << selected << "\n";
    output << "manifest_url=" << releaseManifestUrl << "\n";
}

void quantum::editor::HubWindow::scanInstalls() {
    installs.clear();
    std::error_code ec;
    const std::filesystem::path directory = installsDir();
    if (std::filesystem::is_directory(directory, ec)) {
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory, ec)) {
            if (!entry.is_directory(ec)) {
                continue;
            }
            EditorInstall install;
            install.label = entry.path().filename().string();
            install.path = entry.path();
            install.valid = std::filesystem::exists(entry.path() / kEditorExecutableName, ec);
            installs.push_back(std::move(install));
        }
    }
    std::sort(installs.begin(), installs.end(), [](const EditorInstall& left, const EditorInstall& right) {
        return left.label < right.label;
    });

    selectedInstall = -1;
    for (int i = 0; i < static_cast<int>(installs.size()); ++i) {
        if (!defaultInstallLabel.empty() && installs[i].label == defaultInstallLabel && installs[i].valid) {
            selectedInstall = i;
            break;
        }
    }
    installsScanned = true;
}

void quantum::editor::HubWindow::scanTemplates() {
    templates.clear();
    std::error_code ec;
    const std::filesystem::path templatesRoot = FileUtils::getTemplatesDir();
    if (!templatesRoot.empty() && std::filesystem::is_directory(templatesRoot, ec)) {
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(templatesRoot, ec)) {
            if (!entry.is_directory(ec)) {
                continue;
            }
            if (!std::filesystem::exists(entry.path() / "project.yaml", ec)) {
                continue;
            }
            templates.push_back(entry.path().filename().string());
        }
    }
    std::sort(templates.begin(), templates.end());
    if (selectedTemplate > static_cast<int>(templates.size())) {
        selectedTemplate = 0;
    }
    templatesScanned = true;
}

void quantum::editor::HubWindow::setStatus(const std::string& message) {
    statusMessage = message;
    statusMessageTime = ImGui::GetTime();
    Out::info("Hub: %s", message.c_str());
}

void quantum::editor::HubWindow::fetchRemoteReleases() {
    if (releaseJob && releaseJob->running) {
        setStatus("Release list is already loading.");
        return;
    }

    const std::string manifestUrl = trimmed(releaseManifestUrl);
    if (manifestUrl.empty()) {
        setStatus("Set a release manifest URL first.");
        return;
    }
    if (!isAllowedRemoteUrl(manifestUrl)) {
        setStatus("Release manifest must use HTTPS (HTTP is allowed only for localhost). ");
        return;
    }

    auto job = std::make_unique<ReleaseJob>();
    job->running = true;
    ReleaseJob* jobPointer = job.get();
    job->worker = std::thread([jobPointer, manifestUrl]() {
        ai::HttpClient client;
        const ai::HttpResponse response = client.get(manifestUrl, {}, &jobPointer->cancelRequested);
        std::string error;
        std::vector<RemoteRelease> releases;

        if (!response.error.empty()) {
            error = response.error;
        } else if (response.status < 200 || response.status >= 300) {
            error = "manifest returned HTTP " + std::to_string(response.status);
        } else {
            const Json root = Json::parse(response.body, nullptr, false);
            const auto entries = root.is_object() ? root.find("releases") : root.end();
            if (root.is_discarded() || !root.is_object() ||
                entries == root.end() || !entries->is_array()) {
                error = "manifest must contain a releases array";
            } else {
                for (const Json& node : *entries) {
                    if (!node.is_object()) {
                        continue;
                    }
                    const std::string platform = jsonString(node, "platform");
                    if (!platform.empty() && platform != releasePlatform()) {
                        continue;
                    }

                    RemoteRelease release;
                    release.version = jsonString(node, "version");
                    release.label = jsonString(node, "label");
                    release.url = jsonString(node, "url");
                    release.sha256 = jsonString(node, "sha256");
                    const auto size = node.find("size");
                    if (size != node.end() && size->is_number_unsigned()) {
                        release.size = size->get<unsigned long long>();
                    }
                    if (release.label.empty()) {
                        release.label = "Quantum " + release.version;
                    }
                    if (release.version.empty() || release.url.empty() || release.sha256.empty()) {
                        continue;
                    }
                    if (!isAllowedRemoteUrl(release.url)) {
                        continue;
                    }
                    releases.push_back(std::move(release));
                }
                std::sort(releases.begin(), releases.end(), [](const RemoteRelease& left,
                                                               const RemoteRelease& right) {
                    return left.version > right.version;
                });
                if (releases.empty()) {
                    error = "manifest has no release for " + releasePlatform();
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(jobPointer->mutex);
            jobPointer->error = error;
            jobPointer->releases = std::move(releases);
        }
        jobPointer->success = error.empty();
        jobPointer->running = false;
    });

    releaseJob = std::move(job);
    setStatus("Loading releases...");
}

void quantum::editor::HubWindow::pollRemoteReleases() {
    if (!releaseJob || releaseJob->running) {
        return;
    }

    releaseJob->worker.join();
    const bool success = releaseJob->success;
    std::string error;
    std::vector<RemoteRelease> releases;
    {
        std::lock_guard<std::mutex> lock(releaseJob->mutex);
        error = releaseJob->error;
        releases = std::move(releaseJob->releases);
    }
    releaseJob.reset();

    if (!success) {
        setStatus("Could not load releases: " + error);
        return;
    }
    remoteReleases = std::move(releases);
    setStatus("Loaded " + std::to_string(remoteReleases.size()) + " release(s).");
}

std::filesystem::path quantum::editor::HubWindow::effectiveEditorDir() const {
    if (selectedInstall >= 0 && selectedInstall < static_cast<int>(installs.size()) &&
        installs[selectedInstall].valid) {
        return installs[selectedInstall].path;
    }
    return FileUtils::getExecutableDir();
}

void quantum::editor::HubWindow::launchProject(const std::filesystem::path& projectPath) {
    const std::filesystem::path executable = effectiveEditorDir() / kEditorExecutableName;
    std::error_code ec;
    if (!std::filesystem::exists(executable, ec)) {
        setStatus("Editor executable not found: " + executable.string());
        return;
    }

    const std::vector<std::string> arguments = {"--project", projectPath.string()};
    if (ProcessLaunch::detached(executable, arguments, std::filesystem::current_path())) {
        setStatus("Opening \"" + projectPath.filename().string() + "\"...");
    } else {
        setStatus("Failed to launch the editor for \"" + projectPath.filename().string() + "\".");
    }
}

void quantum::editor::HubWindow::launchCreateProject(const std::filesystem::path& projectPath,
                                            const std::string& templateName) {
    const std::filesystem::path executable = effectiveEditorDir() / kEditorExecutableName;
    std::error_code ec;
    if (!std::filesystem::exists(executable, ec)) {
        setStatus("Editor executable not found: " + executable.string());
        return;
    }

    std::vector<std::string> arguments = {"--project", projectPath.string(), "--create"};
    if (!templateName.empty()) {
        arguments.push_back("--template");
        arguments.push_back(templateName);
    }
    if (ProcessLaunch::detached(executable, arguments, std::filesystem::current_path())) {
        setStatus("Creating \"" + projectPath.filename().string() + "\"...");
    } else {
        setStatus("Failed to launch the editor for \"" + projectPath.filename().string() + "\".");
    }
}

void quantum::editor::HubWindow::openCreateDialog() {
    createDialogOpen = true;
    createPopupOpened = false;
    dialogError.clear();
    if (projectName[0] == '\0') {
        std::snprintf(projectName, sizeof(projectName), "%s", "MyQuantumProject");
    }
    if (projectLocation[0] == '\0') {
        const std::filesystem::path defaultLocation = homeDirectory() / "QuantumProjects";
        std::snprintf(projectLocation, sizeof(projectLocation), "%s", defaultLocation.string().c_str());
    }
}

void quantum::editor::HubWindow::startInstall(const std::filesystem::path& sourceDir, const std::string& label) {
    if (installJob && installJob->running) {
        setStatus("An install is already running.");
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(sourceDir / kEditorExecutableName, ec)) {
        setStatus("\"" + sourceDir.string() + "\" has no " + kEditorExecutableName + ".");
        return;
    }

    // The editor spawns the resource server for every project; warn (but do not
    // block) when the folder was built without it.
    const bool missingResourceServer =
        !std::filesystem::exists(sourceDir / kResourceServerExecutableName, ec);

    std::filesystem::path destination = installsDir() / label;
    std::filesystem::create_directories(installsDir(), ec);
    // Never clobber an existing install: a build copied over itself while an
    // editor from it is running would end up half overwritten.
    for (int suffix = 2; std::filesystem::exists(destination, ec); ++suffix) {
        destination = installsDir() / (label + "-" + std::to_string(suffix));
    }

    auto job = std::make_unique<InstallJob>();
    job->label = destination.filename().string();
    job->destination = destination;
    job->missingResourceServer = missingResourceServer;
    job->running = true;

    InstallJob* jobPointer = job.get();
    job->worker = std::thread([jobPointer, sourceDir, destination]() {
        jobPointer->totalFiles = countBuildFiles(sourceDir);
        std::string error;
        const bool ok = copyBuildFolder(sourceDir, destination, jobPointer->cancelRequested,
                                        jobPointer->copiedFiles, error);
        if (!ok) {
            std::error_code cleanupError;
            std::filesystem::remove_all(extendedLengthPath(destination), cleanupError);
        }
        {
            std::lock_guard<std::mutex> lock(jobPointer->mutex);
            jobPointer->error = error;
        }
        jobPointer->success = ok;
        jobPointer->running = false;
    });

    installJob = std::move(job);
    setStatus("Installing \"" + installJob->label + "\"...");
}

void quantum::editor::HubWindow::startRemoteInstall(const RemoteRelease& release) {
    if (installJob && installJob->running) {
        setStatus("An install is already running.");
        return;
    }
    if (release.url.empty() || release.sha256.empty()) {
        setStatus("This release has no download URL or SHA-256 checksum.");
        return;
    }
    if (!isAllowedRemoteUrl(release.url)) {
        setStatus("Release downloads must use HTTPS.");
        return;
    }

    std::error_code ec;
    const std::string baseLabel = safeInstallLabel(release.label);
    std::filesystem::create_directories(installsDir(), ec);
    if (ec) {
        setStatus("Could not create the install directory: " + ec.message());
        return;
    }

    std::filesystem::path destination = installsDir() / baseLabel;
    for (int suffix = 2; std::filesystem::exists(destination, ec); ++suffix) {
        destination = installsDir() / (baseLabel + "-" + std::to_string(suffix));
    }

    const std::filesystem::path downloadsDir = hubDataDir() / "downloads";
    const std::filesystem::path stagingRoot =
        hubDataDir() / "staging" /
        (baseLabel + "-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::filesystem::path archive = downloadsDir / (baseLabel + ".zip.part");

    auto job = std::make_unique<InstallJob>();
    job->label = destination.filename().string();
    job->destination = destination;
    job->phase = 1;
    job->running = true;
    InstallJob* jobPointer = job.get();

    job->worker = std::thread([jobPointer, release, archive, stagingRoot, destination]() {
        auto fail = [&](const std::string& message) {
            {
                std::lock_guard<std::mutex> lock(jobPointer->mutex);
                jobPointer->error = message;
            }
            std::error_code cleanupError;
            std::filesystem::remove(archive, cleanupError);
            std::filesystem::remove_all(stagingRoot, cleanupError);
            jobPointer->success = false;
            jobPointer->running = false;
        };

        std::error_code ec;
        std::filesystem::create_directories(archive.parent_path(), ec);
        if (ec) {
            fail("Could not create the download directory: " + ec.message());
            return;
        }

        ai::HttpClient client;
        const ai::HttpResponse response = client.downloadToFile(
            release.url, archive, {"Accept: application/zip"}, &jobPointer->cancelRequested);
        if (!response.error.empty()) {
            fail(response.error);
            return;
        }
        if (response.status < 200 || response.status >= 300) {
            fail("download returned HTTP " + std::to_string(response.status));
            return;
        }

        jobPointer->phase = 2;
        std::string hashError;
        const std::string actualHash = quantum::sha256HexOfFile(archive.string(), hashError);
        if (!hashError.empty()) {
            fail("Could not verify download: " + hashError);
            return;
        }
        std::string expectedHash = release.sha256;
        std::transform(expectedHash.begin(), expectedHash.end(), expectedHash.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        if (actualHash != expectedHash) {
            fail("SHA-256 mismatch; the downloaded release was rejected.");
            return;
        }

        jobPointer->phase = 3;
        std::string packageError;
        const std::string baseFolder = ::editor::PackageZip::baseFolderOf(archive.string(), packageError);
        if (!packageError.empty() || baseFolder.empty() || baseFolder == "." || baseFolder == ".." ||
            baseFolder.find('/') != std::string::npos || baseFolder.find('\\') != std::string::npos) {
            fail(packageError.empty() ? "Release archive must contain one safe root folder."
                                      : packageError);
            return;
        }
        if (!::editor::PackageZip::extract(archive.string(), stagingRoot.string(), packageError)) {
            fail(packageError);
            return;
        }

        const std::filesystem::path extractedRoot = stagingRoot / baseFolder;
        if (!std::filesystem::exists(extractedRoot / kEditorExecutableName, ec)) {
            fail("Release archive has no " + std::string(kEditorExecutableName) + ".");
            return;
        }
        jobPointer->missingResourceServer =
            !std::filesystem::exists(extractedRoot / kResourceServerExecutableName, ec);

        std::filesystem::rename(extractedRoot, destination, ec);
        if (ec) {
            fail("Could not finalize the install: " + ec.message());
            return;
        }

        std::filesystem::remove(archive, ec);
        std::filesystem::remove_all(stagingRoot, ec);
        jobPointer->success = true;
        jobPointer->running = false;
    });

    installJob = std::move(job);
    setStatus("Downloading \"" + installJob->label + "\"...");
}

void quantum::editor::HubWindow::pollInstallJob() {
    if (!installJob || installJob->running) {
        return;
    }

    installJob->worker.join();
    const bool success = installJob->success;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(installJob->mutex);
        error = installJob->error;
    }
    const std::string label = installJob->label;
    const bool missingResourceServer = installJob->missingResourceServer;
    installJob.reset();

    if (success) {
        defaultInstallLabel = label;
        installsScanned = false;
        if (missingResourceServer) {
            setStatus("Installed \"" + label + "\" - warning: no " + kResourceServerExecutableName +
                      " in the build folder; projects will not load.");
        } else {
            setStatus("Installed \"" + label + "\".");
        }
    } else if (error == "cancelled") {
        setStatus("Install cancelled.");
    } else {
        setStatus("Install failed: " + error);
    }
}

void quantum::editor::HubWindow::removeInstall(int index) {
    if (index < 0 || index >= static_cast<int>(installs.size())) {
        return;
    }
    const std::string label = installs[index].label;
    std::error_code ec;
    std::filesystem::remove_all(extendedLengthPath(installs[index].path), ec);
    if (ec) {
        setStatus("Could not remove \"" + label + "\": " + ec.message());
        return;
    }

    if (defaultInstallLabel == label) {
        defaultInstallLabel.clear();
        saveConfig();
    }
    installsScanned = false;
    setStatus("Removed \"" + label + "\".");
}

void quantum::editor::HubWindow::drawProjectsTab() {
    const ImVec4 warning(1.0f, 0.45f, 0.35f, 1.0f);

    if (ImGui::Button("New Project...", ImVec2(140, 0))) {
        openCreateDialog();
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Existing...", ImVec2(140, 0))) {
        const std::string picked = FileDialogs::openFileDialog("", 0, true);
        if (!picked.empty()) {
            std::error_code ec;
            if (std::filesystem::exists(std::filesystem::path(picked) / "project.yaml", ec)) {
                AppSettings::addToRecentProjects(picked, true);
                setStatus("Added \"" + std::filesystem::path(picked).filename().string() + "\".");
            } else {
                setStatus("\"" + picked + "\" is not a Quantum project (no project.yaml).");
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(90, 0))) {
        installsScanned = false;
        templatesScanned = false;
    }

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::BeginChild("##hub_projects", ImVec2(0, 0), ImGuiChildFlags_Borders);
    {
        const std::vector<std::filesystem::path> projects = AppSettings::getRecentProjects();
        if (projects.empty()) {
            ImGui::TextWrapped("No projects yet. Use \"New Project...\" to create one, or \"Add Existing...\" to register a project folder.");
        }

        std::error_code ec;
        for (int i = 0; i < static_cast<int>(projects.size()); ++i) {
            const std::filesystem::path& projectPath = projects[i];
            const bool exists = std::filesystem::exists(projectPath / "project.yaml", ec);

            ImGui::PushID(i);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(projectPath.filename().string().c_str());
            if (!exists) {
                ImGui::SameLine();
                ImGui::TextColored(warning, "(missing)");
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", elideMiddle(projectPath.string(), 90).c_str());

            const float buttonWidth = 90.0f;
            const float buttonsWidth = buttonWidth * 2.0f + ImGui::GetStyle().ItemSpacing.x;
            ImGui::SameLine(ImGui::GetContentRegionMax().x - buttonsWidth);
            ImGui::BeginDisabled(!exists);
            if (ImGui::Button("Open", ImVec2(buttonWidth, 0))) {
                launchProject(projectPath);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Folder", ImVec2(buttonWidth, 0))) {
                ProcessLaunch::revealFolder(projectPath);
            }
            ImGui::Separator();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

void quantum::editor::HubWindow::drawInstallsTab() {
    const ImVec4 warning(1.0f, 0.45f, 0.35f, 1.0f);

    const bool installing = installJob && installJob->running;

    if (!remoteReleasesRequested && !releaseJob && remoteReleases.empty()) {
        remoteReleasesRequested = true;
        fetchRemoteReleases();
    }

    ImGui::BeginDisabled(installing);
    if (ImGui::Button("Install This Build", ImVec2(160, 0))) {
        const std::filesystem::path runningDir = FileUtils::getExecutableDir();
        startInstall(runningDir, runningDir.filename().string());
    }
    ImGui::SameLine();
    if (ImGui::Button("Install from Folder...", ImVec2(180, 0))) {
        const std::string picked = FileDialogs::openFileDialog("", 0, true);
        if (!picked.empty()) {
            const std::filesystem::path sourceDir = picked;
            startInstall(sourceDir, sourceDir.filename().string());
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(90, 0))) {
        installsScanned = false;
    }

    ImGui::Dummy(ImVec2(0, 6));
    ImGui::TextDisabled("%s", "Projects open with the selected install. Installing copies the build folder (executable, engine files and templates).");
    ImGui::Dummy(ImVec2(0, 6));

    ImGui::Separator();
    ImGui::TextUnformatted("Remote releases");
    ImGui::TextDisabled("Download a verified editor package published on GitHub.");
    ImGui::SetNextItemWidth(-130);
    if (ImGui::InputText("Manifest URL", releaseManifestUrl, IM_ARRAYSIZE(releaseManifestUrl))) {
        saveConfig();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(installing || (releaseJob && releaseJob->running));
    if (ImGui::Button("Load releases", ImVec2(120, 0))) {
        saveConfig();
        fetchRemoteReleases();
    }
    ImGui::EndDisabled();

    if (releaseJob && releaseJob->running) {
        ImGui::TextDisabled("Contacting release server...");
    } else if (!remoteReleases.empty()) {
        for (int i = 0; i < static_cast<int>(remoteReleases.size()); ++i) {
            const RemoteRelease& release = remoteReleases[i];
            ImGui::PushID(2000 + i);
            ImGui::Separator();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(release.label.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", release.version.c_str());
            if (release.size > 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("%.1f MB", static_cast<double>(release.size) / (1024.0 * 1024.0));
            }
            ImGui::SameLine(ImGui::GetContentRegionMax().x - 150.0f);
            ImGui::BeginDisabled(installing);
            if (ImGui::Button("Download / Install", ImVec2(145, 0))) {
                startRemoteInstall(release);
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
    }

    ImGui::BeginChild("##hub_installs", ImVec2(0, 0), ImGuiChildFlags_Borders);
    {
        const float buttonWidth = 90.0f;
        const float buttonsWidth = buttonWidth * 2.0f + ImGui::GetStyle().ItemSpacing.x;

        const std::filesystem::path runningDir = FileUtils::getExecutableDir();
        ImGui::AlignTextToFramePadding();
        if (ImGui::RadioButton("##hub_running", selectedInstall == -1)) {
            selectedInstall = -1;
            defaultInstallLabel.clear();
            saveConfig();
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("This build (running)");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", elideMiddle(runningDir.string(), 90).c_str());
        ImGui::SameLine(ImGui::GetContentRegionMax().x - buttonsWidth);
        if (ImGui::Button("Folder", ImVec2(buttonWidth, 0))) {
            ProcessLaunch::revealFolder(runningDir);
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(true);
        ImGui::Button("Remove", ImVec2(buttonWidth, 0));
        ImGui::EndDisabled();

        for (int i = 0; i < static_cast<int>(installs.size()); ++i) {
            const EditorInstall& install = installs[i];
            ImGui::Separator();
            ImGui::PushID(1000 + i);
            ImGui::AlignTextToFramePadding();
            if (ImGui::RadioButton("##hub_default", selectedInstall == i)) {
                selectedInstall = i;
                defaultInstallLabel = install.label;
                saveConfig();
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(install.label.c_str());
            if (!install.valid) {
                ImGui::SameLine();
                ImGui::TextColored(warning, "(no editor executable)");
            }
            ImGui::SameLine();
            ImGui::TextDisabled("%s", elideMiddle(install.path.string(), 90).c_str());

            ImGui::SameLine(ImGui::GetContentRegionMax().x - buttonsWidth);
            if (ImGui::Button("Folder", ImVec2(buttonWidth, 0))) {
                ProcessLaunch::revealFolder(install.path);
            }
            ImGui::SameLine();
            if (ImGui::Button("Remove", ImVec2(buttonWidth, 0))) {
                pendingRemove = i;
                removePopupOpened = false;
            }
            ImGui::PopID();
        }

        if (installs.empty()) {
            ImGui::Separator();
            ImGui::TextWrapped("No builds installed yet. Use \"Install This Build\" or \"Install from Folder...\" to add one.");
        }
    }
    ImGui::EndChild();
}

void quantum::editor::HubWindow::drawCreateDialog() {
    if (createDialogOpen && !createPopupOpened) {
        ImGui::OpenPopup(kCreatePopupId);
        createPopupOpened = true;
    }

    // Centered on the app viewport, above the hub window.
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
               viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kCreatePopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Create a new Quantum project");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 4));

        ImGui::TextUnformatted("Name");
        ImGui::SetNextItemWidth(480);
        ImGui::InputText("##hub_project_name", projectName, IM_ARRAYSIZE(projectName));

        ImGui::TextUnformatted("Location");
        ImGui::SetNextItemWidth(367);
        ImGui::InputText("##hub_project_location", projectLocation, IM_ARRAYSIZE(projectLocation));
        ImGui::SameLine();
        if (ImGui::Button("Browse...", ImVec2(105, 0))) {
            const std::string picked = FileDialogs::openFileDialog(projectLocation, 0, true);
            if (!picked.empty()) {
                std::snprintf(projectLocation, sizeof(projectLocation), "%s", picked.c_str());
            }
        }

        ImGui::TextUnformatted("Template");
        const char* templatePreview = "Blank Project";
        if (selectedTemplate > 0 && selectedTemplate <= static_cast<int>(templates.size())) {
            templatePreview = templates[selectedTemplate - 1].c_str();
        }
        ImGui::SetNextItemWidth(480);
        if (ImGui::BeginCombo("##hub_project_template", templatePreview)) {
            if (ImGui::Selectable("Blank Project", selectedTemplate == 0)) {
                selectedTemplate = 0;
            }
            for (int i = 0; i < static_cast<int>(templates.size()); ++i) {
                if (ImGui::Selectable(templates[i].c_str(), selectedTemplate == i + 1)) {
                    selectedTemplate = i + 1;
                }
            }
            ImGui::EndCombo();
        }

        if (!dialogError.empty()) {
            ImGui::Dummy(ImVec2(0, 4));
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", dialogError.c_str());
        }
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Separator();

        if (ImGui::Button("Create", ImVec2(120, 0))) {
            const std::string name = trimmed(projectName);
            const std::string location = trimmed(projectLocation);
            if (name.empty()) {
                dialogError = "Type a project name.";
            } else if (location.empty()) {
                dialogError = "Choose the folder the project is created in.";
            } else {
                std::error_code ec;
                const std::filesystem::path projectPath = std::filesystem::path(location) / name;
                if (std::filesystem::exists(projectPath, ec)) {
                    dialogError = "\"" + projectPath.string() + "\" already exists.";
                } else {
                    const std::string templateName =
                        selectedTemplate > 0 && selectedTemplate <= static_cast<int>(templates.size())
                            ? templates[selectedTemplate - 1]
                            : std::string();
                    launchCreateProject(projectPath, templateName);
                    createDialogOpen = false;
                    createPopupOpened = false;
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            createDialogOpen = false;
            createPopupOpened = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else if (createPopupOpened) {
        // Closed through Escape or the window close button.
        createDialogOpen = false;
        createPopupOpened = false;
    }
}

void quantum::editor::HubWindow::drawRemoveDialog() {
    if (pendingRemove < 0 || pendingRemove >= static_cast<int>(installs.size())) {
        return;
    }

    if (!removePopupOpened) {
        ImGui::OpenPopup(kRemovePopupId);
        removePopupOpened = true;
    }

    ImGui::SetNextWindowSize(ImVec2(460, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal(kRemovePopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Remove \"%s\"?", installs[pendingRemove].label.c_str());
        ImGui::TextDisabled("%s", installs[pendingRemove].path.string().c_str());
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Separator();
        if (ImGui::Button("Remove", ImVec2(120, 0))) {
            removeInstall(pendingRemove);
            pendingRemove = -1;
            removePopupOpened = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            pendingRemove = -1;
            removePopupOpened = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else if (removePopupOpened) {
        pendingRemove = -1;
        removePopupOpened = false;
    }
}

void quantum::editor::HubWindow::show() {
    pollRemoteReleases();
    pollInstallJob();

    if (!installsScanned) {
        scanInstalls();
        // Persist the resolved selection: after an install or removal the
        // stored label only lines up with selectedInstall once the rescan ran.
        saveConfig();
    }
    if (!templatesScanned) {
        scanTemplates();
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    const ImGuiWindowFlags windowFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##QuantumHub", nullptr, windowFlags);
    ImGui::PopStyleVar(3);

    // Sidebar.
    ImGui::BeginChild("##hub_sidebar", ImVec2(230, 0), ImGuiChildFlags_Borders);
    {
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Indent(10);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.75f, 1.0f, 1.0f));
        ImGui::TextUnformatted("QUANTUM HUB");
        ImGui::PopStyleColor();
        ImGui::Unindent(10);
        ImGui::Dummy(ImVec2(0, 12));

        if (ImGui::Selectable("Projects", tab == Tab::Projects, 0, ImVec2(0, 34))) {
            tab = Tab::Projects;
        }
        if (ImGui::Selectable("Installs", tab == Tab::Installs, 0, ImVec2(0, 34))) {
            tab = Tab::Installs;
        }

        const float footerPosition = ImGui::GetWindowHeight() - ImGui::GetFrameHeightWithSpacing() - 4;
        if (footerPosition > 0) {
            ImGui::SetCursorPosY(footerPosition);
        }
        ImGui::TextDisabled("Version %s", QUANTUM_EDITOR_VERSION);
    }
    ImGui::EndChild();

    // Content.
    ImGui::SameLine();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 14));
    ImGui::BeginChild("##hub_content", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();

    if (!statusMessage.empty() && ImGui::GetTime() - statusMessageTime < 8.0) {
        ImGui::TextColored(ImVec4(0.45f, 0.75f, 1.0f, 1.0f), "%s", statusMessage.c_str());
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 4));
    }

    if (installJob && installJob->running) {
        const int total = installJob->totalFiles.load();
        const int copied = installJob->copiedFiles.load();
        const int phase = installJob->phase.load();
        if (phase == 1 && total > 0) {
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "%d / %d files", copied, total);
            ImGui::ProgressBar(static_cast<float>(copied) / static_cast<float>(total), ImVec2(320, 0), overlay);
        } else if (phase == 1) {
            ImGui::TextUnformatted("Downloading...");
        } else if (phase == 2) {
            ImGui::TextUnformatted("Verifying SHA-256...");
        } else if (phase == 3) {
            ImGui::TextUnformatted("Extracting release...");
        } else {
            ImGui::TextUnformatted("Scanning build folder...");
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(("Installing \"" + installJob->label + "\"...").c_str());
        ImGui::SameLine();
        if (ImGui::Button("Cancel install")) {
            installJob->cancelRequested = true;
        }
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 4));
    }

    if (tab == Tab::Projects) {
        drawProjectsTab();
    } else {
        drawInstallsTab();
    }

    ImGui::EndChild();
    ImGui::End();

    drawCreateDialog();
    drawRemoveDialog();
}
