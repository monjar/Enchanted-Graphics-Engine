#pragma once

#include "rhi/Device.hpp"

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <vector>

namespace ege {

    // How a texture is uploaded and sampled.
    //
    // At namespace scope rather than nested in Texture: a nested type's default
    // member initializers are not available inside the enclosing class body, so
    // `const Config& = Config{}` as a default argument would not compile.
    struct TextureConfig {
        // sRGB for anything the eye reads as colour - base colour, emissive.
        // Linear for data - normals, roughness, metallic, occlusion -
        // because applying the sRGB transfer curve to a value that is not a
        // colour corrupts it.
        bool srgb = true;
        bool generateMipmaps = true;
        VkFilter minFilter = VK_FILTER_LINEAR;
        VkFilter magFilter = VK_FILTER_LINEAR;
        VkSamplerAddressMode addressMode = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        // Clamped to the device maximum; 0 disables.
        float maxAnisotropy = 16.f;
    };

    // A sampled 2D image on the GPU.
    //
    // Device has had createImageWithInfo and copyBufferToImage since the
    // tutorial code, both unused: the engine requested samplerAnisotropy at
    // device creation and then never created a sampler. This is what those
    // helpers were for.
    class Texture {
    public:
        // Decodes with stb_image. Throws if the file cannot be read.
        static std::unique_ptr<Texture> fromFile(
            Device& device, const std::string& path, const TextureConfig& config = TextureConfig{});

        // Builds from tightly packed RGBA8 pixels.
        static std::unique_ptr<Texture> fromPixels(
            Device& device,
            const void* pixels,
            uint32_t width,
            uint32_t height,
            const TextureConfig& config = TextureConfig{});

        // A 1x1 texture of a single colour. Used for material slots with no map
        // bound, so the shader never needs a branch for a missing texture.
        static std::unique_ptr<Texture> solidColor(
            Device& device, glm::vec4 color, const TextureConfig& config = TextureConfig{});

        ~Texture();

        Texture(const Texture&) = delete;
        Texture& operator=(const Texture&) = delete;

        VkDescriptorImageInfo descriptorInfo() const;

        uint32_t width() const { return imageWidth; }

        uint32_t height() const { return imageHeight; }

        uint32_t mipLevels() const { return imageMipLevels; }

        VkImageView view() const { return imageView; }

        VkSampler sampler() const { return imageSampler; }

    private:
        Texture(
            Device& device,
            const void* pixels,
            uint32_t w,
            uint32_t h,
            const TextureConfig& config);

        void createImage(const void* pixels, const TextureConfig& config);
        void createView(const TextureConfig& config);
        void createSampler(const TextureConfig& config);

        // Generates the mip chain with successive blits, transitioning each
        // level as it goes. Leaves the whole image in
        // SHADER_READ_ONLY_OPTIMAL.
        void generateMipmaps();

        void transitionLayout(
            VkCommandBuffer commandBuffer,
            VkImageLayout oldLayout,
            VkImageLayout newLayout,
            uint32_t baseMipLevel,
            uint32_t levelCount);

        Device& device;

        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        VkImageView imageView = VK_NULL_HANDLE;
        VkSampler imageSampler = VK_NULL_HANDLE;

        uint32_t imageWidth = 0;
        uint32_t imageHeight = 0;
        uint32_t imageMipLevels = 1;
        VkFormat imageFormat = VK_FORMAT_R8G8B8A8_SRGB;
    };

}  // namespace ege
