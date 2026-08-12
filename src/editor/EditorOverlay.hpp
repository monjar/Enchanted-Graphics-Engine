#pragma once

#include "platform/Window.hpp"
#include "render/PbrRenderSystem.hpp"
#include "rhi/Device.hpp"
#include "scene/World.hpp"

namespace ege {

    // The in-process editor overlay: Dear ImGui drawn as the frame's last
    // pass, straight onto the backbuffer.
    //
    // This is the thin start of the Phase 5 editor - the hierarchy tree and
    // the reflection-driven inspector are the panels every later editor
    // feature hangs off, and building them in-process first means the ECS
    // and reflection APIs get exercised by an actual consumer before the
    // standalone editor application exists. When that application arrives,
    // these panels move into it rather than being rewritten.
    class EditorOverlay {
    public:
        EditorOverlay(
            Window& window, Device& device, VkFormat outputFormat, uint32_t swapchainImageCount);
        ~EditorOverlay();

        EditorOverlay(const EditorOverlay&) = delete;
        EditorOverlay& operator=(const EditorOverlay&) = delete;

        // Starts the ImGui frame. Call once per rendered frame, always -
        // NewFrame and Render must pair even when the overlay is hidden.
        void beginFrame();

        // Declares the panels. Skipped entirely while hidden.
        void buildUi(World& world, const PbrRenderSystem::Stats& stats, float frameTime);

        // Finalises the frame and records the draw data.
        void render(VkCommandBuffer commandBuffer);

        void toggle() { overlayVisible = !overlayVisible; }

        bool visible() const { return overlayVisible; }

        // True while a panel owns the mouse or keyboard, so the camera
        // controller can stand down instead of fighting the UI for input.
        bool wantsInput() const;

    private:
        void drawStatsPanel(const PbrRenderSystem::Stats& stats, float frameTime);
        void drawHierarchyPanel(World& world);
        void drawEntityNode(World& world, EntityId entity);
        void drawInspectorPanel(World& world);

        Device& device;
        VkDescriptorPool imguiPool = VK_NULL_HANDLE;
        // Owned storage for the format the backend's pipeline is created
        // against; the init struct keeps a pointer to it.
        VkFormat colorFormat = VK_FORMAT_UNDEFINED;

        bool overlayVisible = true;
        EntityId selected{};
    };

}  // namespace ege
