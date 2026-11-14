// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef INPUTACTIONMAP_H
#define INPUTACTIONMAP_H

#include "Export.h"
#include "Input.h"
#include "util/FunctionSubscribe.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace quantum {

class SaveGame;
class EventBus;

class QUANTUM_API InputActionMap {
public:
    InputActionMap();

    // Called on the main thread after update() detects an action transition.
    // The callbacks are runtime-only and are intentionally not part of saved bindings.
    FunctionSubscribe<void(const std::string&, float)> onActionPressed;
    FunctionSubscribe<void(const std::string&, float)> onActionReleased;

    void addAction(const std::string& action, const std::string& context = "gameplay");
    void removeAction(const std::string& action);
    bool hasAction(const std::string& action) const;

    void setContext(const std::string& context, bool enabled, int priority = 0);
    bool isContextEnabled(const std::string& context) const;

    void clearBindings(const std::string& action);
    void bindKey(const std::string& action, int key, float scale = 1.0f, const std::string& context = "gameplay");
    void bindKeyChord(const std::string& action, int key, int modifiers, float scale = 1.0f,
                      const std::string& context = "gameplay");
    void bindMouseButton(const std::string& action, int button, float scale = 1.0f, const std::string& context = "gameplay");
    void bindGamepadButton(const std::string& action, int gamepad, int button, float scale = 1.0f, const std::string& context = "gameplay");
    void bindGamepadAxis(const std::string& action, int gamepad, int axis, float scale = 1.0f,
                         float deadZone = 0.15f, const std::string& context = "gameplay");
    void bindGesture(const std::string& action, int gesture, float scale = 1.0f,
                     const std::string& context = "gameplay");

    void update();
    float value(const std::string& action) const;
    bool pressed(const std::string& action) const;
    bool justPressed(const std::string& action) const;
    bool justReleased(const std::string& action) const;
    std::vector<std::string> actionNames() const;
    void publishTransitions(EventBus& eventBus) const;

    bool saveBindings(SaveGame& save) const;
    bool loadBindings(const SaveGame& save);

private:
    enum class BindingType { Key, MouseButton, GamepadButton, GamepadAxis, Gesture };
    struct Binding {
        BindingType type = BindingType::Key;
        int device = 0;
        int code = 0;
        int modifiers = 0;
        float scale = 1.0f;
        float deadZone = 0.15f;
        std::string context = "gameplay";
    };
    struct Action {
        std::string context = "gameplay";
        std::vector<Binding> bindings;
        float current = 0.0f;
        float previous = 0.0f;
    };
    struct Context {
        bool enabled = true;
        int priority = 0;
    };

    std::unordered_map<std::string, Action> actions;
    std::unordered_map<std::string, Context> contexts;
    std::unordered_map<int, float> gestureValues;

    float readBinding(const Binding& binding) const;
    bool bindingActive(const Binding& binding) const;
    static std::string encodeBinding(const Binding& binding);
    static bool decodeBinding(const std::string& encoded, Binding& binding);
};

}

#endif // INPUTACTIONMAP_H
