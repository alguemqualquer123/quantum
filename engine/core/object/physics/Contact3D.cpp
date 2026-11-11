// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "Contact3D.h"

#include "Jolt/Jolt.h"
#include "Jolt/Physics/Body/Body.h"
#include "Jolt/Physics/Collision/ContactListener.h"

#include <algorithm>
#include <cmath>

using namespace quantum;

Contact3D::Contact3D(Scene* scene, const JPH::Body* body1, const JPH::Body* body2, const JPH::ContactManifold* contactManifold, JPH::ContactSettings* contactSettings){
    this->scene = scene;
    this->body1 = body1;
    this->body2 = body2;
    this->contactManifold = contactManifold;
    this->contactSettings = contactSettings;
}

Contact3D::~Contact3D(){

}

Contact3D::Contact3D(const Contact3D& rhs){
    scene = rhs.scene;
    body1 = rhs.body1;
    body2 = rhs.body2;
    contactManifold = rhs.contactManifold;
    contactSettings = rhs.contactSettings;
}

Contact3D& Contact3D::operator=(const Contact3D& rhs){
    scene = rhs.scene;
    body1 = rhs.body1;
    body2 = rhs.body2;
    contactManifold = rhs.contactManifold;
    contactSettings = rhs.contactSettings;
    
    return *this;
}

const JPH::ContactManifold* Contact3D::getJoltContactManifold() const{
    return contactManifold;
}

JPH::ContactSettings* Contact3D::getJoltContactSettings() const{
    return contactSettings;
}

Vector3 Contact3D::getBaseOffset() const{
    return Vector3(contactManifold->mBaseOffset.GetX(), contactManifold->mBaseOffset.GetY(), contactManifold->mBaseOffset.GetZ());
}

Vector3 Contact3D::getWorldSpaceNormal() const{
    return Vector3(contactManifold->mWorldSpaceNormal.GetX(), contactManifold->mWorldSpaceNormal.GetY(), contactManifold->mWorldSpaceNormal.GetZ());
}

float Contact3D::getPenetrationDepth() const{
    return contactManifold->mPenetrationDepth;
}

size_t Contact3D::getShapeIndex1() const{
    return body1->GetShape()->GetSubShapeUserData(contactManifold->mSubShapeID1);
}

size_t Contact3D::getShapeIndex2() const{
    return body2->GetShape()->GetSubShapeUserData(contactManifold->mSubShapeID2);
}

Vector3 Contact3D::getRelativeContactPointsOnA(size_t index) const{
    JPH::Vec3 point = contactManifold->mRelativeContactPointsOn1[index];
    return Vector3(point.GetX(), point.GetY(), point.GetZ());
}

Vector3 Contact3D::getRelativeContactPointsOnB(size_t index) const{
    JPH::Vec3 point = contactManifold->mRelativeContactPointsOn2[index];
    return Vector3(point.GetX(), point.GetY(), point.GetZ());
}

float Contact3D::getCombinedFriction() const{
    return contactSettings->mCombinedFriction;
}

void Contact3D::setCombinedFriction(float combinedFriction){
    contactSettings->mCombinedFriction = combinedFriction;
}

float Contact3D::getCombinedRestitution() const{
    return contactSettings->mCombinedRestitution;
}

void Contact3D::setCombinedRestitution(float combinedRestitution){
    contactSettings->mCombinedRestitution = combinedRestitution;
}

bool Contact3D::isSensor() const{
    return contactSettings->mIsSensor;
}

void Contact3D::setIsSensor(bool sensor){
    contactSettings->mIsSensor = sensor;
}

void Contact3D::applyShapeMaterials(){
    if (!scene || !body1 || !body2 || !contactSettings) return;

    const Entity entity1 = static_cast<Entity>(body1->GetUserData());
    const Entity entity2 = static_cast<Entity>(body2->GetUserData());
    const Body3DComponent* component1 = scene->findComponent<Body3DComponent>(entity1);
    const Body3DComponent* component2 = scene->findComponent<Body3DComponent>(entity2);
    if (!component1 || !component2) return;

    const size_t index1 = getShapeIndex1();
    const size_t index2 = getShapeIndex2();
    if (index1 >= component1->numShapes || index2 >= component2->numShapes) return;

    const Shape3D& shape1 = component1->shapes[index1];
    const Shape3D& shape2 = component2->shapes[index2];
    // Keep the long-standing Body3D::setFriction/setRestitution contract when both shapes use
    // the defaults. A non-default shape value opts this contact into per-shape material mixing.
    const bool customMaterial = std::abs(shape1.friction - 0.6f) > 0.0001f
        || std::abs(shape2.friction - 0.6f) > 0.0001f
        || std::abs(shape1.restitution) > 0.0001f
        || std::abs(shape2.restitution) > 0.0001f;
    if (!customMaterial) return;

    const float friction1 = std::max(0.0f, std::min(1.0f, shape1.friction));
    const float friction2 = std::max(0.0f, std::min(1.0f, shape2.friction));
    const float restitution1 = std::max(0.0f, std::min(1.0f, shape1.restitution));
    const float restitution2 = std::max(0.0f, std::min(1.0f, shape2.restitution));
    contactSettings->mCombinedFriction = std::sqrt(friction1 * friction2);
    contactSettings->mCombinedRestitution = std::max(restitution1, restitution2);
}

