#include "editor/EditorOverlay.hpp"

#include "core/Log.hpp"
#include "reflect/BuiltinTypes.hpp"
#include "scene/ComponentRegistry.hpp"
#include "scene/Components.hpp"

#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>
// The dock builder is the only way to lay panels out from code rather than
// making every new user drag four windows into place. It lives in the
// internal header, which is where ImGui keeps things that are stable enough
// to use but not frozen as public API.
#include <imgui_internal.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ege {

    namespace {

        // The inspector recognises leaf types by TypeInfo identity: of<T>()
        // returns a stable reference, so pointer comparison is exact and a
        // typo cannot silently match the wrong widget.
        const TypeInfo* floatType() {
            return &TypeRegistry::of<float>();
        }

        const TypeInfo* intType() {
            return &TypeRegistry::of<std::int32_t>();
        }

        const TypeInfo* boolType() {
            return &TypeRegistry::of<bool>();
        }

        const TypeInfo* vec2Type() {
            return &TypeRegistry::of<glm::vec2>();
        }

        const TypeInfo* vec3Type() {
            return &TypeRegistry::of<glm::vec3>();
        }

        const TypeInfo* vec4Type() {
            return &TypeRegistry::of<glm::vec4>();
        }

        const TypeInfo* stringType() {
            return &TypeRegistry::of<std::string>();
        }

        // One field, drawn by its reflected description. This function is the
        // entire inspector "framework": a new component type gets an editor UI
        // by being reflected, with nothing written here.
        void drawField(const FieldInfo& field, void* instance) {
            void* address = field.addressIn(instance);
            const TypeInfo* type = &field.type();
            const std::string label{field.name()};

            if (field.isReadOnly()) {
                ImGui::BeginDisabled();
            }

            if (type == floatType()) {
                auto* value = static_cast<float*>(address);
                if (field.hasRange()) {
                    ImGui::SliderFloat(label.c_str(), value, field.range().min, field.range().max);
                } else {
                    ImGui::DragFloat(label.c_str(), value, 0.05f);
                }
            } else if (type == intType()) {
                ImGui::DragInt(label.c_str(), static_cast<int*>(address));
            } else if (type == boolType()) {
                ImGui::Checkbox(label.c_str(), static_cast<bool*>(address));
            } else if (type == vec2Type()) {
                ImGui::DragFloat2(label.c_str(), static_cast<float*>(address), 0.05f);
            } else if (type == vec3Type()) {
                if (field.isColor()) {
                    ImGui::ColorEdit3(label.c_str(), static_cast<float*>(address));
                } else {
                    ImGui::DragFloat3(label.c_str(), static_cast<float*>(address), 0.05f);
                }
            } else if (type == vec4Type()) {
                if (field.isColor()) {
                    ImGui::ColorEdit4(label.c_str(), static_cast<float*>(address));
                } else {
                    ImGui::DragFloat4(label.c_str(), static_cast<float*>(address), 0.05f);
                }
            } else if (type == stringType()) {
                ImGui::LabelText(label.c_str(), "%s", static_cast<std::string*>(address)->c_str());
            } else {
                ImGui::LabelText(label.c_str(), "<%s>", std::string{type->name()}.c_str());
            }

            if (field.isReadOnly()) {
                ImGui::EndDisabled();
            }

            if (field.tooltip() != nullptr && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", field.tooltip());
            }
        }

    }  // namespace

    EditorOverlay::EditorOverlay(
        Window& window, Device& deviceRef, VkFormat outputFormat, uint32_t swapchainImageCount)
        : device{deviceRef} {
        // ImGui allocates and frees descriptor sets for the textures it
        // draws; the pool must allow freeing. Room for the font atlas, the
        // viewport target, and the handful of viewport targets still retired
        // while a panel edge is being dragged.
        constexpr uint32_t maxTextures = 64;
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxTextures};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = maxTextures;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;
        if (vkCreateDescriptorPool(device.device(), &poolInfo, nullptr, &imguiPool) != VK_SUCCESS) {
            throw std::runtime_error{"failed to create the ImGui descriptor pool"};
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
        ImGui::StyleColorsDark();

        // install_callbacks: ImGui chains the input callbacks the engine's
        // Input class installed at window creation, so both see every event.
        ImGui_ImplGlfw_InitForVulkan(window.getGLFWwindow(), true);

        // The UI draws directly onto the swapchain image via dynamic
        // rendering, matching how every other pass works. Known wrinkle: the
        // attachment is sRGB and ImGui emits colours that are already
        // sRGB-encoded, so the hardware encode is applied twice and the UI
        // renders a touch brighter than intended. Livable for an overlay;
        // the standalone editor gets a UNORM view of the image instead.
        colorFormat = outputFormat;

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.Instance = device.instanceHandle();
        initInfo.PhysicalDevice = device.physicalDeviceHandle();
        initInfo.Device = device.device();
        initInfo.QueueFamily = device.graphicsQueueFamily();
        initInfo.Queue = device.graphicsQueue();
        initInfo.DescriptorPool = imguiPool;
        initInfo.MinImageCount = 2;
        initInfo.ImageCount = swapchainImageCount;
        initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        initInfo.PipelineCache = device.pipelineCache();
        initInfo.UseDynamicRendering = true;
        initInfo.PipelineRenderingCreateInfo = {};
        initInfo.PipelineRenderingCreateInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        initInfo.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        initInfo.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFormat;

        if (!ImGui_ImplVulkan_Init(&initInfo)) {
            throw std::runtime_error{"failed to initialise the ImGui Vulkan backend"};
        }

        // After the backend: the viewport's texture handle is a descriptor set
        // allocated through it.
        viewport = std::make_unique<EditorViewport>(device, outputFormat, swapchainImageCount);

        EGE_INFO("Editor overlay ready (F1 toggles)");
    }

    EditorOverlay::~EditorOverlay() {
        // Before the backend goes away: releasing the viewport's texture
        // handle goes back through it.
        viewport.reset();
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        vkDestroyDescriptorPool(device.device(), imguiPool, nullptr);
    }

    void EditorOverlay::prepareFrame(VkExtent2D windowExtent) {
        // Until the panel has been laid out once there is nothing to go on, so
        // the window's own size stands in - which is also what a hidden editor
        // would want the moment F1 brings it back.
        const bool laidOut =
            requestedViewportExtent.width != 0 && requestedViewportExtent.height != 0;
        viewport->resize(laidOut ? requestedViewportExtent : windowExtent);
    }

    EditorOverlay::SceneTarget EditorOverlay::sceneTarget(VkExtent2D windowExtent) const {
        SceneTarget target{};
        if (!overlayVisible || !viewport->valid()) {
            target.extent = windowExtent;
            return target;
        }
        target.offscreen = true;
        target.image = viewport->imageHandle();
        target.view = viewport->viewHandle();
        target.extent = viewport->extent();
        return target;
    }

    void EditorOverlay::beginFrame() {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    bool EditorOverlay::wantsInput() const {
        if (!overlayVisible) {
            return false;
        }
        const ImGuiIO& io = ImGui::GetIO();
        // A text field being edited outranks everything: the cursor may well be
        // sitting over the scene while the typing is meant for the field.
        if (io.WantTextInput) {
            return true;
        }
        // The scene view is the one place where the mouse belongs to the
        // camera rather than the UI - that is what makes it a viewport.
        if (viewportHovered) {
            return false;
        }
        return io.WantCaptureMouse || io.WantCaptureKeyboard;
    }

    void EditorOverlay::buildUi(
        World& world, const PbrRenderSystem::Stats& stats, float frameTime) {
        if (!overlayVisible) {
            return;
        }

        // Something under the cursor from the first frame: the panels are
        // the product here, and an empty inspector demonstrates nothing.
        if (selected.isNull() && !world.all().empty()) {
            selected = world.all().front().id();
        }

        // Panels dock into this rather than floating over the scene, which is
        // the difference between a debug overlay and an editor.
        const ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
        if (!layoutChecked) {
            layoutChecked = true;
            const ImGuiDockNode* node = ImGui::DockBuilderGetNode(dockspaceId);
            // A restored imgui.ini has already populated the node tree; only an
            // editor that has never been arranged gets the built-in layout.
            if (node == nullptr || (!node->IsSplitNode() && node->Windows.Size == 0)) {
                buildDefaultLayout(dockspaceId);
            }
        }

        drawViewportPanel();
        drawStatsPanel(stats, frameTime);
        drawHierarchyPanel(world);
        drawInspectorPanel(world);
    }

    void EditorOverlay::buildDefaultLayout(unsigned int dockspaceId) {
        const ImGuiID root = dockspaceId;
        ImGui::DockBuilderRemoveNode(root);
        ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(root, ImGui::GetMainViewport()->WorkSize);

        // Splitting the centre repeatedly leaves the scene with whatever the
        // panels did not claim, which is what a viewport should get.
        ImGuiID centre = root;
        const ImGuiID left =
            ImGui::DockBuilderSplitNode(centre, ImGuiDir_Left, 0.20f, nullptr, &centre);
        const ImGuiID right =
            ImGui::DockBuilderSplitNode(centre, ImGuiDir_Right, 0.28f, nullptr, &centre);
        ImGuiID hierarchy = left;
        const ImGuiID stats =
            ImGui::DockBuilderSplitNode(hierarchy, ImGuiDir_Down, 0.35f, nullptr, &hierarchy);

        ImGui::DockBuilderDockWindow("Scene", centre);
        ImGui::DockBuilderDockWindow("Hierarchy", hierarchy);
        ImGui::DockBuilderDockWindow("Stats", stats);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderFinish(root);

        EGE_DEBUG("editor: built the default panel layout");
    }

    void EditorOverlay::drawViewportPanel() {
        // No padding: the image is the panel, and a strip of window background
        // around the scene reads as a rendering bug.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.f, 0.f});
        ImGui::Begin("Scene");

        const ImVec2 available = ImGui::GetContentRegionAvail();
        // ImGui lays out in points; the target is allocated in pixels, and on
        // a scaled display those are not the same number.
        const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
        requestedViewportExtent = {
            static_cast<uint32_t>(std::max(available.x * scale.x, 1.f)),
            static_cast<uint32_t>(std::max(available.y * scale.y, 1.f))};

        if (viewport->valid()) {
            ImGui::Image(reinterpret_cast<ImTextureID>(viewport->textureSet()), available);
        }

        // Whether the camera flies. Child windows count so that anything drawn
        // over the scene later - a gizmo, a stats readout - does not steal it.
        viewportHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

        ImGui::End();
        ImGui::PopStyleVar();
    }

    void EditorOverlay::render(VkCommandBuffer commandBuffer) {
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
    }

    void EditorOverlay::drawStatsPanel(const PbrRenderSystem::Stats& stats, float frameTime) {
        ImGui::Begin("Stats");
        ImGui::Text(
            "%.2f ms  (%.0f fps)",
            static_cast<double>(frameTime) * 1000.0,
            1.0 / static_cast<double>(frameTime));
        ImGui::Separator();
        // Culling numbers describe the previous frame: the render that
        // produces them runs after the UI is declared.
        ImGui::Text("candidates  %zu", stats.candidates);
        ImGui::Text("culled      %zu", stats.culled);
        ImGui::Text("drawn       %zu", stats.drawn);
        ImGui::Text("mat binds   %zu", stats.materialBinds);
        ImGui::End();
    }

    void EditorOverlay::drawHierarchyPanel(World& world) {
        ImGui::Begin("Hierarchy");

        for (Entity entity : world.all()) {
            // Roots only; children are reached through their parent.
            const Hierarchy* links = world.find<Hierarchy>(entity.id());
            if (links != nullptr && !links->parent.isNull()) {
                continue;
            }
            drawEntityNode(world, entity.id());
        }

        ImGui::End();
    }

    void EditorOverlay::drawEntityNode(World& world, EntityId entity) {
        const std::string& name = world.nameOf(entity);
        const Hierarchy* links = world.find<Hierarchy>(entity);
        const bool hasChildren = links != nullptr && !links->firstChild.isNull();

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_SpanAvailWidth |
                                   ImGuiTreeNodeFlags_DefaultOpen;
        if (!hasChildren) {
            flags |= ImGuiTreeNodeFlags_Leaf;
        }
        if (entity == selected) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }

        // The entity id is the ImGui id, so identically named siblings stay
        // distinct nodes.
        const bool open = ImGui::TreeNodeEx(
            reinterpret_cast<void*>(static_cast<uintptr_t>(entity.raw())),
            flags,
            "%s",
            name.empty() ? "(unnamed)" : name.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            selected = entity;
        }

        if (open) {
            if (hasChildren) {
                for (EntityId child = links->firstChild; !child.isNull();) {
                    // Read the sibling link before recursing: the pointer we
                    // hold could be invalidated by pool growth if a callback
                    // ever attaches components.
                    const EntityId next = world.fetch<Hierarchy>(child).nextSibling;
                    drawEntityNode(world, child);
                    child = next;
                }
            }
            ImGui::TreePop();
        }
    }

    void EditorOverlay::drawInspectorPanel(World& world) {
        ImGui::Begin("Inspector");

        if (selected.isNull() || !world.alive(selected)) {
            ImGui::TextDisabled("select an entity");
            ImGui::End();
            return;
        }

        // A docked panel is narrower than the floating window this started as,
        // and ImGui puts field labels after the widget - so without a cap the
        // widgets push every label off the right edge.
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);

        // The name lives on the entity itself, not in a component.
        char nameBuffer[128];
        std::strncpy(nameBuffer, world.nameOf(selected).c_str(), sizeof(nameBuffer) - 1);
        nameBuffer[sizeof(nameBuffer) - 1] = '\0';
        if (ImGui::InputText("name", nameBuffer, sizeof(nameBuffer))) {
            world.setName(selected, nameBuffer);
        }

        // Every registered component the entity carries, drawn purely from
        // reflection. Nothing here knows what a Transform is.
        for (const ComponentRegistry::Entry& entry : ComponentRegistry::instance().all()) {
            if (!entry.has(world, selected)) {
                continue;
            }
            if (!ImGui::CollapsingHeader(entry.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                continue;
            }

            void* component = entry.find(world, selected);
            if (component == nullptr || entry.type == nullptr) {
                continue;
            }
            if (entry.type->fields().empty()) {
                ImGui::TextDisabled("(tag)");
                continue;
            }

            ImGui::PushID(entry.name.c_str());
            for (const FieldInfo& field : entry.type->fields()) {
                drawField(field, component);
            }
            ImGui::PopID();
        }

        ImGui::PopItemWidth();
        ImGui::End();
    }

}  // namespace ege
