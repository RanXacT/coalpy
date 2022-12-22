#include "VulkanWorkBundle.h"
#include "VulkanDevice.h"
#include "VulkanResources.h"
#include "VulkanEventPool.h"
#include <coalpy.core/Assert.h>
#include <vector>
#include <unordered_map>

namespace coalpy
{
namespace render
{

namespace
{

inline VkPipelineStageFlagBits getVkStage(ResourceGpuState state)
{
    switch (state)
    {
        case ResourceGpuState::Default:
            return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        case ResourceGpuState::IndirectArgs:
        case ResourceGpuState::Uav:
        case ResourceGpuState::Srv:
            return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        case ResourceGpuState::CopyDst:
        case ResourceGpuState::CopySrc:
            return VK_PIPELINE_STAGE_TRANSFER_BIT;
        case ResourceGpuState::Cbv:
            return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        case ResourceGpuState::Rtv:
            return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        case ResourceGpuState::Present:
            return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
    CPY_ASSERT_FMT(false, "D3d12 state used is not handled in coalpy's rendering", state);
    return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
}

inline VkAccessFlags getVkAccessMask(ResourceGpuState state)
{
    switch (state)
    {
        case ResourceGpuState::Default:
            return VK_ACCESS_NONE;
        case ResourceGpuState::IndirectArgs:
            return VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        case ResourceGpuState::Uav:
            return VK_ACCESS_SHADER_WRITE_BIT;
        case ResourceGpuState::Srv:
            return VK_ACCESS_SHADER_READ_BIT;
        case ResourceGpuState::CopyDst:
            return VK_ACCESS_TRANSFER_WRITE_BIT;
        case ResourceGpuState::CopySrc:
            return VK_ACCESS_TRANSFER_READ_BIT;
        case ResourceGpuState::Cbv:
            return VK_ACCESS_UNIFORM_READ_BIT;
        case ResourceGpuState::Rtv:
            return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        case ResourceGpuState::Present:
            return VK_ACCESS_MEMORY_WRITE_BIT;
    }
    CPY_ASSERT_FMT(false, "D3d12 state used is not handled in coalpy's rendering", state);
    return VK_ACCESS_NONE;
}

inline VkImageLayout getVkImageLayout(ResourceGpuState state)
{
    switch (state)
    {
        case ResourceGpuState::Default:
            return VK_IMAGE_LAYOUT_UNDEFINED;
        case ResourceGpuState::IndirectArgs:
        case ResourceGpuState::Uav:
            return VK_IMAGE_LAYOUT_GENERAL;
        case ResourceGpuState::Srv:
            return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        case ResourceGpuState::CopyDst:
            return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        case ResourceGpuState::CopySrc:
            return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        case ResourceGpuState::Cbv:
            return VK_IMAGE_LAYOUT_UNDEFINED;
        case ResourceGpuState::Rtv:
        case ResourceGpuState::Present:
            return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    CPY_ASSERT_FMT(false, "D3d12 state used is not handled in coalpy's rendering", state);
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

struct EventState
{
    VulkanEventHandle eventHandle;
    VkPipelineStageFlags flags = 0;
};

EventState createSrcBarrierEvent(
    VulkanDevice& device,
    VulkanEventPool& eventPool,
    const std::vector<ResourceBarrier>& barriers,
    VkCommandBuffer cmdBuffer)
{
    CommandLocation srcLocation;
    EventState eventState;
    bool mustReset = false;
    for (auto& b : barriers)
    {
        if (b.type != BarrierType::Begin)
            continue;

        if (!eventState.eventHandle.valid())
        {
            srcLocation = b.srcCmdLocation;
            bool isNew = false;
            eventState.eventHandle = eventPool.allocate(srcLocation, isNew);
            mustReset = !isNew;
        }

        CPY_ASSERT(srcLocation == b.srcCmdLocation);
        eventState.flags |= getVkStage(b.prevState);
        
    }

    if (mustReset)
    {
        VkEvent event = eventPool.getEvent(eventState.eventHandle);
        vkCmdResetEvent(cmdBuffer, event, eventState.flags);
    }
    return eventState;
}

void applyBarriers(
    VulkanDevice& device,
    VulkanEventPool& eventPool,
    const std::vector<ResourceBarrier>& barriers,
    VkCommandBuffer cmdBuffer)
{
    if (barriers.empty())
        return;

    std::vector<VkBufferMemoryBarrier> immBufferBarriers;
    std::vector<VkImageMemoryBarrier> immImageBarriers;

    VkBufferMemoryBarrier buffBarrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr };
    VkImageMemoryBarrier imgBarrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr };

    VulkanResources& resources = device.resources();

    struct DstEventState : public EventState
    {
        VkPipelineStageFlags dstFlags = 0;
        std::vector<VkImageMemoryBarrier> imageBarriers;
        std::vector<VkBufferMemoryBarrier> bufferBarriers;
    };

    CommandLocation srcLocation;
    EventState srcEvent;

    std::unordered_map<CommandLocation, DstEventState, CommandLocationHasher> dstEvents;

    for (const auto& b : barriers)
    {
        if (b.isUav)
            continue;

        DstEventState* dstEventPtr = nullptr;
        if (b.type == BarrierType::Begin)
        {
            if (!srcEvent.eventHandle.valid())
            {
                srcLocation = b.srcCmdLocation;
                bool unused = false;
                srcEvent.eventHandle = eventPool.allocate(srcLocation, unused);
            }

            CPY_ASSERT(srcLocation == b.srcCmdLocation);
            srcEvent.flags |= getVkStage(b.prevState);
        }
        else if (b.type == BarrierType::End)
        {
            auto it = dstEvents.find(b.srcCmdLocation);
            if (it == dstEvents.end())
            {
                VulkanEventHandle eventHandle = eventPool.find(b.srcCmdLocation);
                CPY_ASSERT(eventHandle.valid());
                DstEventState dstEvent;
                dstEvent.eventHandle = eventHandle;
                dstEventPtr = &dstEvents.insert(std::pair<CommandLocation, DstEventState>(b.srcCmdLocation, dstEvent)).first->second;
            }
            else
            {
                dstEventPtr = &it->second;
            }
            dstEventPtr->flags |= getVkStage(b.prevState);
            dstEventPtr->dstFlags |= getVkStage(b.postState);
        }

        if (b.type == BarrierType::Begin)
            continue;

        std::vector<VkBufferMemoryBarrier>& bufferBarriers = b.type == BarrierType::Immediate ? immBufferBarriers : dstEventPtr->bufferBarriers;
        std::vector<VkImageMemoryBarrier>& imgBarriers = b.type == BarrierType::Immediate ? immImageBarriers : dstEventPtr->imageBarriers;
        
        VulkanResource& resource = resources.unsafeGetResource(b.resource);
        VkAccessFlags srcAccessMask = getVkAccessMask(b.prevState);
        VkAccessFlags dstAccessMask = getVkAccessMask(b.postState);
        if (resource.isBuffer())
        {
            auto newBarrier = buffBarrier;
            newBarrier.srcAccessMask = srcAccessMask;
            newBarrier.dstAccessMask = dstAccessMask;
            newBarrier.srcQueueFamilyIndex = device.graphicsFamilyQueueIndex();
            newBarrier.dstQueueFamilyIndex = device.graphicsFamilyQueueIndex();
            newBarrier.buffer = resource.bufferData.vkBuffer;
            newBarrier.offset = 0ull;
            newBarrier.size = resource.bufferData.size;
            bufferBarriers.push_back(newBarrier);
        }
        else
        {
            auto newBarrier = imgBarrier;
            newBarrier.srcAccessMask = srcAccessMask;
            newBarrier.dstAccessMask = dstAccessMask;
            newBarrier.oldLayout = getVkImageLayout(b.prevState);
            newBarrier.newLayout = getVkImageLayout(b.postState);
            newBarrier.srcQueueFamilyIndex = device.graphicsFamilyQueueIndex();
            newBarrier.dstQueueFamilyIndex = device.graphicsFamilyQueueIndex();
            newBarrier.image = resource.textureData.vkImage;
            newBarrier.subresourceRange = resource.textureData.subresourceRange;
        }
    }

    if (srcEvent.eventHandle.valid())
    {
        VkEvent event = eventPool.getEvent(srcEvent.eventHandle);
        vkCmdSetEvent(cmdBuffer, event, srcEvent.flags);
    }

    for (auto pairVal : dstEvents)
    {
        DstEventState& dstEvent = pairVal.second;
        VkEvent event = eventPool.getEvent(dstEvent.eventHandle);
        vkCmdWaitEvents(
            cmdBuffer, 1, &event, dstEvent.flags, dstEvent.dstFlags,
            0, nullptr,
            dstEvent.bufferBarriers.size(), dstEvent.bufferBarriers.data(),
            dstEvent.imageBarriers.size(), dstEvent.imageBarriers.data());
    }
}

}

bool VulkanWorkBundle::load(const WorkBundle& workBundle)
{
    return false;
}

uint64_t VulkanWorkBundle::execute(CommandList** commandLists, int commandListsCount)
{
    return {};
}

}
}