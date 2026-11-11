// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#include "SokolObject.h"
#include "math/Vector3.h"
#include "math/Vector4.h"
#include "math/Matrix4.h"
#include "Log.h"
#include "SokolCmdQueue.h"
#include "SokolShader.h"
#include "Engine.h"

#include <cstring>
#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

using namespace quantum;

namespace {
    sg_pipeline newPipeline(const sg_pipeline_desc& desc){
        if (Engine::isAsyncThread())
            return SokolCmdQueue::add_command_make_pipeline(desc);
        return sg_make_pipeline(desc);
    }

    void freePipeline(sg_pipeline pipeline){
        if (pipeline.id == SG_INVALID_ID)
            return;
        if (Engine::isAsyncThread()){
            SokolCmdQueue::add_command_destroy_pipeline(pipeline);
        }else{
            sg_destroy_pipeline(pipeline);
        }
    }

    // A pipeline carries fixed-function state and the vertex layout only - the buffers come from
    // sg_apply_bindings at draw time - so every mesh that shares a shader variant, a layout and a
    // blend/depth state draws with the same one. Allocating per submesh instead costs six pipelines
    // each (color, offscreen, depth, shadow depth and the two prepass variants), and sokol's pools
    // are fixed size: 8192 slots cap a scene near 1300 meshes, after which sg_make_pipeline() fails,
    // the mesh never reaches loaded, and RenderSystem::update() reruns the whole build every frame.
    //
    // The desc is folded into a plain uint32 array first: hashing or comparing the desc struct itself
    // would read its padding.
    struct PipelineKey {
        uint32_t shader;
        uint32_t primitiveType;
        uint32_t indexType;
        uint32_t cullMode;
        uint32_t faceWinding;
        uint32_t sampleCount;
        uint32_t colorCount;
        uint32_t compute;
        uint32_t alphaToCoverage;
        uint32_t blendColor[4];
        uint32_t depth[6];
        uint32_t stencil[12];
        uint32_t colors[SG_MAX_COLOR_ATTACHMENTS][9];
        uint32_t buffers[SG_MAX_VERTEXBUFFER_BINDSLOTS][3];
        uint32_t attrs[SG_MAX_VERTEX_ATTRIBUTES][3];
    };

    uint32_t bits(float value){
        uint32_t out = 0;
        std::memcpy(&out, &value, sizeof(out));
        return out;
    }

    PipelineKey makeKey(const sg_pipeline_desc& d){
        PipelineKey k{};
        k.shader = d.shader.id;
        k.primitiveType = (uint32_t) d.primitive_type;
        k.indexType = (uint32_t) d.index_type;
        k.cullMode = (uint32_t) d.cull_mode;
        k.faceWinding = (uint32_t) d.face_winding;
        k.sampleCount = (uint32_t) d.sample_count;
        k.colorCount = (uint32_t) d.color_count;
        k.compute = d.compute ? 1u : 0u;
        k.alphaToCoverage = d.alpha_to_coverage_enabled ? 1u : 0u;
        k.blendColor[0] = bits(d.blend_color.r);
        k.blendColor[1] = bits(d.blend_color.g);
        k.blendColor[2] = bits(d.blend_color.b);
        k.blendColor[3] = bits(d.blend_color.a);
        k.depth[0] = (uint32_t) d.depth.pixel_format;
        k.depth[1] = (uint32_t) d.depth.compare;
        k.depth[2] = d.depth.write_enabled ? 1u : 0u;
        k.depth[3] = bits(d.depth.bias);
        k.depth[4] = bits(d.depth.bias_slope_scale);
        k.depth[5] = bits(d.depth.bias_clamp);
        k.stencil[0] = d.stencil.enabled ? 1u : 0u;
        k.stencil[1] = (uint32_t) d.stencil.front.compare;
        k.stencil[2] = (uint32_t) d.stencil.front.fail_op;
        k.stencil[3] = (uint32_t) d.stencil.front.depth_fail_op;
        k.stencil[4] = (uint32_t) d.stencil.front.pass_op;
        k.stencil[5] = (uint32_t) d.stencil.back.compare;
        k.stencil[6] = (uint32_t) d.stencil.back.fail_op;
        k.stencil[7] = (uint32_t) d.stencil.back.depth_fail_op;
        k.stencil[8] = (uint32_t) d.stencil.back.pass_op;
        k.stencil[9] = d.stencil.read_mask;
        k.stencil[10] = d.stencil.write_mask;
        k.stencil[11] = d.stencil.ref;
        for (int i = 0; i < SG_MAX_COLOR_ATTACHMENTS; i++){
            const sg_color_target_state& c = d.colors[i];
            k.colors[i][0] = (uint32_t) c.pixel_format;
            k.colors[i][1] = (uint32_t) c.write_mask;
            k.colors[i][2] = c.blend.enabled ? 1u : 0u;
            k.colors[i][3] = (uint32_t) c.blend.src_factor_rgb;
            k.colors[i][4] = (uint32_t) c.blend.dst_factor_rgb;
            k.colors[i][5] = (uint32_t) c.blend.op_rgb;
            k.colors[i][6] = (uint32_t) c.blend.src_factor_alpha;
            k.colors[i][7] = (uint32_t) c.blend.dst_factor_alpha;
            k.colors[i][8] = (uint32_t) c.blend.op_alpha;
        }
        for (int i = 0; i < SG_MAX_VERTEXBUFFER_BINDSLOTS; i++){
            k.buffers[i][0] = (uint32_t) d.layout.buffers[i].stride;
            k.buffers[i][1] = (uint32_t) d.layout.buffers[i].step_func;
            k.buffers[i][2] = (uint32_t) d.layout.buffers[i].step_rate;
        }
        for (int i = 0; i < SG_MAX_VERTEX_ATTRIBUTES; i++){
            k.attrs[i][0] = (uint32_t) d.layout.attrs[i].buffer_index;
            k.attrs[i][1] = (uint32_t) d.layout.attrs[i].offset;
            k.attrs[i][2] = (uint32_t) d.layout.attrs[i].format;
        }
        return k;
    }

    uint64_t hashKey(const PipelineKey& key){
        const uint8_t* bytes = (const uint8_t*) &key;
        uint64_t hash = 1469598103934665603ull;
        for (size_t i = 0; i < sizeof(key); i++){
            hash = (hash ^ bytes[i]) * 1099511628211ull;
        }
        return hash;
    }

    struct SharedPipeline {
        PipelineKey key;
        sg_pipeline pipeline;
        uint64_t hash;
        int refs;
    };

    // Loading is threaded (Engine::startAsyncThread), so the cache is reached from the update
    // threads and the main one alike.
    std::mutex sharedMutex;
    std::unordered_map<uint64_t, std::vector<std::unique_ptr<SharedPipeline>>> sharedByHash;
    std::unordered_map<uint32_t, SharedPipeline*> sharedById;

    sg_pipeline acquirePipeline(const sg_pipeline_desc& desc){
        PipelineKey key = makeKey(desc);
        uint64_t hash = hashKey(key);

        std::lock_guard<std::mutex> lock(sharedMutex);

        std::vector<std::unique_ptr<SharedPipeline>>& bucket = sharedByHash[hash];
        for (const std::unique_ptr<SharedPipeline>& entry : bucket){
            if (std::memcmp(&entry->key, &key, sizeof(key)) == 0){
                entry->refs++;
                return entry->pipeline;
            }
        }

        sg_pipeline pipeline = newPipeline(desc);
        if (pipeline.id == SG_INVALID_ID)
            return pipeline;

        std::unique_ptr<SharedPipeline> entry(new SharedPipeline());
        entry->key = key;
        entry->hash = hash;
        entry->pipeline = pipeline;
        entry->refs = 1;
        sharedById[pipeline.id] = entry.get();
        bucket.push_back(std::move(entry));
        return pipeline;
    }

    void refPipeline(sg_pipeline pipeline){
        if (pipeline.id == SG_INVALID_ID)
            return;
        std::lock_guard<std::mutex> lock(sharedMutex);
        std::unordered_map<uint32_t, SharedPipeline*>::iterator found = sharedById.find(pipeline.id);
        if (found != sharedById.end())
            found->second->refs++;
    }

    void releasePipeline(sg_pipeline pipeline){
        if (pipeline.id == SG_INVALID_ID)
            return;

        bool last = false;
        uint64_t hash = 0;
        {
            std::lock_guard<std::mutex> lock(sharedMutex);
            std::unordered_map<uint32_t, SharedPipeline*>::iterator found = sharedById.find(pipeline.id);
            if (found == sharedById.end())
                return;

            last = --found->second->refs == 0;
            hash = found->second->hash;
            if (last){
                sharedById.erase(found);
                std::unordered_map<uint64_t, std::vector<std::unique_ptr<SharedPipeline>>>::iterator bucket = sharedByHash.find(hash);
                std::vector<std::unique_ptr<SharedPipeline>>& entries = bucket->second;
                for (size_t i = 0; i < entries.size(); i++){
                    if (entries[i]->pipeline.id == pipeline.id){
                        entries.erase(entries.begin() + (long) i);
                        break;
                    }
                }
                if (entries.empty())
                    sharedByHash.erase(bucket);
            }
        }

        if (last)
            freePipeline(pipeline);
    }

    // endLoad() keeps calling this name: every pipeline a mesh asks for now goes through the
    // shared cache instead of allocating its own.
    sg_pipeline makePipeline(const sg_pipeline_desc& desc){
        return acquirePipeline(desc);
    }

    // The pixel format of the color attachment of the pass that is open right now, NONE for a
    // depth-only or compute pass. Set by beginPass() below, read by matchPassFormat().
    sg_pixel_format passColorFormat = SG_PIXELFORMAT_NONE;

    struct RetargetKey {
        uint32_t bakedId;
        uint32_t format;

        bool operator==(const RetargetKey& o) const {
            return bakedId == o.bakedId && format == o.format;
        }
    };

    struct RetargetKeyHash {
        size_t operator()(const RetargetKey& k) const {
            return std::hash<uint64_t>()(((uint64_t) k.bakedId << 32) | k.format);
        }
    };

    struct Retargeted {
        sg_pipeline baked;
        sg_pipeline pip;
    };

    // One pipeline is baked for one color format, but the same mesh has to draw into the float
    // HDR scene target and into the RGBA8 mirror, probe and camera targets, and sokol rejects an
    // apply whose pipeline format differs from the open pass. So the alternative format is taken
    // from the shared cache too - N objects sharing a baked pipeline share one retargeted twin.
    // Only the render thread reaches it (beginDraw), which is also why it carries no lock.
    std::unordered_map<RetargetKey, Retargeted, RetargetKeyHash> retargetedByFormat;

    sg_pipeline matchPassFormat(sg_pipeline baked, sg_pixel_format format){
        if (baked.id == SG_INVALID_ID || format == SG_PIXELFORMAT_NONE)
            return baked;

        RetargetKey key{baked.id, (uint32_t) format};
        std::unordered_map<RetargetKey, Retargeted, RetargetKeyHash>::iterator found = retargetedByFormat.find(key);
        if (found != retargetedByFormat.end())
            return found->second.pip;

        // sokol never reuses a resource id, so a baked handle that stopped being valid cannot be
        // asked for again: drop its twin, or it would hold a pool slot for the whole session.
        for (std::unordered_map<RetargetKey, Retargeted, RetargetKeyHash>::iterator it = retargetedByFormat.begin();
                it != retargetedByFormat.end(); ){
            if (sg_query_pipeline_state(it->second.baked) != SG_RESOURCESTATE_VALID){
                releasePipeline(it->second.pip);
                it = retargetedByFormat.erase(it);
            }else{
                ++it;
            }
        }

        sg_pipeline_desc desc = sg_query_pipeline_desc(baked);
        // multi-attachment layouts (the G-buffer) pick a format per slot on purpose, and a pass
        // that opens with a different number of attachments has to fail loudly instead of here
        if (desc.color_count != 1 || desc.colors[0].pixel_format == format)
            return baked;

        desc.colors[0].pixel_format = format;
        sg_pipeline pip = acquirePipeline(desc);
        if (pip.id == SG_INVALID_ID)
            return baked;

        retargetedByFormat.emplace(key, Retargeted{baked, pip});
        return pip;
    }
}

SokolObject::SokolObject(){
    pip.id = SG_INVALID_ID;
    depth_pip.id = SG_INVALID_ID;
    shadow_depth_pip.id = SG_INVALID_ID;
    rtt_pip.id = SG_INVALID_ID;
    rtt_invert_pip.id = SG_INVALID_ID;
    gbuffer_pip.id = SG_INVALID_ID;
    nodepth_pip.id = SG_INVALID_ID;
    rtt_nodepth_pip.id = SG_INVALID_ID;
    rtt_noblend_pip.id = SG_INVALID_ID;
    zprepass_pip.id = SG_INVALID_ID;
    zprepass_rtt_pip.id = SG_INVALID_ID;
    rttColorFormat = SG_PIXELFORMAT_RGBA8;
    bind = {};
    loadIndexBuffer.id = SG_INVALID_ID;
    loadIndexOffset = 0;
    pipeline_desc = {};
    bindSlotIndex = 0;
}

SokolObject::SokolObject(const SokolObject& rhs) {
    bind = rhs.bind;
    loadIndexBuffer = rhs.loadIndexBuffer;
    loadIndexOffset = rhs.loadIndexOffset;
    pip = rhs.pip;
    depth_pip = rhs.depth_pip;
    shadow_depth_pip = rhs.shadow_depth_pip;
    rtt_pip = rhs.rtt_pip;
    rtt_invert_pip = rhs.rtt_invert_pip;
    gbuffer_pip = rhs.gbuffer_pip;
    nodepth_pip = rhs.nodepth_pip;
    rtt_nodepth_pip = rhs.rtt_nodepth_pip;
    rtt_noblend_pip = rhs.rtt_noblend_pip;
    zprepass_pip = rhs.zprepass_pip;
    zprepass_rtt_pip = rhs.zprepass_rtt_pip;
    rttColorFormat = rhs.rttColorFormat;
    pipeline_desc = rhs.pipeline_desc;
    bindSlotIndex = rhs.bindSlotIndex;
    bufferToBindSlot = rhs.bufferToBindSlot;

    // the handles are shared, so a copy is a user of them
    retainAll();
}

SokolObject& SokolObject::operator=(const SokolObject& rhs) {
    if (this == &rhs)
        return *this;

    rhs.retainAll();
    // give back whatever this object held before the copy, or those references are lost
    destroy();

    bind = rhs.bind;
    loadIndexBuffer = rhs.loadIndexBuffer;
    loadIndexOffset = rhs.loadIndexOffset;
    pip = rhs.pip;
    depth_pip = rhs.depth_pip;
    shadow_depth_pip = rhs.shadow_depth_pip;
    rtt_pip = rhs.rtt_pip;
    rtt_invert_pip = rhs.rtt_invert_pip;
    gbuffer_pip = rhs.gbuffer_pip;
    nodepth_pip = rhs.nodepth_pip;
    rtt_nodepth_pip = rhs.rtt_nodepth_pip;
    rtt_noblend_pip = rhs.rtt_noblend_pip;
    zprepass_pip = rhs.zprepass_pip;
    zprepass_rtt_pip = rhs.zprepass_rtt_pip;
    rttColorFormat = rhs.rttColorFormat;
    pipeline_desc = rhs.pipeline_desc;
    bindSlotIndex = rhs.bindSlotIndex;
    bufferToBindSlot = rhs.bufferToBindSlot;
    
    return *this;
}

sg_vertex_format SokolObject::getVertexFormat(unsigned int elements, AttributeDataType dataType, bool normalized){
    if (dataType == AttributeDataType::BYTE){
        if (elements == 4)
            return (normalized)?SG_VERTEXFORMAT_BYTE4N:SG_VERTEXFORMAT_BYTE4;
    }
    if (dataType == AttributeDataType::UNSIGNED_BYTE){
        if (elements == 4)
            return (normalized)?SG_VERTEXFORMAT_UBYTE4N:SG_VERTEXFORMAT_UBYTE4;
    }
    if (dataType == AttributeDataType::SHORT){
        if (elements == 2)
            return (normalized)?SG_VERTEXFORMAT_SHORT2N:SG_VERTEXFORMAT_SHORT2;
        if (elements == 4)
            return (normalized)?SG_VERTEXFORMAT_SHORT4N:SG_VERTEXFORMAT_SHORT4;
    }
    if (dataType == AttributeDataType::UNSIGNED_SHORT){
        if (elements == 2)
            return SG_VERTEXFORMAT_USHORT2N;
        if (elements == 4)
            return SG_VERTEXFORMAT_USHORT4N;
    }
    if (dataType == AttributeDataType::FLOAT){
        if (elements == 1)
            return SG_VERTEXFORMAT_FLOAT;
        if (elements == 2)
            return SG_VERTEXFORMAT_FLOAT2;
        if (elements == 3)
            return SG_VERTEXFORMAT_FLOAT3;
        if (elements == 4)
            return SG_VERTEXFORMAT_FLOAT4;
    }
    return SG_VERTEXFORMAT_INVALID;
}

sg_primitive_type SokolObject::getPrimitiveType(PrimitiveType primitiveType){
    if (primitiveType == PrimitiveType::TRIANGLES)
        return SG_PRIMITIVETYPE_TRIANGLES;
    else if (primitiveType == PrimitiveType::TRIANGLE_STRIP)
        return SG_PRIMITIVETYPE_TRIANGLE_STRIP;
    else if (primitiveType == PrimitiveType::LINES)
        return SG_PRIMITIVETYPE_LINES;
    else if (primitiveType == PrimitiveType::POINTS)
        return SG_PRIMITIVETYPE_POINTS;

    return _SG_PRIMITIVETYPE_DEFAULT;
}

sg_cull_mode SokolObject::getCullMode(CullingMode cullingMode){
    if (cullingMode == CullingMode::BACK){
        return SG_CULLMODE_BACK;
    }else if (cullingMode == CullingMode::FRONT){
        return SG_CULLMODE_FRONT;
    }

    return _SG_CULLMODE_DEFAULT;
}

sg_face_winding SokolObject::getFaceWinding(WindingOrder windingOrder){
    if (windingOrder == WindingOrder::CCW){
        return SG_FACEWINDING_CCW;
    }else if (windingOrder == WindingOrder::CW){
        return SG_FACEWINDING_CW;
    }

    return _SG_FACEWINDING_DEFAULT;
}

void SokolObject::beginLoad(PrimitiveType primitiveType){
    // A retry after a failed endLoad() still holds the pipelines the previous attempt took from
    // the cache; dropping the handles without releasing them would keep those references forever.
    destroy();

    bind = {0};
    pip = {0};
    depth_pip = {0};
    shadow_depth_pip = {0};
    rtt_pip = {0};
    rtt_invert_pip = {0};
    gbuffer_pip = {0};
    pipeline_desc = {0};

    pipeline_desc.primitive_type = getPrimitiveType(primitiveType);

    bindSlotIndex = 0;
    bufferToBindSlot.clear();
    uniformBlockSizes.clear();
    uniformBlockNames.clear();
    uniformBlockScratch.clear();
}

void SokolObject::setShader(ShaderRender* shader){
    pipeline_desc.shader = shader->backend.get();
    uniformBlockSizes.clear();
    uniformBlockNames.clear();
    for (const ShaderStage& stage : shader->shaderData.stages){
        for (const ShaderUniformBlock& block : stage.uniformblocks){
            // Uniform block slots are shared by the stages in sokol's apply
            // call. Keep the largest reflected size when a cross-stage block
            // is represented by more than one stage entry.
            auto it = uniformBlockSizes.find((int)block.slot);
            if (it == uniformBlockSizes.end()){
                uniformBlockSizes[(int)block.slot] = block.sizeBytes;
                uniformBlockNames[(int)block.slot] = block.name;
            }else{
                it->second = std::max(it->second, block.sizeBytes);
            }
        }
    }
}

// Set this before beginLoad(): beginLoad() runs destroy(), which releases the pipelines but keeps
// the declared target format, so one call covers every later reload of the same pass.
void SokolObject::setRenderTargetColorFormat(ColorFormat format){
    if (format == ColorFormat::RGBA16F){
        rttColorFormat = SG_PIXELFORMAT_RGBA16F;
    }else if (format == ColorFormat::R16F){
        rttColorFormat = SG_PIXELFORMAT_R16F;
    }else if (format == ColorFormat::RED16){
        rttColorFormat = SG_PIXELFORMAT_R16;
    }else if (format == ColorFormat::RED){
        rttColorFormat = SG_PIXELFORMAT_R8;
    }else{
        rttColorFormat = SG_PIXELFORMAT_RGBA8;
    }
}

void SokolObject::setIndex(BufferRender* buffer, AttributeDataType dataType, size_t offset){
    sg_buffer ibuf = buffer->backend.get();
    bind.index_buffer = ibuf;
    bind.index_buffer_offset = offset;
    loadIndexBuffer = ibuf;
    loadIndexOffset = offset;

    if (dataType == AttributeDataType::UNSIGNED_SHORT){
        pipeline_desc.index_type = SG_INDEXTYPE_UINT16;
    }else if (dataType == AttributeDataType::UNSIGNED_INT){
        pipeline_desc.index_type = SG_INDEXTYPE_UINT32;
    }else{
        pipeline_desc.index_type = SG_INDEXTYPE_NONE;
    }
}

void SokolObject::addAttribute(int slot, BufferRender* buffer, unsigned int elements, AttributeDataType dataType, unsigned int stride, size_t offset, bool normalized, bool perInstance){
    if (slot != -1){
        sg_buffer vbuf = buffer->backend.get();

        // D3D11 cannot have offset (AlignedByteOffset) bigger than 2048
        // https://github.com/floooh/sokol/issues/818
        // Metal also cannot use large offsets
        size_t bufferOffset = 0;
        size_t attrOffset = 0;

        if (Engine::isOpenGL()){
            bufferOffset = 0;
            attrOffset = offset;
        }else{
            bufferOffset = offset;
            attrOffset = 0;
        }
        
        if (bufferToBindSlot.count({vbuf.id, bufferOffset}) == 0){
            bind.vertex_buffers[bindSlotIndex] = vbuf;
            bind.vertex_buffer_offsets[bindSlotIndex] = bufferOffset;
            bufferToBindSlot[{vbuf.id, bufferOffset}] = bindSlotIndex;

            pipeline_desc.layout.buffers[bindSlotIndex].stride = stride;

            if (perInstance){
                pipeline_desc.layout.buffers[bindSlotIndex].step_func = SG_VERTEXSTEP_PER_INSTANCE;
            }

            bindSlotIndex++;
        }

        size_t indexBuf = bufferToBindSlot[{vbuf.id, bufferOffset}];

        pipeline_desc.layout.attrs[slot].buffer_index = indexBuf;
        pipeline_desc.layout.attrs[slot].offset = attrOffset;
        pipeline_desc.layout.attrs[slot].format = getVertexFormat(elements, dataType, normalized);
    }
}

void SokolObject::replaceVertexBuffer(uint32_t fromBufferId, sg_buffer toBuffer, size_t byteOffset){
    // A single interleaved buffer can occupy several bind slots (one per attribute
    // offset on Metal/D3D), so swap every slot that referenced the original buffer.
    for (auto const& kv : bufferToBindSlot){
        if (kv.first.id == fromBufferId){
            bind.vertex_buffers[kv.second] = toBuffer;
            bind.vertex_buffer_offsets[kv.second] = (int)(kv.first.offset + byteOffset);
        }
    }
}

void SokolObject::setIndexBuffer(sg_buffer buffer){
    bind.index_buffer = buffer;
    bind.index_buffer_offset = 0;
}

void SokolObject::resetIndexBuffer(){
    bind.index_buffer = loadIndexBuffer;
    bind.index_buffer_offset = (int)loadIndexOffset;
}

void SokolObject::addStorageBuffer(int slot, ShaderStageType stage, BufferRender* buffer){
    if (slot != -1){
        sg_view sbufview = buffer->backend.getView();

        if (stage == ShaderStageType::VERTEX){
            bind.views[SOKOL_STORAGEBUFFER_VIEW_SLOT_OFFSET + slot] = sbufview;
        }else if (stage == ShaderStageType::FRAGMENT){
            bind.views[SOKOL_STORAGEBUFFER_VIEW_SLOT_OFFSET + slot] = sbufview;
        }
    }
}

void SokolObject::addTexture(std::pair<int, int> slot, ShaderStageType stage, TextureRender* texture){
    if (slot.first != -1){
        sg_view texview = texture->backend.getView();
        sg_sampler sampler = texture->backend.getSampler();
        if (stage == ShaderStageType::VERTEX){
            bind.views[slot.first] = texview;
            bind.samplers[slot.second] = sampler;
        }else if (stage == ShaderStageType::FRAGMENT){
            bind.views[slot.first] = texview;
            bind.samplers[slot.second] = sampler;
        }
    }
}

bool SokolObject::endLoad(uint16_t pipelines, bool enableFaceCulling, bool enableDepthWrite, CullingMode cullingMode, WindingOrder windingOrder, bool prepassFaceCulling){

    if (pipelines & (int)PipelineType::PIP_DEPTH) {
        sg_pipeline_desc pip_depth_desc = pipeline_desc;

        pip_depth_desc.face_winding = getFaceWinding(windingOrder);
        if (enableFaceCulling){
            pip_depth_desc.cull_mode = getCullMode(cullingMode);
        }

        pip_depth_desc.sample_count = 1;
        pip_depth_desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
        pip_depth_desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
        pip_depth_desc.depth.write_enabled = true;
        pip_depth_desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;

        depth_pip = makePipeline(pip_depth_desc);
        if (depth_pip.id == SG_INVALID_ID){
            return false;
        }
    }

    // Depth prepass: the depth shader writes only depth into the color pass target, so
    // formats and winding follow that target (PIP_DEFAULT or PIP_RTT) rather than PIP_DEPTH.
    if (pipelines & ((int)PipelineType::PIP_ZPREPASS | (int)PipelineType::PIP_ZPREPASS_RTT)) {
        sg_pipeline_desc pip_zprepass_desc = pipeline_desc;

        pip_zprepass_desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
        pip_zprepass_desc.depth.write_enabled = true;
        pip_zprepass_desc.colors[0].write_mask = SG_COLORMASK_NONE;

        if (pipelines & (int)PipelineType::PIP_ZPREPASS){
            pip_zprepass_desc.face_winding = getFaceWinding(windingOrder);
            if (prepassFaceCulling){
                pip_zprepass_desc.cull_mode = getCullMode(cullingMode);
            }
            zprepass_pip = makePipeline(pip_zprepass_desc);
            if (zprepass_pip.id == SG_INVALID_ID){
                return false;
            }
        }

        if (pipelines & (int)PipelineType::PIP_ZPREPASS_RTT){
            pip_zprepass_desc.sample_count = 1;
            pip_zprepass_desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
            pip_zprepass_desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
            WindingOrder rttWinding = windingOrder;
            if (Engine::isOpenGL()){
                rttWinding = (windingOrder == WindingOrder::CCW) ? WindingOrder::CW : WindingOrder::CCW;
            }
            pip_zprepass_desc.face_winding = getFaceWinding(rttWinding);
            if (prepassFaceCulling){
                pip_zprepass_desc.cull_mode = getCullMode(cullingMode);
            }
            zprepass_rtt_pip = makePipeline(pip_zprepass_desc);
            if (zprepass_rtt_pip.id == SG_INVALID_ID){
                return false;
            }
        }
    }

    if (pipelines & (int)PipelineType::PIP_SHADOW_DEPTH) {
        // Depth-only pipeline (no color target); keeps the fragment stage for
        // alpha-mask discard. SG_PIXELFORMAT_NONE marks it depth-only in Sokol.
        sg_pipeline_desc pip_shadow_depth_desc = pipeline_desc;

        pip_shadow_depth_desc.face_winding = getFaceWinding(windingOrder);
        if (enableFaceCulling){
            pip_shadow_depth_desc.cull_mode = getCullMode(cullingMode);
        }

        pip_shadow_depth_desc.sample_count = 1;
        pip_shadow_depth_desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
        pip_shadow_depth_desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
        pip_shadow_depth_desc.depth.write_enabled = true;
        pip_shadow_depth_desc.color_count = 0;
        pip_shadow_depth_desc.colors[0].pixel_format = SG_PIXELFORMAT_NONE;

        shadow_depth_pip = makePipeline(pip_shadow_depth_desc);
        if (shadow_depth_pip.id == SG_INVALID_ID){
            return false;
        }
    }

    if (pipelines & (int)PipelineType::PIP_GBUFFER) {
        // Mesh geometry pass with three color attachments (MRT):
        //   color[0] = packed depth (same as PIP_DEPTH, so SSAO/SSR depth is unchanged)
        //   color[1] = view-space normal (octahedral .rg) + roughness (.b) + metallic (.a)
        //   color[2] = linear base color (.rgb) + hasIBL flag (.a)
        sg_pipeline_desc pip_gbuffer_desc = pipeline_desc;

        pip_gbuffer_desc.face_winding = getFaceWinding(windingOrder);
        if (enableFaceCulling){
            pip_gbuffer_desc.cull_mode = getCullMode(cullingMode);
        }

        pip_gbuffer_desc.sample_count = 1;
        pip_gbuffer_desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
        pip_gbuffer_desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
        pip_gbuffer_desc.depth.write_enabled = true;
        pip_gbuffer_desc.color_count = 3;
        pip_gbuffer_desc.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
        pip_gbuffer_desc.colors[1].pixel_format = SG_PIXELFORMAT_RGBA8;
        pip_gbuffer_desc.colors[2].pixel_format = SG_PIXELFORMAT_RGBA8;

        gbuffer_pip = makePipeline(pip_gbuffer_desc);
        if (gbuffer_pip.id == SG_INVALID_ID){
            return false;
        }
    }

    if (pipelines & ((int)PipelineType::PIP_DEFAULT | (int)PipelineType::PIP_DEFAULT_NODEPTH)) {
        sg_pipeline_desc pip_default_desc = pipeline_desc;

        // also without culling: double-sided materials read gl_FrontFacing
        pip_default_desc.face_winding = getFaceWinding(windingOrder);
        if (enableFaceCulling){
            pip_default_desc.cull_mode = getCullMode(cullingMode);
        }

        pip_default_desc.colors[0].write_mask = SG_COLORMASK_RGB;
        pip_default_desc.colors[0].blend.enabled = true;
        pip_default_desc.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
        pip_default_desc.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

        if (pipelines & (int)PipelineType::PIP_DEFAULT){
            pip_default_desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
            pip_default_desc.depth.write_enabled = enableDepthWrite;

            pip = makePipeline(pip_default_desc);
            if (pip.id == SG_INVALID_ID){
                return false;
            }
        }

        if (pipelines & (int)PipelineType::PIP_DEFAULT_NODEPTH){
            pip_default_desc.depth.compare = SG_COMPAREFUNC_ALWAYS;
            pip_default_desc.depth.write_enabled = false;

            nodepth_pip = makePipeline(pip_default_desc);
            if (nodepth_pip.id == SG_INVALID_ID){
                return false;
            }
        }
    }

    // PIP_RTT (offscreen), PIP_RTT_INVERT (planar reflection), PIP_RTT_NODEPTH and PIP_RTT_NOBLEND
    // share an identical pipeline except for winding, depth and blend state, so build the desc once.
    if (pipelines & ((int)PipelineType::PIP_RTT | (int)PipelineType::PIP_RTT_INVERT |
            (int)PipelineType::PIP_RTT_NODEPTH | (int)PipelineType::PIP_RTT_NOBLEND)){
        sg_pipeline_desc pip_rtt_desc = pipeline_desc;

        pip_rtt_desc.sample_count = 1;
        pip_rtt_desc.depth.write_enabled = enableDepthWrite;
        pip_rtt_desc.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
        pip_rtt_desc.colors[0].write_mask = SG_COLORMASK_RGBA;
        pip_rtt_desc.colors[0].blend.enabled = true;
        pip_rtt_desc.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
        pip_rtt_desc.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        pip_rtt_desc.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
        pip_rtt_desc.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        pip_rtt_desc.depth.pixel_format = SG_PIXELFORMAT_DEPTH;
        pip_rtt_desc.colors[0].pixel_format = rttColorFormat;

        // offscreen passes render with a Y-flipped projection on GL (top-left
        // origin), which reverses the triangle winding
        WindingOrder rttWinding = windingOrder;
        if (Engine::isOpenGL()){
            rttWinding = (windingOrder == WindingOrder::CCW) ? WindingOrder::CW : WindingOrder::CCW;
        }
        pip_rtt_desc.face_winding = getFaceWinding(rttWinding);
        if (enableFaceCulling){
            pip_rtt_desc.cull_mode = getCullMode(cullingMode);
        }

        if (pipelines & (int)PipelineType::PIP_RTT){
            rtt_pip = makePipeline(pip_rtt_desc);
            if (rtt_pip.id == SG_INVALID_ID){
                return false;
            }
        }

        if (pipelines & (int)PipelineType::PIP_RTT_NODEPTH){
            sg_pipeline_desc pip_rtt_nodepth_desc = pip_rtt_desc;
            pip_rtt_nodepth_desc.depth.compare = SG_COMPAREFUNC_ALWAYS;
            pip_rtt_nodepth_desc.depth.write_enabled = false;

            rtt_nodepth_pip = makePipeline(pip_rtt_nodepth_desc);
            if (rtt_nodepth_pip.id == SG_INVALID_ID){
                return false;
            }
        }

        if (pipelines & (int)PipelineType::PIP_RTT_NOBLEND){
            sg_pipeline_desc pip_rtt_noblend_desc = pip_rtt_desc;
            pip_rtt_noblend_desc.colors[0].blend.enabled = false;

            rtt_noblend_pip = makePipeline(pip_rtt_noblend_desc);
            if (rtt_noblend_pip.id == SG_INVALID_ID){
                return false;
            }
        }

        if (pipelines & (int)PipelineType::PIP_RTT_INVERT){
            // planar reflection mirrors handedness, reversing winding once more
            WindingOrder invWinding = (rttWinding == WindingOrder::CCW) ? WindingOrder::CW : WindingOrder::CCW;
            pip_rtt_desc.face_winding = getFaceWinding(invWinding);
            rtt_invert_pip = makePipeline(pip_rtt_desc);
            if (rtt_invert_pip.id == SG_INVALID_ID){
                return false;
            }
        }
    }

    return true;
}

bool SokolObject::beginDraw(PipelineType pipType){
    sg_pipeline selectedPipeline = pip;
    if (pipType == PipelineType::PIP_DEPTH){
        selectedPipeline = depth_pip;
    }else if (pipType == PipelineType::PIP_ZPREPASS){
        selectedPipeline = zprepass_pip;
    }else if (pipType == PipelineType::PIP_ZPREPASS_RTT){
        selectedPipeline = zprepass_rtt_pip;
    }else if (pipType == PipelineType::PIP_SHADOW_DEPTH){
        selectedPipeline = shadow_depth_pip;
    }else if (pipType == PipelineType::PIP_GBUFFER){
        selectedPipeline = gbuffer_pip;
    }else if (pipType == PipelineType::PIP_RTT){
        selectedPipeline = rtt_pip;
    }else if (pipType == PipelineType::PIP_RTT_INVERT){
        selectedPipeline = rtt_invert_pip;
    }else if (pipType == PipelineType::PIP_DEFAULT_NODEPTH){
        selectedPipeline = nodepth_pip;
    }else if (pipType == PipelineType::PIP_RTT_NODEPTH){
        selectedPipeline = rtt_nodepth_pip;
    }else if (pipType == PipelineType::PIP_RTT_NOBLEND){
        selectedPipeline = rtt_noblend_pip;
    }

    // The open pass decides which of the two format variants this object may use: applying one
    // baked for another format aborts the apply in the validation layer and, in a release build,
    // draws into a mismatched framebuffer object instead.
    selectedPipeline = matchPassFormat(selectedPipeline, passColorFormat);

    // Deferred resource creation can leave an allocated handle in FAILED state.
    // Never apply it: doing so would turn one creation error into a cascade of
    // invalid pipeline, uniform, binding, and draw validation failures.
    if (selectedPipeline.id == SG_INVALID_ID ||
        sg_query_pipeline_state(selectedPipeline) != SG_RESOURCESTATE_VALID){
        // A refused apply is not silent anywhere else, and the caller reads it as "reload the
        // mesh", so an unbuilt variant costs a whole scene rebuild every frame with nothing
        // drawn. Name the pass that asked for it - once per pipeline type, per process.
        static unsigned int logged = 0;
        const unsigned int typeBit = (unsigned int)pipType;   // PipelineType is already a bit per pass
        if ((logged & typeBit) == 0){
            logged |= typeBit;
            Log::error("Pipeline of type %d is not usable (id %d, state %d); the pass that asked for it has no shader variant",
                (int)pipType, selectedPipeline.id,
                selectedPipeline.id == SG_INVALID_ID ? -1 : (int)sg_query_pipeline_state(selectedPipeline));
        }
        return false;
    }

    sg_apply_pipeline(selectedPipeline);
    return true;
}

void SokolObject::applyUniformBlock(int slot, unsigned int count, void* data){
    if (slot != -1){
        unsigned int reflectedSize = count;
        auto it = uniformBlockSizes.find(slot);
        const auto nameIt = uniformBlockNames.find(slot);
        const bool customBlock = nameIt != uniformBlockNames.end() &&
            nameIt->second.find("customParams") != std::string::npos;
        if (!customBlock && it != uniformBlockSizes.end() && it->second > 0)
            reflectedSize = it->second;

        if (reflectedSize != count){
            // GLSL std140 reflection includes trailing alignment that is not
            // necessarily present in the C++ struct. Sokol validates the
            // complete reflected block size, so provide deterministic zeroed
            // padding while preserving the caller's authored bytes.
            uniformBlockScratch.assign(reflectedSize, 0);
            if (data && count > 0)
                std::memcpy(uniformBlockScratch.data(), data,
                            std::min((size_t)count, uniformBlockScratch.size()));
            sg_apply_uniforms(slot, {uniformBlockScratch.data(), reflectedSize});
        }else{
            sg_apply_uniforms(slot, {data, count});
        }
    }
}

void SokolObject::draw(unsigned int baseElement, unsigned int vertexCount, unsigned int instanceCount){
    //SokolCmdQueue::add_command_apply_bindings(bind);
    sg_apply_bindings(bind);
    //SokolCmdQueue::add_command_draw(0, vertexCount, 1);
    sg_draw(baseElement, vertexCount, instanceCount);
}

void SokolObject::retainAll() const {
    refPipeline(pip);
    refPipeline(depth_pip);
    refPipeline(shadow_depth_pip);
    refPipeline(rtt_pip);
    refPipeline(rtt_invert_pip);
    refPipeline(gbuffer_pip);
    refPipeline(nodepth_pip);
    refPipeline(rtt_nodepth_pip);
    refPipeline(rtt_noblend_pip);
    refPipeline(zprepass_pip);
    refPipeline(zprepass_rtt_pip);
}

void SokolObject::destroy(){
    if (sg_isvalid()){
        releasePipeline(pip);
        releasePipeline(depth_pip);
        releasePipeline(shadow_depth_pip);
        releasePipeline(rtt_pip);
        releasePipeline(rtt_invert_pip);
        releasePipeline(gbuffer_pip);
        releasePipeline(nodepth_pip);
        releasePipeline(rtt_nodepth_pip);
        releasePipeline(rtt_noblend_pip);
        releasePipeline(zprepass_pip);
        releasePipeline(zprepass_rtt_pip);
    }

    pip.id = SG_INVALID_ID;
    depth_pip.id = SG_INVALID_ID;
    shadow_depth_pip.id = SG_INVALID_ID;
    rtt_pip.id = SG_INVALID_ID;
    rtt_invert_pip.id = SG_INVALID_ID;
    gbuffer_pip.id = SG_INVALID_ID;
    nodepth_pip.id = SG_INVALID_ID;
    rtt_nodepth_pip.id = SG_INVALID_ID;
    rtt_noblend_pip.id = SG_INVALID_ID;
    zprepass_pip.id = SG_INVALID_ID;
    zprepass_rtt_pip.id = SG_INVALID_ID;
    bind = {};
    pipeline_desc = {};
    bindSlotIndex = 0;
}

// The graphics context is going away, so the handles left in the cache are simply forgotten:
// a new sg_setup() reuses the ids, and a stale entry would hand one of them out as a pipeline.
void SokolObject::clearPipelineCache(){
    std::lock_guard<std::mutex> lock(sharedMutex);
    sharedById.clear();
    sharedByHash.clear();
    retargetedByFormat.clear();
}

void SokolObject::beginPass(const sg_pass& pass){
    sg_pixel_format format = SG_PIXELFORMAT_NONE;
    if (pass.attachments.colors[0].id != SG_INVALID_ID){
        sg_view_desc view = sg_query_view_desc(pass.attachments.colors[0]);
        if (view.color_attachment.image.id != SG_INVALID_ID)
            format = sg_query_image_desc(view.color_attachment.image).pixel_format;
    }else if (!pass.compute && !pass.swapchain.invalid){
        // a swapchain pass: sokol fills an unset color_format from the sg_setup() defaults
        format = pass.swapchain.color_format;
        if (format == _SG_PIXELFORMAT_DEFAULT)
            format = sg_query_desc().environment.defaults.color_format;
    }

    // NONE (depth-only, compute) and anything unresolved leave the baked pipelines alone
    passColorFormat = format > SG_PIXELFORMAT_NONE ? format : SG_PIXELFORMAT_NONE;

    sg_begin_pass(pass);
}
