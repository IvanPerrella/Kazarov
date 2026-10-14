#include "motion.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>

#include "shaders/motion.frag.spv.hpp"
#include "shaders/motion.vert.spv.hpp"
#include "shaders/motion_camera.frag.spv.hpp"
#include "shaders/bloom.vert.spv.hpp"

namespace gfx::detail {
namespace {

constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;

struct MotionState {
    bool available = false;
    bool enabled = false;
    bool drawing = false;
    bool captured = false;
    StreamlineCamera camera;
    Image image;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipeline camera_pipeline = VK_NULL_HANDLE;
    VkPipelineLayout camera_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout camera_set_layout = VK_NULL_HANDLE;
    VkDescriptorPool camera_pool = VK_NULL_HANDLE;
    VkDescriptorSet camera_sets[kFramesInFlight] = {};
    VkSampler depth_sampler = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
};

MotionState motion;

struct CameraPush {
    float clip_to_previous[16];
    uint32_t reset;
    uint32_t pad;
    float jitter_ndc[2];
};

VkShaderModule Shader(const uint32_t *words, size_t bytes) {
    VkShaderModuleCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = bytes;
    info.pCode = words;
    VkShaderModule module = VK_NULL_HANDLE;
    Check(vkCreateShaderModule(g.device, &info, nullptr, &module), "vkCreateShaderModule(motion)");
    return module;
}

void CreateCameraPipeline() {
    VkSamplerCreateInfo sampler = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    Check(vkCreateSampler(g.device, &sampler, nullptr, &motion.depth_sampler), "vkCreateSampler(motion depth)");

    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo set_layout = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_layout.bindingCount = 1;
    set_layout.pBindings = &binding;
    Check(vkCreateDescriptorSetLayout(g.device, &set_layout, nullptr, &motion.camera_set_layout),
          "vkCreateDescriptorSetLayout(motion camera)");
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight};
    VkDescriptorPoolCreateInfo pool = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = kFramesInFlight;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &pool_size;
    Check(vkCreateDescriptorPool(g.device, &pool, nullptr, &motion.camera_pool),
          "vkCreateDescriptorPool(motion camera)");
    VkDescriptorSetLayout layouts[kFramesInFlight];
    for (auto &item : layouts) item = motion.camera_set_layout;
    VkDescriptorSetAllocateInfo alloc = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = motion.camera_pool;
    alloc.descriptorSetCount = kFramesInFlight;
    alloc.pSetLayouts = layouts;
    Check(vkAllocateDescriptorSets(g.device, &alloc, motion.camera_sets),
          "vkAllocateDescriptorSets(motion camera)");

    VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(CameraPush)};
    VkPipelineLayoutCreateInfo layout = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &motion.camera_set_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    Check(vkCreatePipelineLayout(g.device, &layout, nullptr, &motion.camera_layout),
          "vkCreatePipelineLayout(motion camera)");

    VkShaderModule vs = Shader(bloom_vert, sizeof(bloom_vert));
    VkShaderModule fs = Shader(motion_camera_frag, sizeof(motion_camera_frag));
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    VkPipelineVertexInputStateCreateInfo vertex = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState color = {};
    color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &color;
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkFormat format = kMotionFormat;
    VkPipelineRenderingCreateInfo rendering = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo pipeline = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.pNext = &rendering;
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &vertex;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &multisample;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = motion.camera_layout;
    Check(vkCreateGraphicsPipelines(g.device, g.pipeline_cache, 1, &pipeline, nullptr, &motion.camera_pipeline),
          "vkCreateGraphicsPipelines(motion camera)");
    vkDestroyShaderModule(g.device, fs, nullptr);
    vkDestroyShaderModule(g.device, vs, nullptr);
}

void UpdateDepthView() {
    if (motion.depth_view != VK_NULL_HANDLE) vkDestroyImageView(g.device, motion.depth_view, nullptr);
    motion.depth_view = VK_NULL_HANDLE;
    if (g.display_depth.image == VK_NULL_HANDLE) return;
    VkFormatProperties properties = {};
    vkGetPhysicalDeviceFormatProperties(g.physical_device, g.display_depth.format, &properties);
    if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0) return;
    VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = g.display_depth.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = g.display_depth.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    Check(vkCreateImageView(g.device, &view, nullptr, &motion.depth_view), "vkCreateImageView(motion depth)");
}

} // namespace

void MotionInit() {
    VkFormatProperties properties = {};
    vkGetPhysicalDeviceFormatProperties(g.physical_device, kMotionFormat, &properties);
    if ((properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0) return;

    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(float) * 4};
    VkPipelineLayoutCreateInfo layout = {};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &g.constants_set_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    Check(vkCreatePipelineLayout(g.device, &layout, nullptr, &motion.layout), "vkCreatePipelineLayout(motion)");

    VkShaderModule vs = Shader(motion_vert, sizeof(motion_vert));
    VkShaderModule fs = Shader(motion_frag, sizeof(motion_frag));
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1] = stages[0];
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;

    VkVertexInputBindingDescription binding = {0, sizeof(Vertex3D), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription position = {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                                   offsetof(Vertex3D, position)};
    VkPipelineVertexInputStateCreateInfo vertex = {};
    vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex.vertexBindingDescriptionCount = 1;
    vertex.pVertexBindingDescriptions = &binding;
    vertex.vertexAttributeDescriptionCount = 1;
    vertex.pVertexAttributeDescriptions = &position;
    VkPipelineInputAssemblyStateCreateInfo assembly = {};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport = {};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster = {};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth = {};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_EQUAL;
    VkPipelineColorBlendAttachmentState color = {};
    color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    VkPipelineColorBlendStateCreateInfo blend = {};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &color;
    VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = states;
    VkFormat color_format = kMotionFormat;
    VkPipelineRenderingCreateInfo rendering = {};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color_format;
    rendering.depthAttachmentFormat = g.depth_format;
    rendering.stencilAttachmentFormat = g.depth_format;
    VkGraphicsPipelineCreateInfo pipeline = {};
    pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline.pNext = &rendering;
    pipeline.stageCount = 2;
    pipeline.pStages = stages;
    pipeline.pVertexInputState = &vertex;
    pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &multisample;
    pipeline.pDepthStencilState = &depth;
    pipeline.pColorBlendState = &blend;
    pipeline.pDynamicState = &dynamic;
    pipeline.layout = motion.layout;
    Check(vkCreateGraphicsPipelines(g.device, g.pipeline_cache, 1, &pipeline, nullptr, &motion.pipeline),
          "vkCreateGraphicsPipelines(motion)");
    vkDestroyShaderModule(g.device, fs, nullptr);
    vkDestroyShaderModule(g.device, vs, nullptr);
    CreateCameraPipeline();
    motion.available = true;
    MotionResize();
}

void MotionDestroyTargets() {
    if (motion.depth_view != VK_NULL_HANDLE) {
        vkDestroyImageView(g.device, motion.depth_view, nullptr);
        motion.depth_view = VK_NULL_HANDLE;
    }
    DestroyImage(motion.image);
    motion.captured = false;
}

void MotionResize() {
    if (!motion.available) return;
    MotionDestroyTargets();
    UpdateDepthView();
    if (g.display_color.width == 0 || g.display_color.height == 0) return;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    motion.image = CreateImage(g.display_color.width, g.display_color.height, 1, kMotionFormat, usage);
}

void MotionShutdown() {
    if (!motion.available) return;
    MotionDestroyTargets();
    vkDestroyPipeline(g.device, motion.camera_pipeline, nullptr);
    vkDestroyPipelineLayout(g.device, motion.camera_layout, nullptr);
    vkDestroyDescriptorPool(g.device, motion.camera_pool, nullptr);
    vkDestroyDescriptorSetLayout(g.device, motion.camera_set_layout, nullptr);
    vkDestroySampler(g.device, motion.depth_sampler, nullptr);
    vkDestroyPipeline(g.device, motion.pipeline, nullptr);
    vkDestroyPipelineLayout(g.device, motion.layout, nullptr);
    motion = {};
}

void MotionSetEnabled(bool enabled) { motion.enabled = enabled; }

void MotionSetCamera(const StreamlineCamera &camera) {
    motion.camera = camera;
    static bool reported = false;
    if (!reported && camera.valid) {
        std::fprintf(stderr, "Motion: camera near %.3f, far %.1f, FOV %.2f; depth reprojection ready\n",
                     camera.near_plane, camera.far_plane, camera.fov_radians);
        reported = true;
    }
}

const StreamlineCamera *MotionCamera() { return motion.camera.valid ? &motion.camera : nullptr; }

bool MotionBegin() {
    motion.captured = false;
    if (!motion.available || !motion.enabled || !g.display_pass) return false;
    if (motion.image.width != g.display_color.width || motion.image.height != g.display_color.height) MotionResize();
    if (motion.image.image == VK_NULL_HANDLE) return false;

    static bool debug_begin = false;
    if (!debug_begin) {
        std::fprintf(stderr, "Motion: begin, depth view %s\n", motion.depth_view ? "ready" : "missing");
        debug_begin = true;
    }

    EndRendering();
    VkCommandBuffer cmd = DrawCommands();
    Transition(cmd, motion.image, {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});
    bool camera_pass = motion.camera.valid && motion.depth_view != VK_NULL_HANDLE;
    if (camera_pass) {
        static bool debug_camera = false;
        if (!debug_camera) {
            std::fprintf(stderr, "Motion: camera pass recording\n");
            debug_camera = true;
        }
        Transition(cmd, g.display_depth, {VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                          VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
        VkDescriptorImageInfo image = {motion.depth_sampler, motion.depth_view,
                                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = motion.camera_sets[g.frame_slot];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(g.device, 1, &write, 0, nullptr);
        VkRenderingAttachmentInfo camera_color = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        camera_color.imageView = motion.image.view;
        camera_color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        camera_color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        camera_color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo camera_render = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        camera_render.renderArea.extent = {motion.image.width, motion.image.height};
        camera_render.layerCount = 1;
        camera_render.colorAttachmentCount = 1;
        camera_render.pColorAttachments = &camera_color;
        vkCmdBeginRendering(cmd, &camera_render);
        VkViewport viewport = {0.0f, 0.0f, static_cast<float>(motion.image.width),
                               static_cast<float>(motion.image.height), 0.0f, 1.0f};
        VkRect2D scissor = {{0, 0}, {motion.image.width, motion.image.height}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, motion.camera_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, motion.camera_layout, 0, 1,
                                &motion.camera_sets[g.frame_slot], 0, nullptr);
        CameraPush push = {};
        for (uint32_t row = 0; row < 4; ++row)
            for (uint32_t column = 0; column < 4; ++column)
                push.clip_to_previous[column * 4 + row] = motion.camera.clip_to_prev_clip[row * 4 + column];
        push.reset = motion.camera.reset ? 1u : 0u;
        push.jitter_ndc[0] = 2.0f * motion.camera.jitter_x / static_cast<float>(motion.image.width);
        push.jitter_ndc[1] = 2.0f * motion.camera.jitter_y / static_cast<float>(motion.image.height);
        vkCmdPushConstants(cmd, motion.camera_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        VkMemoryBarrier2 barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
    }
    Transition(cmd, g.display_depth, {VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                      VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT});
    VkClearValue zero = {};
    VkRenderingAttachmentInfo color = {};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = motion.image.view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = camera_pass ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue = zero;
    VkRenderingAttachmentInfo depth = color;
    depth.imageView = g.display_depth.view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    VkRenderingInfo render = {};
    render.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    render.renderArea.extent = {motion.image.width, motion.image.height};
    render.layerCount = 1;
    render.colorAttachmentCount = 1;
    render.pColorAttachments = &color;
    render.pDepthAttachment = &depth;
    render.pStencilAttachment = &depth;
    vkCmdBeginRendering(cmd, &render);
    VkViewport viewport = {0.0f, 0.0f, static_cast<float>(motion.image.width),
                           static_cast<float>(motion.image.height), 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, {motion.image.width, motion.image.height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, motion.pipeline);
    LogicalMapping mapping = MainMapping(motion.image.width, motion.image.height);
    float xform[4] = {2.0f * mapping.scale_x / mapping.pixel_width,
                      2.0f * mapping.scale_y / mapping.pixel_height, 0.0f, 0.0f};
    vkCmdPushConstants(cmd, motion.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(xform), xform);
    motion.drawing = true;
    static bool debug_mesh = false;
    if (!debug_mesh) {
        std::fprintf(stderr, "Motion: mesh pass recording\n");
        debug_mesh = true;
    }
    return true;
}

void MotionDraw(MeshHandle handle, uint32_t first_index, uint32_t index_count,
                const MeshConstants &constants, const DrawState &state) {
    if (!motion.drawing || constants.motion_valid == 0 || !state.depth_write ||
        (constants.flags & kMeshShadow) != 0 || state.depth_test == DepthTest::Always) return;
    Mesh *mesh = LookupMesh(handle);
    if (mesh == nullptr || first_index + index_count > mesh->index_count) return;
    VkCommandBuffer cmd = DrawCommands();
    TransientSpan span = AllocateTransient(sizeof(MeshConstants), g.properties.limits.minUniformBufferOffsetAlignment);
    std::memcpy(span.data, &constants, sizeof(constants));
    uint32_t offset = static_cast<uint32_t>(span.offset);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, motion.layout, 0, 1,
                            &span.constants_set, 1, &offset);
    VkBuffer buffer = mesh->buffer.buffer;
    VkDeviceSize vertex_offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &vertex_offset);
    vkCmdBindIndexBuffer(cmd, buffer, mesh->vertex_count * sizeof(Vertex3D), VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, index_count, 1, first_index, 0, 0);
}

void MotionEnd() {
    if (!motion.drawing) return;
    VkCommandBuffer cmd = DrawCommands();
    vkCmdEndRendering(cmd);
    ToRest(cmd, motion.image);
    ToRest(cmd, g.display_depth);
    motion.drawing = false;
    motion.captured = true;
    static bool debug_end = false;
    if (!debug_end) {
        std::fprintf(stderr, "Motion: captured\n");
        debug_end = true;
    }
    ResetDrawState();
}

const Image *MotionImage() { return motion.captured ? &motion.image : nullptr; }

VkImageView MotionDepthView() { return motion.depth_view; }

void MotionDiscard() {
    motion.captured = false;
    motion.camera = {};
}

} // namespace gfx::detail
