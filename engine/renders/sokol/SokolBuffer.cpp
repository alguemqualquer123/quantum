// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "SokolBuffer.h"
#include "Log.h"
#include "SokolCmdQueue.h"
#include "Engine.h"

#include <unordered_map>

using namespace quantum;

namespace {

    struct SharedEntry {
        sg_buffer buffer;
        sg_view view;
        unsigned int refs = 0;
    };

    struct SharedKeyHash {
        size_t operator()(const SharedBufferKey& k) const {
            // Both halves are already avalanche-mixed, so folding them is enough for a bucket.
            return static_cast<size_t>(k.lo ^ (k.hi * 0x9E3779B97F4A7C15ull));
        }
    };

    // One registry for the process: a mesh copied between components, or a thousand copies of
    // one model, all land on the same entry and therefore on the same GPU storage.
    std::unordered_map<SharedBufferKey, SharedEntry, SharedKeyHash>& sharedBuffers(){
        static std::unordered_map<SharedBufferKey, SharedEntry, SharedKeyHash> map;
        return map;
    }

    // Two independent 64-bit walks over the same bytes, so the key is 128 bits and a collision
    // would need an order of magnitude more buffers than a scene can hold.
    SharedBufferKey hashContent(const void* data, size_t size, BufferType type){
        const unsigned char* bytes = static_cast<const unsigned char*>(data);

        uint64_t lo = 1469598103934665603ull;
        uint64_t hi = 0x9E3779B97F4A7C15ull;
        for (size_t i = 0; i < size; i++){
            lo = (lo ^ bytes[i]) * 1099511628211ull;
            hi = (hi + bytes[i]) * 0x100000001B3ull;
            hi ^= hi >> 29;
        }

        SharedBufferKey key;
        key.size = static_cast<uint32_t>(size);
        key.type = static_cast<uint32_t>(type);
        // Length and type stay in the key so an empty buffer of each kind remains distinct.
        key.lo = lo ^ (uint64_t(key.size) * 0xD6E8FEB86659FD93ull);
        key.hi = hi ^ (uint64_t(key.type) << 32) ^ 0xA0761D6478BD642Full;
        return key;
    }

    // Take the reference a copy of a borrowing SokolBuffer owes the cache. False when the entry
    // is gone, which leaves the copy holding an id nobody can free rather than registering a
    // phantom that a later lookup would hand out as a live buffer.
    bool retainShared(const SharedBufferKey& key){
        auto it = sharedBuffers().find(key);
        if (it == sharedBuffers().end() || it->second.refs == 0){
            return false;
        }
        ++it->second.refs;
        return true;
    }

}

SokolBuffer::SokolBuffer(){
    buffer.id = SG_INVALID_ID;
    view.id = SG_INVALID_ID;
}

SokolBuffer::SokolBuffer(const SokolBuffer& rhs): buffer(rhs.buffer), view(rhs.view), createdUsage(rhs.createdUsage),
                                                  sharedKey(rhs.sharedKey), shared(rhs.shared){
    // The copy holds the same GPU storage, so it owes the cache a reference of its own;
    // without this the first of the two to be released would free what the other still draws.
    if (shared && !retainShared(sharedKey)){
        shared = false;
        sharedKey = SharedBufferKey();
    }
}

SokolBuffer& SokolBuffer::operator=(const SokolBuffer& rhs){
    if (this == &rhs){
        return *this;
    }

    releaseShared();

    buffer = rhs.buffer;
    view = rhs.view;
    createdUsage = rhs.createdUsage;
    sharedKey = rhs.sharedKey;
    shared = rhs.shared;

    if (shared && !retainShared(sharedKey)){
        shared = false;
        sharedKey = SharedBufferKey();
    }

    return *this;
}

void SokolBuffer::releaseShared(){
    if (!shared){
        return;
    }
    shared = false;
    buffer.id = SG_INVALID_ID;
    view.id = SG_INVALID_ID;

    auto it = sharedBuffers().find(sharedKey);
    sharedKey = SharedBufferKey();
    if (it == sharedBuffers().end() || it->second.refs == 0){
        return;
    }

    if (--it->second.refs > 0){
        return;
    }

    const sg_buffer dead = it->second.buffer;
    const sg_view deadView = it->second.view;
    sharedBuffers().erase(it);

    if (deadView.id != SG_INVALID_ID && sg_isvalid()){
        sg_destroy_view(deadView);
    }
    if (dead.id != SG_INVALID_ID && sg_isvalid()){
        sg_destroy_buffer(dead);
    }
}

bool SokolBuffer::createBuffer(unsigned int size, void* data, BufferType type, BufferUsage usage){
    // A buffer this instance already borrowed is handed back before a new one is taken, or the
    // cache would keep a reference nothing draws with.
    releaseShared();

    // Only an IMMUTABLE buffer can stand in for another: it is never updated after creation, so
    // equal bytes means the same storage serves both users for as long as both live. The async
    // path is skipped because there the id sokol returns is only reserved, not yet valid.
    const bool shareable = usage == BufferUsage::IMMUTABLE && data != nullptr && size > 0 &&
                           !Engine::isAsyncThread() && sg_isvalid();

    SharedBufferKey key;
    if (shareable){
        key = hashContent(data, size, type);

        auto it = sharedBuffers().find(key);
        if (it != sharedBuffers().end() && it->second.refs > 0 && it->second.buffer.id != SG_INVALID_ID){
            buffer = it->second.buffer;
            view = it->second.view;
            createdUsage = usage;
            ++it->second.refs;
            sharedKey = key;
            shared = true;
            return true;
        }
        if (it != sharedBuffers().end()){
            sharedBuffers().erase(it);
        }
    }

    sg_buffer_desc vbuf_desc = {0};

    createdUsage = usage;

    if (usage == BufferUsage::IMMUTABLE){
        vbuf_desc.size = (size_t)size;
        vbuf_desc.data = {data, (size_t)size};
    }else{
        vbuf_desc.size = (size_t)size;
    }

    // TODO: Multi-purpose buffer objects
    // https://floooh.github.io/2025-05-19/sokol-gfx-compute-ms2.html
    if (type == BufferType::VERTEX_BUFFER){
        vbuf_desc.usage.vertex_buffer = true;
    } else if (type == BufferType::INDEX_BUFFER){
        vbuf_desc.usage.index_buffer = true;
    } else if (type == BufferType::STORAGE_BUFFER){
        vbuf_desc.usage.storage_buffer = true;
    }

    if (usage == BufferUsage::IMMUTABLE){
        vbuf_desc.usage.immutable = true;
    }else if (usage == BufferUsage::DYNAMIC){
        vbuf_desc.usage.dynamic_update = true;
    }else if (usage == BufferUsage::STREAM){
        vbuf_desc.usage.stream_update = true;
    }

    if (Engine::isAsyncThread()){
        buffer = SokolCmdQueue::add_command_make_buffer(vbuf_desc);
    }else{
        buffer = sg_make_buffer(vbuf_desc);
    }

    if (type == BufferType::STORAGE_BUFFER && buffer.id != SG_INVALID_ID){
        sg_view_desc view_desc = {0};
        view_desc.storage_buffer.buffer = buffer;

        if (Engine::isAsyncThread()){
            view = SokolCmdQueue::add_command_make_view(view_desc);
        }else{
            view = sg_make_view(view_desc);
        }
    }

    // cannot two updates in same draw loop
    //if (usage != BufferUsage::IMMUTABLE){
    //    updateBuffer(size, data);
    //}

    if (buffer.id != SG_INVALID_ID){
        if (shareable){
            // The first user registers what it just made; the ones after it borrow it.
            SharedEntry& entry = sharedBuffers()[key];
            entry.buffer = buffer;
            entry.view = view;
            entry.refs = 1;
            sharedKey = key;
            shared = true;
        }
        return true;
    }

    return false;
}

// called by draw
void SokolBuffer::updateBuffer(unsigned int size, void* data){
    if (buffer.id != SG_INVALID_ID && data && size > 0){
        if (Engine::isAsyncThread()){
            SokolCmdQueue::add_command_update_buffer(buffer, {data, (size_t)size});
        }else{
            sg_update_buffer(buffer, {data, (size_t)size});
        }
    }
}

void SokolBuffer::destroyBuffer(){
    // Shared storage goes back to the cache, which frees it once nobody borrows it any more.
    if (shared){
        releaseShared();
        return;
    }

    if (view.id != SG_INVALID_ID && sg_isvalid()){
        if (Engine::isAsyncThread()){
            SokolCmdQueue::add_command_destroy_view(view);
        }else{
            sg_destroy_view(view);
        }
    }
    if (buffer.id != SG_INVALID_ID && sg_isvalid()){
        if (Engine::isAsyncThread()){
            SokolCmdQueue::add_command_destroy_buffer(buffer);
        }else{
            sg_destroy_buffer(buffer);
        }
    }

    buffer.id = SG_INVALID_ID;
    view.id = SG_INVALID_ID;
}

bool SokolBuffer::isCreated(){
    if (buffer.id != SG_INVALID_ID && sg_isvalid()) {
        return sg_query_buffer_state(buffer) == SG_RESOURCESTATE_VALID;
    }

    return false;
}

BufferUsage SokolBuffer::getCreatedUsage() const{
    return createdUsage;
}

bool SokolBuffer::isShared() const{
    return shared;
}

size_t SokolBuffer::sharedBufferCount(){
    return sharedBuffers().size();
}

void SokolBuffer::clearSharedBuffers(){
    if (sg_isvalid()){
        for (auto& entry : sharedBuffers()){
            if (entry.second.view.id != SG_INVALID_ID){
                sg_destroy_view(entry.second.view);
            }
            if (entry.second.buffer.id != SG_INVALID_ID){
                sg_destroy_buffer(entry.second.buffer);
            }
        }
    }
    sharedBuffers().clear();
}

sg_buffer SokolBuffer::get(){
    return buffer;
}

sg_view SokolBuffer::getView(){
    return view;
}
