// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace doriax::editor {
    struct PaletteCommand {
        std::string id;
        std::string label;
        std::string description;
        std::string shortcut;
    };

    class CommandPaletteWindow {
    private:
        static constexpr size_t SEARCH_BUFFER_SIZE = 256;
        static constexpr int MAX_RESULTS = 40;

        std::vector<PaletteCommand> customCommands;
        std::vector<PaletteCommand> commands;
        std::vector<int> matches;

        char searchBuffer[SEARCH_BUFFER_SIZE];
        std::string lastQuery;
        int selectedIndex;
        bool visible;
        bool openRequested;
        bool closeRequested;
        bool focusInputRequested;
        bool scrollRequested;
        std::string pendingExecuteId;

        std::function<void(const std::string& actionId)> executeCallback;

        static int fuzzyScore(const char* query, const char* haystack);
        void rebuildCommands();
        void filterMatches();
        void executeSelected();
    public:
        static constexpr const char* PALETTE_NAME = "Command Palette";

        CommandPaletteWindow();

        void setExecuteCallback(std::function<void(const std::string& actionId)> callback);
        void setCustomCommands(std::vector<PaletteCommand> commands);
        void toggle();
        void show();
        bool isVisible() const;
    };
}
