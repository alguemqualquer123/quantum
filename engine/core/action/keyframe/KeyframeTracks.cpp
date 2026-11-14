// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "KeyframeTracks.h"

#include "component/KeyframeTracksComponent.h"

using namespace quantum;

KeyframeTracks::KeyframeTracks(Scene* scene): Action(scene){
    addComponent<KeyframeTracksComponent>();
}

KeyframeTracks::KeyframeTracks(Scene* scene, Entity entity): Action(scene, entity){

}

void KeyframeTracks::setTimes(std::vector<float> times){
    KeyframeTracksComponent& keyframe = getComponent<KeyframeTracksComponent>();

    keyframe.times = times;

    // keep per-segment easings aligned (trailing entries would go stale)
    size_t segments = keyframe.times.size() > 1 ? keyframe.times.size() - 1 : 0;
    if (keyframe.easings.size() > segments){
        keyframe.easings.resize(segments);
    }
}

void KeyframeTracks::setEasings(std::vector<EaseType> easings){
    KeyframeTracksComponent& keyframe = getComponent<KeyframeTracksComponent>();

    // CUSTOM has no per-segment function storage; treat it as linear
    for (EaseType& e : easings){
        if (e == EaseType::CUSTOM) e = EaseType::LINEAR;
    }

    keyframe.easings = easings;
}

void KeyframeTracks::setEasing(unsigned int segment, EaseType ease){
    KeyframeTracksComponent& keyframe = getComponent<KeyframeTracksComponent>();

    // CUSTOM has no per-segment function storage; treat it as linear
    if (ease == EaseType::CUSTOM){
        ease = EaseType::LINEAR;
    }

    if (keyframe.easings.size() <= segment){
        keyframe.easings.resize(segment + 1, EaseType::LINEAR);
    }
    keyframe.easings[segment] = ease;
}

void KeyframeTracks::setLoop(bool loop){
    KeyframeTracksComponent& keyframe = getComponent<KeyframeTracksComponent>();

    keyframe.loop = loop;
}

bool KeyframeTracks::isLoop() const{
    KeyframeTracksComponent& keyframe = getComponent<KeyframeTracksComponent>();

    return keyframe.loop;
}

void KeyframeTracks::setRelative(bool relative){
    KeyframeTracksComponent& keyframe = getComponent<KeyframeTracksComponent>();

    keyframe.relative = relative;
}

bool KeyframeTracks::isRelative() const{
    KeyframeTracksComponent& keyframe = getComponent<KeyframeTracksComponent>();

    return keyframe.relative;
}
