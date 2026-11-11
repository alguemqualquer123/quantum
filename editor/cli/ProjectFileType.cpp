// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "cli/ProjectFileType.h"

#include "AppSettings.h"
#include "util/FileUtils.h"
#include "util/ProjectFile.h"
#include "util/ShellAssociation.h"

#include <system_error>

namespace quantum::editor::cli::ProjectFileType {

std::string resolveExtension(const std::string& extension) {
    // An empty argument means "whatever the user configured", and anything else is
    // normalized the same way the marker file name is.
    return ProjectFile::normalizeExtension(
        extension.empty() ? AppSettings::getProjectFileExtension() : extension);
}

std::filesystem::path editorExecutablePath() {
    namespace fs = std::filesystem;

    const fs::path self = FileUtils::getExecutablePath();
    // The editor and the suite executables are built and installed side by side, so
    // the GUI next to the caller is the one a marker should launch.
    const fs::path editor = self.parent_path() / "quantum-editor.exe";
    std::error_code ec;
    return fs::exists(editor, ec) ? editor : self;
}

std::string describe(const std::string& extension) {
    const std::string wanted = resolveExtension(extension);
    if (!ShellAssociation::isSupported()) {
        return wanted + " file type registration is not implemented on this platform";
    }
    if (ShellAssociation::isRegistered(wanted)) {
        return "double-clicking a " + wanted + " file opens it with this editor";
    }
    const std::string owner = ShellAssociation::currentHandler(wanted);
    if (owner.empty()) {
        return wanted + " is not registered to any program";
    }
    return wanted + " belongs to \"" + owner + "\"";
}

bool isRegistered(const std::string& extension) {
    return ShellAssociation::isRegistered(resolveExtension(extension));
}

bool registerFileType(const std::string& extension, std::string& error) {
    ShellAssociation::Target target;
    target.executable = editorExecutablePath();
    target.extension = resolveExtension(extension);
    return ShellAssociation::registerTarget(target, error);
}

bool unregisterFileType(const std::string& extension, std::string& error) {
    return ShellAssociation::unregisterTarget(resolveExtension(extension), error);
}

}
