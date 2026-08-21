#pragma once

#include "reflect/BuiltinTypes.hpp"
#include "rhi/Descriptors.hpp"
#include "rhi/Texture.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <string>

namespace ege {

    // Metallic-roughness material, the glTF 2.0 model.
    //
    // Chosen over specular-glossiness because it is what glTF uses, which is
    // the import path Phase 4 targets, and because it has fewer ways to
    // express a physically impossible surface.
    struct MaterialProperties {
        glm::vec4 baseColorFactor{1.f, 1.f, 1.f, 1.f};
        glm::vec3 emissiveFactor{0.f};
        float metallicFactor = 0.f;
        float roughnessFactor = 0.5f;
        float normalScale = 1.f;
        float occlusionStrength = 1.f;
        float alphaCutoff = 0.5f;
    };

    // A material binds its own descriptor set, holding the four texture slots.
    //
    // Every slot is always bound, using a 1x1 fallback where no map is
    // supplied, so the shader never branches on whether a texture exists. A
    // branch there costs more than the sample it avoids, and an unbound
    // descriptor is a validation error rather than a default.
    class Material {
    public:
        // No device: a material is a descriptor set and four texture
        // references, and every device call it needs is one the pool or the
        // layout already owns.
        Material(DescriptorPool& pool, DescriptorSetLayout& layout);

        Material(const Material&) = delete;
        Material& operator=(const Material&) = delete;

        // Slots left null keep their fallback.
        void setBaseColor(std::shared_ptr<Texture> texture);
        void setNormalMap(std::shared_ptr<Texture> texture);
        // Metallic in blue, roughness in green, matching glTF's packing.
        void setMetallicRoughness(std::shared_ptr<Texture> texture);
        void setEmissive(std::shared_ptr<Texture> texture);

        // Rebuilds the descriptor set. Call after changing any slot.
        void updateDescriptorSet();

        // Takes on another material's properties and texture slots, keeping
        // this object's identity. What hot reload needs: everything already
        // pointing at this material sees the new values, with no reference to
        // re-resolve and no descriptor set to rebind anywhere else.
        void adoptFrom(const Material& other);

        VkDescriptorSet descriptorSet() const { return set; }

        MaterialProperties properties{};

        // The layout every material's set is allocated from. One layout for all
        // materials so the pipeline is bound once rather than per material.
        static std::unique_ptr<DescriptorSetLayout> createLayout(Device& device);

        // Fallbacks, created once and shared. White base colour, flat normal,
        // fully rough non-metal, black emissive.
        static void createDefaults(Device& device);
        static void destroyDefaults();

    private:
        DescriptorPool& pool;
        DescriptorSetLayout& layout;
        VkDescriptorSet set = VK_NULL_HANDLE;

        std::shared_ptr<Texture> baseColor;
        std::shared_ptr<Texture> normalMap;
        std::shared_ptr<Texture> metallicRoughness;
        std::shared_ptr<Texture> emissive;
    };

}  // namespace ege

EGE_REFLECT(ege::MaterialProperties)
EGE_FIELD(baseColorFactor).asColor().tooltip("Multiplied with the base colour texture");
EGE_FIELD(emissiveFactor).asColor();
EGE_FIELD(metallicFactor).range(0.f, 1.f).tooltip("0 is dielectric, 1 is metal");
EGE_FIELD(roughnessFactor).range(0.f, 1.f);
EGE_FIELD(normalScale).range(0.f, 4.f);
EGE_FIELD(occlusionStrength).range(0.f, 1.f);
EGE_FIELD(alphaCutoff).range(0.f, 1.f);
EGE_REFLECT_END()
