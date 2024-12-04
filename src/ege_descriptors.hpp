#pragma once

#include "ege_engine_device.hpp"

// std
#include <memory>
#include <unordered_map>
#include <vector>

namespace ege {

    class EgeDescriptorSetLayout {
    public:
        class Builder {
        public:
            Builder(EgeDevice& egeDevice) : egeDevice{ egeDevice } {}

            Builder& addBinding(
                uint32_t binding,
                VkDescriptorType descriptorType,
                VkShaderStageFlags stageFlags,
                uint32_t count = 1);
            std::unique_ptr<EgeDescriptorSetLayout> build() const;

        private:
            EgeDevice& egeDevice;
            std::unordered_map<uint32_t, VkDescriptorSetLayoutBinding> bindings{};
        };

        EgeDescriptorSetLayout(
            EgeDevice& egeDevice, std::unordered_map<uint32_t, VkDescriptorSetLayoutBinding> bindings);
        ~EgeDescriptorSetLayout();
        EgeDescriptorSetLayout(const EgeDescriptorSetLayout&) = delete;
        EgeDescriptorSetLayout& operator=(const EgeDescriptorSetLayout&) = delete;

        VkDescriptorSetLayout getDescriptorSetLayout() const { return descriptorSetLayout; }

    private:
        EgeDevice& egeDevice;
        VkDescriptorSetLayout descriptorSetLayout;
        std::unordered_map<uint32_t, VkDescriptorSetLayoutBinding> bindings;

        friend class EgeDescriptorWriter;
    };

    class EgeDescriptorPool {
    public:
        class Builder {
        public:
            Builder(EgeDevice& egeDevice) : egeDevice{ egeDevice } {}

            Builder& addPoolSize(VkDescriptorType descriptorType, uint32_t count);
            Builder& setPoolFlags(VkDescriptorPoolCreateFlags flags);
            Builder& setMaxSets(uint32_t count);
            std::unique_ptr<EgeDescriptorPool> build() const;

        private:
            EgeDevice& egeDevice;
            std::vector<VkDescriptorPoolSize> poolSizes{};
            uint32_t maxSets = 1000;
            VkDescriptorPoolCreateFlags poolFlags = 0;
        };

        EgeDescriptorPool(
            EgeDevice& egeDevice,
            uint32_t maxSets,
            VkDescriptorPoolCreateFlags poolFlags,
            const std::vector<VkDescriptorPoolSize>& poolSizes);
        ~EgeDescriptorPool();
        EgeDescriptorPool(const EgeDescriptorPool&) = delete;
        EgeDescriptorPool& operator=(const EgeDescriptorPool&) = delete;

        bool allocateDescriptor(
            const VkDescriptorSetLayout descriptorSetLayout, VkDescriptorSet& descriptor) const;

        void freeDescriptors(std::vector<VkDescriptorSet>& descriptors) const;

        void resetPool();

    private:
        EgeDevice& egeDevice;
        VkDescriptorPool descriptorPool;

        friend class EgeDescriptorWriter;
    };

    class EgeDescriptorWriter {
    public:
        EgeDescriptorWriter(EgeDescriptorSetLayout& setLayout, EgeDescriptorPool& pool);

        EgeDescriptorWriter& writeBuffer(uint32_t binding, VkDescriptorBufferInfo* bufferInfo);
        EgeDescriptorWriter& writeImage(uint32_t binding, VkDescriptorImageInfo* imageInfo);

        bool build(VkDescriptorSet& set);
        void overwrite(VkDescriptorSet& set);

    private:
        EgeDescriptorSetLayout& setLayout;
        EgeDescriptorPool& pool;
        std::vector<VkWriteDescriptorSet> writes;
    };

}  // namespace ege
