#include "editor/EditorOverlay.hpp"

#include "assets/AssetDatabase.hpp"
#include "core/Log.hpp"
#include "reflect/BuiltinTypes.hpp"
#include "scene/ComponentRegistry.hpp"
#include "scene/Components.hpp"
#include "scene/Hierarchy.hpp"
#include "scene/SceneSerializer.hpp"

#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>
// The dock builder is the only way to lay panels out from code rather than
// making every new user drag four windows into place. It lives in the
// internal header, which is where ImGui keeps things that are stable enough
// to use but not frozen as public API.
#include <imgui_internal.h>
// After imgui.h, and not sorted with it: ImGuizmo uses ImGui's types without
// including the header that declares them.
#include <ImGuizmo.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

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

        const TypeInfo* meshRefType() {
            return &TypeRegistry::of<MeshRef>();
        }

        const TypeInfo* materialRefType() {
            return &TypeRegistry::of<MaterialRef>();
        }

        // One payload name per kind rather than one with a kind inside it, so
        // ImGui highlights only the slots a dragged asset can actually go
        // into. A target that lights up and then refuses the drop is worse
        // than one that never lights up.
        const char* assetPayloadName(AssetKind kind) {
            switch (kind) {
                case AssetKind::mesh:
                    return "EGE_ASSET_MESH";
                case AssetKind::material:
                    return "EGE_ASSET_MATERIAL";
                case AssetKind::texture:
                    return "EGE_ASSET_TEXTURE";
                case AssetKind::scene:
                    return "EGE_ASSET_SCENE";
                case AssetKind::unknown:
                    break;
            }
            return "EGE_ASSET";
        }

        // A slot holding an asset of one kind: what it points at now, a drop
        // target for the browser, and a way to empty it. Returns the id that
        // was dropped, if any.
        std::optional<Guid> drawAssetSlot(const char* label, AssetKind kind, Guid current) {
            const AssetDatabase& database = AssetDatabase::instance();
            const AssetRecord* record = database.find(current);

            std::string display = "(none)";
            if (record != nullptr) {
                display = record->name;
            } else if (!current.isNull()) {
                // Named rather than blanked: an asset missing from this
                // project is a different problem from a slot left empty, and
                // the reference is still there to be repaired.
                display = "missing " + current.toString().substr(0, 8);
            }

            std::optional<Guid> dropped;

            ImGui::PushID(label);
            ImGui::Button(display.c_str(), ImVec2{ImGui::CalcItemWidth(), 0.f});
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(assetPayloadName(kind))) {
                    Guid id{};
                    std::memcpy(&id, payload->Data, sizeof(id));
                    dropped = id;
                }
                ImGui::EndDragDropTarget();
            }
            if (ImGui::IsItemHovered() && record != nullptr) {
                ImGui::SetTooltip(
                    "%s\n%s",
                    record->builtin ? "built in" : record->path.string().c_str(),
                    record->id.toString().c_str());
            }
            if (!current.isNull()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) {
                    dropped = Guid{};
                }
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(label);
            ImGui::PopID();

            return dropped;
        }

        // One field, drawn by its reflected description, reporting whether the
        // user changed it. This function is the entire inspector "framework":
        // a new component type gets an editor UI by being reflected, with
        // nothing written here.
        bool drawField(const FieldInfo& field, void* instance) {
            void* address = field.addressIn(instance);
            const TypeInfo* type = &field.type();
            const std::string label{field.name()};
            bool changed = false;

            if (field.isReadOnly()) {
                ImGui::BeginDisabled();
            }

            if (type == floatType()) {
                auto* value = static_cast<float*>(address);
                if (field.hasRange()) {
                    changed = ImGui::SliderFloat(
                        label.c_str(), value, field.range().min, field.range().max);
                } else {
                    changed = ImGui::DragFloat(label.c_str(), value, 0.05f);
                }
            } else if (type == intType()) {
                changed = ImGui::DragInt(label.c_str(), static_cast<int*>(address));
            } else if (type == boolType()) {
                changed = ImGui::Checkbox(label.c_str(), static_cast<bool*>(address));
            } else if (type == vec2Type()) {
                changed = ImGui::DragFloat2(label.c_str(), static_cast<float*>(address), 0.05f);
            } else if (type == vec3Type()) {
                if (field.isColor()) {
                    changed = ImGui::ColorEdit3(label.c_str(), static_cast<float*>(address));
                } else {
                    changed = ImGui::DragFloat3(label.c_str(), static_cast<float*>(address), 0.05f);
                }
            } else if (type == vec4Type()) {
                if (field.isColor()) {
                    changed = ImGui::ColorEdit4(label.c_str(), static_cast<float*>(address));
                } else {
                    changed = ImGui::DragFloat4(label.c_str(), static_cast<float*>(address), 0.05f);
                }
            } else if (type == meshRefType()) {
                auto* ref = static_cast<MeshRef*>(address);
                if (const std::optional<Guid> dropped =
                        drawAssetSlot(label.c_str(), AssetKind::mesh, ref->id)) {
                    *ref = MeshRef{*dropped, AssetDatabase::instance().mesh(*dropped)};
                    changed = true;
                }
            } else if (type == materialRefType()) {
                auto* ref = static_cast<MaterialRef*>(address);
                if (const std::optional<Guid> dropped =
                        drawAssetSlot(label.c_str(), AssetKind::material, ref->id)) {
                    *ref = MaterialRef{*dropped, AssetDatabase::instance().material(*dropped)};
                    changed = true;
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

            return changed;
        }

        // Where Scene > Save and Scene > Load go. One well-known file until
        // there is a project window to open scenes by id from.
        constexpr const char* scenePath = "demo_scene.egescene";

        // The drag-and-drop payload the hierarchy moves entities with. Entity
        // handles are a packed integer, so the payload is the handle itself
        // rather than a pointer into anything that could be reallocated.
        constexpr const char* entityPayload = "EGE_ENTITY";

        // Severity by colour, so a warning is findable in a wall of info
        // without reading any of it.
        ImVec4 colorForLevel(LogLevel level) {
            switch (level) {
                case LogLevel::trace:
                    return ImVec4{0.50f, 0.50f, 0.55f, 1.f};
                case LogLevel::debug:
                    return ImVec4{0.60f, 0.70f, 0.85f, 1.f};
                case LogLevel::warn:
                    return ImVec4{1.00f, 0.80f, 0.35f, 1.f};
                case LogLevel::error:
                case LogLevel::critical:
                    return ImVec4{1.00f, 0.45f, 0.40f, 1.f};
                case LogLevel::info:
                    break;
            }
            return ImGui::GetStyleColorVec4(ImGuiCol_Text);
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
        ImGuizmo::BeginFrame();
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

    void EditorOverlay::buildUi(Context& context) {
        if (!overlayVisible) {
            return;
        }

        World& world = context.world;

        // Something under the cursor from the first frame: the panels are
        // the product here, and an empty inspector demonstrates nothing.
        if (selected.isNull() && !world.all().empty()) {
            selected = world.all().front().id();
        }

        drawMainMenuBar(context);
        beginInteractionUndo(world);

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

        drawViewportPanel(context);
        drawStatsPanel(context);
        drawHierarchyPanel(world);
        drawInspectorPanel(world);
        drawAssetBrowserPanel();
        drawConsolePanel();

        endInteractionUndo();
    }

    void EditorOverlay::beginInteractionUndo(World& world) {
        // Taken when the interaction starts, not when the first change is
        // seen: by then the widget has already written the new value, and the
        // memento would contain one frame of the change it is meant to undo.
        const bool interacting = ImGui::IsAnyItemActive() || ImGuizmo::IsUsing();
        if (interacting && !interactingLastFrame) {
            interactionScene = SceneSerializer::toString(world);
            interactionChangedSomething = false;
        }
        interactingLastFrame = interacting;
    }

    void EditorOverlay::endInteractionUndo() {
        const bool interacting = ImGui::IsAnyItemActive() || ImGuizmo::IsUsing();
        if (interacting || !interactionChangedSomething) {
            return;
        }

        // Pushed directly rather than through record(), which would capture
        // the current - already changed - state instead of the one held since
        // the interaction began.
        undo.recordCaptured(std::move(interactionScene), "edit");
        interactionScene.clear();
        interactionChangedSomething = false;
    }

    void EditorOverlay::applyUndo(World& world, bool redo) {
        // Restoring respawns everything, so the selection is re-found by name
        // rather than kept: the handle it holds will not survive.
        const std::string name = world.alive(selected) ? world.nameOf(selected) : std::string{};

        if (redo) {
            undo.redo(world);
        } else {
            undo.undo(world);
        }

        selected = name.empty() ? EntityId{} : world.findByName(name).id();
        pending = PendingEdit{};
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

        const ImGuiID console =
            ImGui::DockBuilderSplitNode(centre, ImGuiDir_Down, 0.28f, nullptr, &centre);

        ImGuiID inspector = right;
        const ImGuiID assets =
            ImGui::DockBuilderSplitNode(inspector, ImGuiDir_Down, 0.4f, nullptr, &inspector);

        ImGui::DockBuilderDockWindow("Scene", centre);
        ImGui::DockBuilderDockWindow("Console", console);
        ImGui::DockBuilderDockWindow("Hierarchy", hierarchy);
        ImGui::DockBuilderDockWindow("Stats", stats);
        ImGui::DockBuilderDockWindow("Inspector", inspector);
        ImGui::DockBuilderDockWindow("Assets", assets);
        ImGui::DockBuilderFinish(root);

        EGE_DEBUG("editor: built the default panel layout");
    }

    void EditorOverlay::drawMainMenuBar(Context& context) {
        if (!ImGui::BeginMainMenuBar()) {
            return;
        }

        if (ImGui::BeginMenu("Scene")) {
            // Disabled while playing: writing the running world over the file
            // would make Stop restore something nobody authored.
            ImGui::BeginDisabled(!context.playMode.isEditing());
            if (ImGui::MenuItem("Save")) {
                try {
                    SceneSerializer::save(context.world, scenePath);
                } catch (const std::exception& error) {
                    EGE_ERROR("{}", error.what());
                }
            }
            if (ImGui::MenuItem("Load")) {
                try {
                    SceneSerializer::load(context.world, scenePath);
                    selected = EntityId{};
                    // The history describes a world that no longer exists;
                    // undoing into it would silently replace the one just
                    // opened.
                    undo.clear();
                } catch (const std::exception& error) {
                    EGE_ERROR("{}", error.what());
                }
            }
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }

        // Undo is meaningless while playing: the world is running, and Stop is
        // already the way back to what was authored.
        const bool undoAvailable = context.playMode.isEditing();
        if (ImGui::BeginMenu("Edit")) {
            ImGui::BeginDisabled(!undoAvailable || !undo.canUndo());
            if (ImGui::MenuItem(("Undo " + undo.undoLabel()).c_str(), "Ctrl+Z")) {
                applyUndo(context.world, false);
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!undoAvailable || !undo.canRedo());
            if (ImGui::MenuItem(("Redo " + undo.redoLabel()).c_str(), "Ctrl+Shift+Z")) {
                applyUndo(context.world, true);
            }
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }

        // The shortcuts, checked here rather than in the menu bodies, which
        // only run while the menu is open.
        if (undoAvailable && !ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z) ||
                ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y)) {
                applyUndo(context.world, true);
            } else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) {
                applyUndo(context.world, false);
            }
        }

        // Play controls belong here rather than over the viewport: they change
        // what the whole editor is doing, not what the scene view shows.
        ImGui::Separator();
        PlayMode& play = context.playMode;
        if (play.isEditing()) {
            if (ImGui::MenuItem("Play")) {
                play.play(context.world);
            }
        } else if (play.isPlaying()) {
            if (ImGui::MenuItem("Pause")) {
                play.pause();
            }
        } else if (ImGui::MenuItem("Resume")) {
            play.resume();
        }

        if (ImGui::MenuItem("Step")) {
            play.step(context.world);
        }

        ImGui::BeginDisabled(play.isEditing());
        if (ImGui::MenuItem("Stop")) {
            play.stop(context.world);
            // Restoring respawns everything, so whatever was selected is a
            // stale handle now. The history survives: Stop returns the world
            // to what it was when Play was pressed, which is the state the
            // top of the undo stack was recorded against.
            selected = EntityId{};
        }
        ImGui::EndDisabled();

        if (!play.isEditing()) {
            ImGui::Separator();
            ImGui::TextColored(
                ImVec4{1.f, 0.8f, 0.35f, 1.f}, "%s", play.isPaused() ? "paused" : "playing");
        }

        ImGui::EndMainMenuBar();
    }

    void EditorOverlay::drawViewportPanel(Context& context) {
        World& world = context.world;
        const Camera& camera = context.camera;
        // No padding: the image is the panel, and a strip of window background
        // around the scene reads as a rendering bug.
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.f, 0.f});
        ImGui::Begin("Scene");

        const ImVec2 available = ImGui::GetContentRegionAvail();
        const ImVec2 imageOrigin = ImGui::GetCursorScreenPos();
        // ImGui lays out in points; the target is allocated in pixels, and on
        // a scaled display those are not the same number.
        const ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
        requestedViewportExtent = {
            static_cast<uint32_t>(std::max(available.x * scale.x, 1.f)),
            static_cast<uint32_t>(std::max(available.y * scale.y, 1.f))};

        if (viewport->valid()) {
            ImGui::Image(reinterpret_cast<ImTextureID>(viewport->textureSet()), available);
        }

        // Onto the scene window's own draw list, in the image's rectangle, so
        // the handles land over the render rather than over the whole window.
        ImGuizmo::SetOrthographic(false);
        // Off, so an arrow always points along the positive axis. Left on,
        // ImGuizmo turns axes towards the camera to make them easier to grab -
        // which is a reasonable default, and a poor one for a scene whose up
        // is -Y, where an arrow pointing up could mean either sign.
        ImGuizmo::AllowAxisFlip(false);
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(imageOrigin.x, imageOrigin.y, available.x, available.y);
        drawGizmo(world, camera);

        // Floating over the top-left of the image rather than above it: a
        // toolbar in the layout would take a strip out of the render.
        ImGui::SetCursorScreenPos(ImVec2{imageOrigin.x + 8.f, imageOrigin.y + 8.f});
        drawGizmoToolbar();

        // Whether the camera flies. Child windows count so that anything drawn
        // over the scene - the toolbar, a future stats readout - is still the
        // scene as far as this is concerned.
        viewportHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

        ImGui::End();
        ImGui::PopStyleVar();
    }

    void EditorOverlay::drawGizmoToolbar() {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{6.f, 4.f});
        ImGui::BeginChild(
            "gizmoToolbar",
            ImVec2{0.f, 0.f},
            ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_FrameStyle);

        const auto modeButton = [this](const char* label, GizmoMode mode) {
            const bool active = gizmoMode == mode;
            if (active) {
                ImGui::PushStyleColor(
                    ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            }
            if (ImGui::Button(label)) {
                gizmoMode = mode;
            }
            if (active) {
                ImGui::PopStyleColor();
            }
            ImGui::SameLine();
        };

        modeButton("Move", GizmoMode::translate);
        modeButton("Rotate", GizmoMode::rotate);
        modeButton("Scale", GizmoMode::scale);

        // Local space is meaningless for a non-uniform scale gizmo's own axes,
        // and ImGuizmo forces world there anyway; saying so beats a control
        // that silently does nothing.
        ImGui::BeginDisabled(gizmoMode == GizmoMode::scale);
        bool worldSpace = gizmoWorldSpace;
        if (ImGui::Checkbox("world", &worldSpace)) {
            gizmoWorldSpace = worldSpace;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Checkbox("snap", &gizmoSnaps);

        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    void EditorOverlay::drawGizmo(World& world, const Camera& camera) {
        if (!world.alive(selected) || world.find<Transform>(selected) == nullptr) {
            return;
        }

        glm::mat4 view = camera.getView();
        // ImGuizmo projects with OpenGL's convention, where clip-space +Y is
        // the top of the screen; Vulkan's is the bottom. Without the flip the
        // handles are drawn mirrored about the horizon and drag the wrong way.
        glm::mat4 projection = camera.getProjection();
        projection[1][1] *= -1.f;

        // A gizmo moves the entity where it appears to be, which under a
        // parent is not what its own transform says. Manipulate in world space
        // and divide the parent back out afterwards.
        const EntityId parent = hierarchy::parentOf(world, selected);
        const glm::mat4 parentMatrix =
            parent.isNull() ? glm::mat4{1.f} : hierarchy::worldMatrix(world, parent);

        // Re-found after worldMatrix, which may attach a Hierarchy and grow a
        // pool; the earlier pointer could no longer be valid.
        Transform* transform = world.find<Transform>(selected);
        if (transform == nullptr) {
            return;
        }
        glm::mat4 worldMatrix = parentMatrix * transform->mat4();

        ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
        glm::vec3 snap{0.25f};
        switch (gizmoMode) {
            case GizmoMode::translate:
                break;
            case GizmoMode::rotate:
                operation = ImGuizmo::ROTATE;
                snap = glm::vec3{15.f};
                break;
            case GizmoMode::scale:
                operation = ImGuizmo::SCALE;
                snap = glm::vec3{0.1f};
                break;
        }

        const bool moved = ImGuizmo::Manipulate(
            &view[0][0],
            &projection[0][0],
            operation,
            gizmoWorldSpace ? ImGuizmo::WORLD : ImGuizmo::LOCAL,
            &worldMatrix[0][0],
            nullptr,
            gizmoSnaps ? &snap[0] : nullptr);

        if (!moved) {
            return;
        }

        *transform = Transform::fromMatrix(glm::inverse(parentMatrix) * worldMatrix);
        hierarchy::markDirty(world, selected);
        interactionChangedSomething = true;
    }

    void EditorOverlay::render(VkCommandBuffer commandBuffer) {
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
    }

    void EditorOverlay::drawStatsPanel(const Context& context) {
        const PbrRenderSystem::Stats& stats = context.stats;
        ImGui::Begin("Stats");
        ImGui::Text(
            "%.2f ms  (%.0f fps)",
            static_cast<double>(context.frameTime) * 1000.0,
            1.0 / static_cast<double>(context.frameTime));
        ImGui::Separator();
        // Culling numbers describe the previous frame: the render that
        // produces them runs after the UI is declared.
        ImGui::Text("candidates  %zu", stats.candidates);
        ImGui::Text("culled      %zu", stats.culled);
        ImGui::Text("drawn       %zu", stats.drawn);
        ImGui::Text("mat binds   %zu", stats.materialBinds);
        if (!context.playMode.isEditing()) {
            ImGui::Separator();
            ImGui::Text("snapshot    %zu B", context.playMode.snapshotSize());
        }
        ImGui::End();
    }

    void EditorOverlay::drawAssetBrowserPanel() {
        ImGui::Begin("Assets");

        const AssetDatabase& database = AssetDatabase::instance();

        ImGui::SetNextItemWidth(-1.f);
        ImGui::InputTextWithHint("##assetFilter", "filter", assetFilter, sizeof(assetFilter));
        const std::string needle{assetFilter};
        ImGui::Separator();

        ImGui::BeginChild("assetList");
        constexpr std::array<AssetKind, 4> kinds{
            AssetKind::mesh, AssetKind::material, AssetKind::texture, AssetKind::scene};
        for (const AssetKind kind : kinds) {
            std::vector<const AssetRecord*> matching;
            for (const AssetRecord& record : database.all()) {
                if (record.kind != kind) {
                    continue;
                }
                if (!needle.empty() && record.name.find(needle) == std::string::npos) {
                    continue;
                }
                matching.push_back(&record);
            }
            if (matching.empty()) {
                continue;
            }

            // Sorted by name rather than by discovery order: a browser whose
            // contents move when an unrelated file is added is unusable.
            std::sort(
                matching.begin(), matching.end(), [](const AssetRecord* a, const AssetRecord* b) {
                    return a->name < b->name;
                });

            const std::string header =
                std::string{assetKindName(kind)} + " (" + std::to_string(matching.size()) + ")";
            if (!ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                continue;
            }

            for (const AssetRecord* record : matching) {
                ImGui::PushID(static_cast<int>(record->id.low() & 0x7fffffff));
                ImGui::Selectable(record->name.c_str());

                // Dragging is what the browser is for: the inspector's slots
                // are the other end of this.
                if (ImGui::BeginDragDropSource()) {
                    const Guid id = record->id;
                    ImGui::SetDragDropPayload(assetPayloadName(record->kind), &id, sizeof(id));
                    ImGui::TextUnformatted(record->name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "%s\n%s",
                        record->builtin ? "built in" : record->path.string().c_str(),
                        record->id.toString().c_str());
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        ImGui::End();
    }

    void EditorOverlay::drawConsolePanel() {
        ImGui::Begin("Console");

        if (ImGui::Button("Clear")) {
            LogBuffer::instance().clear();
        }
        ImGui::SameLine();

        constexpr std::array<LogLevel, 5> selectableLevels{
            LogLevel::trace, LogLevel::debug, LogLevel::info, LogLevel::warn, LogLevel::error};
        ImGui::SetNextItemWidth(90.f);
        if (ImGui::BeginCombo("##level", logLevelName(consoleMinimumLevel))) {
            for (LogLevel level : selectableLevels) {
                if (ImGui::Selectable(logLevelName(level), level == consoleMinimumLevel)) {
                    consoleMinimumLevel = level;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-110.f);
        ImGui::InputTextWithHint("##filter", "filter", consoleFilter, sizeof(consoleFilter));
        ImGui::SameLine();
        ImGui::Checkbox("follow", &consoleFollowsTail);
        ImGui::Separator();

        const std::vector<LogBuffer::Record> records = LogBuffer::instance().snapshot();
        const std::string needle{consoleFilter};

        ImGui::BeginChild(
            "log", ImVec2{0.f, 0.f}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
        for (const LogBuffer::Record& record : records) {
            if (record.level < consoleMinimumLevel) {
                continue;
            }
            if (!needle.empty() && record.message.find(needle) == std::string::npos) {
                continue;
            }
            ImGui::PushStyleColor(ImGuiCol_Text, colorForLevel(record.level));
            ImGui::TextUnformatted(record.message.c_str());
            ImGui::PopStyleColor();
        }

        // Only while already at the bottom, so scrolling back to read
        // something is not undone by the next line that arrives.
        const std::uint64_t revision = LogBuffer::instance().revision();
        if (consoleFollowsTail && revision != lastSeenLogRevision) {
            ImGui::SetScrollHereY(1.f);
        }
        lastSeenLogRevision = revision;
        ImGui::EndChild();

        ImGui::End();
    }

    void EditorOverlay::drawHierarchyPanel(World& world) {
        ImGui::Begin("Hierarchy");

        if (ImGui::Button("New")) {
            pending = PendingEdit{PendingEdit::Kind::spawn, EntityId{}, EntityId{}};
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!world.alive(selected));
        if (ImGui::Button("Delete")) {
            pending = PendingEdit{PendingEdit::Kind::despawn, selected, EntityId{}};
        }
        ImGui::EndDisabled();
        ImGui::Separator();

        // The tree lives in a child filling the rest of the panel so that the
        // empty space below the last node is still part of it: dropping there
        // is how an entity is dragged back out to the root.
        ImGui::BeginChild("tree");
        for (Entity entity : world.all()) {
            // Roots only; children are reached through their parent.
            const Hierarchy* links = world.find<Hierarchy>(entity.id());
            if (links != nullptr && !links->parent.isNull()) {
                continue;
            }
            drawEntityNode(world, entity.id());
        }
        ImGui::EndChild();

        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(entityPayload)) {
                EntityId dropped{};
                std::memcpy(&dropped, payload->Data, sizeof(dropped));
                pending = PendingEdit{PendingEdit::Kind::reparent, dropped, EntityId{}};
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::End();

        applyPendingEdit(world);
    }

    void EditorOverlay::applyPendingEdit(World& world) {
        const PendingEdit edit = pending;
        pending = PendingEdit{};

        if (edit.kind == PendingEdit::Kind::none) {
            return;
        }

        const char* label = "edit";
        switch (edit.kind) {
            case PendingEdit::Kind::spawn:
                label = "spawn";
                break;
            case PendingEdit::Kind::despawn:
                label = "delete";
                break;
            case PendingEdit::Kind::reparent:
                label = "reparent";
                break;
            case PendingEdit::Kind::none:
                break;
        }
        // Before the change, so the memento holds the state to come back to.
        undo.record(world, label);

        switch (edit.kind) {
            case PendingEdit::Kind::none:
                return;

            case PendingEdit::Kind::spawn: {
                // A Transform from the start: an entity with no place in space
                // is not what anyone means by "new entity", and the inspector
                // would open on nothing.
                Entity created = world.spawn("Entity");
                created.attach<Transform>(Transform{});
                if (world.alive(edit.subject)) {
                    hierarchy::setParent(world, created.id(), edit.subject);
                }
                selected = created.id();
                break;
            }

            case PendingEdit::Kind::despawn:
                if (world.alive(edit.subject)) {
                    // Subtree, not just the entity: children left behind would
                    // be orphaned at the root, which is never what deleting a
                    // parent in a scene tree is taken to mean.
                    hierarchy::despawnSubtree(world, edit.subject);
                }
                break;

            case PendingEdit::Kind::reparent:
                if (!world.alive(edit.subject)) {
                    break;
                }
                // A null target unparents. setParent refuses cycles rather
                // than building a tree the next traversal would hang on, which
                // dragging a parent onto its own child does constantly.
                if (!hierarchy::setParent(world, edit.subject, edit.target)) {
                    EGE_WARN(
                        "cannot parent '{}' under '{}': it is already an ancestor",
                        world.nameOf(edit.subject),
                        world.nameOf(edit.target));
                }
                break;
        }

        if (!world.alive(selected)) {
            selected = EntityId{};
        }
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

        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload(entityPayload, &entity, sizeof(entity));
            ImGui::TextUnformatted(name.empty() ? "(unnamed)" : name.c_str());
            ImGui::EndDragDropSource();
        }

        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(entityPayload)) {
                EntityId dropped{};
                std::memcpy(&dropped, payload->Data, sizeof(dropped));
                pending = PendingEdit{PendingEdit::Kind::reparent, dropped, entity};
            }
            ImGui::EndDragDropTarget();
        }

        if (ImGui::BeginPopupContextItem()) {
            selected = entity;
            if (ImGui::MenuItem("Add child")) {
                pending = PendingEdit{PendingEdit::Kind::spawn, entity, EntityId{}};
            }
            if (ImGui::MenuItem("Delete")) {
                pending = PendingEdit{PendingEdit::Kind::despawn, entity, EntityId{}};
            }
            ImGui::EndPopup();
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
        bool edited = false;
        std::string removing;
        for (const ComponentRegistry::Entry& entry : ComponentRegistry::instance().all()) {
            if (!entry.has(world, selected)) {
                continue;
            }
            const bool open =
                ImGui::CollapsingHeader(entry.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen);

            // Detaching is deferred past the loop for the same reason
            // structural edits are deferred past the hierarchy walk: the
            // component being drawn lives in the pool this would rearrange.
            if (ImGui::BeginPopupContextItem(entry.name.c_str())) {
                if (ImGui::MenuItem("Remove component")) {
                    removing = entry.name;
                }
                ImGui::EndPopup();
            }

            if (!open) {
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
                edited = drawField(field, component) || edited;
            }
            ImGui::PopID();
        }

        ImGui::PopItemWidth();

        // Attaching is deferred past the draw loop too, so that the list being
        // iterated and the list being added to are never the same list mid-walk.
        std::string adding;
        ImGui::Separator();
        if (ImGui::Button("Add Component")) {
            ImGui::OpenPopup("addComponent");
        }
        if (ImGui::BeginPopup("addComponent")) {
            bool anyAvailable = false;
            for (const ComponentRegistry::Entry& entry : ComponentRegistry::instance().all()) {
                if (entry.has(world, selected)) {
                    continue;
                }
                anyAvailable = true;
                if (ImGui::MenuItem(entry.name.c_str())) {
                    adding = entry.name;
                }
            }
            if (!anyAvailable) {
                ImGui::TextDisabled("(nothing left to add)");
            }
            ImGui::EndPopup();
        }

        ImGui::End();

        if (!adding.empty()) {
            if (const ComponentRegistry::Entry* entry =
                    ComponentRegistry::instance().find(adding)) {
                undo.record(world, "add " + adding);
                entry->attach(world, selected);
            }
        }
        if (!removing.empty()) {
            if (const ComponentRegistry::Entry* entry =
                    ComponentRegistry::instance().find(removing)) {
                undo.record(world, "remove " + removing);
                entry->detach(world, selected);
            }
        }

        // A transform edited here is a transform whose cached world matrix is
        // now wrong - and, for a parent, so is every descendant's. Without
        // this the numbers in the inspector change and nothing moves, which is
        // the bug the viewport made impossible to miss.
        if (edited || !adding.empty() || !removing.empty()) {
            hierarchy::markDirty(world, selected);
        }
        interactionChangedSomething = interactionChangedSomething || edited;
    }

}  // namespace ege
