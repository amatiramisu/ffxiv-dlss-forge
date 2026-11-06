// DXVK's Vulkan interop interfaces (dxvk/src/dxgi/dxgi_interfaces.h) and NGX's Vulkan resource type and core
// entry points (DLSS SDK 310.9.16). NVSDK_NGX_VULKAN_Init_ProjectID_Ext takes its arguments in the order the
// static NGX library passes them.
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>
#include <windows.h>
#include <d3d11_4.h>
#include "ngx_abi.h"

namespace dxvk_interop
{
	struct IDXGIVkInteropDevice;

	struct IDXGIVkInteropSurface : IUnknown
	{
		virtual HRESULT STDMETHODCALLTYPE GetDevice(IDXGIVkInteropDevice **) = 0;
		virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(VkImage *, VkImageLayout *, VkImageCreateInfo *) = 0;
	};

	struct IDXGIVkInteropDevice : IUnknown
	{
		virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance *, VkPhysicalDevice *, VkDevice *) = 0;
		virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue *, uint32_t *) = 0;
		virtual void STDMETHODCALLTYPE TransitionSurfaceLayout(IDXGIVkInteropSurface *, const VkImageSubresourceRange *, VkImageLayout, VkImageLayout) = 0;
		virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
		virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
		virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
	};

	struct IDXGIVkInteropDevice1 : IDXGIVkInteropDevice
	{
		virtual void STDMETHODCALLTYPE GetSubmissionQueue1(VkQueue *, uint32_t *queue_index, uint32_t *family) = 0;
		virtual HRESULT STDMETHODCALLTYPE CreateTexture2DFromVkImage(const D3D11_TEXTURE2D_DESC1 *, VkImage, ID3D11Texture2D **) = 0;
	};

	constexpr GUID iid_device = { 0xe2ef5fa5, 0xdc21, 0x4af7, { 0x90, 0xc4, 0xf6, 0x7e, 0xf6, 0xa0, 0x93, 0x23 } };
	constexpr GUID iid_device1 = { 0xe2ef5fa5, 0xdc21, 0x4af7, { 0x90, 0xc4, 0xf6, 0x7e, 0xf6, 0xa0, 0x93, 0x24 } };
	constexpr GUID iid_surface = { 0x5546cf8c, 0x77e7, 0x4341, { 0xb0, 0x5d, 0x8d, 0x4d, 0x50, 0x00, 0xe7, 0x7d } };
}

namespace ngxvk
{
	struct image_view_info { VkImageView view; VkImage image; VkImageSubresourceRange range; VkFormat format; unsigned int width, height; };
	struct buffer_info { VkBuffer buffer; unsigned int size; };
	// NVSDK_NGX_Resource_VK; NGX parameters carry a pointer to it
	struct resource { union { image_view_info image; buffer_info buffer; } res; int type; bool read_write; };
	static_assert(sizeof(resource) == 56, "NVSDK_NGX_Resource_VK layout");

	using PFN_Init_ProjectID_Ext = ngx::Result (*)(const char *project_id, int engine_type, const char *engine_version, const wchar_t *data_path,
		VkInstance, VkPhysicalDevice, VkDevice, PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, int sdk_version, const void *feature_info);
	using PFN_CreateFeature1 = ngx::Result (*)(VkDevice, VkCommandBuffer, int feature, ngx::Parameter *, ngx::Handle **);
	using PFN_EvaluateFeature = ngx::Result (*)(VkCommandBuffer, const ngx::Handle *, const ngx::Parameter *, void *callback);
}
