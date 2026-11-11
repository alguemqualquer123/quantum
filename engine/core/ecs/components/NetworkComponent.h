// (c) SR VINIX
// SPDX-License-Identifier: MIT

#ifndef QUANTUM_NETWORK_COMPONENT_H
#define QUANTUM_NETWORK_COMPONENT_H

#include "../../Export.h"
#include "../Component.h"
#include <cstdint>
#include "../Entity.h"

namespace quantum {

// Network replication policy shared by the authoritative server and its clients.
struct QUANTUM_API NetworkComponent : public Component {
    // Unique network ID for this entity (assigned by the server).
    uint32_t netId = 0;
    // If true, this entity's transform (position/rotation/scale) is replicated.
    bool replicateTransform = true;
    // Client/owner identifier allowed to author this entity. 0 means server-owned.
    uint32_t ownerId = 0;
    // Optional interest group. 0 means that the entity is visible to every group.
    uint32_t interestGroup = 0;
    // Optional server-side culling radius in world units. 0 means no radius limit.
    float interestRadius = 0.0f;
    // Server-owned entities reject client-side authoritative state updates.
    bool serverAuthority = true;
};

}

#endif
