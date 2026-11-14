// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "util/ShellAssociation.h"

#include "util/ProjectFile.h"

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    // SHChangeNotify and the SHCNE_/SHCNF_ constants come from ShlObj_core.h, which
    // only shlobj.h pulls in; shellapi.h declares none of them.
    #include <shlobj.h>
#endif

#include <string>

namespace quantum::editor::ShellAssociation {

namespace {

#ifdef _WIN32
    // The ProgID is the stable half of the pair: the extension a user configures may
    // change, but every version of the editor writes the same program identifier.
    // Projects associated by an older build keep their key in HKCU; rebinding the
    // extension on the next associate points it at this one.
    constexpr const wchar_t* PROGID = L"Quantum.Project";
    constexpr const wchar_t* DESCRIPTION = L"Quantum Project";
    constexpr const wchar_t* CLASS_ROOT = L"Software\\Classes\\";
    // The name Windows gives to a key's default value.
    constexpr const wchar_t* DEFAULT_VALUE = L"";

    std::string describeStatus(const std::string& what, LSTATUS status) {
        return what + " (Win32 error " + std::to_string(static_cast<unsigned long>(status)) + ")";
    }

    // The extension is normalized to a-z0-9 before it reaches this, so a plain
    // widening is exact. Paths are widened through std::filesystem, which keeps the
    // native form it was built with.
    std::wstring asciiToWide(const std::string& ascii) {
        return std::wstring(ascii.begin(), ascii.end());
    }

    std::wstring quoted(const std::wstring& value) {
        return L'"' + value + L'"';
    }

    std::string wideToUtf8(const std::wstring& value) {
        if (value.empty()) {
            return std::string();
        }
        const int needed = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                                               static_cast<int>(value.size()),
                                               nullptr, 0, nullptr, nullptr);
        if (needed <= 0) {
            return std::string();
        }
        std::string out(static_cast<size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
                            out.data(), needed, nullptr, nullptr);
        return out;
    }

    bool setString(const std::wstring& subKey, const wchar_t* valueName,
                   const std::wstring& data, std::string& error) {
        HKEY key = nullptr;
        DWORD disposition = 0;
        LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, nullptr,
                                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                                        &key, &disposition);
        if (status != ERROR_SUCCESS) {
            error = describeStatus("cannot open HKCU\\" + wideToUtf8(subKey), status);
            return false;
        }
        status = RegSetValueExW(key, valueName, 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(data.c_str()),
                                static_cast<DWORD>((data.size() + 1) * sizeof(wchar_t)));
        const LSTATUS closeStatus = RegCloseKey(key);
        if (status != ERROR_SUCCESS) {
            error = describeStatus("cannot write HKCU\\" + wideToUtf8(subKey), status);
            return false;
        }
        if (closeStatus != ERROR_SUCCESS) {
            error = describeStatus("cannot close HKCU\\" + wideToUtf8(subKey), closeStatus);
            return false;
        }
        return true;
    }

    std::wstring readString(HKEY root, const std::wstring& subKey, const wchar_t* valueName) {
        wchar_t buffer[512];
        DWORD size = sizeof(buffer);
        DWORD type = 0;
        const LSTATUS status = RegGetValueW(root, subKey.c_str(), valueName,
                                            RRF_RT_REG_SZ, &type, buffer, &size);
        if (status != ERROR_SUCCESS || type != REG_SZ) {
            return std::wstring();
        }
        // REG_SZ may or may not carry its terminator in the reported size.
        const DWORD characters = size / sizeof(wchar_t);
        return std::wstring(buffer, characters > 0 ? characters - 1 : 0);
    }

    std::string progId() {
        return wideToUtf8(std::wstring(PROGID));
    }
#endif
}

bool isSupported() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

std::string currentHandler(const std::string& extension) {
#ifdef _WIN32
    const std::wstring extensionKey = asciiToWide(ProjectFile::normalizeExtension(extension));

    // Read the *effective* handler: HKEY_CLASSES_ROOT is the view Explorer itself
    // uses, the per-user classes merged over the machine-wide ones, so a Delphi
    // installed for all users shows up here even though we only ever write to HKCU.
    const std::wstring effective = readString(HKEY_CLASSES_ROOT, extensionKey, DEFAULT_VALUE);
    if (!effective.empty()) {
        return wideToUtf8(effective);
    }
    return wideToUtf8(readString(HKEY_CURRENT_USER, std::wstring(CLASS_ROOT) + extensionKey,
                                 DEFAULT_VALUE));
#else
    (void)extension;
    return std::string();
#endif
}

bool isRegistered(const std::string& extension) {
#ifdef _WIN32
    return currentHandler(extension) == progId();
#else
    return false;
#endif
}

bool registerTarget(const Target& target, std::string& error) {
    error.clear();
#ifdef _WIN32
    if (target.executable.empty()) {
        error = "the editor executable path is unknown";
        return false;
    }
    const std::string extension = ProjectFile::normalizeExtension(target.extension);
    std::wstring subKey = std::wstring(CLASS_ROOT) + asciiToWide(extension);

    // Never take over an extension another program already claims.
    const std::string owner = currentHandler(extension);
    if (!owner.empty() && owner != progId()) {
        error = "the extension " + extension + " is already registered to \"" + owner +
                "\"; set a different project file extension in Editor Settings";
        return false;
    }

    const std::wstring progIdKey = std::wstring(CLASS_ROOT) + PROGID;
    bool ok = setString(progIdKey, DEFAULT_VALUE, std::wstring(DESCRIPTION), error);
    if (ok) {
        // With no badge of its own the marker wears the editor's: Explorer takes the
        // first icon group stored in the executable.
        const std::wstring icon = target.icon.empty()
            ? quoted(target.executable.wstring()) + L",0"
            : quoted(target.icon.wstring());
        ok = setString(progIdKey + L"\\DefaultIcon", DEFAULT_VALUE, icon, error);
    }
    if (ok) {
        ok = setString(progIdKey + L"\\shell\\open\\command", DEFAULT_VALUE,
                       quoted(target.executable.wstring()) + L" " + quoted(L"%1"), error);
    }
    if (ok) {
        ok = setString(subKey, DEFAULT_VALUE, std::wstring(PROGID), error);
    }
    if (!ok) {
        return false;
    }

    // Explorer caches the badge it draws and the handler it launches; without this
    // the new association only shows up after a sign-out.
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
#else
    (void)target;
    error = "registering a project file type is only implemented on Windows";
    return false;
#endif
}

bool unregisterTarget(const std::string& extension, std::string& error) {
    error.clear();
#ifdef _WIN32
    const std::wstring normalizedExtension = asciiToWide(ProjectFile::normalizeExtension(extension));
    const std::wstring subKey = std::wstring(CLASS_ROOT) + normalizedExtension;

    // Drop the extension only while it still points at us, so an association some
    // other installer made in the meantime survives.
    if (currentHandler(ProjectFile::normalizeExtension(extension)) == progId()) {
        const LSTATUS status = RegDeleteKeyW(HKEY_CURRENT_USER, subKey.c_str());
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
            error = describeStatus("cannot remove HKCU\\" + wideToUtf8(subKey), status);
            return false;
        }
    }

    // The ProgID subtree holds DefaultIcon and shell\open\command, so it goes whole.
    const LSTATUS progIdStatus = RegDeleteTreeW(HKEY_CURRENT_USER,
                                                (std::wstring(CLASS_ROOT) + PROGID).c_str());
    if (progIdStatus != ERROR_SUCCESS && progIdStatus != ERROR_FILE_NOT_FOUND) {
        error = describeStatus("cannot remove the Quantum.Project registry keys", progIdStatus);
        return false;
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
#else
    (void)extension;
    error = "registering a project file type is only implemented on Windows";
    return false;
#endif
}

}
