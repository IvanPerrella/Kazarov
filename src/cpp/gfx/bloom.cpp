#include "bloom.hpp"

#include <algorithm>
#include <array>
#include "atmosphere.hpp"
#include "motion.hpp"
#include "streamline.hpp"

#include "shaders/bloom.frag.spv.hpp"
#include "shaders/bloom.vert.spv.hpp"

namespace gfx::detail {
namespace {

struct BloomState {
    bool initialized = false;
    bool enabled = false;
    Image bright;
    Image blur;
    Image output;
    Image scene;
    std::array<Image, kFramesInFlight> sr_output;
    std::array<Image, kFramesInFlight> hudless;
    std::array<bool, kFramesInFlight> hudless_valid = {};
    bool scene_captured = false;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::array<std::array<VkDescriptorSet, 4>, kFramesInFlight> sets = {};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::array<VkPipeline, 2> pipelines = {};
};

BloomState bloom;

struct Settings {
    float texel[2];
    int pass;
    float strength;
    int linear_input;
};

VkShaderModule Shader(const uint32_t *words, size_t bytes) {
    VkShaderModuleCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = bytes;
    info.pCode = words;
    VkShaderModule module = VK_NULL_HANDLE;
    Check(vkCreateShaderModule(g.device, &info, nullptr, &module), "vkCreateShaderModule(bloom)");
    return module;
}

void DestroyTargets() {
    for (Image &image : bloom.sr_output) DestroyImage(image);
    for (Image &image : bloom.hudless) DestroyImage(image);
    DestroyImage(bloom.scene);
    DestroyImage(bloom.output);
    DestroyImage(bloom.blur);
    DestroyImage(bloom.bright);
}

StreamlineImage StreamlineView(const Image &image, VkImageView view = VK_NULL_HANDLE) {
    StreamlineImage result;
    result.image = image.image;
    result.view = view == VK_NULL_HANDLE ? image.view : view;
    result.memory = image.memory.memory;
    result.layout = image.state.layout;
    result.format = image.format;
    result.usage = image.usage;
    result.width = image.width;
    result.height = image.height;
    return result;
}

Image &SuperResolvedScene(VkCommandBuffer cmd, Image &source) {
    const StreamlineCamera *camera = MotionCamera();
    const Image *motion = MotionImage();
    Image &output = bloom.sr_output[g.frame_slot];
    if (g.config.dlss_quality == DlssQuality::Off || !StreamlineSupportsSuperResolution() || !camera || !motion ||
        output.image == VK_NULL_HANDLE || MotionDepthView() == VK_NULL_HANDLE ||
        source.width != g.display_depth.width || source.height != g.display_depth.height)
        return source;

    ToRest(cmd, source);
    ToRest(cmd, *const_cast<Image *>(motion));
    Transition(cmd, g.display_depth, {VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
    Transition(cmd, output, {VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
    StreamlineFrame frame;
    frame.index = g.streamline_frame_serial;
    frame.dlss_quality = g.config.dlss_quality;
    frame.command_buffer = cmd;
    frame.camera = *camera;
    frame.scene_color = StreamlineView(source);
    frame.output_color = StreamlineView(output);
    frame.depth = StreamlineView(g.display_depth, MotionDepthView());
    frame.motion = StreamlineView(*motion);
    frame.output_width = output.width;
    frame.output_height = output.height;
    frame.dense_motion = true;
    frame.clean_hudless = true;
    frame.hdr_color = true;
    bool evaluated = StreamlineEvaluateSuperResolution(frame);
    ToRest(cmd, g.display_depth);
    if (!evaluated) return source;
    ToRest(cmd, output);
    return output;
}

void DrawPass(VkCommandBuffer cmd, uint32_t index, Image &source, Image &glow, Image &target,
              int pass, float strength) {
    VkDescriptorImageInfo images[2] = {
        {bloom.sampler, source.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {bloom.sampler, glow.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    VkWriteDescriptorSet writes[2] = {};
    VkDescriptorSet set = bloom.sets[g.frame_slot][index];
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(g.device, 2, writes, 0, nullptr);

    Transition(cmd, target, {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});
    VkRenderingAttachmentInfo attachment = {};
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageView = target.view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo render = {};
    render.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    render.renderArea.extent = {target.width, target.height};
    render.layerCount = 1;
    render.colorAttachmentCount = 1;
    render.pColorAttachments = &attachment;
    vkCmdBeginRendering(cmd, &render);

    VkViewport viewport = {0.0f, 0.0f, static_cast<float>(target.width), static_cast<float>(target.height),
                           0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, {target.width, target.height}};
    Settings settings = {{1.0f / source.width, 1.0f / source.height}, pass, strength,
                         source.format == kHdrFormat ? 1 : 0};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloom.pipelines[pass == 3 ? 1 : 0]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bloom.layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, bloom.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(settings), &settings);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    ToRest(cmd, target);
}

} // namespace

void BloomResize() {
    if (!bloom.initialized) {
        return;
    }
    DestroyTargets();
    const uint32_t width = std::max(1u, g.swapchain.extent.width / 2u);
    const uint32_t height = std::max(1u, g.swapchain.extent.height / 2u);
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    bloom.bright = CreateImage(width, height, 1, VK_FORMAT_R16G16B16A16_SFLOAT, usage);
    bloom.blur = CreateImage(width, height, 1, VK_FORMAT_R16G16B16A16_SFLOAT, usage);
    bloom.output = CreateImage(g.swapchain.extent.width, g.swapchain.extent.height, 1, kColorFormat, usage);
    bloom.scene = CreateImage(g.swapchain.extent.width, g.swapchain.extent.height, 1, g.display_color.format,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (g.config.enhanced_hdr && g.config.dlss_quality != DlssQuality::Off &&
        StreamlineSupportsSuperResolution()) {
        for (Image &image : bloom.sr_output) {
            image = CreateImage(g.swapchain.extent.width, g.swapchain.extent.height, 1, kHdrFormat,
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        }
    }
    if (g.config.enhanced_hdr) {
        for (Image &image : bloom.hudless) {
            image = CreateImage(g.swapchain.extent.width, g.swapchain.extent.height, 1, kColorFormat,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        }
    }
    bloom.scene_captured = false;
    bloom.hudless_valid.fill(false);
}

void BloomResetCapture() {
    bloom.scene_captured = false;
    bloom.hudless_valid[g.frame_slot] = false;
    AtmosphereResetEmissives();
}

void BloomCaptureScene() {
    if (!bloom.initialized || g.target != kMainTarget || !g.in_frame ||
        (!bloom.enabled && !(g.display_pass && g.config.enhanced_hdr))) return;
    EndRendering();
    VkCommandBuffer cmd = DrawCommands();
    Image &source = MainColorTarget();
    if (g.display_pass && source.format == kHdrFormat) {
        // Tone-map the linear HDR world before the HUD
        // so text and icons retain their original SDR colours and sharp edges.
        bloom.scene_captured = false;
        Image &fogged = AtmosphereApply(cmd, source, MainDepth());
        AtmosphereCompositeEmissives(fogged, &fogged != &source);
        Image &resolved = SuperResolvedScene(cmd, fogged);
        Image &mapped = BloomApply(cmd, resolved);
        Transition(cmd, mapped, TransferSrc());
        Transition(cmd, g.display_ui, TransferDst());
        VkImageCopy copy = {};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {mapped.width, mapped.height, 1};
        vkCmdCopyImage(cmd, mapped.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       g.display_ui.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Image &hudless = bloom.hudless[g.frame_slot];
        if (hudless.image != VK_NULL_HANDLE) {
            Transition(cmd, hudless, TransferDst());
            vkCmdCopyImage(cmd, mapped.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           hudless.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            ToRest(cmd, hudless);
            bloom.hudless_valid[g.frame_slot] = true;
        }
        ToRest(cmd, mapped);
        ToRest(cmd, g.display_ui);
        // Bloom binds its own set 0, pipeline and dynamic viewport/scissor in this command
        // buffer. Restore the draw path's descriptor and invalidate its cached state before
        // replay continues with HUD textures on the SDR target.
        ResetDrawState();
        g.display_ui_active = true;
        return;
    }
    Transition(cmd, source, TransferSrc());
    Transition(cmd, bloom.scene, TransferDst());
    VkImageBlit region = {};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.srcOffsets[1] = {static_cast<int32_t>(source.width), static_cast<int32_t>(source.height), 1};
    region.dstOffsets[1] = {static_cast<int32_t>(bloom.scene.width), static_cast<int32_t>(bloom.scene.height), 1};
    vkCmdBlitImage(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   bloom.scene.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
    ToRest(cmd, source);
    ToRest(cmd, bloom.scene);
    bloom.scene_captured = true;
}

const Image *BloomHudlessColor() {
    Image &image = bloom.hudless[g.frame_slot];
    return g.display_ui_active && bloom.hudless_valid[g.frame_slot] && image.image != VK_NULL_HANDLE
               ? &image : nullptr;
}

void BloomDestroyTargets() {
    if (bloom.initialized) {
        DestroyTargets();
    }
}

void BloomInit() {
    VkSamplerCreateInfo sampler = {};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 1.0f;
    Check(vkCreateSampler(g.device, &sampler, nullptr, &bloom.sampler), "vkCreateSampler(bloom)");

    VkDescriptorSetLayoutBinding bindings[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo descriptor_layout = {};
    descriptor_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptor_layout.bindingCount = 2;
    descriptor_layout.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(g.device, &descriptor_layout, nullptr, &bloom.set_layout),
          "vkCreateDescriptorSetLayout(bloom)");

    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight * 8};
    VkDescriptorPoolCreateInfo pool = {};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = kFramesInFlight * 4;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &pool_size;
    Check(vkCreateDescriptorPool(g.device, &pool, nullptr, &bloom.pool), "vkCreateDescriptorPool(bloom)");
    for (auto &sets : bloom.sets) {
        std::array<VkDescriptorSetLayout, 4> layouts;
        layouts.fill(bloom.set_layout);
        VkDescriptorSetAllocateInfo alloc = {};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = bloom.pool;
        alloc.descriptorSetCount = 4;
        alloc.pSetLayouts = layouts.data();
        Check(vkAllocateDescriptorSets(g.device, &alloc, sets.data()), "vkAllocateDescriptorSets(bloom)");
    }

    VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Settings)};
    VkPipelineLayoutCreateInfo pipeline_layout = {};
    pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout.setLayoutCount = 1;
    pipeline_layout.pSetLayouts = &bloom.set_layout;
    pipeline_layout.pushConstantRangeCount = 1;
    pipeline_layout.pPushConstantRanges = &push;
    Check(vkCreatePipelineLayout(g.device, &pipeline_layout, nullptr, &bloom.layout),
          "vkCreatePipelineLayout(bloom)");

    VkShaderModule vs = Shader(bloom_vert, sizeof(bloom_vert));
    VkShaderModule fs = Shader(bloom_frag, sizeof(bloom_frag));
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
    color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &color;
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
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
    pipeline.layout = bloom.layout;
    Check(vkCreateGraphicsPipelines(g.device, g.pipeline_cache, 1, &pipeline, nullptr, &bloom.pipelines[0]),
          "vkCreateGraphicsPipelines(bloom)");
    format = kColorFormat;
    Check(vkCreateGraphicsPipelines(g.device, g.pipeline_cache, 1, &pipeline, nullptr, &bloom.pipelines[1]),
          "vkCreateGraphicsPipelines(bloom SDR composite)");
    vkDestroyShaderModule(g.device, fs, nullptr);
    vkDestroyShaderModule(g.device, vs, nullptr);
    bloom.initialized = true;
    BloomResize();
}

void BloomShutdown() {
    if (!bloom.initialized) {
        return;
    }
    DestroyTargets();
    for (VkPipeline pipeline : bloom.pipelines) vkDestroyPipeline(g.device, pipeline, nullptr);
    vkDestroyPipelineLayout(g.device, bloom.layout, nullptr);
    vkDestroyDescriptorPool(g.device, bloom.pool, nullptr);
    vkDestroyDescriptorSetLayout(g.device, bloom.set_layout, nullptr);
    vkDestroySampler(g.device, bloom.sampler, nullptr);
    bloom = {};
}

Image &BloomApply(VkCommandBuffer cmd, Image &source) {
    if (!bloom.initialized || (!bloom.enabled && source.format != kHdrFormat)) {
        return source;
    }
    ToRest(cmd, source);
    Image &glow_source = bloom.scene_captured ? bloom.scene : source;
    DrawPass(cmd, 0, glow_source, glow_source, bloom.bright, 0, 0.0f);
    DrawPass(cmd, 1, bloom.bright, bloom.bright, bloom.blur, 1, 0.0f);
    DrawPass(cmd, 2, bloom.blur, bloom.blur, bloom.bright, 2, 0.0f);
    DrawPass(cmd, 3, source, bloom.bright, bloom.output, 3, bloom.enabled ? 0.18f : 0.0f);
    bloom.scene_captured = false;
    return bloom.output;
}

} // namespace gfx::detail

namespace gfx {

void SetBloom(bool enabled) { detail::bloom.enabled = enabled; }

} // namespace gfx
