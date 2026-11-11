// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef SokolBuffer_h
#define SokolBuffer_h

#include <cstdint>

#include "render/Render.h"
#include "sokol_gfx.h"

namespace quantum{
    // Identity of an immutable buffer's contents, used to hand the same GPU storage to every
    // mesh whose bytes match. Two independent 64-bit hashes over the data, so a collision needs
    // roughly 2^64 buffers rather than the few million a scene can hold.
    struct SharedBufferKey{
        uint64_t lo = 0;
        uint64_t hi = 0;
        uint32_t size = 0;
        uint32_t type = 0;

        bool operator==(const SharedBufferKey& o) const{
            return lo == o.lo && hi == o.hi && size == o.size && type == o.type;
        }
    };

    class QUANTUM_API SokolBuffer{

    private:
        sg_buffer buffer;
        sg_view view; // only created for storage buffers
        // usage the live buffer was created with: a sokol buffer keeps it for life,
        // so a usage change means the buffer has to be recreated
        BufferUsage createdUsage = BufferUsage::IMMUTABLE;
        // Set while this instance borrows the shared entry for `sharedKey`. A thousand copies
        // of one model then hold one vertex and one index buffer, which is what takes the scene
        // mesh count off the fixed-size sokol buffer pool.
        SharedBufferKey sharedKey;
        bool shared = false;

        void releaseShared();
        void adoptShared(const SharedBufferKey& key);

    public:
        SokolBuffer();
        SokolBuffer(const SokolBuffer& rhs);
        SokolBuffer& operator=(const SokolBuffer& rhs);

        bool createBuffer(unsigned int size, void* data, BufferType type, BufferUsage usage);
        void updateBuffer(unsigned int size, void* data);
        void destroyBuffer();
        bool isCreated();
        BufferUsage getCreatedUsage() const;
        // True while this buffer is GPU storage shared with other components.
        bool isShared() const;

        sg_buffer get();
        sg_view getView();

        // Live shared buffers, for the resource monitor and the headless scale probe.
        static size_t sharedBufferCount();
        static void clearSharedBuffers();
    };
}

#endif /* SokolBuffer_h */
