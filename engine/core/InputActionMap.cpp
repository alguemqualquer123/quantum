// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "InputActionMap.h"

#include "Input.h"
#include "event/EventBus.h"
#include "io/SaveGame.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <sstream>

namespace quantum {

namespace {

constexpr uint32_t MAX_SAVED_CONTEXTS = 256;
constexpr uint32_t MAX_CONTEXT_NAME_BYTES = 128;
constexpr uint32_t SAVED_CONTEXTS_VERSION = 1;

void appendU32(std::string& output, uint32_t value) {
    output.push_back(static_cast<char>(value & 0xffu));
    output.push_back(static_cast<char>((value >> 8) & 0xffu));
    output.push_back(static_cast<char>((value >> 16) & 0xffu));
    output.push_back(static_cast<char>((value >> 24) & 0xffu));
}

bool readU32(const std::string& input, size_t& offset, uint32_t& value) {
    if (offset > input.size() || input.size() - offset < 4) return false;
    value = static_cast<uint32_t>(static_cast<unsigned char>(input[offset]))
        | (static_cast<uint32_t>(static_cast<unsigned char>(input[offset + 1])) << 8)
        | (static_cast<uint32_t>(static_cast<unsigned char>(input[offset + 2])) << 16)
        | (static_cast<uint32_t>(static_cast<unsigned char>(input[offset + 3])) << 24);
    offset += 4;
    return true;
}

}

InputActionMap::InputActionMap() {
    contexts.emplace("gameplay", Context{});
}

void InputActionMap::addAction(const std::string& action, const std::string& context) {
    if (action.empty() || action.find(',') != std::string::npos) return;
    auto& entry = actions[action];
    entry.context = context.empty() ? "gameplay" : context;
    contexts.try_emplace(entry.context, Context{});
}

void InputActionMap::removeAction(const std::string& action) { actions.erase(action); }
bool InputActionMap::hasAction(const std::string& action) const { return actions.find(action) != actions.end(); }

void InputActionMap::setContext(const std::string& context, bool enabled, int priority) {
    if (context.empty()) return;
    contexts[context] = {enabled, priority};
}

bool InputActionMap::isContextEnabled(const std::string& context) const {
    const auto it = contexts.find(context);
    return it != contexts.end() && it->second.enabled;
}

void InputActionMap::clearBindings(const std::string& action) {
    const auto it = actions.find(action);
    if (it != actions.end()) it->second.bindings.clear();
}

void InputActionMap::bindKey(const std::string& action, int key, float scale, const std::string& context) {
    bindKeyChord(action, key, 0, scale, context);
}

void InputActionMap::bindKeyChord(const std::string& action, int key, int modifiers, float scale,
                                  const std::string& context) {
    addAction(action, context);
    actions[action].bindings.push_back({BindingType::Key, 0, key, modifiers, scale, 0.0f, context});
}

void InputActionMap::bindMouseButton(const std::string& action, int button, float scale, const std::string& context) {
    addAction(action, context);
    actions[action].bindings.push_back({BindingType::MouseButton, 0, button, 0, scale, 0.0f, context});
}

void InputActionMap::bindGamepadButton(const std::string& action, int gamepad, int button, float scale, const std::string& context) {
    addAction(action, context);
    actions[action].bindings.push_back({BindingType::GamepadButton, gamepad, button, 0, scale, 0.0f, context});
}

void InputActionMap::bindGamepadAxis(const std::string& action, int gamepad, int axis, float scale,
                                     float deadZone, const std::string& context) {
    addAction(action, context);
    actions[action].bindings.push_back({BindingType::GamepadAxis, gamepad, axis, 0, scale,
        std::clamp(deadZone, 0.0f, 0.99f), context});
}

void InputActionMap::bindGesture(const std::string& action, int gesture, float scale,
                                 const std::string& context) {
    if (gesture < static_cast<int>(GestureType::Tap) || gesture > static_cast<int>(GestureType::LongPress)) return;
    addAction(action, context);
    actions[action].bindings.push_back({BindingType::Gesture, 0, gesture, 0, scale, 0.0f, context});
}

bool InputActionMap::bindingActive(const Binding& binding) const {
    const auto it = contexts.find(binding.context);
    return it != contexts.end() && it->second.enabled;
}

float InputActionMap::readBinding(const Binding& binding) const {
    if (!bindingActive(binding)) return 0.0f;
    switch (binding.type) {
    case BindingType::Key:
        if ((Input::getModifiers() & binding.modifiers) != binding.modifiers) return 0.0f;
        return Input::isKeyPressed(binding.code) ? binding.scale : 0.0f;
    case BindingType::MouseButton: return Input::isMousePressed(binding.code) ? binding.scale : 0.0f;
    case BindingType::GamepadButton:
        return Input::isGamepadButtonPressed(binding.device, binding.code) ? binding.scale : 0.0f;
    case BindingType::GamepadAxis: {
        const float raw = Input::getGamepadAxis(binding.device, binding.code);
        const float magnitude = std::fabs(raw);
        if (magnitude <= binding.deadZone) return 0.0f;
        const float normalized = (magnitude - binding.deadZone) / (1.0f - binding.deadZone);
        return std::copysign(normalized, raw) * binding.scale;
    }
    case BindingType::Gesture: {
        const auto gesture = gestureValues.find(binding.code);
        return gesture == gestureValues.end() ? 0.0f : gesture->second * binding.scale;
    }
    }
    return 0.0f;
}

void InputActionMap::update() {
    gestureValues.clear();
    for (const GestureEvent& event : Input::consumeGestureEvents()) {
        const int type = static_cast<int>(event.type);
        gestureValues[type] = std::max(gestureValues[type], 1.0f);
    }
    for (auto& pair : actions) {
        Action& action = pair.second;
        action.previous = action.current;
        action.current = 0.0f;
        int selectedPriority = std::numeric_limits<int>::min();
        // An enabled higher-priority context owns the action even while its
        // binding is idle; otherwise lower-priority gameplay input leaks
        // through menus and modal contexts.
        for (const Binding& binding : action.bindings) {
            const auto context = contexts.find(binding.context);
            if (context != contexts.end() && context->second.enabled) {
                selectedPriority = std::max(selectedPriority, context->second.priority);
            }
        }
        if (selectedPriority == std::numeric_limits<int>::min()) continue;
        for (const Binding& binding : action.bindings) {
            const auto context = contexts.find(binding.context);
            if (context == contexts.end() || !context->second.enabled) continue;
            if (context->second.priority != selectedPriority) continue;
            const float candidate = readBinding(binding);
            if (std::fabs(candidate) > std::fabs(action.current)) {
                action.current = candidate;
            }
        }
    }

    // Dispatch only after all actions have sampled the same input frame. This keeps
    // callbacks deterministic and lets a callback safely query another action.
    for (const std::string& actionName : actionNames()) {
        const auto it = actions.find(actionName);
        if (it == actions.end()) continue;
        const Action& action = it->second;
        const bool pressedNow = std::fabs(action.current) > 0.0001f;
        const bool pressedBefore = std::fabs(action.previous) > 0.0001f;
        if (pressedNow == pressedBefore) continue;
        if (pressedNow) onActionPressed.call(actionName, action.current);
        else onActionReleased.call(actionName, action.previous);
    }
}

float InputActionMap::value(const std::string& action) const {
    const auto it = actions.find(action);
    return it == actions.end() ? 0.0f : it->second.current;
}

bool InputActionMap::pressed(const std::string& action) const { return std::fabs(value(action)) > 0.0001f; }

bool InputActionMap::justPressed(const std::string& action) const {
    const auto it = actions.find(action);
    return it != actions.end() && std::fabs(it->second.current) > 0.0001f && std::fabs(it->second.previous) <= 0.0001f;
}

bool InputActionMap::justReleased(const std::string& action) const {
    const auto it = actions.find(action);
    return it != actions.end() && std::fabs(it->second.current) <= 0.0001f && std::fabs(it->second.previous) > 0.0001f;
}

std::vector<std::string> InputActionMap::actionNames() const {
    std::vector<std::string> names;
    names.reserve(actions.size());
    for (const auto& pair : actions) names.push_back(pair.first);
    std::sort(names.begin(), names.end());
    return names;
}

void InputActionMap::publishTransitions(EventBus& eventBus) const {
    for (const std::string& action : actionNames()) {
        const auto it = actions.find(action);
        if (it == actions.end()) continue;
        const Action& state = it->second;
        const bool pressed = std::fabs(state.current) > 0.0001f;
        const bool wasPressed = std::fabs(state.previous) > 0.0001f;
        if (pressed == wasPressed) continue;

        const float value = state.current;
        std::vector<uint8_t> payload(sizeof(float));
        std::memcpy(payload.data(), &value, sizeof(float));
        eventBus.publishQueued("input.action." + action + (pressed ? ".pressed" : ".released"),
            std::move(payload));
    }
}

std::string InputActionMap::encodeBinding(const Binding& binding) {
    const char type = static_cast<char>('0' + static_cast<int>(binding.type));
    std::ostringstream stream;
    stream << type << ':' << binding.device << ':' << binding.code << ':' << binding.modifiers << ':' << binding.scale << ':'
           << binding.deadZone << ':' << binding.context;
    return stream.str();
}

bool InputActionMap::decodeBinding(const std::string& encoded, Binding& binding) {
    std::istringstream stream(encoded);
    std::vector<std::string> fields;
    std::string field;
    while (std::getline(stream, field, ':')) fields.push_back(field);
    if (fields.size() != 6 && fields.size() != 7) return false;
    const auto parseInt = [](const std::string& text, int& result) {
        char* end = nullptr;
        const long value = std::strtol(text.c_str(), &end, 10);
        if (end == text.c_str() || *end != '\0' || value < std::numeric_limits<int>::min()
            || value > std::numeric_limits<int>::max()) return false;
        result = static_cast<int>(value);
        return true;
    };
    const auto parseFloat = [](const std::string& text, float& result) {
        char* end = nullptr;
        result = std::strtof(text.c_str(), &end);
        return end != text.c_str() && *end == '\0' && std::isfinite(result);
    };
    int type = -1;
    if (!parseInt(fields[0], type) || !parseInt(fields[1], binding.device)
        || !parseInt(fields[2], binding.code)) return false;
    const size_t offset = fields.size() == 7 ? 4 : 3;
    binding.modifiers = 0;
    if (fields.size() == 7 && !parseInt(fields[3], binding.modifiers)) return false;
    if (!parseFloat(fields[offset], binding.scale) || !parseFloat(fields[offset + 1], binding.deadZone)) return false;
    binding.context = fields[offset + 2];
    if (type < 0 || type > 4 || binding.context.empty()) return false;
    binding.type = static_cast<BindingType>(type);
    return true;
}

bool InputActionMap::saveBindings(SaveGame& save) const {
    std::string names;
    for (const auto& pair : actions) {
        if (!names.empty()) names += ',';
        names += pair.first;
        std::string encoded;
        for (const Binding& binding : pair.second.bindings) {
            if (!encoded.empty()) encoded += ';';
            encoded += encodeBinding(binding);
        }
        save.setString("input.action." + pair.first, encoded);
    }
    save.setString("input.actions", names);

    std::vector<std::string> contextNames;
    contextNames.reserve(contexts.size());
    for (const auto& pair : contexts) {
        if (pair.first.empty() || pair.first.size() > MAX_CONTEXT_NAME_BYTES) return false;
        contextNames.push_back(pair.first);
    }
    std::sort(contextNames.begin(), contextNames.end());
    if (contextNames.size() > MAX_SAVED_CONTEXTS) return false;
    std::string contextData;
    appendU32(contextData, SAVED_CONTEXTS_VERSION);
    appendU32(contextData, static_cast<uint32_t>(contextNames.size()));
    for (const std::string& name : contextNames) {
        const Context& context = contexts.at(name);
        appendU32(contextData, static_cast<uint32_t>(name.size()));
        contextData.append(name);
        contextData.push_back(context.enabled ? '\x01' : '\x00');
        appendU32(contextData, static_cast<uint32_t>(static_cast<int32_t>(context.priority)));
    }
    save.setString("input.contexts", contextData);
    return true;
}

bool InputActionMap::loadBindings(const SaveGame& save) {
    const std::string names = save.getString("input.actions");
    if (names.empty()) return false;

    const std::string contextData = save.getString("input.contexts");
    if (!contextData.empty()) {
        size_t offset = 0;
        uint32_t contextVersion = 0;
        uint32_t contextCount = 0;
        if (!readU32(contextData, offset, contextVersion) || contextVersion != SAVED_CONTEXTS_VERSION
                || !readU32(contextData, offset, contextCount) || contextCount > MAX_SAVED_CONTEXTS) return false;
        contexts.clear();
        for (uint32_t i = 0; i < contextCount; ++i) {
            uint32_t nameLength = 0;
            if (!readU32(contextData, offset, nameLength) || nameLength == 0
                    || nameLength > MAX_CONTEXT_NAME_BYTES || nameLength > contextData.size() - offset) return false;
            const std::string name = contextData.substr(offset, nameLength);
            offset += nameLength;
            if (offset >= contextData.size()) return false;
            const uint8_t enabled = static_cast<uint8_t>(contextData[offset++]);
            if (enabled > 1) return false;
            uint32_t encodedPriority = 0;
            if (!readU32(contextData, offset, encodedPriority)) return false;
            const int32_t priority = static_cast<int32_t>(encodedPriority);
            contexts[name] = {enabled != 0, static_cast<int>(priority)};
        }
        if (offset != contextData.size()) return false;
    }

    std::istringstream nameStream(names);
    std::string action;
    while (std::getline(nameStream, action, ',')) {
        if (action.empty()) continue;
        addAction(action);
        actions[action].bindings.clear();
        std::istringstream bindingStream(save.getString("input.action." + action));
        std::string encoded;
        while (std::getline(bindingStream, encoded, ';')) {
            Binding binding;
            if (decodeBinding(encoded, binding)) {
                contexts.try_emplace(binding.context, Context{});
                actions[action].bindings.push_back(binding);
            }
        }
    }
    return true;
}

}
