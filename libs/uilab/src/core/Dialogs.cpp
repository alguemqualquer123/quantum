#include "core/Dialogs.h"

#include "core/Validation.h"

#include <imgui_stdlib.h>

#include <utility>

namespace uilab {

void DialogManager::Message(const std::string& title, const std::string& message) {
    request_ = DialogRequest();
    request_.open = true;
    request_.kind = DialogKind::Message;
    request_.title = title;
    request_.message = message;
    request_.okLabel = "Close";
    request_.hasCancel = false;
    justOpened_ = true;
}

void DialogManager::Confirm(const std::string& title, const std::string& message, bool destructive,
                            std::function<void(bool)> onClose) {
    request_ = DialogRequest();
    request_.open = true;
    request_.kind = DialogKind::Confirm;
    request_.title = title;
    request_.message = message;
    request_.okLabel = destructive ? "Delete" : "Confirm";
    request_.hasCancel = true;
    request_.destructive = destructive;
    confirmCallback_ = std::move(onClose);
    justOpened_ = true;
}

void DialogManager::TextInput(const std::string& title, const std::string& label, const std::string& initial,
                              const std::string& rule, const std::string& pattern,
                              std::function<void(bool, std::string)> onClose) {
    request_ = DialogRequest();
    request_.open = true;
    request_.kind = DialogKind::TextInput;
    request_.title = title;
    request_.inputLabel = label;
    request_.inputValue = initial;
    request_.inputRule = rule;
    request_.inputPattern = pattern;
    request_.inputHint = ValidationRuleHint(rule);
    request_.hasCancel = true;
    request_.okLabel = "Apply";
    textCallback_ = std::move(onClose);
    justOpened_ = true;
}

void DialogManager::CreateWidgetDialog(const std::string& defaultName, std::vector<std::string> kindChoices,
                                       std::function<void(bool, std::string, std::string, int)> onClose) {
    request_ = DialogRequest();
    request_.open = true;
    request_.kind = DialogKind::CreateWidget;
    request_.title = "Create Widget";
    request_.message = "Add a new component to the registry. The name is validated while you type.";
    request_.hasCancel = true;
    request_.okLabel = "Create";
    request_.kindChoices = std::move(kindChoices);
    request_.kindSelection = 0;
    request_.nameField.label = "Widget name";
    request_.nameField.value = defaultName;
    request_.nameField.rule = "identifier";
    request_.nameField.hint = ValidationRuleHint("identifier");
    request_.tagField.label = "Style class";
    request_.tagField.value = "default";
    request_.tagField.rule = "nonempty";
    request_.tagField.hint = ValidationRuleHint("nonempty");
    createCallback_ = std::move(onClose);
    justOpened_ = true;
}

void DialogManager::ValidateFields() {
    if (request_.kind == DialogKind::TextInput) {
        ValidateText(request_.inputValue, request_.inputRule, request_.inputPattern, request_.inputError);
    } else if (request_.kind == DialogKind::CreateWidget) {
        ValidateText(request_.nameField.value, request_.nameField.rule, request_.nameField.pattern,
                     request_.nameField.error);
        ValidateText(request_.tagField.value, request_.tagField.rule, request_.tagField.pattern,
                     request_.tagField.error);
    }
}

bool DialogManager::Draw(double wallTime) {
    if (!request_.open)
        return false;

    ValidateFields();

    const char* id = "##uilab_modal";
    ImGui::OpenPopup(id);
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        // Popup could not open (eg. viewport not ready) - keep the request queued.
        return false;
    }

    if (justOpened_) {
        ++openCount_;
        lastOpenTime_ = wallTime;
        justOpened_ = false;
        ImGui::SetKeyboardFocusHere(0);
    }

    EventLog* unusedLog = nullptr;
    (void)unusedLog;

    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.15f);
    ImGui::TextUnformatted(request_.title.c_str());
    ImGui::PopFont();
    ImGui::Separator();

    if (!request_.message.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 380.0f);
        ImGui::TextUnformatted(request_.message.c_str());
        ImGui::PopTextWrapPos();
    }

    bool accepted = false;
    bool cancelled = false;
    bool canAccept = true;

    if (request_.kind == DialogKind::TextInput) {
        ImGui::Spacing();
        ImGui::TextUnformatted(request_.inputLabel.c_str());
        ImGui::SetNextItemWidth(380.0f);
        const bool enter = ImGui::InputText("##value", &request_.inputValue,
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        const bool valid = request_.inputError.empty();
        if (!valid) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.38f, 0.34f, 1.00f));
            ImGui::TextWrapped("%s", request_.inputError.c_str());
            ImGui::PopStyleColor();
        } else if (!request_.inputHint.empty()) {
            ImGui::TextDisabled("%s", request_.inputHint.c_str());
        }
        canAccept = valid;
        if (enter && valid)
            accepted = true;
    } else if (request_.kind == DialogKind::CreateWidget) {
        ImGui::Spacing();
        ImGui::SetNextItemWidth(380.0f);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        const bool enter =
            ImGui::InputText(request_.nameField.label.c_str(), &request_.nameField.value,
                             ImGuiInputTextFlags_EnterReturnsTrue);
        if (!request_.nameField.error.empty())
            ImGui::TextWrapped("%s", request_.nameField.error.c_str());
        else
            ImGui::TextDisabled("%s", request_.nameField.hint.c_str());

        ImGui::SetNextItemWidth(380.0f);
        ImGui::InputText(request_.tagField.label.c_str(), &request_.tagField.value);
        if (!request_.tagField.error.empty())
            ImGui::TextWrapped("%s", request_.tagField.error.c_str());
        else
            ImGui::TextDisabled("%s", request_.tagField.hint.c_str());

        ImGui::Spacing();
        ImGui::TextUnformatted("Component kind");
        ImGui::SetNextItemWidth(380.0f);
        if (!request_.kindChoices.empty()) {
            std::vector<const char*> ptrs;
            ptrs.reserve(request_.kindChoices.size());
            for (const std::string& s : request_.kindChoices)
                ptrs.push_back(s.c_str());
            ImGui::Combo("##kind", &request_.kindSelection, ptrs.data(), static_cast<int>(ptrs.size()));
        }
        ImGui::Checkbox("Inherit style from template", &request_.inheritStyle);

        canAccept = request_.nameField.error.empty() && request_.tagField.error.empty();
        if (enter && canAccept)
            accepted = true;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (request_.destructive) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.24f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.84f, 0.30f, 0.28f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.60f, 0.18f, 0.16f, 1.0f));
    }

    ImGui::BeginDisabled(!canAccept);
    if (ImGui::Button(request_.okLabel.c_str(), ImVec2(120.0f, 0.0f)))
        accepted = true;
    ImGui::EndDisabled();

    if (request_.destructive)
        ImGui::PopStyleColor(3);

    if (request_.hasCancel) {
        ImGui::SameLine();
        if (ImGui::Button(request_.cancelLabel.c_str(), ImVec2(120.0f, 0.0f)))
            cancelled = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsAnyItemActive())
            cancelled = true;
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        cancelled = true;
    }

    bool closed = false;
    if (accepted || cancelled) {
        const bool ok = accepted;
        // Capture the payload before the request is reset by the handlers.
        const std::string textValue = request_.inputValue;
        const std::string widgetName = request_.nameField.value;
        const std::string widgetTag = request_.tagField.value;
        const int widgetKind = request_.kindSelection;
        const DialogKind kind = request_.kind;

        if (ok) {
            if (kind == DialogKind::Confirm && confirmCallback_)
                confirmCallback_(true);
            else if (kind == DialogKind::TextInput && textCallback_)
                textCallback_(true, textValue);
            else if (kind == DialogKind::CreateWidget && createCallback_)
                createCallback_(true, widgetName, widgetTag, widgetKind);
        } else {
            if (kind == DialogKind::Confirm && confirmCallback_)
                confirmCallback_(false);
            else if (kind == DialogKind::TextInput && textCallback_)
                textCallback_(false, std::string());
            else if (kind == DialogKind::CreateWidget && createCallback_)
                createCallback_(false, std::string(), std::string(), 0);
        }

        confirmCallback_ = nullptr;
        textCallback_ = nullptr;
        createCallback_ = nullptr;
        request_ = DialogRequest();
        ImGui::CloseCurrentPopup();
        closed = true;
    }

    ImGui::EndPopup();
    return closed;
}

} // namespace uilab
