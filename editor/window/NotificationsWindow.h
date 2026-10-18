// (c) Eduardo Doria
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace doriax::editor {
    enum class NotificationLevel {
        Info,
        Success,
        Warning,
        Error
    };

    struct NotificationEntry {
        NotificationLevel level = NotificationLevel::Info;
        std::string title;
        std::string message;
        std::string timestamp;
    };

    class NotificationsWindow {
    private:
        struct Toast {
            NotificationEntry entry;
            float expireTime;
        };

        static constexpr size_t MAX_HISTORY = 500;
        static constexpr size_t MAX_TOASTS = 5;
        static constexpr float TOAST_LIFETIME = 5.0f;

        std::mutex dataMutex;
        std::deque<NotificationEntry> history;      // oldest first, guarded by dataMutex
        std::vector<NotificationEntry> incoming;    // thread-safe hand-off queue, guarded by dataMutex

        std::vector<Toast> toasts;                  // UI thread only

        bool windowOpen;
        bool focusRequested;
        bool hasNotification;

        void drainIncoming();
        void drawHistoryWindow();
        void drawToasts();
    public:
        static constexpr const char* WINDOW_NAME = "Notifications";

        NotificationsWindow();

        // Safe to call from any thread (build/resource threads)
        void push(NotificationLevel level, const std::string& title, const std::string& message);

        void show();
        void clearHistory();
        void setOpen(bool open);
        bool isOpen() const;
    };
}
