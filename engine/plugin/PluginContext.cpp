// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "PluginContext.h"

#include "Engine.h"
#include "Log.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <filesystem>
#include <fstream>

using namespace quantum;
using namespace quantum::plugin;

namespace {

    const char* levelTag(PluginLogLevel level) {
        switch (level) {
            case PluginLogLevel::Verbose: return "verbose";
            case PluginLogLevel::Debug: return "debug";
            case PluginLogLevel::Info: return "info";
            case PluginLogLevel::Warning: return "warn";
            default: return "error";
        }
    }

    std::string formatMessage(const char* format, va_list arguments) {
        if (!format) return std::string();

        char stack[1024];
        va_list copy;
        va_copy(copy, arguments);
        const int needed = std::vsnprintf(stack, sizeof(stack), format, copy);
        va_end(copy);

        // A plugin log line that does not fit is a plugin bug, but dropping the tail is friendlier
        // than dropping the line.
        if (needed < 0) return std::string(format);
        if (needed < static_cast<int>(sizeof(stack))) return std::string(stack, needed);

        std::vector<char> large(static_cast<size_t>(needed) + 1);
        va_copy(copy, arguments);
        std::vsnprintf(large.data(), large.size(), format, copy);
        va_end(copy);
        return std::string(large.data());
    }

    std::string joinPath(const std::string& directory, const std::string& leaf) {
        if (directory.empty()) return leaf;
        if (directory.back() == '/' || directory.back() == '\\') return directory + leaf;
        return directory + "/" + leaf;
    }

    // The whole traversal policy, and the reason a plugin can be handed a path the user typed.
    // Absolute paths, drive letters, UNC prefixes and any ".." component are refused before a
    // single filesystem call is made, so the check does not depend on the root being canonical.
    // An empty path is legal and means the data directory itself - listFiles("") is the common call.
    bool isRelativeInsideData(const std::string& relative) {
        if (relative.empty()) return true;
        if (relative[0] == '/' || relative[0] == '\\') return false;
        if (relative.size() > 1 && relative[1] == ':') return false;

        size_t start = 0;
        while (start <= relative.size()) {
            const size_t separator = relative.find_first_of("/\\", start);
            const size_t end = separator == std::string::npos ? relative.size() : separator;
            if (relative.compare(start, end - start, "..") == 0) return false;
            if (end == relative.size()) break;
            start = end + 1;
        }
        return true;
    }

    constexpr std::uintmax_t MAX_PLUGIN_FILE_BYTES = 64ull * 1024 * 1024;

}

PluginContext::PluginContext(const PluginManifest& ownerManifest, PluginSettings& ownerSettings,
                             PluginRegistry& extensionRegistry, std::vector<PluginLogEntry>& sink,
                             std::string root):
    manifestRef(&ownerManifest), settingsRef(&ownerSettings), extensions(&extensionRegistry),
    logSink(&sink), dataRoot(std::move(root)) {
}

std::string PluginContext::resourcePath(const std::string& relative) const {
    if (relative.empty()) return manifestRef->directory;
    if (!manifestRef->directory.empty() && manifestRef->directory.back() == '/') {
        return manifestRef->directory + relative;
    }
    return manifestRef->directory + "/" + relative;
}

bool PluginContext::hasPermission(const std::string& permission) const {
    return manifestRef->hasPermission(permission);
}

bool PluginContext::require(const std::string& permission) const {
    if (manifestRef->hasPermission(permission)) return true;

    // Once per permission: the gated calls sit in a tick, and a plugin that forgot to declare
    // "tick" would otherwise write the same line 60 times a second.
    for (const std::string& reported : denialsReported) {
        if (reported == permission) return false;
    }
    if (denialsReported.size() < 32) denialsReported.push_back(permission);

    writeLog(PluginLogLevel::Warning, "capability \"" + permission +
             "\" denied: add it to the \"permissions\" array of plugin.json");
    return false;
}

Scene* PluginContext::scene() const {
    if (!require("scene")) return nullptr;

    // getMainScene(), not getScene(): the latter indexes scenes[0] without checking the size, and a
    // plugin is allowed to ask during initialize(), before the engine has any scene at all. The main
    // scene is a plain static that reads as null in exactly that case, and it is the one a world
    // generator wants rather than the top of a transition stack.
    return Engine::getMainScene();
}

void PluginContext::writeLog(PluginLogLevel level, const std::string& message) const {
    if (logSink) {
        // The same bound the manager puts on its own lines: this vector lives for the whole session
        // and a plugin logging from a tick would grow it without limit.
        constexpr size_t MAX_CONTEXT_LOG_LINES = 1024;
        while (logSink->size() >= MAX_CONTEXT_LOG_LINES) logSink->erase(logSink->begin());

        PluginLogEntry entry;
        entry.level = level;
        entry.pluginId = manifestRef->id;
        entry.message = message;
        logSink->push_back(entry);
    }

    // Mirrored into the engine log so a headless run shows plugin output without the editor.
    const std::string line = "[plugin " + manifestRef->id + "] [" + levelTag(level) + "] " + message;
    if (level == PluginLogLevel::Error) Log::error("%s", line.c_str());
    else if (level == PluginLogLevel::Warning) Log::warn("%s", line.c_str());
    else if (level == PluginLogLevel::Verbose) Log::verbose("%s", line.c_str());
    else Log::print("%s", line.c_str());
}

void PluginContext::log(PluginLogLevel level, const char* format, ...) const {
    if (!require("log")) return;

    va_list arguments;
    va_start(arguments, format);
    const std::string message = formatMessage(format, arguments);
    va_end(arguments);

    writeLog(level, message);
}

bool PluginContext::registerCommand(const std::string& name, const std::string& description) const {
    const bool registered = extensions->registerCommand(manifestRef->id, name, description);
    if (!registered) {
        // writeLog, not log(): this is the engine reporting on the plugin, and a plugin that never
        // asked for the "log" permission still has to be able to see why its command was refused.
        writeLog(PluginLogLevel::Error, "command registration refused: " +
                 extensions->lastError());
    }
    return registered;
}

bool PluginContext::registerPanel(const std::string& title) const {
    return extensions->registerPanel(manifestRef->id,
                                     title.empty() ? manifestRef->name : title);
}

bool PluginContext::registerTool(const std::string& name, const std::string& label,
                                 const std::string& action) const {
    const bool registered = extensions->registerTool(manifestRef->id, name, label, action);
    if (!registered) {
        writeLog(PluginLogLevel::Error, "tool registration refused: " +
                 extensions->lastError());
    }
    return registered;
}

bool PluginContext::registerWindow(const std::string& windowId, const std::string& title,
                                   const std::string& category, const std::string& icon,
                                   int width, int height) const {
    if (!require("ui")) return false;

    PluginWindowInfo window;
    window.pluginId = manifestRef->id;
    window.id = windowId;
    window.title = title.empty() ? manifestRef->name : title;
    window.category = category;
    window.icon = icon;
    // Zero is the plugin saying "you decide", and the host's default is what an editor window
    // without a preference gets; a negative size is a plugin bug, not a request.
    window.width = width > 0 ? width : 0;
    window.height = height > 0 ? height : 0;

    if (!extensions->registerWindow(window)) {
        writeLog(PluginLogLevel::Error, "window registration refused: " + extensions->lastError());
        return false;
    }
    return true;
}

bool PluginContext::registerMenuItem(const std::string& label, const std::string& command,
                                     const std::string& category) const {
    if (!require("ui")) return false;

    // The item is only as privileged as the command behind it, so it has to name a command this
    // plugin registered. Registering the item before the command is a plugin-order mistake, and the
    // message says so because the alternative is a menu entry that silently does nothing.
    if (extensions->ownerOfCommand(command) != manifestRef->id) {
        writeLog(PluginLogLevel::Error, "menu item \"" + label +
                 "\" refused: this plugin does not register the command \"" + command + "\"");
        return false;
    }

    PluginMenuItemInfo item;
    item.pluginId = manifestRef->id;
    item.category = category;
    item.label = label;
    item.command = command;

    if (!extensions->registerMenuItem(item)) {
        writeLog(PluginLogLevel::Error, "menu item refused: " + extensions->lastError());
        return false;
    }
    return true;
}

bool PluginContext::openWindow(const std::string& windowId) const {
    if (!require("ui")) return false;

    if (!extensions->requestWindowOpen(manifestRef->id, windowId)) {
        writeLog(PluginLogLevel::Error, "window open refused: " + extensions->lastError());
        return false;
    }
    return true;
}

bool PluginContext::resolveDataPath(const std::string& relative, std::string& absoluteOut) const {
    absoluteOut.clear();

    if (!require("files")) return false;

    if (dataRoot.empty()) {
        writeLog(PluginLogLevel::Error,
                  "no plugin data directory in this engine build");
        return false;
    }
    if (!isRelativeInsideData(relative)) {
        writeLog(PluginLogLevel::Warning, "path \"" + relative +
                 "\" refused: only paths inside the plugin data directory are writable, and \"..\", "
                 "absolute paths and drive letters are not allowed");
        return false;
    }

    // The empty path is the directory itself, and without the trailing separator a caller can strip
    // this prefix off a walked path to get the name it should hand back to the plugin.
    const std::string joined = relative.empty() ? dataRoot : joinPath(dataRoot, relative);
    // Checked again after the join: the policy above is what refuses a bad path, this is what makes
    // sure a root handed in by the manager that already ends in a separator cannot be stepped out of.
    const std::string rootPrefix = dataRoot.back() == '/' || dataRoot.back() == '\\' ? dataRoot
                                                                                     : dataRoot + "/";
    if (!relative.empty() && joined.compare(0, rootPrefix.size(), rootPrefix) != 0) {
        writeLog(PluginLogLevel::Warning, "path \"" + relative + "\" escapes the plugin data directory");
        return false;
    }

    absoluteOut = joined;
    return true;
}

bool PluginContext::readFile(const std::string& relative, std::string& contentOut) const {
    contentOut.clear();

    std::string path;
    if (!resolveDataPath(relative, path)) return false;

    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(path), ec) && !ec) {
        // Silent. A plugin asking for a file it has not written yet is the ordinary first run, and a
        // window that reads on each describe would otherwise turn that into a log line per frame.
        // What did exist and cannot be read still reports below.
        return false;
    }
    const std::uintmax_t size = std::filesystem::file_size(std::filesystem::path(path), ec);
    if (ec) {
        writeLog(PluginLogLevel::Warning, "cannot read \"" + relative + "\": " + ec.message());
        return false;
    }
    if (size > MAX_PLUGIN_FILE_BYTES) {
        writeLog(PluginLogLevel::Error, "\"" + relative + "\" is " + std::to_string(size) +
                 " byte(s); the plugin file API reads at most " +
                 std::to_string(static_cast<unsigned long long>(MAX_PLUGIN_FILE_BYTES)));
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        writeLog(PluginLogLevel::Warning, "cannot open \"" + relative + "\" for reading");
        return false;
    }
    contentOut.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return true;
}

bool PluginContext::writeFile(const std::string& relative, const std::string& content,
                              bool append) const {
    std::string path;
    if (!resolveDataPath(relative, path)) return false;

    std::error_code ec;
    const std::filesystem::path target(path);
    const std::filesystem::path parent = target.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    std::ios::openmode mode = std::ios::binary | std::ios::out;
    if (append) mode |= std::ios::app;

    std::ofstream output(target, mode);
    if (!output.is_open()) {
        writeLog(PluginLogLevel::Warning, "cannot open \"" + relative + "\" for writing: " +
                 (ec ? ec.message() : std::string("the directory could not be created")));
        return false;
    }
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.close();

    // A stream that failed mid-write reports nothing else, so the failbit is the only signal there
    // is: a full disk has to read as a failed write and not as a short file.
    if (!output) {
        writeLog(PluginLogLevel::Error, "writing \"" + relative + "\" failed after " +
                 std::to_string(content.size()) + " byte(s) were requested");
        return false;
    }
    return true;
}

bool PluginContext::writeTextFile(const std::string& relative, const std::string& content) const {
    return writeFile(relative, content, false);
}

bool PluginContext::appendTextFile(const std::string& relative, const std::string& content) const {
    return writeFile(relative, content, true);
}

bool PluginContext::removeFile(const std::string& relative) const {
    std::string path;
    if (!resolveDataPath(relative, path)) return false;

    std::error_code ec;
    if (std::filesystem::remove(std::filesystem::path(path), ec) == 0 || ec) {
        writeLog(PluginLogLevel::Warning, "cannot remove \"" + relative + "\": " +
                 (ec ? ec.message() : std::string("it does not exist")));
        return false;
    }
    return true;
}

bool PluginContext::fileExists(const std::string& relative) const {
    std::string path;
    if (!resolveDataPath(relative, path)) return false;

    std::error_code ec;
    const bool found = std::filesystem::exists(std::filesystem::path(path), ec);
    return found && !ec;
}

std::vector<std::string> PluginContext::listFiles(const std::string& relativeDirectory,
                                                  bool includeDirectories) const {
    std::vector<std::string> names;

    // A listing of a folder the plugin never wrote is empty, which is also the answer for a first
    // run: the data directory is created by the first write, not by looking for it.
    std::string path;
    if (!resolveDataPath(relativeDirectory, path)) return names;

    constexpr size_t MAX_LISTED = 4096;

    const std::filesystem::path rootPath(path);
    std::error_code ec;
    std::filesystem::recursive_directory_iterator walk(rootPath, ec);
    if (ec) return names;

    // A plugin may nest a folder called "." or ".." inside its own data directory, and those names
    // are still inside it. Only a path that *begins* with the step out is dropped - relative_to does
    // not resolve links, so that is the one hole this function has to plug itself.
    const auto startsWithStepOut = [](const std::string& relative) {
        return relative.compare(0, 2, "../") == 0 || relative == "..";
    };

    for (const std::filesystem::directory_entry& entry : walk) {
        if (names.size() >= MAX_LISTED) break;

        // Relative and generic, so what a plugin gets back is the same spelling it would have
        // written: stripping a prefix off a native path would hand a Windows plugin a name with the
        // separator it did not ask for. `lexically_relative` because that is the name MSVC has, it
        // never throws (there is nothing to throw with _HAS_EXCEPTIONS=0), and the entries all come
        // from walking rootPath anyway.
        const std::string relative = entry.path().lexically_relative(rootPath).generic_string();
        if (relative.empty() || startsWithStepOut(relative)) continue;

        if (entry.is_directory(ec)) {
            if (includeDirectories && !ec) names.push_back(relative + "/");
            continue;
        }
        if (!ec && entry.is_symlink()) continue;
        names.push_back(relative);
    }

    return names;
}
