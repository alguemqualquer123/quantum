// Notifications.h - toast / notification center drawn as an overlay.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace uilab {

enum class ToastKind : int { Info = 0, Success, Warning, Error, Count };

const char* ToastKindName(ToastKind k);

struct Toast {
    uint64_t id = 0;
    ToastKind kind = ToastKind::Info;
    std::string title;
    std::string body;
    float ttl = 4.0f;    // seconds
    float age = 0.0f;
    int frame = 0;
    bool dismissed = false;
};

class NotificationCenter {
public:
    void Push(ToastKind kind, std::string title, std::string body, float ttl = 4.0f);
    void Update(float dt);
    void Clear();
    void Dismiss(uint64_t id);

    // Draws the toast stack in the bottom-right corner of the main viewport.
    // Call once per frame, outside of any window. Returns the number of toasts
    // that were dismissed by the user during this call.
    int Draw(float dt);

    size_t Count() const { return items_.size(); }
    size_t CountKind(ToastKind kind) const;
    const std::vector<Toast>& Items() const { return items_; }

    bool pauseOnHover = true;
    float maxWidth = 340.0f;
    int maxVisible = 5;

private:
    std::vector<Toast> items_;
    uint64_t nextId_ = 1;
    bool hovered_ = false;
};

} // namespace uilab
