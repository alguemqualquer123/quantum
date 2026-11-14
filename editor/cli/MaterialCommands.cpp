// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "MaterialCommands.h"

#include "util/DefaultMaterial.h"
#include "util/MaterialSets.h"

#include "yaml-cpp/yaml.h"

#include <iostream>
#include <string>
#include <system_error>

namespace quantum::editor::cli {

    namespace {

        std::string commandNameOf(const char* argv0) {
            const std::string full(argv0 ? argv0 : "quantum-editor");
            const size_t slash = full.find_last_of("/\\");
            return slash == std::string::npos ? full : full.substr(slash + 1);
        }

        void printUsage(const std::string& commandName) {
            std::cout <<
                "Usage: " << commandName << " material <command> [options]\n"
                "\n"
                "The material set every primitive the editor creates starts with. A project\n"
                "owns its own copy of the maps, because a scene stores each texture relative\n"
                "to the project's assets root.\n"
                "\n"
                "  status  [--project <dir>] [--set <name>]\n"
                "                        What the engine ships and what the project has\n"
                "  install [--project <dir>] [--set <name>]\n"
                "                        Copy the missing maps into the project\n"
                "  list                    Every set the engine ships\n"
                "\n"
                "  --set <name>   Which engine set, matched loosely against its folder and\n"
                "                 stem (\"ground-grass\", \"grass\"). Without it the command\n"
                "                 keeps working on the default primitive material.\n"
                "  --assets <dir>   assets root override, relative to --project or absolute\n"
                "                   (default: the assetsDir of project.yaml, then the project)\n"
                "  -h, --help       This page\n";
        }

        struct Options {
            std::string command;
            std::string projectPath;
            std::string assetsDir;
            std::string set;
            bool help = false;
        };

        bool parseArgs(int argc, char** argv, Options& options, std::string& error) {
            // argv[0] is the `material` token itself, exactly like the components and
            // export subcommands, so the verb is the argument behind it.
            options.command = argc >= 2 && argv[1] ? argv[1] : "";

            for (int i = 2; i < argc; ++i) {
                const std::string argument = argv[i] ? argv[i] : "";

                if (argument == "-h" || argument == "--help") {
                    options.help = true;
                    continue;
                }
                if (argument == "--project") {
                    if (i + 1 >= argc) {
                        error = "--project needs a directory";
                        return false;
                    }
                    options.projectPath = argv[++i];
                    continue;
                }
                if (argument == "--assets") {
                    if (i + 1 >= argc) {
                        error = "--assets needs a directory";
                        return false;
                    }
                    options.assetsDir = argv[++i];
                    continue;
                }
                if (argument == "--set") {
                    if (i + 1 >= argc) {
                        error = "--set needs a material set name";
                        return false;
                    }
                    options.set = argv[++i];
                    continue;
                }
                if (!argument.empty() && argument[0] == '-') {
                    error = "unknown argument '" + argument + "'";
                    return false;
                }
                error = "unexpected argument '" + argument + "'";
                return false;
            }

            if (options.help || options.command.empty()) {
                return true;
            }
            if (options.command != "status" && options.command != "install"
                    && options.command != "list") {
                error = "unknown command '" + options.command + "'";
                return false;
            }
            return true;
        }

        // The same rule Project::getAssetsPath() follows: assetsDir is read from
        // project.yaml and resolved against the project, and "." means the project
        // itself - which is why a template can keep Dirt_01.png at its root.
        fs::path resolveAssetsRoot(const Options& options, std::string& error) {
            const fs::path project(options.projectPath);
            std::error_code ec;
            if (!fs::is_directory(project, ec)) {
                error = "not a directory: " + project.string();
                return {};
            }

            std::string assetsDir = options.assetsDir;
            if (assetsDir.empty()) {
                const fs::path projectFile = project / "project.yaml";
                if (fs::exists(projectFile, ec)) {
                    try {
                        const YAML::Node node = YAML::LoadFile(projectFile.string());
                        if (node["assetsDir"]) {
                            assetsDir = node["assetsDir"].as<std::string>();
                        }
                    } catch (const YAML::Exception& e) {
                        error = "cannot read project.yaml: " + e.msg;
                        return {};
                    }
                }
            }
            if (assetsDir.empty() || assetsDir == ".") {
                return project;
            }

            const fs::path requested(assetsDir);
            return (requested.is_absolute() ? requested : project / requested).lexically_normal();
        }

        void printReport(const fs::path& assetsRoot, const Material& material) {
            if (material.baseColorTexture.empty()) {
                std::cout << "project maps: " << assetsRoot.string()
                          << " - no set there, install it first\n";
                return;
            }

            // The exact strings a scene file will carry, so the CLI report and the
            // serializer cannot drift apart.
            std::cout << "\nthe material a new primitive gets:\n"
                      << "  name                     " << material.name << "\n"
                      << "  baseColorTexture         " << material.baseColorTexture.getPath() << "\n"
                      << "  normalTexture            " << material.normalTexture.getPath() << "\n"
                      << "  metallicRoughnessTexture " << material.metallicRoughnessTexture.getPath() << "\n"
                      << "  heightTexture            " << material.heightTexture.getPath() << "\n"
                      << "  heightScale              " << material.heightScale << "\n"
                      << "  metallicFactor           " << material.metallicFactor << "\n"
                      << "  roughnessFactor          " << material.roughnessFactor << "\n";

            // Resolved the way Platform::getAssetPath() resolves it for the project.
            const fs::path absolute = assetsRoot / material.baseColorTexture.getPath();
            std::cout << "\nits folder:   " << absolute.parent_path().generic_string() << "\n"
                      << "on disk:      " << (fs::exists(absolute) ? "yes" : "NO FILE") << "\n";
        }

    }

    int runMaterialCommand(int argc, char** argv, const char* executableName) {
        const std::string commandName = commandNameOf(executableName);

        Options options;
        std::string error;
        if (!parseArgs(argc, argv, options, error)) {
            std::cerr << commandName << " material: " << error << "\n\n";
            printUsage(commandName);
            return 2;
        }
        if (options.help) {
            printUsage(commandName);
            return 0;
        }

        if (options.command == "list") {
            const std::vector<MaterialSet> sets = MaterialSets::engineSets();
            if (sets.empty()) {
                std::cerr << "the engine ships no material library\n";
                return 1;
            }
            std::cout << "engine material library: "
                      << MaterialSets::engineMaterialsRoot().generic_string() << "\n";
            for (const MaterialSet& set : sets) {
                std::cout << "  " << set.directory.filename().generic_string() << "/" << set.name
                          << "  (" << set.mapCount() << " maps)\n";
            }
            return 0;
        }

        if (!options.set.empty()) {
            const MaterialSet set = MaterialSets::findEngineSet(options.set);
            if (set.directory.empty()) {
                std::cerr << commandName << " material: no engine set matches \"" << options.set
                          << "\" - try `" << commandName << " material list`\n";
                return 2;
            }
            if (options.projectPath.empty()) {
                std::cout << "engine set: " << set.directory.generic_string() << "\n";
                for (const fs::path& file : set.files) {
                    std::cout << "  " << file.filename().generic_string() << "\n";
                }
                return 0;
            }

            const fs::path assetsRoot = resolveAssetsRoot(options, error);
            if (assetsRoot.empty()) {
                std::cerr << commandName << " material: " << error << "\n";
                return 2;
            }

            std::vector<std::string> copied;
            std::string installError;
            if (options.command == "install") {
                if (!MaterialSets::installSet(set, assetsRoot, &copied, &installError)) {
                    std::cerr << commandName << " material install: " << installError << "\n";
                    return 1;
                }
            }
            std::cout << "\nset: " << set.directory.filename().generic_string() << "/" << set.name
                      << "  (" << set.mapCount() << " maps)\n";

            // The scene only samples a few of them, and it must store each one relative to
            // the assets root - print exactly that, so wiring a template is a copy-paste.
            const std::string prefix = (fs::path("materials") / set.directory.filename()).generic_string() + "/";
            const auto relative = [&prefix](const fs::path& file) {
                return file.empty() ? std::string("<none>") : prefix + file.filename().generic_string();
            };
            std::cout << "  baseColor            " << relative(set.baseColor) << "\n"
                      << "  normalTexture        " << relative(set.normal.empty() ? set.normalDX : set.normal) << "\n"
                      << "  metallicRoughness    " << relative(set.metallicRoughness.empty()
                                            ? set.roughness : set.metallicRoughness) << "\n"
                      << "  heightTexture        " << relative(set.height) << "\n"
                      << "  occlusionTexture     " << relative(set.packedAO.empty()
                                            ? set.occlusion : set.packedAO) << "\n";
            if (options.command == "install") {
                std::cout << "\ninstalled " << copied.size() << " maps into "
                          << assetsRoot.generic_string() << "\n";
            }
            return 0;
        }

        const fs::path source = DefaultMaterial::sourceDir();
        std::cout << "engine set:   " << (source.empty() ? std::string("<not found>") : source.generic_string()) << "\n";
        for (const std::string& name : DefaultMaterial::fileNames()) {
            std::cout << "  " << name << "\n";
        }

        if (options.projectPath.empty()) {
            std::cerr << "\n" << commandName << " material " << options.command
                      << ": --project is required\n";
            return 2;
        }

        const fs::path assetsRoot = resolveAssetsRoot(options, error);
        if (assetsRoot.empty()) {
            std::cerr << commandName << " material: " << error << "\n";
            return 2;
        }

        if (options.command == "install") {
            std::string installError;
            if (!DefaultMaterial::ensureInstalled(assetsRoot, &installError)) {
                std::cerr << commandName << " material install: " << installError << "\n";
                return 1;
            }
            std::cout << "\ninstalled\n";
        } else if (!DefaultMaterial::isInstalled(assetsRoot)) {
            std::cout << "\nnot installed - " << commandName << " material install --project \""
                      << options.projectPath << "\"\n";
            return 1;
        }

        printReport(assetsRoot, DefaultMaterial::build(assetsRoot));
        return 0;
    }

}
