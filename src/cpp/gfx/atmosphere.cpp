#include "atmosphere.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "platform/settings.hpp"

#include "shaders/bloom.vert.spv.hpp"
#include "shaders/atmosphere.frag.spv.hpp"

namespace gfx::detail {
namespace {

struct FogPush {
    float inverse_view_projection[16];
    float camera_position[4];
    float light_direction[4];
    float light_color[4];
    float parameters[4];
};
static_assert(sizeof(FogPush) == 128);

// std140 packs four map-cell elevations in each vec4. A separate uniform buffer
// keeps the 128-byte push constant and the ray-tracing light block unchanged.
struct FogDeckField {
    float heights[100][4] = {};
    float background_color[4] = {};
    float lower_color[4] = {};
    float clear_color[4] = {};
    float density[4] = {};
    float distance[4] = {};
    float effects[4] = {};
    float local_limit[4] = {};
};
static_assert(sizeof(FogDeckField) == 1712);

struct FogState {
    bool initialized = false;
    bool fog_enabled = false;
    AtmosphereSettings settings;
    bool camera_valid = false;
    bool reported_apply = false;
    bool deck_field_valid = false;
    FogDeckField deck_field;
    uint64_t camera_frame = 0;
    FogPush push = {};
    Image output;
    VkImageView depth_view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, kFramesInFlight> sets = {};
    std::array<Buffer, kFramesInFlight> light_buffers = {};
    std::array<Buffer, kFramesInFlight> deck_buffers = {};
    LocalLightBlock lights;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool profile_enabled = false;
    uint32_t timestamp_bits = 0;
    std::array<VkQueryPool, kFramesInFlight> profile_queries = {};
    std::array<bool, kFramesInFlight> profile_pending = {};
    double profile_gpu_ms = 0.0;
    uint32_t profile_frames = 0;
};
FogState fog;

// Matrices in MeshConstants are column-major. Gauss-Jordan is used here only once
// per display frame to derive the inverse camera matrix, never per mesh or pixel.
bool Invert4(const float matrix[16], float inverse[16]) {
    double rows[4][8] = {};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) rows[r][c] = matrix[c * 4 + r];
        rows[r][4 + r] = 1.0;
    }
    for (int column = 0; column < 4; ++column) {
        int pivot = column;
        for (int r = column + 1; r < 4; ++r) {
            if (std::abs(rows[r][column]) > std::abs(rows[pivot][column])) pivot = r;
        }
        if (std::abs(rows[pivot][column]) < 1e-10) return false;
        if (pivot != column) {
            for (int c = 0; c < 8; ++c) std::swap(rows[column][c], rows[pivot][c]);
        }
        double scale = rows[column][column];
        for (int c = 0; c < 8; ++c) rows[column][c] /= scale;
        for (int r = 0; r < 4; ++r) {
            if (r == column) continue;
            double factor = rows[r][column];
            for (int c = 0; c < 8; ++c) rows[r][c] -= factor * rows[column][c];
        }
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) inverse[c * 4 + r] = static_cast<float>(rows[r][4 + c]);
    }
    return true;
}

void Multiply4(const float left[16], const float right[16], float out[16]) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            out[c * 4 + r] = left[0 * 4 + r] * right[c * 4 + 0] +
                             left[1 * 4 + r] * right[c * 4 + 1] +
                             left[2 * 4 + r] * right[c * 4 + 2] +
                             left[3 * 4 + r] * right[c * 4 + 3];
        }
    }
}

VkShaderModule Shader(const uint32_t *words, size_t bytes) {
    VkShaderModuleCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = bytes;
    info.pCode = words;
    VkShaderModule module = VK_NULL_HANDLE;
    Check(vkCreateShaderModule(g.device, &info, nullptr, &module), "vkCreateShaderModule(atmosphere fog)");
    return module;
}

void UpdateDepthView() {
    if (fog.depth_view != VK_NULL_HANDLE) {
        vkDestroyImageView(g.device, fog.depth_view, nullptr);
        fog.depth_view = VK_NULL_HANDLE;
    }
    if (g.display_depth.image == VK_NULL_HANDLE) return;
    VkImageViewCreateInfo view = {};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = g.display_depth.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = g.display_depth.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    Check(vkCreateImageView(g.device, &view, nullptr, &fog.depth_view),
          "vkCreateImageView(atmosphere depth)");
}

} // namespace

bool AtmosphereDepthSupported() {
    if (g.physical_device == VK_NULL_HANDLE || g.depth_format == VK_FORMAT_UNDEFINED) return false;
    VkFormatProperties properties = {};
    vkGetPhysicalDeviceFormatProperties(g.physical_device, g.depth_format, &properties);
    return (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
}

void AtmosphereSetDeckField(std::span<const float, 400> cell_heights) {
    static_assert(sizeof(cell_heights[0]) == sizeof(float));
    std::memcpy(fog.deck_field.heights, cell_heights.data(), sizeof(fog.deck_field.heights));
    fog.deck_field_valid = true;
}

void AtmosphereTrackCamera(const MeshConstants &constants) {
    if (!fog.initialized || !fog.fog_enabled || !g.display_pass || g.target != kMainTarget ||
        (constants.flags & (kMeshAtmosphere | kMeshShadow)) != kMeshAtmosphere ||
        fog.camera_frame == g.frame_serial) return;

    float inverse_model[16], view_projection[16];
    if (!InvertAffineTransform(constants.model_to_world, inverse_model)) return;
    Multiply4(constants.mvp, inverse_model, view_projection);
    if (!Invert4(view_projection, fog.push.inverse_view_projection)) return;
    std::memcpy(fog.push.camera_position, constants.camera_position,
                sizeof(fog.push.camera_position));

    // This camera transform is common to world draws, but their legacy directional lights
    // vary by material and by room draw order. Keep the bulk medium cool and stable;
    // actual world-space lanterns below provide the localized warm in-scattering.
    std::fill(std::begin(fog.push.light_color), std::end(fog.push.light_color), 0.0f);
    fog.push.light_direction[0] = 0.0f;
    fog.push.light_direction[1] = 1.0f;
    fog.push.light_direction[2] = 0.0f;
    fog.push.parameters[0] = fog.settings.background_density;
    fog.push.parameters[1] = fog.settings.clear_distance;
    fog.push.parameters[2] = 8.0f;
    fog.push.parameters[3] = 0.003f;
    fog.camera_frame = g.frame_serial;
    fog.camera_valid = true;
}

void AtmosphereSetLights(std::span<const LocalLight> lights) {
    fog.lights = {};
    fog.lights.count[0] = static_cast<uint32_t>(std::min(lights.size(),
                                                         size_t{kMaxLocalLights}));
    for (uint32_t i = 0; i < fog.lights.count[0]; ++i) {
        std::memcpy(fog.lights.position_radius[i], lights[i].position_radius,
                    sizeof(lights[i].position_radius));
        std::memcpy(fog.lights.color_intensity[i], lights[i].color_intensity,
                    sizeof(lights[i].color_intensity));
    }
}

const LocalLightBlock &AtmosphereLights() {
    return fog.lights;
}
void AtmosphereSetSettings(const AtmosphereSettings &settings) {
    fog.settings = settings;
    std::copy_n(settings.background_color, 3, fog.deck_field.background_color);
    std::copy_n(settings.lower_color, 3, fog.deck_field.lower_color);
    std::copy_n(settings.clear_color, 3, fog.deck_field.clear_color);
    fog.deck_field.density[0] = settings.background_density;
    fog.deck_field.density[1] = settings.clear_distance;
    fog.deck_field.density[2] = settings.lower_density;
    fog.deck_field.density[3] = settings.lower_height_falloff;
    fog.deck_field.distance[0] = settings.far_density;
    fog.deck_field.distance[1] = settings.far_start;
    fog.deck_field.distance[2] = settings.far_end;
    fog.deck_field.distance[3] = settings.uncovered_distance;
    fog.deck_field.effects[0] = settings.deck_cell_size;
    fog.deck_field.effects[1] = settings.local_scattering_strength;
    std::copy_n(settings.local_scattering_limit, 3, fog.deck_field.local_limit);
    fog.fog_enabled = settings.enabled;
    if (!fog.fog_enabled) fog.camera_valid = false;
}
bool AtmosphereEnabled() { return fog.fog_enabled; }

void AtmosphereDestroyTarget() {
    if (!fog.initialized) return;
    if (fog.depth_view != VK_NULL_HANDLE) {
        vkDestroyImageView(g.device, fog.depth_view, nullptr);
        fog.depth_view = VK_NULL_HANDLE;
    }
    DestroyImage(fog.output);
    fog.camera_valid = false;
}

void AtmosphereResize() {
    if (!fog.initialized) return;
    AtmosphereDestroyTarget();
    if (!g.config.enhanced_hdr || !AtmosphereDepthSupported()) return;
    fog.output = CreateImage(g.display_color.width, g.display_color.height, 1, kHdrFormat,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    UpdateDepthView();
}

void AtmosphereInit() {
    if (!g.config.enhanced_hdr || !AtmosphereDepthSupported()) return;
    if (!fog.deck_field_valid) {
        for (auto &row : fog.deck_field.heights) std::fill(std::begin(row), std::end(row), 8.0f);
    }
    VkSamplerCreateInfo sampler = {};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    Check(vkCreateSampler(g.device, &sampler, nullptr, &fog.sampler),
          "vkCreateSampler(atmosphere fog)");

    VkDescriptorSetLayoutBinding bindings[4] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    bindings[2].binding = 2;
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[3] = bindings[2];
    bindings[3].binding = 3;
    VkDescriptorSetLayoutCreateInfo descriptor_layout = {};
    descriptor_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptor_layout.bindingCount = 4;
    descriptor_layout.pBindings = bindings;
    Check(vkCreateDescriptorSetLayout(g.device, &descriptor_layout, nullptr, &fog.set_layout),
          "vkCreateDescriptorSetLayout(atmosphere fog)");

    VkDescriptorPoolSize pool_sizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight * 2},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight * 2},
    };
    VkDescriptorPoolCreateInfo pool = {};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = kFramesInFlight;
    pool.poolSizeCount = 2;
    pool.pPoolSizes = pool_sizes;
    Check(vkCreateDescriptorPool(g.device, &pool, nullptr, &fog.pool),
          "vkCreateDescriptorPool(atmosphere fog)");
    std::array<VkDescriptorSetLayout, kFramesInFlight> layouts;
    layouts.fill(fog.set_layout);
    VkDescriptorSetAllocateInfo alloc = {};
    alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc.descriptorPool = fog.pool;
    alloc.descriptorSetCount = kFramesInFlight;
    alloc.pSetLayouts = layouts.data();
    Check(vkAllocateDescriptorSets(g.device, &alloc, fog.sets.data()),
          "vkAllocateDescriptorSets(atmosphere fog)");
    for (Buffer &buffer : fog.light_buffers) {
        buffer = CreateBuffer(sizeof(LocalLightBlock), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    }
    for (Buffer &buffer : fog.deck_buffers) {
        buffer = CreateBuffer(sizeof(FogDeckField), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
    }

    VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FogPush)};
    VkPipelineLayoutCreateInfo pipeline_layout = {};
    pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout.setLayoutCount = 1;
    pipeline_layout.pSetLayouts = &fog.set_layout;
    pipeline_layout.pushConstantRangeCount = 1;
    pipeline_layout.pPushConstantRanges = &push;
    Check(vkCreatePipelineLayout(g.device, &pipeline_layout, nullptr, &fog.layout),
          "vkCreatePipelineLayout(atmosphere fog)");

    VkShaderModule vs = Shader(bloom_vert, sizeof(bloom_vert));
    VkShaderModule fs = Shader(atmosphere_frag, sizeof(atmosphere_frag));
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
    VkFormat format = kHdrFormat;
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
    pipeline.layout = fog.layout;
    Check(vkCreateGraphicsPipelines(g.device, g.pipeline_cache, 1, &pipeline, nullptr, &fog.pipeline),
          "vkCreateGraphicsPipelines(atmosphere fog)");
    vkDestroyShaderModule(g.device, fs, nullptr);
    vkDestroyShaderModule(g.device, vs, nullptr);
    fog.initialized = true;
    const char *profile_env = SettingsEnv("RAY_PROFILE");
    fog.profile_enabled = profile_env != nullptr &&
                          (std::strcmp(profile_env, "1") == 0 ||
                           std::strcmp(profile_env, "timing") == 0);
    if (fog.profile_enabled) {
        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g.physical_device, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(g.physical_device, &family_count, families.data());
        fog.timestamp_bits = g.queue_family < family_count ?
                             families[g.queue_family].timestampValidBits : 0;
        if (fog.timestamp_bits != 0) {
            VkQueryPoolCreateInfo query = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            query.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query.queryCount = 2;
            for (VkQueryPool &pool : fog.profile_queries) {
                Check(vkCreateQueryPool(g.device, &query, nullptr, &pool),
                      "vkCreateQueryPool(atmosphere fog profile)");
            }
        }
    }
    AtmosphereResize();
}

void AtmosphereShutdown() {
    if (!fog.initialized) return;
    AtmosphereDestroyTarget();
    for (VkQueryPool pool : fog.profile_queries) {
        if (pool != VK_NULL_HANDLE) vkDestroyQueryPool(g.device, pool, nullptr);
    }
    for (Buffer &buffer : fog.light_buffers) DestroyBuffer(buffer);
    for (Buffer &buffer : fog.deck_buffers) DestroyBuffer(buffer);
    vkDestroyPipeline(g.device, fog.pipeline, nullptr);
    vkDestroyPipelineLayout(g.device, fog.layout, nullptr);
    vkDestroyDescriptorPool(g.device, fog.pool, nullptr);
    vkDestroyDescriptorSetLayout(g.device, fog.set_layout, nullptr);
    vkDestroySampler(g.device, fog.sampler, nullptr);
    fog = {};
}

Image &AtmosphereApply(VkCommandBuffer cmd, Image &source, Image &depth) {
    const char *skip_reason = nullptr;
    if (!fog.initialized) skip_reason = "uninitialized";
    else if (!fog.fog_enabled) skip_reason = "disabled";
    else if (!fog.camera_valid) skip_reason = "no_camera";
    else if (fog.camera_frame != g.frame_serial) skip_reason = "stale_camera";
    else if (!g.display_pass) skip_reason = "not_display";
    else if (!g.config.enhanced_hdr || source.format != kHdrFormat) skip_reason = "not_hdr";
    else if (&depth != &g.display_depth || source.width != depth.width ||
             source.height != depth.height) skip_reason = "depth_mismatch";
    else if (fog.output.image == VK_NULL_HANDLE || fog.depth_view == VK_NULL_HANDLE)
        skip_reason = "missing_target";
    if (skip_reason != nullptr) {
        return source;
    }

    ToRest(cmd, source);
    VkQueryPool profile_pool = fog.profile_queries[g.frame_slot];
    if (profile_pool != VK_NULL_HANDLE) {
        if (fog.profile_pending[g.frame_slot]) {
            uint64_t timestamps[2] = {};
            if (vkGetQueryPoolResults(g.device, profile_pool, 0, 2, sizeof(timestamps),
                                      timestamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                uint64_t delta = timestamps[1] - timestamps[0];
                if (fog.timestamp_bits < 64) delta &= (uint64_t{1} << fog.timestamp_bits) - 1;
                fog.profile_gpu_ms += static_cast<double>(delta) *
                                      g.properties.limits.timestampPeriod * 1e-6;
                ++fog.profile_frames;
                if (fog.profile_frames >= 60) {
                    std::fprintf(stderr, "Atmosphere fog: GPU %.3f ms/frame\n",
                                 fog.profile_gpu_ms / fog.profile_frames);
                    fog.profile_gpu_ms = 0.0;
                    fog.profile_frames = 0;
                }
            }
        }
        vkCmdResetQueryPool(cmd, profile_pool, 0, 2);
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, profile_pool, 0);
        fog.profile_pending[g.frame_slot] = true;
    }
    Transition(cmd, depth, {VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT});
    VkDescriptorImageInfo images[2] = {
        {fog.sampler, source.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {fog.sampler, fog.depth_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
    };
    VkDescriptorSet set = fog.sets[g.frame_slot];
    Buffer &light_buffer = fog.light_buffers[g.frame_slot];
    std::memcpy(light_buffer.memory.mapped, &AtmosphereLights(), sizeof(LocalLightBlock));
    VkDescriptorBufferInfo light_info = {light_buffer.buffer, 0, sizeof(LocalLightBlock)};
    Buffer &deck_buffer = fog.deck_buffers[g.frame_slot];
    // The map field is rebuilt only when the floor layout changes. Copy it into
    // the current flight slot so no in-flight display pass observes a CPU write.
    std::memcpy(deck_buffer.memory.mapped, &fog.deck_field, sizeof(FogDeckField));
    VkDescriptorBufferInfo deck_info = {deck_buffer.buffer, 0, sizeof(FogDeckField)};
    VkWriteDescriptorSet writes[4] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = set;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].pBufferInfo = &light_info;
    writes[3] = writes[2];
    writes[3].dstBinding = 3;
    writes[3].pBufferInfo = &deck_info;
    vkUpdateDescriptorSets(g.device, 4, writes, 0, nullptr);

    Transition(cmd, fog.output, {VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT});
    VkRenderingAttachmentInfo attachment = {};
    attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment.imageView = fog.output.view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering = {};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent = {fog.output.width, fog.output.height};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &attachment;
    vkCmdBeginRendering(cmd, &rendering);
    VkViewport viewport = {0.0f, 0.0f, static_cast<float>(fog.output.width),
                           static_cast<float>(fog.output.height), 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, {fog.output.width, fog.output.height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fog.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fog.layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, fog.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FogPush), &fog.push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    if (profile_pool != VK_NULL_HANDLE) {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, profile_pool, 1);
    }
    ToRest(cmd, fog.output);
    ToRest(cmd, depth);
    if (!fog.reported_apply) {
        std::fprintf(stderr, "Atmosphere: fullscreen depth fog applied with %u local lights\n",
                     AtmosphereLights().count[0]);
        fog.reported_apply = true;
    }
    fog.camera_valid = false;
    return fog.output;
}

} // namespace gfx::detail

namespace gfx {
void SetAtmosphere(const AtmosphereSettings &settings) { detail::AtmosphereSetSettings(settings); }
void SetAtmosphereDeckHeights(std::span<const float, 400> heights) {
    detail::AtmosphereSetDeckField(heights);
}
void SetLocalLights(std::span<const LocalLight> lights) {
    detail::g.local_lights.assign(lights.begin(), lights.end());
    detail::AtmosphereSetLights(lights);
}
} // namespace gfx
