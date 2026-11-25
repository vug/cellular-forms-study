// The menu in the headset: a Dear ImGui context of its own, drawn into an offscreen image and
// shown as an OpenXR quad layer that floats in the room. The controller's pointer ray is the
// mouse, its trigger the button, its thumbstick the wheel. (The desktop viewer's ImGui is not
// touched: this is a separate context with only the Vulkan backend.)
#pragma once
#include <imgui.h>

#include "headset.hpp"
#include "vk_context.hpp"
#include "xr_math.hpp"

namespace cfvr {

class Menu {
public:
    Menu(cfv::Context& ctx, Headset& xr, VkExtent2D pixels, float widthMetres);
    ~Menu();
    Menu(const Menu&) = delete;
    Menu& operator=(const Menu&) = delete;

    bool open = false;
    Pose pose;  // centre of the panel in the reference space; it faces along its +Z

    // Puts the panel in front of the head, upright, turned towards it.
    void placeInFront(const Pose& head);
    // Where the ray of `aim` (along its -Z) meets the panel, in pixels of the image, and how far
    // along the ray; false when it misses.
    bool hit(const Pose& aim, float& px, float& py, float& distance) const;

    // Starts an ImGui frame with this pointer state (px, py in pixels; pointing false: the
    // pointer is off the panel). Build the windows after this.
    void beginFrame(float dt, bool pointing, float px, float py, bool button, float wheel);
    // Ends the frame and records drawing it into the swapchain image `index` (acquired).
    void record(VkCommandBuffer cmd, uint32_t index);
    XrCompositionLayerQuad layer(XrSpace space) const;

    Swapchain swapchain;
    VkExtent2D pixels() const { return extent_; }
    float widthMetres() const { return width_; }
    float heightMetres() const { return height_; }
    bool wantsPointer() const;  // the pointer is over a widget (ImGui wants the mouse)
    // Makes the menu's ImGui context the current one (for what draws into it, or frees what it
    // registered with its Vulkan backend).
    void makeCurrent() const { ImGui::SetCurrentContext(imgui_); }

private:
    cfv::Context& ctx_;
    Headset& xr_;
    VkExtent2D extent_;
    float width_, height_;
    VkFormat format_ = VK_FORMAT_R8G8B8A8_UNORM;
    cfv::Image image_;
    ImGuiContext* imgui_ = nullptr;
};

}  // namespace cfvr
