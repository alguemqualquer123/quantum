// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#pragma once

#include <vector>
#include <string>
#include "Backend.h"
#include "imgui.h"
#include "nfd.hpp"
#include "util/Util.h"

namespace quantum::editor{

    enum FileDialogType {
        FILE_DIALOG_ALL = 0,
        FILE_DIALOG_IMAGE = 1 << 0,
        FILE_DIALOG_FONT = 1 << 1,
        FILE_DIALOG_MODEL = 1 << 2,
        FILE_DIALOG_AUDIO = 1 << 3,
        FILE_DIALOG_SHADER = 1 << 4,
        FILE_DIALOG_SCRIPT = 1 << 5,
        FILE_DIALOG_BUNDLE = 1 << 6,
        FILE_DIALOG_MATERIAL = 1 << 7
    };

    class FileDialogs{

    public:
        inline static std::string openFileDialog(std::string defaultPath = "", int filterFlags = FILE_DIALOG_ALL, bool selectDirectory = false) {
            std::string retPath;

            if (selectDirectory) {
                // Directory selection
                nfdu8char_t* outPath;
                nfdpickfolderu8args_t args = {0};
                args.defaultPath = defaultPath.c_str();
                args.parentWindow = *static_cast<nfdwindowhandle_t*>(Backend::getNFDWindowHandle());

                // Use NFD_PickFolderU8_With for consistency with other functions
                nfdresult_t result = NFD_PickFolderU8_With(&outPath, &args);

                if (result == NFD_OKAY) {
                    retPath = outPath;
                    NFD_FreePathU8(outPath);
                } else if (result == NFD_ERROR) {
                    printf("Error: %s\n", NFD_GetError());
                }
            } else {
                // File selection
                nfdu8char_t* path;
                nfdopendialogu8args_t args = {0};

                std::vector<std::string> extensionStorage;
                std::vector<nfdfilteritem_t> filterItems;
                buildFilterItems(filterFlags, extensionStorage, filterItems);

                if (!filterItems.empty()) {
                    args.filterCount = (nfdfiltersize_t)filterItems.size();
                    args.filterList = filterItems.data();
                }

                args.defaultPath = defaultPath.c_str();
                args.parentWindow = *static_cast<nfdwindowhandle_t*>(Backend::getNFDWindowHandle());

                const nfdresult_t res = NFD_OpenDialogU8_With(&path, &args);
                switch (res) {
                    case NFD_OKAY:
                        retPath = path;
                        NFD_FreePathU8(path);
                        break;
                    case NFD_ERROR:
                        printf("Error: %s", NFD_GetError());
                        break;
                    default:
                        break;
                }
            }

            return retPath;
        }

        inline static std::vector<std::string> openFileDialogMultiple(std::string defaultPath = "", int filterFlags = FILE_DIALOG_ALL){
            std::vector<std::string> filePaths;
            const nfdpathset_t* pathSet;
            nfdopendialogu8args_t args = {0};

            std::vector<std::string> extensionStorage;
            std::vector<nfdfilteritem_t> filterItems;
            buildFilterItems(filterFlags, extensionStorage, filterItems);

            if (!filterItems.empty()) {
                args.filterCount = (nfdfiltersize_t)filterItems.size();
                args.filterList = filterItems.data();
            }

            args.defaultPath = defaultPath.c_str();
            args.parentWindow = *static_cast<nfdwindowhandle_t*>(Backend::getNFDWindowHandle());

            const nfdresult_t res = NFD_OpenDialogMultipleU8_With(&pathSet, &args);
            switch (res) {
                case NFD_OKAY:
                    nfdpathsetsize_t num_paths;
                    if (NFD_PathSet_GetCount(pathSet, &num_paths) != NFD_OKAY) {
                        printf("Error: NFD_PathSet_GetCount failed: %s\n", NFD_GetError());
                        break;
                    }
                    nfdpathsetsize_t i;
                    for (i = 0; i != num_paths; ++i) {
                        char* path;
                        if (NFD_PathSet_GetPathU8(pathSet, i, &path) != NFD_OKAY) {
                            printf("Error: NFD_PathSet_GetPathU8 failed: %s\n", NFD_GetError());
                            break;
                        }
                        filePaths.push_back(path);
                        NFD_PathSet_FreePathU8(path);
                    }
                    NFD_PathSet_Free(pathSet);

                    break;
                case NFD_ERROR:
                    printf("Error: %s", NFD_GetError());
                    break;
                default:
                    break;
            }

            return filePaths;
        }

        inline static std::vector<std::string> pickFolderDialogMultiple(){
            std::vector<std::string> folderPaths;
            const nfdpathset_t* pathSet;
            nfdpickfolderu8args_t args = {0};

            args.parentWindow = *static_cast<nfdwindowhandle_t*>(Backend::getNFDWindowHandle());

            const nfdresult_t res = NFD_PickFolderMultipleU8_With(&pathSet, &args);
            switch (res) {
                case NFD_OKAY:
                    nfdpathsetsize_t num_paths;
                    if (NFD_PathSet_GetCount(pathSet, &num_paths) != NFD_OKAY) {
                        printf("Error: NFD_PathSet_GetCount failed: %s\n", NFD_GetError());
                        break;
                    }
                    nfdpathsetsize_t i;
                    for (i = 0; i != num_paths; ++i) {
                        char* path;
                        if (NFD_PathSet_GetPathU8(pathSet, i, &path) != NFD_OKAY) {
                            printf("Error: NFD_PathSet_GetPathU8 failed: %s\n", NFD_GetError());
                            break;
                        }
                        folderPaths.push_back(path);
                        NFD_PathSet_FreePathU8(path);
                    }
                    NFD_PathSet_Free(pathSet);

                    break;
                case NFD_ERROR:
                    printf("Error: %s", NFD_GetError());
                    break;
                default:
                    break;
            }

            return folderPaths;
        }

        inline static std::string saveFileDialog(std::string defaultPath = "", std::string defaultName = "", bool sceneOnly = false) {
            std::string retPath;
            char* path;
            nfdsavedialogu8args_t args = {0};

            if (sceneOnly) {
                nfdfilteritem_t filterItem[1] = {
                    { "Scene files", "scene" }
                };
                args.filterCount = 1;
                args.filterList = filterItem;
            }
            args.defaultPath = defaultPath.c_str();
            args.defaultName = defaultName.c_str();  // Set the default filename
            args.parentWindow = *static_cast<nfdwindowhandle_t*>(Backend::getNFDWindowHandle());

            const nfdresult_t res = NFD_SaveDialogU8_With(&path, &args);
            switch (res) {
                case NFD_OKAY:
                    retPath = path;
                    NFD_FreePathU8(path);
                    break;
                case NFD_ERROR:
                    printf("Error: %s", NFD_GetError());
                    break;
                default:
                    break;
            }

            return retPath;
        }

    private:
        inline static void buildFilterItems(int filterFlags, std::vector<std::string>& storage, std::vector<nfdfilteritem_t>& filterItems) {
            // Pointers into storage.back() must stay valid until NFD returns,
            // so reserve before pushing and never resize afterwards.
            storage.reserve(8);

            auto addFilter = [&](int flag, const char* label, const std::string& extensions) {
                if ((filterFlags & flag) == 0) {
                    return;
                }
                storage.push_back(extensions);
                filterItems.push_back({label, storage.back().c_str()});
            };

            addFilter(FILE_DIALOG_IMAGE, "Image files", Util::getImageExtensions());
            addFilter(FILE_DIALOG_FONT, "Font files", Util::getFontExtensions());
            addFilter(FILE_DIALOG_MODEL, "Model files", Util::getModelExtensions());
            addFilter(FILE_DIALOG_AUDIO, "Sound files", Util::getAudioExtensions());
            addFilter(FILE_DIALOG_SHADER, "Shader files", Util::getShaderExtensions());
            addFilter(FILE_DIALOG_SCRIPT, "Script files", Util::getScriptExtensions());
            addFilter(FILE_DIALOG_BUNDLE, "Entity bundles", Util::getBundleExtensions());
            addFilter(FILE_DIALOG_MATERIAL, "Material files", Util::getMaterialExtensions());
        }
    };

}
