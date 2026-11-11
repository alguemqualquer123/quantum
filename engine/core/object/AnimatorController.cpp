// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "AnimatorController.h"
#include "Object.h"
#include "subsystem/ActionSystem.h"

using namespace quantum;

AnimatorController::AnimatorController(Scene* scene): EntityHandle(scene){
    addComponent<AnimatorControllerComponent>();
}

AnimatorController::AnimatorController(Scene* scene, Entity entity): EntityHandle(scene, entity){
    if (scene->findComponent<AnimatorControllerComponent>(entity) == nullptr){
        addComponent<AnimatorControllerComponent>();
    }
}

AnimatorController::~AnimatorController(){

}

Object AnimatorController::getObject() const{
    return Object(scene, entity);
}

void AnimatorController::play(){
    scene->getSystem<ActionSystem>()->animatorPlay(entity);
}

void AnimatorController::stop(){
    scene->getSystem<ActionSystem>()->animatorStop(entity, 0.25f);
}

void AnimatorController::playState(const std::string& name){
    scene->getSystem<ActionSystem>()->animatorPlayState(entity, name, 0.0f);
}

void AnimatorController::playState(const std::string& name, float fadeTime){
    scene->getSystem<ActionSystem>()->animatorPlayState(entity, name, fadeTime);
}

void AnimatorController::setEnabled(bool enabled){
    getComponent<AnimatorControllerComponent>().enabled = enabled;
}

bool AnimatorController::isEnabled() const{
    return getComponent<AnimatorControllerComponent>().enabled;
}

bool AnimatorController::isPlaying() const{
    return getComponent<AnimatorControllerComponent>().isPlaying;
}

std::string AnimatorController::getCurrentStateName() const{
    return getComponent<AnimatorControllerComponent>().getCurrentStateName();
}

float AnimatorController::getCurrentStateTime() const{
    return getComponent<AnimatorControllerComponent>().currentStateTime;
}

void AnimatorController::setFloat(const std::string& name, float value){
    getComponent<AnimatorControllerComponent>().setFloat(name, value);
}

float AnimatorController::getFloat(const std::string& name) const{
    return getComponent<AnimatorControllerComponent>().getFloat(name);
}

void AnimatorController::setInteger(const std::string& name, int value){
    getComponent<AnimatorControllerComponent>().setInteger(name, value);
}

int AnimatorController::getInteger(const std::string& name) const{
    return getComponent<AnimatorControllerComponent>().getInteger(name);
}

void AnimatorController::setBool(const std::string& name, bool value){
    getComponent<AnimatorControllerComponent>().setBool(name, value);
}

bool AnimatorController::getBool(const std::string& name) const{
    return getComponent<AnimatorControllerComponent>().getBool(name);
}

void AnimatorController::setTrigger(const std::string& name){
    getComponent<AnimatorControllerComponent>().setTrigger(name);
}

void AnimatorController::resetTrigger(const std::string& name){
    getComponent<AnimatorControllerComponent>().resetTrigger(name);
}
