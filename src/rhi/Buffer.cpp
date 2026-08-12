/*
 * Encapsulates a vulkan buffer
 *
 * Initially based off VulkanBuffer by Sascha Willems -
 * https://github.com/SaschaWillems/Vulkan/blob/master/base/VulkanBuffer.h
 */

#include "rhi/Buffer.hpp"

// std
#include <cassert>
#include <cstring>

namespace ege {

    /**
     * Returns the minimum instance size required to be compatible with devices minOffsetAlignment
     *
     * @param instanceSize The size of an instance
     * @param minOffsetAlignment The minimum required alignment, in bytes, for the offset member (eg
     * minUniformBufferOffsetAlignment)
     *
     * @return VkResult of the buffer mapping call
     */
    VkDeviceSize Buffer::getAlignment(VkDeviceSize instanceSize, VkDeviceSize minOffsetAlignment) {
        if (minOffsetAlignment > 0) {
            return (instanceSize + minOffsetAlignment - 1) & ~(minOffsetAlignment - 1);
        }
        return instanceSize;
    }

    Buffer::Buffer(
        Device& deviceRef,
        VkDeviceSize instanceSize_,
        uint32_t instanceCount_,
        VkBufferUsageFlags usageFlags_,
        VkMemoryPropertyFlags memoryPropertyFlags_,
        VkDeviceSize minOffsetAlignment_)
        : device{deviceRef},
          instanceSize{instanceSize_},
          instanceCount{instanceCount_},
          usageFlags{usageFlags_},
          memoryPropertyFlags{memoryPropertyFlags_} {
        alignmentSize = getAlignment(instanceSize_, minOffsetAlignment_);
        bufferSize = alignmentSize * instanceCount_;
        deviceRef.createBuffer(bufferSize, usageFlags_, memoryPropertyFlags_, buffer, allocation);
    }

    Buffer::~Buffer() {
        unmap();
        device.destroyBuffer(buffer, allocation);
    }

    /**
     * Map a memory range of this buffer. If successful, mapped points to the specified buffer
     * range.
     *
     * @param size (Optional) Size of the memory range to map. Pass VK_WHOLE_SIZE to map the
     * complete buffer range.
     * @param offset (Optional) Byte offset from beginning
     *
     * @return VkResult of the buffer mapping call
     */
    VkResult Buffer::map(VkDeviceSize size, VkDeviceSize offset) {
        assert(buffer && memory && "Called map on buffer before create");
        // VMA maps the whole allocation; the offset and size arguments are kept
        // for interface compatibility with the previous vkMapMemory call, and
        // writeToBuffer applies the offset itself.
        (void)size;
        (void)offset;
        return vmaMapMemory(device.allocator(), allocation, &mapped);
    }

    /**
     * Unmap a mapped memory range
     *
     * @note Does not return a result as vkUnmapMemory can't fail
     */
    void Buffer::unmap() {
        if (mapped) {
            vmaUnmapMemory(device.allocator(), allocation);
            mapped = nullptr;
        }
    }

    /**
     * Copies the specified data to the mapped buffer. Default value writes whole buffer range
     *
     * @param data Pointer to the data to copy
     * @param size (Optional) Size of the data to copy. Pass VK_WHOLE_SIZE to flush the complete
     * buffer range.
     * @param offset (Optional) Byte offset from beginning of mapped region
     *
     */
    void Buffer::writeToBuffer(void* data, VkDeviceSize size, VkDeviceSize offset) {
        assert(mapped && "Cannot copy to unmapped buffer");

        if (size == VK_WHOLE_SIZE) {
            memcpy(mapped, data, bufferSize);
        } else {
            char* memOffset = static_cast<char*>(mapped);
            memOffset += offset;
            memcpy(memOffset, data, size);
        }
    }

    /**
     * Flush a memory range of the buffer to make it visible to the device
     *
     * @note Only required for non-coherent memory
     *
     * @param size (Optional) Size of the memory range to flush. Pass VK_WHOLE_SIZE to flush the
     * complete buffer range.
     * @param offset (Optional) Byte offset from beginning
     *
     * @return VkResult of the flush call
     */
    VkResult Buffer::flush(VkDeviceSize size, VkDeviceSize offset) {
        // VMA is a no-op for coherent memory and issues the range flush
        // otherwise, so callers no longer have to know which they got.
        return vmaFlushAllocation(device.allocator(), allocation, offset, size);
    }

    /**
     * Invalidate a memory range of the buffer to make it visible to the host
     *
     * @note Only required for non-coherent memory
     *
     * @param size (Optional) Size of the memory range to invalidate. Pass VK_WHOLE_SIZE to
     * invalidate the complete buffer range.
     * @param offset (Optional) Byte offset from beginning
     *
     * @return VkResult of the invalidate call
     */
    VkResult Buffer::invalidate(VkDeviceSize size, VkDeviceSize offset) {
        return vmaInvalidateAllocation(device.allocator(), allocation, offset, size);
    }

    /**
     * Create a buffer info descriptor
     *
     * @param size (Optional) Size of the memory range of the descriptor
     * @param offset (Optional) Byte offset from beginning
     *
     * @return VkDescriptorBufferInfo of specified offset and range
     */
    VkDescriptorBufferInfo Buffer::descriptorInfo(VkDeviceSize size, VkDeviceSize offset) {
        return VkDescriptorBufferInfo{
            buffer,
            offset,
            size,
        };
    }

    /**
     * Copies "instanceSize" bytes of data to the mapped buffer at an offset of index *
     * alignmentSize
     *
     * @param data Pointer to the data to copy
     * @param index Used in offset calculation
     *
     */
    void Buffer::writeToIndex(void* data, uint32_t index) {
        writeToBuffer(data, instanceSize, index * alignmentSize);
    }

    /**
     *  Flush the memory range at index * alignmentSize of the buffer to make it visible to the
     * device
     *
     * @param index Used in offset calculation
     *
     */
    VkResult Buffer::flushIndex(uint32_t index) {
        return flush(alignmentSize, index * alignmentSize);
    }

    /**
     * Create a buffer info descriptor
     *
     * @param index Specifies the region given by index * alignmentSize
     *
     * @return VkDescriptorBufferInfo for instance at index
     */
    VkDescriptorBufferInfo Buffer::descriptorInfoForIndex(uint32_t index) {
        return descriptorInfo(alignmentSize, index * alignmentSize);
    }

    /**
     * Invalidate a memory range of the buffer to make it visible to the host
     *
     * @note Only required for non-coherent memory
     *
     * @param index Specifies the region to invalidate: index * alignmentSize
     *
     * @return VkResult of the invalidate call
     */
    VkResult Buffer::invalidateIndex(uint32_t index) {
        return invalidate(alignmentSize, index * alignmentSize);
    }

}  // namespace ege