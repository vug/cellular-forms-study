#include "menu.hpp"

#include <imgui_impl_vulkan.h>

#include <cfloat>
#include <cmath>
#include <stdexcept>

#include "vr_vk.hpp"

namespace cfvr {

Menu::Menu(cfv::Context& ctx, Headset& xr, VkExtent2D pixels, float widthMetres)
    : ctx_(ctx), xr_(xr), extent_(pixels), width_(widthMetres),
      height_(widthMetres * static_cast<float>(pixels.height) / static_cast<float>(pixels.width)) {
    const int64_t format = xr_.pickFormat({VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM});
    if (format == 0) throw std::runtime_error("the OpenXR runtime offers no 8-bit colour swapchain format for the menu");
    swapchain = xr_.createSwapchain(extent_.width, extent_.height, format, XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT);
    format_ = unormOf(static_cast<VkFormat>(format));
    image_ = ctx_.createImage(extent_, format_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    IMGUI_CHECKVERSION();
    ImGuiContext* previous = ImGui::GetCurrentContext();
    imgui_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(imgui_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendPlatformName = "cf_vr";
    io.MouseDrawCursor = true;  // there is no system cursor in the headset
    io.DisplaySize = ImVec2(static_cast<float>(extent_.width), static_cast<float>(extent_.height));
    ImGui::StyleColorsDark();
    // Large enough to read at arm's length: about 1 degree per line of text.
    const float scale = static_cast<float>(extent_.width) / 620.0f;
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.WindowRounding = 0.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.96f;

    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_4;
    ii.Instance = ctx_.instance;
    ii.PhysicalDevice = ctx_.gpu;
    ii.Device = ctx_.device;
    ii.QueueFamily = ctx_.queueFamily;
    ii.Queue = ctx_.queue;
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = 2;
    ii.ImageCount = 2;
    ii.UseDynamicRendering = true;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &format_;
    if (!ImGui_ImplVulkan_Init(&ii)) throw std::runtime_error("ImGui_ImplVulkan_Init failed (menu)");
    ImGui::SetCurrentContext(previous != nullptr ? previous : imgui_);
}

Menu::~Menu() {
    vkDeviceWaitIdle(ctx_.device);
    ImGui::SetCurrentContext(imgui_);
    ImGui_ImplVulkan_Shutdown();
    ImGui::DestroyContext(imgui_);
    ctx_.destroy(image_);
    xr_.destroy(swapchain);
}

void Menu::placeInFront(const Pose& head) {
    Vec3 forward = rotate(head.q, Vec3{0, 0, -1});
    forward.y = 0.0f;
    if (cf::length(forward) < 1.0e-3f) forward = Vec3{0, 0, -1};
    forward = cfv::normalize(forward);
    pose.p = head.p + forward * 0.62f + Vec3{0.0f, -0.12f, 0.0f};
    // +Z of the panel towards the head, leaning back a little.
    const float yaw = std::atan2(-forward.x, -forward.z);
    pose.q = normalized(axisAngle(Vec3{0, 1, 0}, yaw) * axisAngle(Vec3{1, 0, 0}, -0.15f));
}

bool Menu::hit(const Pose& aim, float& px, float& py, float& distance) const {
    const Pose local = inverse(pose) * aim;
    const Vec3 o = local.p;
    const Vec3 d = rotate(local.q, Vec3{0, 0, -1});
    if (std::fabs(d.z) < 1.0e-5f) return false;
    const float t = -o.z / d.z;
    if (!(t > 0.0f) || t > 10.0f) return false;
    const Vec3 at = o + d * t;
    const float u = at.x / width_ + 0.5f, v = 0.5f - at.y / height_;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return false;
    px = u * static_cast<float>(extent_.width);
    py = v * static_cast<float>(extent_.height);
    distance = t;
    return true;
}

bool Menu::wantsPointer() const {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(imgui_);
    const bool want = ImGui::GetIO().WantCaptureMouse;
    ImGui::SetCurrentContext(previous);
    return want;
}

void Menu::beginFrame(float dt, bool pointing, float px, float py, bool button, float wheel) {
    ImGui::SetCurrentContext(imgui_);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(extent_.width), static_cast<float>(extent_.height));
    io.DeltaTime = dt > 0.0f && dt < 1.0f ? dt : 1.0f / 72.0f;
    if (pointing) io.AddMousePosEvent(px, py);
    else io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    io.AddMouseButtonEvent(0, pointing && button);
    if (pointing && wheel != 0.0f) io.AddMouseWheelEvent(0.0f, wheel);
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
}

void Menu::record(VkCommandBuffer cmd, uint32_t index) {
    ImGui::SetCurrentContext(imgui_);
    ImGui::Render();
    cfv::transition(cmd, image_.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = image_.view;
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};  // transparent around the window
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, extent_};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &attachment;
    vkCmdBeginRendering(cmd, &info);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    vkCmdEndRendering(cmd);
    copyToSwapchain(cmd, image_.image, swapchain.images[index], extent_);
}

XrCompositionLayerQuad Menu::layer(XrSpace space) const {
    XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};
    // ImGui's blending leaves colour times alpha in the image: premultiplied, as OpenXR expects
    // unless told otherwise.
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = space;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = swapchain.handle;
    q.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(extent_.width), static_cast<int32_t>(extent_.height)}};
    q.subImage.imageArrayIndex = 0;
    q.pose.orientation = {pose.q.x, pose.q.y, pose.q.z, pose.q.w};
    q.pose.position = {pose.p.x, pose.p.y, pose.p.z};
    q.size = {width_, height_};
    return q;
}

}  // namespace cfvr
