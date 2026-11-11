// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "RotationAction.h"

#include "util/Angle.h"

using namespace quantum;

RotationAction::RotationAction(Scene* scene): TimedAction(scene){
    addComponent<RotationActionComponent>();
}

RotationAction::RotationAction(Scene* scene, Entity entity): TimedAction(scene, entity){

}

void RotationAction::setAction(Quaternion startRotation, Quaternion endRotation, float duration, bool loop){
    TimedAction::setAction(duration, loop);

    RotationActionComponent& rotAction = getComponent<RotationActionComponent>();

    rotAction.startRotation = startRotation;
    rotAction.endRotation = endRotation;
    rotAction.spin = false;
}

void RotationAction::setSpinAction(Quaternion startRotation, Vector3 axis, float angle, float duration, bool loop){
    TimedAction::setAction(duration, loop);

    RotationActionComponent& rotAction = getComponent<RotationActionComponent>();

    rotAction.startRotation = startRotation;
    rotAction.spin = true;
    rotAction.axis = axis;
    rotAction.angle = Angle::defaultToDeg(angle);
}