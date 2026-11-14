// (c) SR VINIX and contributors
// SPDX-License-Identifier: MIT

#ifndef sokolobject_h
#define sokolobject_h

#include "render/Render.h"
#include "buffer/Attribute.h"
#include "render/BufferRender.h"
#include "render/ShaderRender.h"
#include "render/TextureRender.h"

#include "sokol_gfx.h"

#include <map>
#include <string>
#include <vector>


namespace quantum{

    struct UniformStageSlot{
        sg_shader_stage stage;
        int slot;
    };

    struct BufferInfo{
        uint32_t id;
        size_t offset;

        bool operator==(const BufferInfo &o) const {
            return id == o.id && offset == o.offset;
        }

        bool operator<(const BufferInfo &o) const {
            return id < o.id || (id == o.id && offset < o.offset);
        }
    };

    class SokolObject{

    private:

        sg_bindings bind;
        sg_buffer loadIndexBuffer;
        size_t loadIndexOffset;

        sg_pipeline pip;
        sg_pipeline depth_pip;
        sg_pipeline shadow_depth_pip;
        sg_pipeline rtt_pip;
        sg_pipeline rtt_invert_pip;
        sg_pipeline gbuffer_pip;
        sg_pipeline nodepth_pip;
        sg_pipeline rtt_nodepth_pip;
        sg_pipeline rtt_noblend_pip;
        sg_pipeline zprepass_pip;
        sg_pipeline zprepass_rtt_pip;

        // The color format the PIP_RTT family is built for. sokol refuses a pipeline whose
        // colors[0].pixel_format differs from the pass attachment, so an offscreen pass that
        // renders into a float target has to say which one before it loads.
        sg_pixel_format rttColorFormat;

        sg_pipeline_desc pipeline_desc;

        size_t bindSlotIndex;

        std::map< BufferInfo, size_t > bufferToBindSlot;
        std::map<int, unsigned int> uniformBlockSizes;
        std::map<int, std::string> uniformBlockNames;
        std::vector<unsigned char> uniformBlockScratch;


        sg_vertex_format getVertexFormat(unsigned int elements, AttributeDataType dataType, bool normalized);
        sg_primitive_type getPrimitiveType(PrimitiveType primitiveType);
        sg_cull_mode getCullMode(CullingMode cullingMode);
        sg_face_winding getFaceWinding(WindingOrder windingOrder);

        // pipelines come from a shared cache, so holding one means holding a reference to it
        void retainAll() const;

    public:

        SokolObject();
        SokolObject(const SokolObject& rhs);
        SokolObject& operator=(const SokolObject& rhs);

        void beginLoad(PrimitiveType primitiveType);
        void setShader(ShaderRender* shader);
        // Which pixel format the offscreen (PIP_RTT) pipeline writes to. Default RGBA8, which is
        // what every pre-existing render target is; the HDR chain calls this for its float levels.
        void setRenderTargetColorFormat(ColorFormat format);
        void setIndex(BufferRender* buffer, AttributeDataType dataType, size_t offset);
        void addAttribute(int slot, BufferRender* buffer, unsigned int elements, AttributeDataType dataType, unsigned int stride, size_t offset, bool normalized, bool perInstance);
        // rebind every vertex bind-slot that was filled from fromBufferId to toBuffer,
        // keeping the same per-slot layout/offset (used to swap a per-view instance
        // buffer in just before draw). Affects only the bindings, not the pipeline.
        void replaceVertexBuffer(uint32_t fromBufferId, sg_buffer toBuffer, size_t byteOffset);
        void setIndexBuffer(sg_buffer buffer);
        void resetIndexBuffer();
        void addStorageBuffer(int slot, ShaderStageType stage, BufferRender* buffer);
        void addTexture(std::pair<int, int> slot, ShaderStageType stage, TextureRender* texture);
        // prepassFaceCulling applies to the PIP_ZPREPASS pipelines, which must cull like the color pass
        bool endLoad(uint16_t pipelines, bool enableFaceCulling, bool enableDepthWrite, CullingMode cullingMode, WindingOrder windingOrder, bool prepassFaceCulling = true);

        bool beginDraw(PipelineType pipType);
        void applyUniformBlock(int slot, unsigned int count, void* data);
        void draw(unsigned int baseElement, unsigned int vertexCount, unsigned int instanceCount);

        void destroy();

        // drop every cached handle: called when the graphics context is torn down
        static void clearPipelineCache();

        // Opens a pass and remembers the pixel format of its first color attachment, so
        // beginDraw() can pick the pipeline variant baked for that format. Every pass that
        // draws engine geometry has to be opened here or its meshes get rejected.
        static void beginPass(const sg_pass& pass);

    };
}
#endif //sokolobject_h
