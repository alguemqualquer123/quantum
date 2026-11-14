// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

namespace editor{

    // A package is a ZIP: the same container Unreal's Fab downloads, readable by any tool, with a
    // plugin.json at the root of one folder inside it.
    //
    // Only what a package needs is implemented: the central directory, method 0 (stored) and method 8
    // (deflate), and writing back deflated entries. No encryption, no ZIP64 - a package that needs
    // 4 GB of one file is a data set, not a plugin, and `create` says so instead of writing a
    // truncated archive.
    //
    // Inflate comes from the vendored ufbx (ufbx_inflate, linked into the editor for FBX) and the
    // deflate from the vendored stb (stbi_zlib_compress, already used by engine/core/util/PngWriter),
    // so no dependency is added.
    struct ZipEntry{
        std::string name;
        bool directory = false;
        unsigned int method = 0;
        unsigned int crc = 0;
        unsigned long long compressedSize = 0;
        unsigned long long uncompressedSize = 0;
    };

    class PackageZip{
    public:
        // Every file under sourceDirectory becomes an entry prefixed by the folder name, so
        // unpacking one package always lands in its own directory instead of scattering files.
        //
        // Build output is left out: a plugin folder inside the build tree carries its own CMake and
        // MSBuild scaffolding, and a package full of .vcxproj files installs nothing a player needs.
        // What was skipped is reported, because a package that silently lost a file is worse than one
        // that carries a few kilobytes of junk (see isBuildArtifact for the exact rule).
        static bool create(const std::string& zipPath, const std::string& sourceDirectory,
                           std::string& errorOut, std::vector<std::string>* skippedOut = nullptr);

        static bool list(const std::string& zipPath, std::vector<ZipEntry>& out, std::string& errorOut);

        // Refuses an entry whose name escapes the target (the classic zip-slip): a package is
        // third-party bytes, and "it came from the registry" is not a reason to let it write next to
        // the project.
        static bool extract(const std::string& zipPath, const std::string& targetDirectory,
                            std::string& errorOut);

        static bool readEntry(const std::string& zipPath, const std::string& entryName,
                              std::vector<unsigned char>& out, std::string& errorOut);

        // Structure only: EOCD, central directory, and each local header where the directory says it
        // is. Used by `plugins verify` before anything is installed.
        static bool isReadable(const std::string& zipPath, std::string& errorOut);

        static std::string baseFolderOf(const std::string& zipPath, std::string& errorOut);

        // True for a path (relative to the package folder, '/' separated) that is build scaffolding
        // rather than plugin content.
        static bool isBuildArtifact(const std::string& relativePath);
    };

}
