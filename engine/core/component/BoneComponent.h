// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef BONE_COMPONENT_H
#define BONE_COMPONENT_H

#include <string>

namespace quantum{

    struct QUANTUM_API BoneComponent{
        Entity model = NULL_ENTITY;

        int index;

        Vector3 bindPosition;
        Quaternion bindRotation;
        Vector3 bindScale;

        Matrix4 offsetMatrix; // inverse bind matrix
    };

}

#endif //BONE_COMPONENT_H