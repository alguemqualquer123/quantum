// Dialogs.h - modal dialog manager (message / confirm / validated text input /
// create-component dialog). One modal at a time, driven by the main loop.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/EventLog.h"

namespace uilab {

enum class DialogKind : int {
    Message = 0,
    Confirm,
    TextInput,
    CreateWidget
};

struct CreateWidgetField {
    std::string label;
    std::string value;
    std::string rule = "nonempty";
    std::string pattern;
    std::string error;
    std::string hint;
};

struct DialogRequest {
    bool open = false;
    DialogKind kind = DialogKind::Message;
    std::string title;
    std::string message;
    std::string okLabel = "OK";
    std::string cancelLabel = "Cancel";
    bool hasCancel = false;
    bool destructive = false;

    // TextInput payload
    std::string inputLabel = "Value";
    std::string inputValue;
    std::string inputRule = "nonempty";
    std::string inputPattern;
    std::string inputHint;
    std::string inputError;

    // CreateWidget payload
    std::string categoryName;
    std::vector<std::string> kindChoices;
    int kindSelection = 0;
    CreateWidgetField nameField;
    CreateWidgetField tagField;
    bool inheritStyle = true;
};

class DialogManager {
public:
    void Message(const std::string& title, const std::string& message);
    void Confirm(const std::string& title, const std::string& message, bool destructive,
                 std::function<void(bool)> onClose);
    void TextInput(const std::string& title, const std::string& label, const std::string& initial,
                   const std::string& rule, const std::string& pattern,
                   std::function<void(bool, std::string)> onClose);
    void CreateWidgetDialog(const std::string& defaultName, std::vector<std::string> kindChoices,
                            std::function<void(bool, std::string, std::string, int)> onClose);

    bool IsOpen() const { return request_.open; }
    const DialogRequest& Request() const { return request_; }
    DialogKind CurrentKind() const { return request_.kind; }

    // Draws the modal. Returns true when a dialog was closed this frame.
    bool Draw(double wallTime);

    int OpenCount() const { return openCount_; }
    double LastOpenTime() const { return lastOpenTime_; }

private:
    void ValidateFields();

    DialogRequest request_;
    std::function<void(bool)> confirmCallback_;
    std::function<void(bool, std::string)> textCallback_;
    std::function<void(bool, std::string, std::string, int)> createCallback_;
    int openCount_ = 0;
    double lastOpenTime_ = 0.0;
    bool justOpened_ = false;
};

} // namespace uilab
