// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

namespace quantum::editor {

    class CommandLine {
    public:
        // Runs the project export subcommand.
        //
        // `argc` and `argv` should already be shifted past the `export`
        // subcommand token (i.e. argv[0] is the subcommand itself).
        // `executableName` is used only for help and error output.
        static int runExportCommand(int argc, char** argv, const char* executableName);

        // Runs the standalone shader generation subcommand.
        static int runShadersCommand(int argc, char** argv, const char* executableName);

        static int runScriptCommand(int argc, char** argv, const char* executableName);

        static int runBenchmarkHelp(int argc, char** argv, const char* executableName);

        // Runs the project file type subcommand: it reports, installs or removes the
        // shell association that opens a project from its marker file.
        static int runProjectFileCommand(int argc, char** argv, const char* executableName);

        // Prints the top level usage, or reports an unrecognized argument.
        static int runHelpCommand(int argc, char** argv, const char* executableName);
    };

}
