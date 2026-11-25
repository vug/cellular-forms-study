#include "rays.hpp"

#include <cstring>
#include <iterator>

namespace cfvr {

namespace {

const uint32_t kRayVert[] = {
#include "ray.vert.inc"
};
const uint32_t kRayFrag[] = {
#include "ray.frag.inc"
};

struct Push {
    float corner[4][4];
    float color[4];
    float fade[4];
};

}  // namespace

Rays::Rays(cfv::Context& ctx, VkFormat targetFormat) : ctx_(ctx) {
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &range;
    cfv::vk_check(vkCreatePipelineLayout(ctx_.device, &li, nullptr, &layout_), "vkCreatePipelineLayout (rays)");

    VkShaderModule vert = ctx_.createShaderModule(kRayVert, std::size(kRayVert));
    VkShaderModule frag = ctx_.createShaderModule(kRayFrag, std::size(kRayFrag));
    VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &targetFormat;
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pNext = &rendering;
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = layout_;
    const VkResult r = vkCreateGraphicsPipelines(ctx_.device, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline_);
    vkDestroyShaderModule(ctx_.device, vert, nullptr);
    vkDestroyShaderModule(ctx_.device, frag, nullptr);
    cfv::vk_check(r, "vkCreateGraphicsPipelines (rays)");
}

Rays::~Rays() {
    if (pipeline_) vkDestroyPipeline(ctx_.device, pipeline_, nullptr);
    if (layout_) vkDestroyPipelineLayout(ctx_.device, layout_, nullptr);
}

void Rays::draw(VkCommandBuffer cmd, VkExtent2D extent, const Pose& eye, const float tangents[4], const std::vector<RaySegment>& segments) const {
    if (segments.empty()) return;
    const Mat4 proj = frustum(tangents[0], tangents[1], tangents[2], tangents[3], 0.01f, 100.0f);
    const Pose toEye = inverse(eye);
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    const float halfWidth = 0.0015f;  // metres
    for (const RaySegment& s : segments) {
        const Vec3 a = apply(toEye, s.start), b = apply(toEye, s.end);
        // Sideways: across the ray and across the line of sight to its middle.
        Vec3 side = cf::cross(b - a, (a + b) * 0.5f);
        if (cf::length(side) < 1.0e-9f) continue;  // seen end-on
        side = cfv::normalize(side) * halfWidth;
        const Vec3 corners[4] = {a - side, a + side, b - side, b + side};
        Push push{};
        for (int i = 0; i < 4; ++i) {
            const Vec4 c = transform(proj, corners[i]);
            push.corner[i][0] = c.x;
            push.corner[i][1] = c.y;
            push.corner[i][2] = c.z;
            push.corner[i][3] = c.w;
        }
        std::memcpy(push.color, s.color, sizeof push.color);
        push.fade[0] = s.fadeStart;
        push.fade[1] = s.fadeEnd;
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
        vkCmdDraw(cmd, 6, 1, 0, 0);
    }
}

}  // namespace cfvr
