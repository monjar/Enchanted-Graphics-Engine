#pragma once

#include "rhi/Descriptors.hpp"
#include "rhi/Device.hpp"

#include <memory>
#include <vector>

namespace ege {

    // Everything image-based lighting needs, generated once at startup:
    //
    //   environment  - a procedural HDR sky in a mipmapped cubemap
    //   irradiance   - its cosine convolution, for diffuse ambient
    //   prefiltered  - GGX convolutions per roughness in the mip chain,
    //                  for specular reflections
    //   BRDF LUT     - the split-sum scale/bias table, environment-independent
    //
    // Each map is produced by fullscreen-triangle passes into cube faces via
    // dynamic rendering - the same machinery the frame graph uses per frame,
    // driven directly here because this is one-time resource initialisation,
    // like texture upload, not part of any frame.
    //
    // The cubemap plumbing lives inside this class for now; it migrates into
    // rhi/Texture when cubemaps get a second user (Phase 4 point-light
    // shadows want one).
    class EnvironmentLighting {
    public:
        explicit EnvironmentLighting(Device& device);
        ~EnvironmentLighting();

        EnvironmentLighting(const EnvironmentLighting&) = delete;
        EnvironmentLighting& operator=(const EnvironmentLighting&) = delete;

        VkDescriptorImageInfo environmentInfo() const;
        VkDescriptorImageInfo irradianceInfo() const;
        VkDescriptorImageInfo prefilteredInfo() const;
        VkDescriptorImageInfo brdfLutInfo() const;

    private:
        struct CubeMap {
            VkImage image = VK_NULL_HANDLE;
            VmaAllocation allocation = VK_NULL_HANDLE;
            VkImageView cubeView = VK_NULL_HANDLE;
            // One view per (mip, face), for rendering into: mip * 6 + face.
            std::vector<VkImageView> faceViews;
            uint32_t size = 0;
            uint32_t mipLevels = 1;
        };

        CubeMap createCubeMap(uint32_t size, uint32_t mipLevels, VkImageUsageFlags usage);
        void destroyCubeMap(CubeMap& cube);

        void createSampler();
        void createDescriptors();

        void generateEnvironment();
        void generateIrradiance();
        void generatePrefiltered();
        void generateBrdfLut();

        Device& device;

        CubeMap environment;
        CubeMap irradiance;
        CubeMap prefiltered;

        VkImage brdfLut = VK_NULL_HANDLE;
        VmaAllocation brdfLutAllocation = VK_NULL_HANDLE;
        VkImageView brdfLutView = VK_NULL_HANDLE;

        // One clamped linear sampler serves every map.
        VkSampler sampler = VK_NULL_HANDLE;

        // The convolution passes sample the environment map.
        std::unique_ptr<DescriptorSetLayout> samplerSetLayout;
        std::unique_ptr<DescriptorPool> descriptorPool;
        VkDescriptorSet environmentSet = VK_NULL_HANDLE;

        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    };

}  // namespace ege
