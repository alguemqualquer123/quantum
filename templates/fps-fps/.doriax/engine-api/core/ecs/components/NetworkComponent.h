#pragma once

#include "../Component.h"
#include <cstdint>
#include "../Entity.h"

namespace doriax {

// Minimal network component used for replication. Stores an identifier that is shared
// across client and server and a flag indicating whether the entity should be
// synchronized.
struct NetworkComponent : public Component {
    // Unique network ID for this entity (assigned by the server).
    uint32_t netId = 0;
    // If true, this entity's transform (position/rotation/scale) is replicated.
    bool replicateTransform = true;
    // Future fields can be added for custom state replication.
};

}
