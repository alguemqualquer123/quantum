// (c) SR VINIX
// SPDX-License-Identifier: MIT

#include "Decal.h"

#include <algorithm>

using namespace quantum;

Decal::Decal(Scene* scene) : Object(scene) {
    addComponent<DecalComponent>();
}

Decal::Decal(Scene* scene, Entity entity) : Object(scene, entity) {
}

Decal::~Decal() {
}

void Decal::setEnabled(bool value) {
    getComponent<DecalComponent>().enabled = value;
}

bool Decal::isEnabled() const {
    return getComponent<DecalComponent>().enabled;
}

void Decal::setColor(Vector3 value) {
    getComponent<DecalComponent>().color = value;
}

Vector3 Decal::getColor() const {
    return getComponent<DecalComponent>().color;
}

void Decal::setSize(Vector3 value) {
    getComponent<DecalComponent>().size = Vector3(
        std::max(0.001f, value.x), std::max(0.001f, value.y), std::max(0.001f, value.z));
}

Vector3 Decal::getSize() const {
    return getComponent<DecalComponent>().size;
}

void Decal::setOpacity(float value) {
    getComponent<DecalComponent>().opacity = std::clamp(value, 0.0f, 1.0f);
}

float Decal::getOpacity() const {
    return getComponent<DecalComponent>().opacity;
}

void Decal::setDepthFade(float value) {
    getComponent<DecalComponent>().depthFade = std::max(0.0f, value);
}

float Decal::getDepthFade() const {
    return getComponent<DecalComponent>().depthFade;
}

void Decal::setTexture(const std::string& path) {
    getComponent<DecalComponent>().texture.setPath(path);
}

std::string Decal::getTexture() const {
    return getComponent<DecalComponent>().texture.getPath();
}
