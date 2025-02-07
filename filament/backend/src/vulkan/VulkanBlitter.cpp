/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "VulkanBlitter.h"
#include "VulkanContext.h"
#include "VulkanHandles.h"

#include <utils/Panic.h>

#include "generated/vkshaders/vkshaders.h"

#define FILAMENT_VULKAN_CHECK_BLIT_FORMAT 0

using namespace bluevk;

namespace filament {
namespace backend {

// Helper function for populating barrier fields based on the desired image layout.
// This logic is specific to blitting, please keep this private to VulkanBlitter.
static VulkanLayoutTransition transitionHelper(VulkanLayoutTransition transition) {
    switch (transition.newLayout) {
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_GENERAL:
            transition.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            transition.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            transition.srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            transition.dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            break;

        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
        default:
            transition.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            transition.dstAccessMask = 0;
            transition.srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            transition.dstStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            break;
    }
    return transition;
}

void VulkanBlitter::blitColor(VkCommandBuffer cmdBuffer, BlitArgs args) {
    lazyInit();
    const VulkanAttachment src = args.srcTarget->getColor(args.targetIndex);
    const VulkanAttachment dst = args.dstTarget->getColor(0);
    const VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;

#if FILAMENT_VULKAN_CHECK_BLIT_FORMAT
    const VkPhysicalDevice gpu = mContext.physicalDevice;
    VkFormatProperties info;
    vkGetPhysicalDeviceFormatProperties(gpu, src.format, &info);
    if (!ASSERT_POSTCONDITION_NON_FATAL(info.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT,
            "Source format is not blittable")) {
        return;
    }
    vkGetPhysicalDeviceFormatProperties(gpu, dst.format, &info);
    if (!ASSERT_POSTCONDITION_NON_FATAL(info.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT,
            "Destination format is not blittable")) {
        return;
    }
#endif

    blitFast(aspect, args.filter, args.srcTarget, src, dst, args.srcRectPair, args.dstRectPair,
            cmdBuffer);
}

void VulkanBlitter::blitDepth(VkCommandBuffer cmdBuffer, BlitArgs args) {
    lazyInit();
    const VulkanAttachment src = args.srcTarget->getDepth();
    const VulkanAttachment dst = args.dstTarget->getDepth();
    const VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;

#if FILAMENT_VULKAN_CHECK_BLIT_FORMAT
    const VkPhysicalDevice gpu = mContext.physicalDevice;
    VkFormatProperties info;
    vkGetPhysicalDeviceFormatProperties(gpu, src.format, &info);
    if (!ASSERT_POSTCONDITION_NON_FATAL(info.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT,
            "Depth format is not blittable")) {
        return;
    }
    vkGetPhysicalDeviceFormatProperties(gpu, dst.format, &info);
    if (!ASSERT_POSTCONDITION_NON_FATAL(info.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT,
            "Depth format is not blittable")) {
        return;
    }
#endif

    blitFast(aspect, args.filter, args.srcTarget, src, dst, args.srcRectPair, args.dstRectPair,
            cmdBuffer);
}

void VulkanBlitter::blitFast(VkImageAspectFlags aspect, VkFilter filter,
    const VulkanRenderTarget* srcTarget, VulkanAttachment src, VulkanAttachment dst,
    const VkOffset3D srcRect[2], const VkOffset3D dstRect[2], VkCommandBuffer cmdbuffer) {
    const VkImageBlit blitRegions[1] = {{
        .srcSubresource = { aspect, src.level, src.layer, 1 },
        .srcOffsets = { srcRect[0], srcRect[1] },
        .dstSubresource = { aspect, dst.level, dst.layer, 1 },
        .dstOffsets = { dstRect[0], dstRect[1] }
    }};

    const VkExtent2D srcExtent = srcTarget->getExtent();

    const VkImageResolve resolveRegions[1] = {{
        .srcSubresource = { aspect, src.level, src.layer, 1 },
        .srcOffset = srcRect[0],
        .dstSubresource = { aspect, dst.level, dst.layer, 1 },
        .dstOffset = dstRect[0],
        .extent = { srcExtent.width, srcExtent.height, 1 }
    }};

    const VkImageSubresourceRange srcRange = {
        .aspectMask = aspect,
        .baseMipLevel = src.level,
        .levelCount = 1,
        .baseArrayLayer = src.layer,
        .layerCount = 1,
    };

    const VkImageSubresourceRange dstRange = {
        .aspectMask = aspect,
        .baseMipLevel = dst.level,
        .levelCount = 1,
        .baseArrayLayer = dst.layer,
        .layerCount = 1,
    };

    transitionImageLayout(cmdbuffer, {
        src.image,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        srcRange,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT
    });

    transitionImageLayout(cmdbuffer, {
        dst.image,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        dstRange,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
    });

    if (src.texture && src.texture->samples > 1 && dst.texture && dst.texture->samples == 1) {
        assert_invariant(aspect != VK_IMAGE_ASPECT_DEPTH_BIT && "Resolve with depth is not yet supported.");
        vkCmdResolveImage(cmdbuffer, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, resolveRegions);
    } else {
        vkCmdBlitImage(cmdbuffer, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, blitRegions, filter);
    }

    if (src.texture) {
        transitionImageLayout(cmdbuffer, transitionHelper({
            .image = src.image,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = getTextureLayout(src.texture->usage),
            .subresources = srcRange
        }));
    } else if (!mContext.currentSurface->headlessQueue) {
        transitionImageLayout(cmdbuffer, transitionHelper({
            .image = src.image,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .subresources = srcRange
        }));
    }

    // Determine the desired texture layout for the destination while ensuring that the default
    // render target is supported, which has no associated texture.
    const VkImageLayout desiredLayout = dst.texture ? getTextureLayout(dst.texture->usage) :
            getSwapChainAttachment(mContext).layout;

    transitionImageLayout(cmdbuffer, transitionHelper({
        .image = dst.image,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = desiredLayout,
        .subresources = dstRange,
    }));
}

void VulkanBlitter::shutdown() noexcept {
    if (mContext.device) {
        vkDestroyShaderModule(mContext.device, mVertex, VKALLOC);
        vkDestroyShaderModule(mContext.device, mFragment, VKALLOC);
    }
}

// If we created these shader modules in the constructor, the device might not be ready yet.
// It is easier to do lazy initialization, which can also improve load time.
void VulkanBlitter::lazyInit() noexcept {
    if (mVertex) {
        return;
    }
    assert_invariant(mContext.device);

    VkShaderModuleCreateInfo moduleInfo = {};
    moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    VkResult result;

    moduleInfo.codeSize = VKSHADERS_BLITCOLORVS_SIZE;
    moduleInfo.pCode = (uint32_t*) VKSHADERS_BLITCOLORVS_DATA;
    result = vkCreateShaderModule(mContext.device, &moduleInfo, VKALLOC, &mVertex);
    ASSERT_POSTCONDITION(result == VK_SUCCESS, "Unable to create vertex shader for blit.");

    moduleInfo.codeSize = VKSHADERS_BLITCOLORFS_SIZE;
    moduleInfo.pCode = (uint32_t*) VKSHADERS_BLITCOLORFS_DATA;
    result = vkCreateShaderModule(mContext.device, &moduleInfo, VKALLOC, &mFragment);
    ASSERT_POSTCONDITION(result == VK_SUCCESS, "Unable to create fragment shader for blit.");
}

} // namespace filament
} // namespace backend
