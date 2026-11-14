// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef ROTATETRACKS_H
#define ROTATETRACKS_H

#include "KeyframeTracks.h"

namespace quantum{
    class QUANTUM_API RotateTracks: public KeyframeTracks{

    public:
        RotateTracks(Scene* scene);
        RotateTracks(Scene* scene, Entity entity);
        RotateTracks(Scene* scene, std::vector<float> times, std::vector<Quaternion> values);

        void setValues(std::vector<Quaternion> values);
    };
}

#endif //ROTATETRACKS_H