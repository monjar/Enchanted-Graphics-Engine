#pragma once

#include "editor/EditorViewport.hpp"
#include "platform/Window.hpp"
#include "render/PbrRenderSystem.hpp"
#include "rhi/Device.hpp"
#include "scene/World.hpp"

#include <memory>

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

        // Where the frame's scene passes should render. While the editor is up
        // that is the viewport's offscreen image, sampled by the panel; while
        // it is hidden the caller renders straight to the backbuffer as if the
        // editor did not exist.
        struct SceneTarget {
            bool offscreen = false;
            VkImage image = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            VkExtent2D extent{0, 0};
        };

        // Sizes the offscreen target to what the viewport panel asked for last
        // frame. Call before beginFrame: a resize replaces the descriptor set
        // the panel draws with, and doing that after the draw list has already
        // referenced it would leave ImGui pointing at a freed set.
        void prepareFrame(VkExtent2D windowExtent);

        SceneTarget sceneTarget(VkExtent2D windowExtent) const;

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
        // controller can stand down instead of fighting the UI for input. The
        // scene view is not a panel in this sense: hovering it is how the
        // camera is flown.
        bool wantsInput() const;

    private:
        // A structural change asked for while the hierarchy was being walked.
        // Spawning or despawning mid-walk would invalidate the sibling links
        // the walk is following, so the request is recorded and applied once
        // the tree has been drawn.
        struct PendingEdit {
            enum class Kind { none, spawn, despawn, reparent };

            Kind kind = Kind::none;
            EntityId subject{};
            EntityId target{};
        };

        void buildDefaultLayout(unsigned int dockspaceId);
        void drawStatsPanel(const PbrRenderSystem::Stats& stats, float frameTime);
        void drawViewportPanel();
        void drawHierarchyPanel(World& world);
        void drawEntityNode(World& world, EntityId entity);
        void drawInspectorPanel(World& world);
        void applyPendingEdit(World& world);

        Device& device;
        VkDescriptorPool imguiPool = VK_NULL_HANDLE;
        // Owned storage for the format the backend's pipeline is created
        // against; the init struct keeps a pointer to it.
        VkFormat colorFormat = VK_FORMAT_UNDEFINED;

        std::unique_ptr<EditorViewport> viewport;
        // What the panel had room for last frame, in pixels. Zero until it has
        // been laid out once, which is why prepareFrame takes a fallback.
        VkExtent2D requestedViewportExtent{0, 0};
        bool viewportHovered = false;
        // Only ever built once, and only when no saved layout was restored -
        // a user who has arranged their panels keeps that arrangement.
        bool layoutChecked = false;

        bool overlayVisible = true;
        EntityId selected{};
        PendingEdit pending{};
    };

}  // namespace ege
