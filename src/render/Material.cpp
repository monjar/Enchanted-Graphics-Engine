#include "render/Material.hpp"

#include "core/Assert.hpp"

namespace ege {

    namespace {

        std::shared_ptr<Texture> defaultBaseColor;
        std::shared_ptr<Texture> defaultNormal;
        std::shared_ptr<Texture> defaultMetallicRoughness;
        std::shared_ptr<Texture> defaultEmissive;

    }  // namespace

    std::unique_ptr<DescriptorSetLayout> Material::createLayout(Device& device) {
        return DescriptorSetLayout::Builder(device)
            .addBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
            .addBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
            .addBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
            .addBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT)
            .build();
    }

    void Material::createDefaults(Device& device) {
        if (defaultBaseColor) {
            return;
        }

        // Base colour and emissive are colours, so sRGB. Normal and
        // metallic-roughness are data, so linear - running a normal through the
        // sRGB curve tilts every one of them.
        TextureConfig colorConfig{};
        colorConfig.srgb = true;
        TextureConfig dataConfig{};
        dataConfig.srgb = false;

        defaultBaseColor = Texture::solidColor(device, glm::vec4{1.f}, colorConfig);
        // (0.5, 0.5, 1) unpacks to the +Z tangent-space normal, meaning "flat".
        defaultNormal = Texture::solidColor(device, glm::vec4{0.5f, 0.5f, 1.f, 1.f}, dataConfig);
        // White, so that every channel is the multiplicative identity and the
        // material's factors pass through unchanged. An earlier attempt used
        // (1, 1, 0) meaning "fully rough, non-metal", which looks reasonable
        // but is wrong: metallic reads from blue and is multiplied by
        // metallicFactor, so a zero here forced every material to be a
        // dielectric no matter what its factor said.
        defaultMetallicRoughness = Texture::solidColor(device, glm::vec4{1.f}, dataConfig);
        defaultEmissive = Texture::solidColor(device, glm::vec4{0.f, 0.f, 0.f, 1.f}, colorConfig);
    }

    void Material::destroyDefaults() {
        defaultBaseColor.reset();
        defaultNormal.reset();
        defaultMetallicRoughness.reset();
        defaultEmissive.reset();
    }

    Material::Material(Device& deviceRef, DescriptorPool& poolRef, DescriptorSetLayout& layoutRef)
        : device{deviceRef}, pool{poolRef}, layout{layoutRef} {
        EGE_VERIFY(
            defaultBaseColor != nullptr,
            "Material::createDefaults must run before any material is created");

        baseColor = defaultBaseColor;
        normalMap = defaultNormal;
        metallicRoughness = defaultMetallicRoughness;
        emissive = defaultEmissive;

        updateDescriptorSet();
    }

    void Material::setBaseColor(std::shared_ptr<Texture> texture) {
        baseColor = texture ? std::move(texture) : defaultBaseColor;
    }

    void Material::setNormalMap(std::shared_ptr<Texture> texture) {
        normalMap = texture ? std::move(texture) : defaultNormal;
    }

    void Material::setMetallicRoughness(std::shared_ptr<Texture> texture) {
        metallicRoughness = texture ? std::move(texture) : defaultMetallicRoughness;
    }

    void Material::setEmissive(std::shared_ptr<Texture> texture) {
        emissive = texture ? std::move(texture) : defaultEmissive;
    }

    void Material::adoptFrom(const Material& other) {
        properties = other.properties;
        baseColor = other.baseColor;
        normalMap = other.normalMap;
        metallicRoughness = other.metallicRoughness;
        emissive = other.emissive;
        updateDescriptorSet();
    }

    void Material::updateDescriptorSet() {
        VkDescriptorImageInfo baseInfo = baseColor->descriptorInfo();
        VkDescriptorImageInfo normalInfo = normalMap->descriptorInfo();
        VkDescriptorImageInfo metallicInfo = metallicRoughness->descriptorInfo();
        VkDescriptorImageInfo emissiveInfo = emissive->descriptorInfo();

        DescriptorWriter{layout, pool}
            .writeImage(0, &baseInfo)
            .writeImage(1, &normalInfo)
            .writeImage(2, &metallicInfo)
            .writeImage(3, &emissiveInfo)
            .build(set);
    }

}  // namespace ege
