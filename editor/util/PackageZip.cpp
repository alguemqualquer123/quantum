// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "PackageZip.h"

#include "Export.h"
#include "util/Checksum.h"

#include "stb_image_write.h"
#include "ufbx.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

using namespace editor;

// The same reuse pattern PngWriter.cpp and Clipboard.cpp already use: the symbol is defined inside
// the implementation block of stb_image_write.h, which the engine compiles once into the static `stb`
// library that quantum links PUBLIC, so every consumer of the engine already has it.
extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len, int quality);

namespace{

    const unsigned int LOCAL_SIGNATURE    = 0x04034b50u;
    const unsigned int CENTRAL_SIGNATURE  = 0x02014b50u;
    const unsigned int END_SIGNATURE      = 0x06054b50u;
    const size_t LOCAL_HEADER_SIZE        = 30;
    const size_t CENTRAL_HEADER_SIZE      = 46;
    const size_t END_HEADER_SIZE          = 22;

    // 1980-01-01 00:00:00 in DOS form. Fixed on purpose: a package built twice from the same folder
    // has to hash the same, or the catalog's sha256 changes every time someone rebuilds it.
    const unsigned short DOS_TIME = 0x0000;
    const unsigned short DOS_DATE = 0x0021;

    const unsigned short FLAG_UTF8_NAMES = 0x0800;

    void put16(std::vector<unsigned char>& out, unsigned short value){
        out.push_back(static_cast<unsigned char>(value & 0xFF));
        out.push_back(static_cast<unsigned char>((value >> 8) & 0xFF));
    }

    void put32(std::vector<unsigned char>& out, unsigned int value){
        for (int i = 0; i < 4; i++) out.push_back(static_cast<unsigned char>((value >> (i * 8)) & 0xFF));
    }

    unsigned short get16(const std::vector<unsigned char>& bytes, size_t at){
        return static_cast<unsigned short>(bytes[at]) | static_cast<unsigned short>(bytes[at + 1] << 8);
    }

    unsigned int get32(const std::vector<unsigned char>& bytes, size_t at){
        unsigned int value = 0;
        for (int i = 0; i < 4; i++) value |= static_cast<unsigned int>(bytes[at + i]) << (i * 8);
        return value;
    }

    bool wholeFileToMemory(const std::string& path, std::vector<unsigned char>& out, std::string& errorOut){
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream.is_open()){
            errorOut = "cannot open " + path;
            return false;
        }
        const std::streamoff length = stream.tellg();
        if (length < 0){
            errorOut = "cannot size " + path;
            return false;
        }
        out.resize(static_cast<size_t>(length));
        if (length > 0){
            stream.seekg(0);
            stream.read(reinterpret_cast<char*>(out.data()), length);
            if (!stream.good()){
                errorOut = "cannot read " + path;
                return false;
            }
        }
        return true;
    }

    bool writeWholeFile(const std::string& path, const unsigned char* data, size_t size, std::string& errorOut){
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream.is_open()){
            errorOut = "cannot write " + path;
            return false;
        }
        if (size > 0) stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!stream.good()){
            errorOut = "the disk refused " + path;
            return false;
        }
        return true;
    }

    // ZIP stores forward slashes; a Windows path has to be converted or the archive is unreadable
    // everywhere else.
    std::string zipName(const std::string& native){
        std::string out = native;
        std::replace(out.begin(), out.end(), '\\', '/');
        return out;
    }

    bool slamsIntoParent(const std::string& name){
        size_t start = 0;
        while (start <= name.size()){
            const size_t end = name.find('/', start);
            const std::string part = name.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (part == "..") return true;
            if (part.find(':') != std::string::npos) return true;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return false;
    }

    struct CentralRecord{
        ZipEntry entry;
        unsigned int localOffset = 0;
    };

    // Reads the central directory. Everything the extractor needs comes from here, never from a local
    // header, because a streaming writer leaves the sizes zeroed in the local one.
    bool readCentralDirectory(const std::vector<unsigned char>& bytes, std::vector<CentralRecord>& out,
                              std::string& errorOut){
        out.clear();

        if (bytes.size() < END_HEADER_SIZE){
            errorOut = "the package is too small to be a ZIP";
            return false;
        }

        // The end record is the last thing in the file, followed by a comment of up to 64 KB.
        const size_t scanFrom = bytes.size() - std::min<size_t>(bytes.size(), END_HEADER_SIZE + 65535);
        long long foundAt = -1;
        for (size_t i = bytes.size() - END_HEADER_SIZE + 1; i-- > scanFrom;){
            if (get32(bytes, i) == END_SIGNATURE){ foundAt = static_cast<long long>(i); break; }
        }
        if (foundAt < 0){
            errorOut = "no ZIP end record: the download is truncated or this is not a package";
            return false;
        }

        const size_t end = static_cast<size_t>(foundAt);
        const unsigned int totalRecords = get16(bytes, end + 10);
        const unsigned int directorySize = get32(bytes, end + 12);
        const unsigned int directoryOffset = get32(bytes, end + 16);

        if (totalRecords == 0xFFFF || directoryOffset == 0xFFFFFFFF){
            errorOut = "ZIP64 packages are not supported; the package is over 65535 entries or 4 GB";
            return false;
        }
        const size_t directoryEnd = static_cast<size_t>(directoryOffset) + directorySize;
        if (directoryOffset > bytes.size() || directoryEnd > bytes.size()){
            errorOut = "the ZIP central directory points outside the file";
            return false;
        }
        // The declared size has to land on the end record: that is what every other tool checks, and
        // a writer that miscounts its own field produces an archive they reject while we would shrug.
        if (directoryEnd != end){
            errorOut = "the ZIP central directory is " +
                       std::to_string(end > directoryEnd ? end - directoryEnd
                                                          : directoryEnd - end) +
                       " byte(s) from where the end record says it ends";
            return false;
        }

        size_t cursor = directoryOffset;
        for (unsigned int i = 0; i < totalRecords; i++){
            if (cursor + CENTRAL_HEADER_SIZE > bytes.size() || get32(bytes, cursor) != CENTRAL_SIGNATURE){
                errorOut = "the ZIP central directory is broken at record " + std::to_string(i);
                return false;
            }
            const unsigned short nameLength = get16(bytes, cursor + 28);
            const unsigned short extraLength = get16(bytes, cursor + 30);
            const unsigned short commentLength = get16(bytes, cursor + 32);

            CentralRecord record;
            record.entry.method = get16(bytes, cursor + 10);
            record.entry.crc = get32(bytes, cursor + 16);
            record.entry.compressedSize = get32(bytes, cursor + 20);
            record.entry.uncompressedSize = get32(bytes, cursor + 24);
            record.localOffset = get32(bytes, cursor + 42);

            const size_t nameStart = cursor + CENTRAL_HEADER_SIZE;
            if (nameStart + nameLength > bytes.size()){
                errorOut = "a ZIP entry name runs past the end of the file";
                return false;
            }
            record.entry.name = std::string(reinterpret_cast<const char*>(&bytes[nameStart]), nameLength);
            record.entry.directory = !record.entry.name.empty() && record.entry.name.back() == '/';

            out.push_back(record);
            cursor = nameStart + nameLength + extraLength + commentLength;
        }

        // Strict, the way Python's zipfile is: the directory has to end exactly where the end record
        // starts. A writer that miscounts its own size field produces an archive other tools refuse to
        // open, and a package that only our extractor reads is worthless on a registry.
        if (cursor != end){
            errorOut = "the ZIP central directory is " + std::to_string(end > cursor ? end - cursor
                                                                                      : cursor - end) +
                       " byte(s) away from where its own size field says it ends";
            return false;
        }
        return true;
    }

    // The payload of one entry, decompressed. The local header only tells where the bytes start: the
    // sizes and the method are taken from the central record.
    bool readEntryBytes(const std::vector<unsigned char>& bytes, const CentralRecord& record,
                        std::vector<unsigned char>& out, std::string& errorOut){
        const unsigned int offset = record.localOffset;
        if (offset + LOCAL_HEADER_SIZE > bytes.size() || get32(bytes, offset) != LOCAL_SIGNATURE){
            errorOut = "entry \"" + record.entry.name + "\": no local header where the index says";
            return false;
        }
        const unsigned short nameLength = get16(bytes, offset + 26);
        const unsigned short extraLength = get16(bytes, offset + 28);
        const size_t dataStart = static_cast<size_t>(offset) + LOCAL_HEADER_SIZE + nameLength + extraLength;
        if (dataStart + record.entry.compressedSize > bytes.size()){
            errorOut = "entry \"" + record.entry.name + "\": its data runs past the end of the file";
            return false;
        }

        out.assign(record.entry.uncompressedSize, 0);
        const unsigned char* payload = &bytes[dataStart];

        if (record.entry.method == 0){
            if (record.entry.uncompressedSize != record.entry.compressedSize){
                errorOut = "entry \"" + record.entry.name + "\": stored sizes disagree";
                return false;
            }
            out.assign(payload, payload + record.entry.compressedSize);
        }else if (record.entry.method == 8){
            ufbx_inflate_retain retain;
            retain.initialized = false;
            ufbx_inflate_input input;
            memset(&input, 0, sizeof(input));
            input.total_size = record.entry.uncompressedSize;
            input.data = payload;
            input.data_size = record.entry.compressedSize;
            // A ZIP deflate stream has neither the zlib header nor the adler32 trailer that ufbx's
            // own FBX path expects.
            input.no_header = true;
            input.no_checksum = true;

            const ptrdiff_t got = ufbx_inflate(out.data(), out.size(), &input, &retain);
            if (got < 0){
                errorOut = "entry \"" + record.entry.name + "\": the deflate stream is corrupt";
                return false;
            }
            if (static_cast<unsigned long long>(got) != record.entry.uncompressedSize){
                errorOut = "entry \"" + record.entry.name + "\": unpacked " + std::to_string(got) +
                           " of " + std::to_string(record.entry.uncompressedSize) + " bytes";
                return false;
            }
        }else{
            errorOut = "entry \"" + record.entry.name + "\": compression method " +
                       std::to_string(record.entry.method) + " is not stored or deflate";
            return false;
        }

        if (record.entry.crc != quantum::crc32Of(out.data(), out.size())){
            errorOut = "entry \"" + record.entry.name + "\": the CRC does not match, the bytes are damaged";
            return false;
        }
        return true;
    }

    // stbi_zlib_compress returns a zlib stream (2-byte header, raw deflate, 4-byte adler32). A ZIP
    // method 8 entry is the raw middle only, so the wrapper comes off here.
    bool deflateRaw(const std::vector<unsigned char>& data, std::vector<unsigned char>& out){
        if (data.empty()) return false;
        int compressedLength = 0;
        std::vector<unsigned char> copy = data;
        unsigned char* zlib = stbi_zlib_compress(copy.data(), static_cast<int>(copy.size()),
                                                 &compressedLength, 9);
        if (!zlib) return false;
        const bool sane = compressedLength > 6;
        if (sane){
            out.assign(zlib + 2, zlib + compressedLength - 4);
        }
        free(zlib);
        return sane;
    }

}

bool PackageZip::isBuildArtifact(const std::string& relativePath){
    // CMake and MSBuild write their scaffolding *inside* the target's output directory, so a plugin
    // folder in the build tree carries .vcxproj files and object directories next to the DLL and the
    // manifest. None of those names can collide with a plugin.json, which is what the rule is checked
    // against: the manifest a package is identified by is never dropped.
    //
    // The linker byproducts and debug symbols are out too - a 1.6 MB .pdb next to a 118 KB DLL is
    // ten times the download for nothing a player can use, and no asset is one of these extensions.
    static const char* const skippedFolders[] = { "CMakeFiles", ".staging", ".cache" };
    static const char* const skippedNames[] = { "cmake_install.cmake" };
    static const char* const skippedSuffixes[] = { ".vcxproj", ".vcxproj.filters", ".vcxproj.user",
                                                   ".tlog", ".log", ".obj", ".tmp",
                                                   ".exp", ".lib", ".pdb" };

    size_t start = 0;
    while (start <= relativePath.size()) {
        const size_t slash = relativePath.find('/', start);
        const std::string component = relativePath.substr(
            start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!component.empty()) {
            for (const char* folder : skippedFolders) {
                if (component == folder) return true;
            }
            // A CMake object directory is named after the target: `<name>.dir`, `<name>.dir/Debug`.
            if (component.size() > 4 && component.compare(component.size() - 4, 4, ".dir") == 0) {
                return true;
            }
            if (slash == std::string::npos) {
                for (const char* name : skippedNames) {
                    if (component == name) return true;
                }
                for (const char* suffix : skippedSuffixes) {
                    const size_t length = std::string(suffix).size();
                    if (component.size() > length &&
                        component.compare(component.size() - length, length, suffix) == 0) {
                        return true;
                    }
                }
            }
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return false;
}

bool PackageZip::create(const std::string& zipPath, const std::string& sourceDirectory,
                        std::string& errorOut, std::vector<std::string>* skippedOut){
    errorOut.clear();

    std::error_code ec;
    std::filesystem::path root = std::filesystem::weakly_canonical(sourceDirectory, ec);
    if (ec || !std::filesystem::is_directory(root, ec)){
        errorOut = "not a directory: " + sourceDirectory;
        return false;
    }
    const std::string folder = zipName(root.filename().string());
    if (folder.empty()){
        errorOut = "the package folder needs a name to go inside the archive";
        return false;
    }

    std::vector<std::filesystem::path> files;
    std::vector<std::filesystem::path> directories;
    for (std::filesystem::recursive_directory_iterator walk(root,
             std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && walk != std::filesystem::recursive_directory_iterator(); ++walk){
        std::error_code nameEc;
        const std::string relative =
            zipName(std::filesystem::relative(walk->path(), root, nameEc).string());
        // Build scaffolding is reported, not silently dropped: the person packaging needs to know
        // the archive is smaller than the folder it came from.
        if (!nameEc && isBuildArtifact(relative)) {
            if (skippedOut && !relative.empty()) skippedOut->push_back(relative);
            std::error_code dirEc;
            if (walk->is_directory(dirEc)) walk.disable_recursion_pending();
            continue;
        }
        if (walk->is_directory(ec)) directories.push_back(walk->path());
        else if (walk->is_regular_file(ec)) files.push_back(walk->path());
    }
    if (ec){
        errorOut = "cannot walk " + sourceDirectory + ": " + ec.message();
        return false;
    }
    // Sorted, not iteration order: two machines with the same files must produce the same bytes.
    std::sort(files.begin(), files.end());
    std::sort(directories.begin(), directories.end());

    if (files.size() + directories.size() + 1 > 65535){
        errorOut = "a package has " + std::to_string(files.size()) +
                   " files; the ZIP format this writes stops at 65534 entries";
        return false;
    }

    std::vector<unsigned char> archive;
    std::vector<CentralRecord> records;

    auto appendEntry = [&](const std::string& name, const std::vector<unsigned char>& data,
                           bool asDirectory){
        CentralRecord record;
        record.entry.name = name;
        record.entry.directory = asDirectory;
        record.localOffset = static_cast<unsigned int>(archive.size());

        std::vector<unsigned char> payload;
        if (asDirectory || data.empty()){
            record.entry.method = 0;
        }else if (deflateRaw(data, payload) && payload.size() < data.size()){
            record.entry.method = 8;
        }else{
            record.entry.method = 0;
            payload = data;
        }

        record.entry.compressedSize = payload.size();
        record.entry.uncompressedSize = data.size();
        record.entry.crc = asDirectory ? 0u : quantum::crc32Of(data.data(), data.size());

        put32(archive, LOCAL_SIGNATURE);
        put16(archive, 20);                       // version needed
        put16(archive, FLAG_UTF8_NAMES);
        put16(archive, static_cast<unsigned short>(record.entry.method));
        put16(archive, DOS_TIME);
        put16(archive, DOS_DATE);
        put32(archive, record.entry.crc);
        put32(archive, static_cast<unsigned int>(record.entry.compressedSize));
        put32(archive, static_cast<unsigned int>(record.entry.uncompressedSize));
        put16(archive, static_cast<unsigned short>(name.size()));
        put16(archive, 0);                        // no extra field
        archive.insert(archive.end(), name.begin(), name.end());
        archive.insert(archive.end(), payload.begin(), payload.end());

        records.push_back(record);
    };

    // Every directory that still holds a retained file, by its path relative to the source folder.
    std::set<std::string> kept;
    for (const std::filesystem::path& file : files){
        std::error_code relEc;
        std::string parent = zipName(std::filesystem::relative(file, root, relEc).string());
        if (relEc || parent.empty()) continue;
        for (size_t slash = parent.find_last_of('/'); slash != std::string::npos;
             slash = parent.find_last_of('/')){
            parent = parent.substr(0, slash);
            if (parent.empty()) break;
            kept.insert(parent);
        }
    }

    for (const std::filesystem::path& directory : directories){
        std::string relative = zipName(std::filesystem::relative(directory, root, ec).string());
        if (ec || relative.empty() || relative == ".") continue;
        // A folder whose every file the filter above dropped stays out too: an entry for a directory
        // no tool can fill is noise, and strict readers warn about archives that carry scaffolding.
        if (kept.count(relative) == 0) continue;
        if (relative.back() != '/') relative += '/';
        appendEntry(folder + "/" + relative, std::vector<unsigned char>(), true);
    }

    std::vector<unsigned char> data;
    for (const std::filesystem::path& file : files){
        const std::string relative = zipName(std::filesystem::relative(file, root, ec).string());
        if (ec || relative.empty()) continue;
        if (!wholeFileToMemory(file.string(), data, errorOut)) return false;
        appendEntry(folder + "/" + relative, data, false);
    }

    if (records.empty()){
        errorOut = sourceDirectory + " has no files to package";
        return false;
    }

    const unsigned int directoryOffset = static_cast<unsigned int>(archive.size());
    for (const CentralRecord& record : records){
        put32(archive, CENTRAL_SIGNATURE);
        put16(archive, 20);                       // version made by
        put16(archive, 20);                       // version needed
        put16(archive, FLAG_UTF8_NAMES);
        put16(archive, static_cast<unsigned short>(record.entry.method));
        put16(archive, DOS_TIME);
        put16(archive, DOS_DATE);
        put32(archive, record.entry.crc);
        put32(archive, static_cast<unsigned int>(record.entry.compressedSize));
        put32(archive, static_cast<unsigned int>(record.entry.uncompressedSize));
        put16(archive, static_cast<unsigned short>(record.entry.name.size()));
        put16(archive, 0);                        // extra
        put16(archive, 0);                        // comment
        put16(archive, 0);                        // disk
        put16(archive, 0);                        // internal attributes
        put32(archive, 0);                        // external attributes
        put32(archive, record.localOffset);
        archive.insert(archive.end(), record.entry.name.begin(), record.entry.name.end());
    }

    // Measured before the end record is appended: the size field describes the central directory
    // alone, and writing it inside the put32 call would count the 12 bytes already emitted.
    const unsigned int directorySize = static_cast<unsigned int>(archive.size() - directoryOffset);

    put32(archive, END_SIGNATURE);
    put16(archive, 0);
    put16(archive, 0);
    put16(archive, static_cast<unsigned short>(records.size()));
    put16(archive, static_cast<unsigned short>(records.size()));
    put32(archive, directorySize);
    put32(archive, directoryOffset);
    put16(archive, 0);

    std::filesystem::create_directories(std::filesystem::path(zipPath).parent_path(), ec);
    std::ofstream stream(zipPath, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()){
        errorOut = "cannot write " + zipPath;
        return false;
    }
    stream.write(reinterpret_cast<const char*>(archive.data()), static_cast<std::streamsize>(archive.size()));
    if (!stream.good()){
        errorOut = "the disk refused " + zipPath;
        return false;
    }
    return true;
}

bool PackageZip::list(const std::string& zipPath, std::vector<ZipEntry>& out, std::string& errorOut){
    out.clear();
    std::vector<unsigned char> bytes;
    if (!wholeFileToMemory(zipPath, bytes, errorOut)) return false;

    std::vector<CentralRecord> records;
    if (!readCentralDirectory(bytes, records, errorOut)) return false;
    for (const CentralRecord& record : records) out.push_back(record.entry);
    return true;
}

bool PackageZip::isReadable(const std::string& zipPath, std::string& errorOut){
    std::vector<unsigned char> bytes;
    if (!wholeFileToMemory(zipPath, bytes, errorOut)) return false;

    std::vector<CentralRecord> records;
    if (!readCentralDirectory(bytes, records, errorOut)) return false;

    // Every entry is opened, not just indexed: an archive whose index lies about an offset is a
    // failure worth catching before a package is offered for install.
    for (const CentralRecord& record : records){
        if (record.entry.directory) continue;
        std::vector<unsigned char> unused;
        if (!readEntryBytes(bytes, record, unused, errorOut)) return false;
        if (slamsIntoParent(record.entry.name)){
            errorOut = "entry \"" + record.entry.name + "\" escapes the install folder";
            return false;
        }
    }
    return true;
}

bool PackageZip::extract(const std::string& zipPath, const std::string& targetDirectory,
                         std::string& errorOut){
    errorOut.clear();
    std::vector<unsigned char> bytes;
    if (!wholeFileToMemory(zipPath, bytes, errorOut)) return false;

    std::vector<CentralRecord> records;
    if (!readCentralDirectory(bytes, records, errorOut)) return false;
    if (records.empty()){
        errorOut = "the package has no entries";
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(targetDirectory, ec);
    if (ec){
        errorOut = "cannot create " + targetDirectory + ": " + ec.message();
        return false;
    }

    std::vector<unsigned char> data;
    for (const CentralRecord& record : records){
        if (slamsIntoParent(record.entry.name)){
            errorOut = "entry \"" + record.entry.name + "\" tries to write outside the install folder";
            return false;
        }

        std::filesystem::path target = std::filesystem::path(targetDirectory) /
                                       std::filesystem::path(record.entry.name);
        if (record.entry.directory){
            std::filesystem::create_directories(target, ec);
            if (ec){
                errorOut = "cannot create " + target.generic_string() + ": " + ec.message();
                return false;
            }
            continue;
        }
        if (!readEntryBytes(bytes, record, data, errorOut)) return false;
        if (!writeWholeFile(target.generic_string(), data.data(), data.size(), errorOut)) return false;
    }
    return true;
}

bool PackageZip::readEntry(const std::string& zipPath, const std::string& entryName,
                           std::vector<unsigned char>& out, std::string& errorOut){
    std::vector<unsigned char> bytes;
    if (!wholeFileToMemory(zipPath, bytes, errorOut)) return false;

    std::vector<CentralRecord> records;
    if (!readCentralDirectory(bytes, records, errorOut)) return false;
    for (const CentralRecord& record : records){
        if (record.entry.name != entryName) continue;
        if (slamsIntoParent(entryName)){
            errorOut = "entry \"" + entryName + "\" escapes the install folder";
            return false;
        }
        return readEntryBytes(bytes, record, out, errorOut);
    }
    errorOut = "no entry \"" + entryName + "\" in the package";
    return false;
}

std::string PackageZip::baseFolderOf(const std::string& zipPath, std::string& errorOut){
    std::vector<ZipEntry> entries;
    if (!list(zipPath, entries, errorOut)) return std::string();
    if (entries.empty()){
        errorOut = "the package has no entries";
        return std::string();
    }
    const std::string& first = entries.front().name;
    const size_t slash = first.find('/');
    if (slash == std::string::npos){
        errorOut = "the package is not rooted in one folder";
        return std::string();
    }
    return first.substr(0, slash);
}
