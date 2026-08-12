#include "editor/EditorOverlay.hpp"

#include "core/Log.hpp"
#include "reflect/BuiltinTypes.hpp"
#include "scene/ComponentRegistry.hpp"
#include "scene/Components.hpp"

#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>

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
        // draws; the pool must allow freeing.
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 16;
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

        EGE_INFO("Editor overlay ready (F1 toggles)");
    }

    EditorOverlay::~EditorOverlay() {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        vkDestroyDescriptorPool(device.device(), imguiPool, nullptr);
    }

    void EditorOverlay::beginFrame() {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    bool EditorOverlay::wantsInput() const {
        const ImGuiIO& io = ImGui::GetIO();
        return overlayVisible && (io.WantCaptureMouse || io.WantCaptureKeyboard);
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

        drawStatsPanel(stats, frameTime);
        drawHierarchyPanel(world);
        drawInspectorPanel(world);
    }

    void EditorOverlay::render(VkCommandBuffer commandBuffer) {
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
    }

    void EditorOverlay::drawStatsPanel(const PbrRenderSystem::Stats& stats, float frameTime) {
        // Sensible places on a fresh machine; once the user drags anything,
        // imgui.ini remembers and these stop applying.
        ImGui::SetNextWindowPos(ImVec2{10.f, 420.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2{170.f, 130.f}, ImGuiCond_FirstUseEver);
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
        ImGui::SetNextWindowPos(ImVec2{10.f, 10.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2{190.f, 400.f}, ImGuiCond_FirstUseEver);
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
        ImGui::SetNextWindowPos(ImVec2{540.f, 10.f}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2{250.f, 320.f}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Inspector");

        if (selected.isNull() || !world.alive(selected)) {
            ImGui::TextDisabled("select an entity");
            ImGui::End();
            return;
        }

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

        ImGui::End();
    }

}  // namespace ege
