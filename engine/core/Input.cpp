// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT


#include "Input.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#include <Xinput.h>
#pragma comment(lib, "xinput9_1_0.lib")
#endif

using namespace quantum;

std::map<int,bool> Input::keyPressed;
std::map<int,bool> Input::mousePressed;
Vector2 Input::mousePosition;
Vector2 Input::mouseScroll;
std::vector<Touch> Input::touches;
std::vector<Gamepad> Input::gamepads;
bool Input::mousedEntered;
int Input::modifiers;

namespace {
struct TouchStart {
    quantum::Vector2 position;
    std::chrono::steady_clock::time_point time;
};

std::map<int, TouchStart> touchStarts;
std::vector<quantum::GestureEvent> pendingGestures;

void finishTouch(int pointer, const quantum::Vector2& end, float forcedDuration) {
    const auto startIt = touchStarts.find(pointer);
    if (startIt == touchStarts.end()) return;
    const quantum::Vector2 start = startIt->second.position;
    const float duration = forcedDuration >= 0.0f
        ? forcedDuration
        : std::chrono::duration<float>(std::chrono::steady_clock::now() - startIt->second.time).count();
    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    const float distanceSquared = dx * dx + dy * dy;
    quantum::GestureEvent event;
    event.pointer = pointer;
    event.start = start;
    event.end = end;
    event.duration = (std::max)(0.0f, duration);
    if (event.duration >= 0.5f && distanceSquared < 2500.0f) {
        event.type = quantum::GestureType::LongPress;
    } else if (distanceSquared >= 1024.0f) {
        event.type = quantum::GestureType::Swipe;
    } else {
        event.type = quantum::GestureType::Tap;
    }
    pendingGestures.push_back(event);
    touchStarts.erase(startIt);
}
}

void Input::injectKeyState(int key, bool pressed, int mods){
    if (pressed) addKeyPressed(key);
    else releaseKeyPressed(key);
    setModifiers(mods);
}

void Input::injectTouchStart(int pointer, float x, float y){
    addTouch(pointer, x, y);
}

void Input::injectTouchMove(int pointer, float x, float y){
    setTouchPosition(pointer, x, y);
}

void Input::injectTouchEnd(int pointer, float x, float y, float durationSeconds){
    setTouchPosition(pointer, x, y);
    finishTouch(pointer, Vector2(x, y), durationSeconds);
    removeTouch(pointer);
}

std::vector<GestureEvent> Input::consumeGestureEvents(){
    std::vector<GestureEvent> events;
    events.swap(pendingGestures);
    return events;
}

void Input::addKeyPressed(int key){
    keyPressed[key] = true;
}

void Input::releaseKeyPressed(int key){
    keyPressed[key] = false;
}

void Input::addMousePressed(int button){
    mousePressed[button] = true;
}

void Input::releaseMousePressed(int button){
    mousePressed[button] = false;
}

void Input::setMousePosition(float x, float y){
    mousePosition.x = x;
    mousePosition.y = y;
}

void Input::setMouseScroll(float xoffset, float yoffset){
    mouseScroll.x = xoffset;
    mouseScroll.y = yoffset;
}

void Input::addTouch(int pointer, float x, float y){
    if (findTouchIndex(pointer) == -1){
        touches.push_back({pointer, Vector2(x, y)});
        touchStarts[pointer] = {Vector2(x, y), std::chrono::steady_clock::now()};
    }
}

void Input::setTouchPosition(int pointer, float x, float y){
    size_t index = findTouchIndex(pointer);
    if (index >=0 && index < touches.size())
        touches[index].position = Vector2(x, y);
}

void Input::removeTouch(int pointer){
    size_t index = findTouchIndex(pointer);
    if (index != -1){
        finishTouch(pointer, touches[index].position, -1.0f);
        touches.erase (touches.begin()+index);
    }
}

void Input::clearTouches(){
    touches.clear();
    touchStarts.clear();
}

void Input::addGamepad(int id, const std::string& name){
    size_t index = findGamepadIndex(id);
    if (index == -1){
        Gamepad gamepad;
        gamepad.id = id;
        gamepad.name = name;
        // Triggers rest at -1 in the [-1, 1] convention. Platforms seed their own
        // local state to -1 and skip the connect event, so seed the queryable
        // state here too or getGamepadAxis would report a resting trigger as 0.
        gamepad.axisValue[D_GAMEPAD_AXIS_LEFT_TRIGGER] = -1.0f;
        gamepad.axisValue[D_GAMEPAD_AXIS_RIGHT_TRIGGER] = -1.0f;
        gamepads.push_back(gamepad);
    }else{
        gamepads[index].name = name;
    }
}

void Input::removeGamepad(int id){
    size_t index = findGamepadIndex(id);
    if (index != -1)
        gamepads.erase(gamepads.begin()+index);
}

void Input::clearGamepads(){
    gamepads.clear();
}

void Input::addGamepadButtonPressed(int id, int button){
    size_t index = findGamepadIndex(id);
    if (index != -1)
        gamepads[index].buttonPressed[button] = true;
}

void Input::releaseGamepadButtonPressed(int id, int button){
    size_t index = findGamepadIndex(id);
    if (index != -1)
        gamepads[index].buttonPressed[button] = false;
}

void Input::setGamepadAxisValue(int id, int axis, float value){
    size_t index = findGamepadIndex(id);
    if (index != -1)
        gamepads[index].axisValue[axis] = value;
}

void Input::addMouseEntered(){
    mousedEntered = true;
}

void Input::releaseMouseEntered(){
    mousedEntered = false;
}

void Input::setModifiers(int mods){
    modifiers = mods;
}

bool Input::isKeyPressed(int key){
    return keyPressed[key];
}

bool Input::isMousePressed(int button){
    return mousePressed[button];
}

bool Input::isTouch(){
    return (touches.size() > 0);
}

bool Input::isMouseEntered(){
    return mousedEntered;
}

Vector2 Input::getMousePosition(){
    return mousePosition;
}

Vector2 Input::getMouseScroll(){
    return mouseScroll;
}

Vector2 Input::getTouchPosition(int pointer){
    size_t index = findTouchIndex(pointer);
    if (index >=0 && index < touches.size())
        return touches[index].position;
    
    return Vector2(-1, -1); 
}

std::vector<Touch> Input::getTouches(){
    return touches;
}

size_t Input::numTouches(){
    return touches.size();
}

bool Input::isGamepadConnected(int id){
    return (findGamepadIndex(id) != -1);
}

std::string Input::getGamepadName(int id){
    size_t index = findGamepadIndex(id);
    if (index != -1)
        return gamepads[index].name;

    return "";
}

bool Input::isGamepadButtonPressed(int id, int button){
    size_t index = findGamepadIndex(id);
    if (index != -1)
        return gamepads[index].buttonPressed[button];

    return false;
}

float Input::getGamepadAxis(int id, int axis){
    size_t index = findGamepadIndex(id);
    if (index != -1)
        return gamepads[index].axisValue[axis];

    return 0.0f;
}

bool Input::setGamepadVibration(int id, float lowFrequency, float highFrequency){
#if defined(_WIN32)
    if (id < 0 || id > 3) return false;
    XINPUT_VIBRATION vibration{};
    vibration.wLeftMotorSpeed = static_cast<WORD>(std::clamp(lowFrequency, 0.0f, 1.0f) * 65535.0f);
    vibration.wRightMotorSpeed = static_cast<WORD>(std::clamp(highFrequency, 0.0f, 1.0f) * 65535.0f);
    return XInputSetState(static_cast<DWORD>(id), &vibration) == ERROR_SUCCESS;
#else
    (void)id;
    (void)lowFrequency;
    (void)highFrequency;
    return false;
#endif
}

bool Input::stopGamepadVibration(int id){
    return setGamepadVibration(id, 0.0f, 0.0f);
}

std::vector<Gamepad> Input::getGamepads(){
    return gamepads;
}

size_t Input::numGamepads(){
    return gamepads.size();
}

int Input::getGamepadId(size_t index){
    if (index < gamepads.size())
        return gamepads[index].id;

    return -1;
}

int Input::getModifiers(){
    return modifiers;
}

size_t Input::findTouchIndex(int pointer){
    size_t index = -1;
    for(int i = 0; i < touches.size(); i++){
        if (touches[i].pointer == pointer)
            index = i;
    }

    return index;
}

size_t Input::findGamepadIndex(int id){
    size_t index = -1;
    for(int i = 0; i < gamepads.size(); i++){
        if (gamepads[i].id == id)
            index = i;
    }

    return index;
}
