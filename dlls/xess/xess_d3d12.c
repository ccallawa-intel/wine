/*
 * XeSS DLL - D3D12 implementation
 * Copyright 2025 the Wine project
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "wine/debug.h"
#define COBJMACROS

#include <initguid.h>

#include "xess_wine.h"
#include "unixlib.h"

#include "extern/xess_d3d12.h"
#define VKD3D_NO_WIN32_TYPES
#include <vkd3d.h>
#undef VKD3D_NO_WIN32_TYPES

#include "vkd3d-proton-interop.h"

WINE_DEFAULT_DEBUG_CHANNEL(xess);

static VkImageView get_vk_image_view(VkDevice vk_device, PFN_vkCreateImageView pfn_vkCreateImageView,
    VkImage vk_image, VkFormat format, const D3D12_RESOURCE_DESC *desc, VkImageAspectFlags aspect_mask)
{
    VkImageViewCreateInfo view_info;
    VkImageView image_view = VK_NULL_HANDLE;
    VkResult vk_result;

    memset(&view_info, 0, sizeof(view_info));
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = vk_image;
    view_info.viewType = (desc->DepthOrArraySize > 1) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    view_info.subresourceRange.aspectMask = aspect_mask;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = desc->MipLevels;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = desc->DepthOrArraySize;

    vk_result = pfn_vkCreateImageView(vk_device, &view_info, NULL, &image_view);
    if (vk_result != VK_SUCCESS)
    {
        WARN("Failed to create VkImageView: %d\n", vk_result);
        return VK_NULL_HANDLE;
    }

    return image_view;
}

static xess_result_t translate_texture_resource(
    ID3D12DXVKInteropDevice3 *interop,
    VkDevice vk_device,
    PFN_vkCreateImageView pfn_vkCreateImageView,
    ID3D12Resource *pTexture,
    xess_vk_image_view_info *pTextureInfo,
    VkImageView *pImageView,
    const char *texture_name)
{
    D3D12_RESOURCE_DESC desc;
    UINT64 vk_handle;
    UINT64 buffer_offset;
    HRESULT hr;

    TRACE("Handling %s texture...\n", texture_name);

    hr = ID3D12DXVKInteropDevice3_GetVulkanResourceInfo1(interop, pTexture,
        &vk_handle, &buffer_offset, &pTextureInfo->format);
    if (FAILED(hr))
    {
        WARN("Failed to get %s texture info: %lx\n", texture_name, hr);
        return XESS_RESULT_ERROR_UNKNOWN;
    }

    desc = ID3D12Resource_GetDesc(pTexture);
    TRACE("%s texture: %I64ux%u, MipLevels=%u, ArraySize=%u, Format=%u\n",
          texture_name, desc.Width, desc.Height, desc.MipLevels, desc.DepthOrArraySize, desc.Format);

    pTextureInfo->image = (VkImage)vk_handle;
    pTextureInfo->width = desc.Width;
    pTextureInfo->height = desc.Height;
    pTextureInfo->subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    pTextureInfo->subresourceRange.baseMipLevel = 0;
    pTextureInfo->subresourceRange.levelCount = desc.MipLevels;
    pTextureInfo->subresourceRange.baseArrayLayer = 0;
    pTextureInfo->subresourceRange.layerCount = desc.DepthOrArraySize;

    /* Create VkImageView */
    *pImageView = get_vk_image_view(vk_device, pfn_vkCreateImageView,
        pTextureInfo->image, pTextureInfo->format, &desc, VK_IMAGE_ASPECT_COLOR_BIT);
    if (*pImageView == VK_NULL_HANDLE)
    {
        WARN("Failed to create %s texture image view\n", texture_name);
        return XESS_RESULT_ERROR_UNKNOWN;
    }
    pTextureInfo->imageView = *pImageView;

    TRACE("Finished %s texture handler.\n", texture_name);
    return XESS_RESULT_SUCCESS;
}

static xess_result_t translate_heap_to_vk_memory(ID3D12Heap *heap, VkDeviceMemory *vk_memory, uint64_t *base_offset)
{
    ID3D12DXVKInteropDevice3 *interop = NULL;
    ID3D12Device *device = NULL;
    UINT64 vk_memory_u64;
    UINT64 heap_offset_u64;
    UINT32 vk_memory_type;
    HRESULT hr;

    TRACE("(%p, %p, %p)\n", heap, vk_memory, base_offset);

    if (!heap)
    {
        TRACE("No heap provided, skipping external heap handling.\n");
        *vk_memory = VK_NULL_HANDLE;
        *base_offset = 0;
        return XESS_RESULT_SUCCESS;
    }

    hr = ID3D12Heap_GetDevice(heap, &IID_ID3D12Device, (void **)&device);
    if (FAILED(hr))
    {
        WARN("Failed to get heap device: %#lx\n", hr);
        return XESS_RESULT_ERROR_UNSUPPORTED;
    }

    TRACE("Heap device: %p\n", device);

    hr = ID3D12Device_QueryInterface(device, &IID_ID3D12DXVKInteropDevice3, (void **)&interop);
    ID3D12Device_Release(device);
    if (FAILED(hr))
    {
        WARN("ID3D12DXVKInteropDevice3 unavailable (%#lx), ignoring external heap and falling back to internal XeSS allocation.\n", hr);
        *vk_memory = VK_NULL_HANDLE;
        *base_offset = 0;
        return XESS_RESULT_SUCCESS;
    }

    TRACE("Interop: %p\n", interop);

    hr = ID3D12DXVKInteropDevice3_GetVulkanHeapInfo(interop, heap, &vk_memory_u64, &heap_offset_u64, &vk_memory_type);
    ID3D12DXVKInteropDevice3_Release(interop);
    if (FAILED(hr))
    {
        WARN("GetVulkanHeapInfo failed: %#lx\n", hr);
        ID3D12Device_Release(device);
        return XESS_RESULT_ERROR_UNSUPPORTED;
    }

    *vk_memory = (VkDeviceMemory)vk_memory_u64;
    *base_offset = heap_offset_u64;
    TRACE("Got memory handle %p with offset %I64ux and memory type %u.\n", vk_memory, *base_offset, vk_memory_type);
    ID3D12Device_Release(device);
    return XESS_RESULT_SUCCESS;
}

/* heap pointer tracking helpers */
static ID3D12Heap *xess_d3d12_temp_buffer_heap;
static ID3D12Heap *xess_d3d12_temp_texture_heap;

static void xess_d3d12_store_init_heaps(ID3D12Heap *temp_buffer_heap, ID3D12Heap *temp_texture_heap)
{
    if (temp_buffer_heap)
        ID3D12Heap_AddRef(temp_buffer_heap);
    if (temp_texture_heap)
        ID3D12Heap_AddRef(temp_texture_heap);

    if (xess_d3d12_temp_buffer_heap)
        ID3D12Heap_Release(xess_d3d12_temp_buffer_heap);
    if (xess_d3d12_temp_texture_heap)
        ID3D12Heap_Release(xess_d3d12_temp_texture_heap);

    xess_d3d12_temp_buffer_heap = temp_buffer_heap;
    xess_d3d12_temp_texture_heap = temp_texture_heap;
}

static void xess_d3d12_get_init_heaps(ID3D12Heap **temp_buffer_heap, ID3D12Heap **temp_texture_heap)
{
    *temp_buffer_heap = xess_d3d12_temp_buffer_heap;
    *temp_texture_heap = xess_d3d12_temp_texture_heap;
}

void xess_d3d12_clear_init_heap_store()
{
    if (xess_d3d12_temp_texture_heap)
        ID3D12Heap_Release(xess_d3d12_temp_texture_heap);
    if (xess_d3d12_temp_buffer_heap)
        ID3D12Heap_Release(xess_d3d12_temp_buffer_heap);

    xess_d3d12_temp_buffer_heap = NULL;
    xess_d3d12_temp_texture_heap = NULL;
}

xess_result_t CDECL xessD3D12CreateContext(ID3D12Device *pDevice, xess_context_handle_t *phContext)
{
    VkDevice vk_device;
    VkPhysicalDevice vk_physical_device;
    VkInstance vk_instance;
    ID3D12DXVKInteropDevice3 *interop = NULL;
    struct xess_vk_create_context_params unix_params;
    HRESULT hr;

    TRACE("(%p, %p)\n", pDevice, phContext);

    hr = ID3D12Device_QueryInterface(pDevice, &IID_ID3D12DXVKInteropDevice3, (void**)&interop);
    if (FAILED(hr))
    {
        ERR("Failed to query interface: %#lx\n", hr);
        return XESS_RESULT_ERROR_DEVICE;
    }

    hr = ID3D12DXVKInteropDevice3_GetVulkanHandles(interop, &vk_instance, &vk_physical_device, &vk_device);
    ID3D12DXVKInteropDevice3_Release(interop);
    if (FAILED(hr))
    {
        ERR("GetVulkanHandles failed: %#lx\n", hr);
        return XESS_RESULT_ERROR_DEVICE;
    }

    TRACE("vk_instance: %p, vk_physical_device: %p, vk_device: %p\n",
            vk_instance, vk_physical_device, vk_device);

    memset(&unix_params, 0, sizeof(unix_params));
    unix_params.instance = vk_instance;
    unix_params.physicalDevice = vk_physical_device;
    unix_params.device = vk_device;
    unix_params.phContext = phContext;

    WINE_UNIX_CALL(unix_xessVKCreateContext, &unix_params);
    TRACE("xessVKCreateContext result: %s (0x%x)\n", xess_result_to_string(unix_params.result), unix_params.result);
    return unix_params.result;
}

xess_result_t CDECL xessD3D12BuildPipelines(xess_context_handle_t hContext,
    ID3D12PipelineLibrary *pPipelineLibrary, bool blocking, uint32_t initFlags)
{
    TRACE("(%p, %p, %u, 0x%x)\n", hContext, pPipelineLibrary, blocking, initFlags);
    return XESS_RESULT_ERROR_NOT_IMPLEMENTED;
}

xess_result_t CDECL xessD3D12Init(xess_context_handle_t hContext, const xess_d3d12_init_params_t *pInitParams)
{
    xess_result_t result;
    xess_vk_init_params_t vk_init_params;
    struct xess_vk_get_init_params_params unix_params;
    uint64_t buffer_heap_base_offset = 0;
    uint64_t texture_heap_base_offset = 0;

    TRACE("(%p, %p)\n", hContext, pInitParams);

    memset(&vk_init_params, 0, sizeof(vk_init_params));
    vk_init_params.outputResolution.x = pInitParams->outputResolution.x;
    vk_init_params.outputResolution.y = pInitParams->outputResolution.y;
    vk_init_params.qualitySetting = pInitParams->qualitySetting;
    vk_init_params.initFlags = pInitParams->initFlags;
    vk_init_params.creationNodeMask = pInitParams->creationNodeMask;
    vk_init_params.visibleNodeMask = pInitParams->visibleNodeMask;

    result = translate_heap_to_vk_memory(pInitParams->pTempBufferHeap,
        &vk_init_params.tempBufferHeap, &buffer_heap_base_offset);
    if (result != XESS_RESULT_SUCCESS)
        return result;

    result = translate_heap_to_vk_memory(pInitParams->pTempTextureHeap,
        &vk_init_params.tempTextureHeap, &texture_heap_base_offset);
    if (result != XESS_RESULT_SUCCESS)
        return result;

    vk_init_params.bufferHeapOffset = pInitParams->bufferHeapOffset + buffer_heap_base_offset;
    vk_init_params.textureHeapOffset = pInitParams->textureHeapOffset + texture_heap_base_offset;
    vk_init_params.pipelineCache = VK_NULL_HANDLE; // pipelines are optional and hard to implement

    memset(&unix_params, 0, sizeof(unix_params));
    unix_params.hContext = hContext;
    unix_params.pInitParams = &vk_init_params;
    WINE_UNIX_CALL(unix_xessVKInit, &unix_params);

    if (unix_params.result == XESS_RESULT_SUCCESS)
    {
        xess_d3d12_store_init_heaps(pInitParams->pTempBufferHeap, pInitParams->pTempTextureHeap);
    }

    TRACE("xessVKInit result: %s (0x%x)\n", xess_result_to_string(unix_params.result), unix_params.result);
    return unix_params.result;
}

xess_result_t CDECL xessD3D12GetInitParams(xess_context_handle_t hContext, xess_d3d12_init_params_t *pInitParams)
{
    xess_vk_init_params_t vk_init_params;
    struct xess_vk_get_init_params_params unix_params;

    TRACE("(%p, %p)\n", hContext, pInitParams);

    unix_params.hContext = hContext;
    unix_params.pInitParams = &vk_init_params;
    WINE_UNIX_CALL(unix_xessVKGetInitParams, &unix_params);
    if (unix_params.result != XESS_RESULT_SUCCESS) {
        TRACE("xessVKGetInitParams result: %s (0x%x)\n", xess_result_to_string(unix_params.result), unix_params.result);
        return unix_params.result;
    }

    memset(pInitParams, 0, sizeof(*pInitParams));
    pInitParams->outputResolution = vk_init_params.outputResolution;
    pInitParams->qualitySetting = vk_init_params.qualitySetting;
    pInitParams->initFlags = vk_init_params.initFlags;
    pInitParams->creationNodeMask = vk_init_params.creationNodeMask;
    pInitParams->visibleNodeMask = vk_init_params.visibleNodeMask;
    xess_d3d12_get_init_heaps(&pInitParams->pTempBufferHeap, &pInitParams->pTempTextureHeap);
    pInitParams->bufferHeapOffset = vk_init_params.bufferHeapOffset;
    pInitParams->textureHeapOffset = vk_init_params.textureHeapOffset;
    pInitParams->pPipelineLibrary = NULL; // pipelines are optional and hard to implement

    return XESS_RESULT_SUCCESS;
}

xess_result_t CDECL xessD3D12Execute(xess_context_handle_t hContext,
    ID3D12GraphicsCommandList *pCommandList, const xess_d3d12_execute_params_t *pExecParams)
{
    xess_result_t result;
    ID3D12Device *pDevice = NULL;
    ID3D12DXVKInteropDevice3 *interop = NULL;
    VkCommandBuffer vk_command_buffer = VK_NULL_HANDLE;
    xess_vk_execute_params_t vk_exec_params;
    struct xess_vk_execute_params unix_params;
    HRESULT hr;
    VkDevice vk_device = VK_NULL_HANDLE;
    VkPhysicalDevice vk_physical_device = VK_NULL_HANDLE;
    VkInstance vk_instance = VK_NULL_HANDLE;
    VkImageView image_views[6] = {VK_NULL_HANDLE};
    UINT image_view_count = 0;
    PFN_vkCreateImageView pfn_vkCreateImageView = NULL;
    PFN_vkDestroyImageView pfn_vkDestroyImageView = NULL;

    TRACE("(%p, %p, %p)\n", hContext, pCommandList, pExecParams);

    memset(&vk_exec_params, 0, sizeof(vk_exec_params));
    memset(&unix_params, 0, sizeof(unix_params));

    /* Get the D3D12 device from the command list */
    hr = ID3D12GraphicsCommandList_GetDevice(pCommandList, &IID_ID3D12Device, (void**)&pDevice);
    if (FAILED(hr))
    {
        WARN("Failed to get ID3D12Device from command list: %lx\n", hr);
        return XESS_RESULT_ERROR_INVALID_ARGUMENT;
    }

    /* Get the interop interface */
    hr = ID3D12Device_QueryInterface(pDevice, &IID_ID3D12DXVKInteropDevice3, (void**)&interop);
    if (FAILED(hr))
    {
        WARN("Failed to get ID3D12DXVKInteropDevice interface: %lx\n", hr);
        ID3D12Device_Release(pDevice);
        return XESS_RESULT_ERROR_UNSUPPORTED;
    }

    TRACE("Got DXVK interop\n");

    /* Get Vulkan handles for creating image views */
    hr = ID3D12DXVKInteropDevice3_GetVulkanHandles(interop, &vk_instance, &vk_physical_device, &vk_device);
    if (FAILED(hr))
    {
        WARN("Failed to get Vulkan handles: %lx\n", hr);
        ID3D12DXVKInteropDevice3_Release(interop);
        ID3D12Device_Release(pDevice);
        return XESS_RESULT_ERROR_UNSUPPORTED;
    }

    TRACE("Got Vulkan handles vk_instance=%p, vk_physical_device=%p, vk_device=%p\n", vk_instance, vk_physical_device, vk_device);

    /* Get Vulkan function pointers */
    pfn_vkCreateImageView = (PFN_vkCreateImageView)vkGetDeviceProcAddr(vk_device, "vkCreateImageView");
    pfn_vkDestroyImageView = (PFN_vkDestroyImageView)vkGetDeviceProcAddr(vk_device, "vkDestroyImageView");
    if (!pfn_vkCreateImageView || !pfn_vkDestroyImageView)
    {
        WARN("Failed to get Vulkan function pointers\n");
        ID3D12DXVKInteropDevice3_Release(interop);
        ID3D12Device_Release(pDevice);
        return XESS_RESULT_ERROR_UNSUPPORTED;
    }

    TRACE("Got Vulkan function pointers pfn_vkCreateImageView=%p, pfn_vkDestroyImageView=%p\n", pfn_vkCreateImageView, pfn_vkDestroyImageView);

    /* Get Vulkan command buffer from D3D12 command list */
    hr = ID3D12DXVKInteropDevice3_BeginVkCommandBufferInterop(interop, (ID3D12CommandList*)pCommandList, &vk_command_buffer);
    if (FAILED(hr))
    {
        WARN("Failed to get VkCommandBuffer: %lx\n", hr);
        ID3D12DXVKInteropDevice3_Release(interop);
        ID3D12Device_Release(pDevice);
        return XESS_RESULT_ERROR_UNKNOWN;
    }

    TRACE("Got Vulkan interop command buffer vk_command_buffer=%p\n", vk_command_buffer);

    /* Get color texture VkImage and format */
    if (pExecParams->pColorTexture)
    {
        result = translate_texture_resource(interop, vk_device, pfn_vkCreateImageView,
            pExecParams->pColorTexture, &vk_exec_params.colorTexture,
            &image_views[image_view_count], "color");
        if (result != XESS_RESULT_SUCCESS)
            goto cleanup;
        image_view_count++;
    }

    /* Get velocity texture if provided */
    if (pExecParams->pVelocityTexture)
    {
        result = translate_texture_resource(interop, vk_device, pfn_vkCreateImageView,
            pExecParams->pVelocityTexture, &vk_exec_params.velocityTexture,
            &image_views[image_view_count], "velocity");
        if (result != XESS_RESULT_SUCCESS)
            goto cleanup;
        image_view_count++;
    }

    /* Get depth texture if provided */
    if (pExecParams->pDepthTexture)
    {
        result = translate_texture_resource(interop, vk_device, pfn_vkCreateImageView,
            pExecParams->pDepthTexture, &vk_exec_params.depthTexture,
            &image_views[image_view_count], "depth");
        if (result != XESS_RESULT_SUCCESS)
            goto cleanup;
        image_view_count++;
    }

    /* Get exposure scale texture if provided */
    if (pExecParams->pExposureScaleTexture)
    {
        result = translate_texture_resource(interop, vk_device, pfn_vkCreateImageView,
            pExecParams->pExposureScaleTexture, &vk_exec_params.exposureScaleTexture,
            &image_views[image_view_count], "exposure scale");
        if (result != XESS_RESULT_SUCCESS)
            goto cleanup;
        image_view_count++;
    }

    /* Get responsive pixel mask texture if provided */
    if (pExecParams->pResponsivePixelMaskTexture)
    {
        result = translate_texture_resource(interop, vk_device, pfn_vkCreateImageView,
            pExecParams->pResponsivePixelMaskTexture, &vk_exec_params.responsivePixelMaskTexture,
            &image_views[image_view_count], "responsive pixel mask");
        if (result != XESS_RESULT_SUCCESS)
            goto cleanup;
        image_view_count++;
    }

    /* Get output texture VkImage and format */
    if (pExecParams->pOutputTexture)
    {
        result = translate_texture_resource(interop, vk_device, pfn_vkCreateImageView,
            pExecParams->pOutputTexture, &vk_exec_params.outputTexture,
            &image_views[image_view_count], "output");
        if (result != XESS_RESULT_SUCCESS)
            goto cleanup;
        image_view_count++;
    }

    /* Copy execution parameters */
    vk_exec_params.jitterOffsetX = pExecParams->jitterOffsetX;
    vk_exec_params.jitterOffsetY = pExecParams->jitterOffsetY;
    vk_exec_params.exposureScale = pExecParams->exposureScale;
    vk_exec_params.resetHistory = pExecParams->resetHistory;
    vk_exec_params.inputWidth = pExecParams->inputWidth;
    vk_exec_params.inputHeight = pExecParams->inputHeight;
    vk_exec_params.inputColorBase = pExecParams->inputColorBase;
    vk_exec_params.inputMotionVectorBase = pExecParams->inputMotionVectorBase;
    vk_exec_params.inputDepthBase = pExecParams->inputDepthBase;
    vk_exec_params.inputResponsiveMaskBase = pExecParams->inputResponsiveMaskBase;
    vk_exec_params.reserved0 = pExecParams->reserved0;
    vk_exec_params.outputColorBase = pExecParams->outputColorBase;

    /* Execute XeSS */
    unix_params.hContext = hContext;
    unix_params.pCommandBuffer = vk_command_buffer;
    unix_params.pExecParams = &vk_exec_params;
    TRACE("Executing XeSS call...\n");
    WINE_UNIX_CALL(unix_xessVKExecute, &unix_params);
    result = unix_params.result;

cleanup:
    /* End Vulkan command buffer interop */
    if (vk_command_buffer != VK_NULL_HANDLE)
        ID3D12DXVKInteropDevice3_EndVkCommandBufferInterop(interop, (ID3D12CommandList*)pCommandList);

    /* Destroy created image views */
    if (pfn_vkDestroyImageView)
    {
        UINT i;
        for (i = 0; i < image_view_count; i++)
        {
            if (image_views[i] != VK_NULL_HANDLE)
                pfn_vkDestroyImageView(vk_device, image_views[i], NULL);
        }
    }

    ID3D12DXVKInteropDevice3_Release(interop);
    ID3D12Device_Release(pDevice);
    TRACE("xessVKExecute result: %s (0x%x)\n", xess_result_to_string(result), result);
    return result;
}
