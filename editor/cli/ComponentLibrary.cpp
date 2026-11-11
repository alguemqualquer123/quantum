// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "ComponentLibrary.h"

#include "util/FileUtils.h"

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>

namespace quantum::editor::cli {

    namespace {

        std::string trimmed(const std::string& value) {
            size_t begin = value.find_first_not_of(" \t\r\n");
            if (begin == std::string::npos) {
                return std::string();
            }
            const size_t end = value.find_last_not_of(" \t\r\n");
            return value.substr(begin, end - begin + 1);
        }

        std::string lowered(std::string value) {
            std::transform(value.begin(), value.end(), value.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return value;
        }

        std::string readWholeFile(const fs::path& path, bool& ok) {
            std::ifstream file(path, std::ios::in | std::ios::binary);
            if (!file) {
                ok = false;
                return std::string();
            }
            ok = true;
            return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        }

        // Every library file starts with the licence header and then a comment line
        // that says what the component is for, which is the only description the
        // listing has. Returns empty when the file has no such line.
        std::string summaryOf(const std::string& text) {
            std::istringstream stream(text);
            std::string line;
            while (std::getline(stream, line)) {
                line = trimmed(line);
                if (line.empty()) {
                    continue;
                }
                if (line.rfind("--", 0) != 0) {
                    return std::string();
                }

                const std::string body = trimmed(line.substr(2));
                if (body.empty() || body.rfind("(c)", 0) == 0 || body.rfind("SPDX", 0) == 0) {
                    continue;
                }
                return body;
            }
            return std::string();
        }

        // The header also names the group the component belongs to, which is how the editor's
        // "Add new component" dialog sorts them into categories instead of one long list.
        std::string categoryOf(const std::string& text) {
            std::istringstream stream(text);
            std::string line;
            while (std::getline(stream, line)) {
                line = trimmed(line);
                if (!line.empty() && line[0] != '-') {
                    return std::string();
                }
                const size_t marker = line.find("-- category:");
                if (marker != std::string::npos) {
                    return trimmed(line.substr(marker + 12));
                }
            }
            return std::string();
        }

        std::string commandBaseName(const char* executableName) {
            if (!executableName || !*executableName) {
                return "quantum-editor";
            }
            const std::string filename = fs::path(executableName).filename().string();
            return filename.empty() ? std::string("quantum-editor") : filename;
        }

        void printUsage(const std::string& commandName) {
            std::cout
                << "Usage:\n"
                << "  " << commandName << " components list [--project <dir>]\n"
                << "                          Show the components the engine ships with\n"
                << "  " << commandName << " components install --project <dir> [options]\n"
                << "                          Copy components into the project's Lua folder\n\n"
                << "Options:\n"
                << "  --lua <dir>        Lua root to install into (default: luaDir from project.yaml)\n"
                << "  --into <subdir>    Sub folder inside the Lua root (default: Components)\n"
                << "  --only <a,b>       Install only these components\n"
                << "  --overwrite        Replace files the project already has\n"
                << "  --no-demos         Skip the example scenes and the kit's sprites and thunder\n"
                << "  --engine-dir <dir> Look for the library somewhere other than next to the executable\n\n"
                << "Installed components are ordinary project scripts. The editor also lists every\n"
                << "component the engine ships in Inspector -> New component, under the Ready-made\n"
                << "scripts groups, and installs the file when you pick one.\n";
        }

        struct ComponentsOptions {
            bool list = false;
            bool install = false;
            bool overwrite = false;
            bool noDemos = false;
            bool help = false;
            std::string projectPath;
            std::string luaDir;
            std::string subdir = "Components";
            std::string engineDir;
            std::vector<std::string> names;
        };

        bool requireValue(int argc, char** argv, int& index, std::string& value, std::string& error) {
            if (index + 1 >= argc) {
                error = std::string(argv[index]) + " needs a value";
                return false;
            }
            value = argv[++index];
            return true;
        }

        bool parseComponentsArgs(int argc, char** argv, ComponentsOptions& options, std::string& error) {
            // argv[0] is the `components` token itself, exactly like the export and
            // project-file subcommands, so the verb is the argument behind it.
            const std::string verb = argc >= 2 && argv[1] ? argv[1] : "";
            if (verb == "list") {
                options.list = true;
            } else if (verb == "install") {
                options.install = true;
            } else if (verb.empty() || verb == "--help" || verb == "-h" || verb == "help") {
                options.help = true;
            } else {
                error = "expected `list` or `install`, got `" + verb + "`";
                return false;
            }

            for (int i = 2; i < argc; ++i) {
                const std::string arg = argv[i];

                if (arg == "--help" || arg == "-h") {
                    options.help = true;
                } else if (arg == "--overwrite") {
                    options.overwrite = true;
                } else if (arg == "--no-demos") {
                    options.noDemos = true;
                } else if (arg == "--project") {
                    if (!requireValue(argc, argv, i, options.projectPath, error)) return false;
                } else if (arg == "--lua") {
                    if (!requireValue(argc, argv, i, options.luaDir, error)) return false;
                } else if (arg == "--into") {
                    if (!requireValue(argc, argv, i, options.subdir, error)) return false;
                } else if (arg == "--engine-dir") {
                    if (!requireValue(argc, argv, i, options.engineDir, error)) return false;
                } else if (arg == "--only") {
                    std::string list;
                    if (!requireValue(argc, argv, i, list, error)) return false;
                    std::istringstream stream(list);
                    std::string item;
                    while (std::getline(stream, item, ',')) {
                        item = trimmed(item);
                        if (!item.empty()) {
                            options.names.push_back(item);
                        }
                    }
                } else if (!arg.empty() && arg[0] == '-') {
                    error = "unknown option: " + arg;
                    return false;
                } else {
                    error = "unexpected argument: " + arg;
                    return false;
                }
            }

            if (options.help) {
                return true;
            }
            if (!options.list && !options.install) {
                error = "expected `list` or `install`";
                return false;
            }
            if (options.install && options.projectPath.empty()) {
                error = "install needs --project <dir>";
                return false;
            }
            return true;
        }

        // The editor resolves a script against the project's Lua root, which project.yaml
        // stores relative to the project (default "."). Reading the same key here keeps the
        // command and the Inspector pointing at one folder.
        fs::path resolveLuaRoot(const ComponentsOptions& options, std::string& error) {
            const fs::path project(options.projectPath);
            std::error_code ec;
            if (!fs::is_directory(project, ec)) {
                error = "not a directory: " + project.string();
                return {};
            }

            std::string luaDir = options.luaDir;
            if (luaDir.empty()) {
                const fs::path projectFile = project / "project.yaml";
                if (fs::exists(projectFile, ec)) {
                    try {
                        const YAML::Node node = YAML::LoadFile(projectFile.string());
                        if (node["luaDir"]) {
                            luaDir = trimmed(node["luaDir"].as<std::string>());
                        }
                    } catch (const YAML::Exception& e) {
                        error = "cannot read project.yaml: " + e.msg;
                        return {};
                    }
                }
            }
            if (luaDir.empty()) {
                luaDir = ".";
            }

            const fs::path requested(luaDir);
            const fs::path root = requested.is_absolute() ? requested : project / requested;
            // luaDir is often "." and the path is printed back to the user, so drop
            // the noise instead of showing `<project>\.\Components`.
            return root.lexically_normal();
        }

    }

    fs::path findComponentsDir() {
        std::error_code ec;

        const fs::path shipped = FileUtils::getEngineDir() / "components";
        if (fs::is_directory(shipped, ec)) {
            return shipped;
        }

        // The copy next to the executable only appears once the build has run
        // copy_quantum_editor_engine; a checkout that has not built it yet still has
        // the library in the source tree.
        fs::path dir = FileUtils::getExecutableDir();
        for (int depth = 0; depth <= 5 && !dir.empty(); ++depth) {
            const fs::path candidate = dir / "engine" / "components";
            if (fs::is_directory(candidate, ec)) {
                return candidate;
            }
            dir = dir.parent_path();
        }

        return {};
    }

    std::vector<ReadyMadeComponent> listReadyMadeComponents(const fs::path& componentsDir) {
        std::vector<ReadyMadeComponent> components;

        std::error_code ec;
        if (componentsDir.empty() || !fs::is_directory(componentsDir, ec)) {
            return components;
        }

        const fs::directory_iterator end;
        for (fs::directory_iterator it(componentsDir, ec); !ec && it != end; it.increment(ec)) {
            // One level on purpose: the checks/ folder beside the library holds the
            // verification scripts, which are not components.
            if (!it->is_regular_file(ec) || it->path().extension() != ".lua") {
                continue;
            }

            ReadyMadeComponent component;
            component.name = it->path().stem().string();
            component.source = it->path();

            bool ok = false;
            const std::string text = readWholeFile(component.source, ok);
            component.summary = summaryOf(text);
            component.category = categoryOf(text);
            components.push_back(component);
        }

        std::sort(components.begin(), components.end(),
                  [](const ReadyMadeComponent& a, const ReadyMadeComponent& b) { return a.name < b.name; });
        return components;
    }

    ReadyMadeInstallResult installReadyMadeComponents(const fs::path& componentsDir,
                                                      const fs::path& luaRoot,
                                                      const fs::path& projectRoot,
                                                      const std::string& subdir,
                                                      const std::vector<std::string>& names,
                                                      bool overwrite,
                                                      const std::function<void(const std::string&)>& log,
                                                      bool withDemos) {
        ReadyMadeInstallResult result;
        const std::vector<ReadyMadeComponent> library = listReadyMadeComponents(componentsDir);
        if (library.empty()) {
            if (log) {
                log("no components found in " + componentsDir.string());
            }
            result.failed = 1;
            return result;
        }

        result.destination = luaRoot;
        if (!subdir.empty()) {
            result.destination /= subdir;
        }

        std::vector<ReadyMadeComponent> selected;
        if (names.empty()) {
            selected = library;
        } else {
            for (const std::string& name : names) {
                const std::string wanted = lowered(trimmed(name));
                const auto found = std::find_if(library.begin(), library.end(),
                                                [&wanted](const ReadyMadeComponent& component) {
                                                    return lowered(component.name) == wanted;
                                                });
                if (found == library.end()) {
                    ++result.failed;
                    if (log) {
                        log("not in the engine library: " + name);
                    }
                    continue;
                }
                selected.push_back(*found);
            }
        }

        if (selected.empty()) {
            return result;
        }

        std::error_code ec;
        fs::create_directories(result.destination, ec);
        if (ec) {
            if (log) {
                log("cannot create " + result.destination.string() + ": " + ec.message());
            }
            result.failed = static_cast<int>(selected.size());
            return result;
        }

        // One file, one outcome, one log line - the library loop and the demo loop below
        // both need it, and "the project owns its copy" is the rule they share.
        auto place = [&](const fs::path& source, const fs::path& destination, const std::string& label) {
            {
                std::ifstream probe(destination, std::ios::in);
                if (probe.good() && !overwrite) {
                    ++result.skipped;
                    if (log) {
                        log("kept the project's own " + label + " (use --overwrite to replace it)");
                    }
                    return;
                }
            }

            bool ok = false;
            const std::string content = readWholeFile(source, ok);
            if (!ok) {
                ++result.failed;
                if (log) {
                    log("cannot read " + source.string());
                }
                return;
            }

            switch (FileUtils::writeFile(destination, content)) {
                case FileUtils::WriteResult::Written:
                    ++result.copied;
                    if (log) {
                        log("installed " + label);
                    }
                    break;
                case FileUtils::WriteResult::Unchanged:
                    ++result.skipped;
                    if (log) {
                        log("already up to date: " + label);
                    }
                    break;
                case FileUtils::WriteResult::Failed:
                    ++result.failed;
                    if (log) {
                        log("cannot write " + destination.string());
                    }
                    break;
            }
        };

        for (const ReadyMadeComponent& component : selected) {
            place(component.source, result.destination / (component.name + ".lua"), component.name + ".lua");
        }

        // The library's own examples: kit_demo.lua and the demo_*.lua scenes that wire real
        // components together. They sit at <luaRoot>/demos, not inside the Components folder, so the
        // runner reaches them with `--scene demos/<name>`; require("Components.<Name>") still
        // resolves because it is read from the Lua root, not from the folder the demo sits in.
        // Only a whole-library install gets them: a --only install cannot run a demo that needs
        // components the project does not have.
        const int libraryCopied = result.copied;
        if (withDemos && names.empty()) {
            const fs::path demosSource = componentsDir / "demos";
            std::error_code demoEc;
            if (fs::is_directory(demosSource, demoEc)) {
                const fs::path demosDir = luaRoot / "demos";
                fs::create_directories(demosDir, demoEc);
                if (!demoEc) {
                    const fs::directory_iterator end;
                    std::vector<fs::path> demoFiles;
                    for (fs::directory_iterator it(demosSource, demoEc); !demoEc && it != end; it.increment(demoEc)) {
                        if (it->is_regular_file(demoEc) && it->path().extension() == ".lua") {
                            demoFiles.push_back(it->path());
                        }
                    }
                    std::sort(demoFiles.begin(), demoFiles.end());
                    for (const fs::path& file : demoFiles) {
                        place(file, demosDir / file.filename(), "demos/" + file.filename().string());
                    }
                }
            }
        }

        result.demos = result.copied - libraryCopied;
        result.copied = libraryCopied;

        // The kit's own textures and sounds: the sprites the drops are drawn from and the thunder
        // sample the storm demo schedules. They sit next to the components in the engine so one
        // install delivers a demo that runs, and they are skipped for the same reason the demos are
        // - a --only install cannot run the scene they belong to.
        if (withDemos && names.empty()) {
            const fs::path assetsSource = componentsDir / "assets";
            std::error_code assetEc;
            if (fs::is_directory(assetsSource, assetEc)) {
                if (projectRoot.empty()) {
                    if (log) {
                        log("no project folder given: the kit's assets were not copied");
                    }
                } else {
                    const fs::path assetsDir = projectRoot / "assets";
                    std::vector<fs::path> assets;
                    const fs::recursive_directory_iterator assetsEnd;
                    for (fs::recursive_directory_iterator it(assetsSource, assetEc); !assetEc && it != assetsEnd; it.increment(assetEc)) {
                        if (it->is_regular_file(assetEc)) {
                            assets.push_back(it->path());
                        }
                    }
                    std::sort(assets.begin(), assets.end());
                    const int base = result.copied;
                    for (const fs::path& file : assets) {
                        const fs::path relative = std::filesystem::relative(file, assetsSource, assetEc);
                        place(file, assetsDir / relative, "assets/" + relative.generic_string());
                    }
                    result.assets = result.copied - base;
                    result.copied = base;
                }
            }
        }

        return result;
    }

    int runComponentsCommand(int argc, char** argv, const char* executableName) {
        const std::string commandName = commandBaseName(executableName);

        ComponentsOptions options;
        std::string error;
        if (!parseComponentsArgs(argc, argv, options, error)) {
            std::cerr << commandName << " components: " << error << "\n\n";
            printUsage(commandName);
            return 2;
        }
        if (options.help) {
            printUsage(commandName);
            return 0;
        }

        const fs::path componentsDir = options.engineDir.empty() ? findComponentsDir()
                                                                 : fs::path(options.engineDir) / "components";
        if (componentsDir.empty() || !fs::is_directory(componentsDir)) {
            std::cerr << commandName << " components: the engine ships no library at "
                      << (componentsDir.empty() ? std::string("<engine folder>/components") : componentsDir.string())
                      << "\n";
            return 1;
        }

        const std::vector<ReadyMadeComponent> library = listReadyMadeComponents(componentsDir);

        if (options.list) {
            std::cout << "Engine component library (" << library.size() << " components) at "
                      << componentsDir.string() << "\n";

            // Grouped the way the editor's dialog shows them, so the two can be compared
            std::vector<ReadyMadeComponent> grouped = library;
            std::sort(grouped.begin(), grouped.end(), [](const ReadyMadeComponent& a, const ReadyMadeComponent& b) {
                return a.category != b.category ? a.category < b.category : a.name < b.name;
            });

            std::string shown;
            for (const ReadyMadeComponent& component : grouped) {
                if (component.category != shown) {
                    shown = component.category;
                    std::cout << "\n" << (shown.empty() ? "Ungrouped" : shown) << "\n";
                }
                std::cout << "  " << component.name;
                if (!component.summary.empty()) {
                    std::cout << "  -  " << component.summary;
                }
                std::cout << "\n";
            }
            if (!options.projectPath.empty()) {
                std::cout << "\nInstall them with: " << commandName << " components install --project \""
                          << options.projectPath << "\"\n";
            }
            return library.empty() ? 1 : 0;
        }

        const fs::path luaRoot = resolveLuaRoot(options, error);
        if (luaRoot.empty()) {
            std::cerr << commandName << " components: " << error << "\n";
            return 2;
        }

        const ReadyMadeInstallResult result = installReadyMadeComponents(
            componentsDir, luaRoot, options.projectPath, options.subdir, options.names, options.overwrite,
            [](const std::string& line) { std::cout << "  " << line << "\n"; }, !options.noDemos);

        std::cout << "\n" << result.copied << " copied, " << result.skipped << " already there, "
                  << result.failed << " failed, into " << result.destination.string() << "\n";
        if (result.demos > 0) {
            std::cout << result.demos << " example scenes in demos/ - run one with "
                      << "--scene demos/demo_beach\n";
        }
        if (result.assets > 0) {
            std::cout << result.assets << " kit assets in assets/ (the sprites the particles use "
                      << "and the thunder sample)\n";
        }
        std::cout << "Attach one in the editor: Inspector -> Scripts -> Add Script -> Lua Script, "
                  << "then pick " << (options.subdir.empty() ? std::string("") : options.subdir + "/")
                  << "<Name>.lua as its Source File.\n";

        return result.failed == 0 ? 0 : 1;
    }

}
