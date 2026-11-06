// DLSS NR (NGX feature 18) through NGX's Vulkan API on DXVK's own VkDevice.
//
// forge-nrvk.exe              Asks NGX which Vulkan extensions NR needs (NVIDIA's documented query), checks them
//                             against DXVK's device, initialises NGX Vulkan on DXVK's handles, then creates and
//                             evaluates NR on D3D11 textures addressed through DXVK's interop.
// forge-nrvk.exe --d3d12-ref  Runs Forge's D3D12 bridge on the same picture and writes its model input and
//                             answer (nrvk-ref-in.f32, nrvk-ref-out.f32) for the Vulkan run to compare against.
// forge-nrvk.exe --with-sr    Also runs the game's kind of DLSS SR (NGX D3D11, DLAA) before and after Vulkan NR in the
//                             same process. FORGE_NRVK_SIZE=WxH sets the picture size (1920x1080 by default).
// forge-nrvk.exe --push       Adds VK_KHR_push_descriptor and VK_KHR_buffer_device_address to the VkDevice DXVK creates
//                             (DXVK's vkCreateDevice is reached through the GetProcAddress imports of its dxgi.dll and d3d11.dll).
//
// Export signatures follow NVIDIA's static NGX library (nvsdk_ngx_s.lib 310.9.16): NVSDK_NGX_VULKAN_Init_with_ProjectID
// calls the core's NVSDK_NGX_VULKAN_Init_ProjectID_Ext with (id, engine, version, path, instance, physical device,
// device, gipa, gdpa, sdk version, feature info); the other entry points pass their arguments through unchanged.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#include "log.h"
#include "ngx_abi.h"
#include "ngx_hook.h"
#include "nr_bridge.h"
#include "settings.h"

#include <windows.h>
#include <d3d11.h>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace ngx;

namespace
{
	int g_failures = 0;
	constexpr int feature_nr = 18;
	unsigned int W = 1920, H = 1080; // FORGE_NRVK_SIZE=3840x2160 changes it
	constexpr int frames_per_run = 9;

	void check(bool ok, const char *fmt, ...)
	{
		char buf[1024];
		va_list a;
		va_start(a, fmt);
		vsnprintf(buf, sizeof(buf), fmt, a);
		va_end(a);
		printf("[%s] %s\n", ok ? "PASS" : "FAIL", buf);
		FORGE_LOG("[%s] %s", ok ? "PASS" : "FAIL", buf);
		if (!ok)
			g_failures++;
	}

	// ---- DXVK interop (dxvk/src/dxgi/dxgi_interfaces.h) ----
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
	constexpr GUID iid_interop_device = { 0xe2ef5fa5, 0xdc21, 0x4af7, { 0x90, 0xc4, 0xf6, 0x7e, 0xf6, 0xa0, 0x93, 0x23 } };
	constexpr GUID iid_interop_surface = { 0x5546cf8c, 0x77e7, 0x4341, { 0xb0, 0x5d, 0x8d, 0x4d, 0x50, 0x00, 0xe7, 0x7d } };

	// ---- NGX types (nvsdk_ngx_defs.h / nvsdk_ngx_defs_vk.h, SDK 310.9.16) ----
	struct path_list { const wchar_t *const *path; unsigned int length; };
	struct logging_info { void (*callback)(const char *, int, int); int min_level; bool disable_other_sinks; };
	struct feature_common_info { path_list paths; void *internal; logging_info logging; };
	struct project_desc { const char *project_id; int engine_type; const char *engine_version; };
	struct app_identifier { int type; union { project_desc project; unsigned long long app_id; } v; };
	struct feature_discovery_info { int sdk_version; int feature_id; app_identifier identifier; const wchar_t *data_path; const feature_common_info *feature_info; };
	struct feature_requirement { int supported; unsigned int min_hw_arch; char min_os[255]; };
	struct image_view_info_vk { VkImageView view; VkImage image; VkImageSubresourceRange range; VkFormat format; unsigned int width, height; };
	struct buffer_info_vk { VkBuffer buffer; unsigned int size; };
	struct resource_vk { union { image_view_info_vk image; buffer_info_vk buffer; } res; int type; bool read_write; };
	static_assert(sizeof(feature_discovery_info) == 56, "NVSDK_NGX_FeatureDiscoveryInfo layout");
	static_assert(sizeof(resource_vk) == 56, "NVSDK_NGX_Resource_VK layout");

	using PFN_GetFeatureRequirements = Result (*)(VkInstance, VkPhysicalDevice, const feature_discovery_info *, feature_requirement *);
	using PFN_GetInstanceExtReq = Result (*)(const feature_discovery_info *, uint32_t *, VkExtensionProperties **);
	using PFN_GetDeviceExtReq = Result (*)(VkInstance, VkPhysicalDevice, const feature_discovery_info *, uint32_t *, VkExtensionProperties **);
	using PFN_RequiredExtensions = Result (*)(unsigned int *, const char ***, unsigned int *, const char ***);
	using PFN_InitProjectIdExt = Result (*)(const char *, int, const char *, const wchar_t *, VkInstance, VkPhysicalDevice, VkDevice,
		PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, int, const feature_common_info *);
	using PFN_VkGetParams = Result (*)(Parameter **);
	using PFN_VkDestroyParams = Result (*)(Parameter *);
	using PFN_VkCreateFeature1 = Result (*)(VkDevice, VkCommandBuffer, int, Parameter *, Handle **);
	using PFN_VkEvaluate = Result (*)(VkCommandBuffer, const Handle *, const Parameter *, void *);
	using PFN_VkRelease = Result (*)(Handle *);
	using PFN_VkShutdown1 = Result (*)(VkDevice);

	constexpr const char *project_id = "a0f57b54-1daf-4934-90ae-c4035c19df04";
	wchar_t g_dir[MAX_PATH] = {};
	std::string g_network;

	void ngx_log(const char *msg, int level, int)
	{
		if (msg == nullptr)
			return;
		size_t len = std::strlen(msg);
		while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r'))
			--len;
		for (const char *noise : { "NGXLoadConfig", "ModifyExistingFolderDACL", "IsFeatureDenied", "NGXReportStatusFeedback", "LoadMappingFileData", "[main:" })
			if (std::strstr(msg, noise) != nullptr)
				return;
		FORGE_LOG("NGX[%d]: %.*s", level, static_cast<int>(len), msg);
		if (const char *c = std::strstr(msg, "Created feature"))
			if (const char *a = std::strstr(c, "-> "))
			{
				g_network.clear();
				for (const char *p = a + 3; *p && *p != ')' && *p != '\n'; ++p)
					g_network += *p;
			}
	}

	// ---- helpers shared with the harness ----
	uint16_t to_half(float f)
	{
		uint32_t x;
		std::memcpy(&x, &f, 4);
		const uint32_t sign = (x >> 16) & 0x8000;
		int32_t exp = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
		uint32_t mant = x & 0x7FFFFF;
		if (exp <= 0)
			return static_cast<uint16_t>(sign);
		if (exp >= 31)
			return static_cast<uint16_t>(sign | 0x7C00);
		uint32_t h = sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
		const uint32_t rem = mant & 0x1FFF;
		if (rem > 0x1000 || (rem == 0x1000 && (h & 1)))
			h++;
		return static_cast<uint16_t>(h);
	}

	float from_half(uint16_t h)
	{
		const uint32_t sign = (h & 0x8000u) << 16;
		const uint32_t exp = (h >> 10) & 0x1F;
		const uint32_t mant = h & 0x3FF;
		uint32_t x;
		if (exp == 0)
		{
			if (mant == 0)
				x = sign;
			else
			{
				const float f = std::ldexp(static_cast<float>(mant), -24);
				return sign ? -f : f;
			}
		}
		else if (exp == 31)
			x = sign | 0x7F800000 | (mant << 13);
		else
			x = sign | ((exp - 15 + 127) << 23) | (mant << 13);
		float f;
		std::memcpy(&f, &x, 4);
		return f;
	}

	ID3D11Texture2D *make_tex(ID3D11Device *dev, UINT w, UINT h, DXGI_FORMAT fmt, UINT bind, const void *data, UINT pitch)
	{
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = w;
		d.Height = h;
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.Format = fmt;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = bind;
		D3D11_SUBRESOURCE_DATA init = { data, pitch, 0 };
		ID3D11Texture2D *t = nullptr;
		dev->CreateTexture2D(&d, data != nullptr ? &init : nullptr, &t);
		return t;
	}

	std::vector<float> read_rgba16f(ID3D11Device *dev, ID3D11DeviceContext *ctx, ID3D11Texture2D *src)
	{
		D3D11_TEXTURE2D_DESC d;
		src->GetDesc(&d);
		d.Usage = D3D11_USAGE_STAGING;
		d.BindFlags = 0;
		d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		d.MiscFlags = 0;
		ID3D11Texture2D *staging = nullptr;
		dev->CreateTexture2D(&d, nullptr, &staging);
		std::vector<float> out;
		if (staging == nullptr)
			return out;
		ctx->CopyResource(staging, src);
		D3D11_MAPPED_SUBRESOURCE m;
		if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
		{
			out.resize(static_cast<size_t>(d.Width) * d.Height * 4);
			for (UINT y = 0; y < d.Height; ++y)
			{
				const auto *row = reinterpret_cast<const uint16_t *>(static_cast<const uint8_t *>(m.pData) + static_cast<size_t>(y) * m.RowPitch);
				for (UINT i = 0; i < d.Width * 4; ++i)
					out[static_cast<size_t>(y) * d.Width * 4 + i] = from_half(row[i]);
			}
			ctx->Unmap(staging, 0);
		}
		staging->Release();
		return out;
	}

	double psnr_rgb(const std::vector<float> &a, const std::vector<float> &b)
	{
		if (a.size() != b.size() || a.empty())
			return 0.0;
		double se = 0.0;
		size_t n = 0;
		for (size_t i = 0; i < a.size(); ++i)
		{
			if ((i & 3) == 3)
				continue;
			const double d = static_cast<double>(a[i]) - b[i];
			se += d * d;
			n++;
		}
		const double mse = se / static_cast<double>(n);
		return mse <= 1e-12 ? 99.0 : 10.0 * std::log10(1.0 / mse);
	}

	// The harness's synthetic picture, colour in [0, 1], flat depth, zero motion
	struct scene
	{
		ID3D11Texture2D *color = nullptr, *depth = nullptr, *mv = nullptr;
		std::vector<float> reference;
	};

	scene make_scene(ID3D11Device *dev)
	{
		scene s;
		std::vector<uint16_t> c(static_cast<size_t>(W) * H * 4);
		s.reference.resize(c.size());
		for (UINT y = 0; y < H; ++y)
			for (UINT x = 0; x < W; ++x)
			{
				const float fx = static_cast<float>(x), fy = static_cast<float>(y);
				const float dx = fx - 960.0f, dy = fy - 540.0f;
				const float disc = (dx * dx + dy * dy < 300.0f * 300.0f) ? 0.25f : 0.0f;
				const float base[3] = {
					0.35f + 0.25f * std::sin(fx * 0.11f) * std::cos(fy * 0.07f) + disc,
					0.40f + 0.20f * std::sin(fx * 0.05f + fy * 0.04f),
					0.30f + 0.20f * std::cos(fy * 0.13f) - disc * 0.5f,
				};
				for (int k = 0; k < 3; ++k)
				{
					const size_t i = (static_cast<size_t>(y) * W + x) * 4 + k;
					c[i] = to_half(base[k]);
					s.reference[i] = from_half(c[i]);
				}
				const size_t a = (static_cast<size_t>(y) * W + x) * 4 + 3;
				c[a] = to_half(1.0f);
				s.reference[a] = 1.0f;
			}
		s.color = make_tex(dev, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, c.data(), W * 8);
		std::vector<float> d(static_cast<size_t>(W) * H, 0.5f);
		s.depth = make_tex(dev, W, H, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_SHADER_RESOURCE, d.data(), W * 4);
		std::vector<uint32_t> m(static_cast<size_t>(W) * H, 0u);
		s.mv = make_tex(dev, W, H, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_SHADER_RESOURCE, m.data(), W * 4);
		return s;
	}

	std::wstring here(const wchar_t *name)
	{
		return std::wstring(g_dir) + L"\\" + name;
	}

	bool save_f32(const wchar_t *name, const std::vector<float> &v)
	{
		FILE *f = _wfopen(here(name).c_str(), L"wb");
		if (f == nullptr)
			return false;
		const bool ok = fwrite(v.data(), sizeof(float), v.size(), f) == v.size();
		fclose(f);
		return ok;
	}

	std::vector<float> load_f32(const wchar_t *name, size_t count)
	{
		std::vector<float> v;
		FILE *f = _wfopen(here(name).c_str(), L"rb");
		if (f == nullptr)
			return v;
		v.resize(count);
		if (fread(v.data(), sizeof(float), count, f) != count)
			v.clear();
		fclose(f);
		return v;
	}

	ID3D11Texture2D *upload_rgba16f(ID3D11Device *dev, const std::vector<float> &px, UINT bind)
	{
		std::vector<uint16_t> c(px.size());
		for (size_t i = 0; i < px.size(); ++i)
			c[i] = to_half(px[i]);
		return make_tex(dev, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, bind, c.data(), W * 8);
	}

	// ---- NGX device extensions on DXVK's device ----
	using PFN_GetProcAddress = FARPROC(WINAPI *)(HMODULE, LPCSTR);
	PFN_GetProcAddress g_real_gpa = nullptr;
	PFN_vkGetInstanceProcAddr g_real_vk_gipa = nullptr;
	VkInstance g_last_instance = VK_NULL_HANDLE;
	bool g_push_added = false;

	// The device extensions NGX names for NR that DXVK does not request itself
	const char *const g_ngx_device_exts[] = { VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME };

	VKAPI_ATTR VkResult VKAPI_CALL hk_vkCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDevice *out)
	{
		const auto real = reinterpret_cast<PFN_vkCreateDevice>(g_real_vk_gipa(g_last_instance, "vkCreateDevice"));
		const auto enum_exts = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(g_real_vk_gipa(g_last_instance, "vkEnumerateDeviceExtensionProperties"));
		std::vector<const char *> names(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
		uint32_t count = 0;
		enum_exts(pd, nullptr, &count, nullptr);
		std::vector<VkExtensionProperties> props(count);
		enum_exts(pd, nullptr, &count, props.data());
		std::string added;
		const char *only = getenv("FORGE_NRVK_ADD"); // e.g. "push" or "bda"; both when unset
		for (const char *want : g_ngx_device_exts)
		{
			if (only != nullptr && !std::strstr(only, std::strcmp(want, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) == 0 ? "push" : "bda"))
				continue;
			bool present = false, supported = false;
			for (const char *n : names)
				present |= std::strcmp(n, want) == 0;
			for (const auto &p : props)
				supported |= std::strcmp(p.extensionName, want) == 0;
			if (!present && supported)
			{
				names.push_back(want);
				added += added.empty() ? "" : ", ";
				added += want;
			}
		}
		VkDeviceCreateInfo copy = *ci;
		copy.enabledExtensionCount = static_cast<uint32_t>(names.size());
		copy.ppEnabledExtensionNames = names.data();
		const VkResult r = real(pd, &copy, alloc, out);
		g_push_added = r == VK_SUCCESS && !added.empty();
		FORGE_LOG("vkCreateDevice from DXVK: %u extensions requested, added: %s, result %d", ci->enabledExtensionCount, added.empty() ? "(none)" : added.c_str(), r);
		return r;
	}

	VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hk_vkGetInstanceProcAddr(VkInstance instance, const char *name)
	{
		if (instance != VK_NULL_HANDLE)
			g_last_instance = instance;
		if (std::strcmp(name, "vkCreateDevice") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&hk_vkCreateDevice);
		if (std::strcmp(name, "vkGetInstanceProcAddr") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(&hk_vkGetInstanceProcAddr);
		return g_real_vk_gipa(instance, name);
	}

	FARPROC WINAPI hk_GetProcAddress(HMODULE module, LPCSTR name)
	{
		const FARPROC real = g_real_gpa(module, name);
		if (real != nullptr && reinterpret_cast<uintptr_t>(name) > 0xFFFF && std::strcmp(name, "vkGetInstanceProcAddr") == 0)
		{
			g_real_vk_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(real);
			return reinterpret_cast<FARPROC>(&hk_vkGetInstanceProcAddr);
		}
		return real;
	}

	// Replaces GetProcAddress in the import table of `module`
	bool patch_gpa_import(HMODULE module)
	{
		auto *base = reinterpret_cast<uint8_t *>(module);
		const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
		const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
		const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		if (dir.VirtualAddress == 0)
			return false;
		bool patched = false;
		for (auto *imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); imp->Name != 0; ++imp)
		{
			auto *names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
			auto *slots = reinterpret_cast<IMAGE_THUNK_DATA *>(base + imp->FirstThunk);
			for (; names->u1.AddressOfData != 0; ++names, ++slots)
			{
				if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
					continue;
				const auto *by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
				if (std::strcmp(reinterpret_cast<const char *>(by_name->Name), "GetProcAddress") != 0)
					continue;
				DWORD old = 0;
				VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), PAGE_READWRITE, &old);
				if (g_real_gpa == nullptr)
					g_real_gpa = reinterpret_cast<PFN_GetProcAddress>(slots->u1.Function);
				slots->u1.Function = reinterpret_cast<ULONG_PTR>(&hk_GetProcAddress);
				VirtualProtect(&slots->u1.Function, sizeof(slots->u1.Function), old, &old);
				patched = true;
			}
		}
		return patched;
	}

	// ---- D3D12 reference: Forge's bridge on the scene ----
	int d3d12_reference(ID3D11Device *dev, ID3D11DeviceContext *ctx)
	{
		check(forge::ngx_hook::install(GetModuleHandleW(nullptr)), "GetProcAddress import routed");
		HMODULE core = LoadLibraryW(L"_nvngx.dll");
		check(core != nullptr, "NGX core loaded");
		if (core == nullptr)
			return 1;
		GetProcAddress(core, "NVSDK_NGX_D3D11_GetCapabilityParameters"); // makes the routed core known to Forge

		scene sc = make_scene(dev);
		ID3D11Texture2D *image = make_tex(dev, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
		forge::nr::frame f;
		f.ctx = ctx;
		f.image = image;
		f.w = W;
		f.h = H;
		f.depth = sc.depth;
		f.motion = sc.mv;
		f.guide_w = W;
		f.guide_h = H;
		f.depth_inverted = true;
		forge::nr::config c;
		c.enabled = true;
		c.device = 1;
		bool ok = true;
		for (int i = 0; i < frames_per_run; ++i)
		{
			ctx->CopyResource(image, sc.color);
			ok = forge::nr::run(f, c) && ok;
		}
		ctx->Flush();
		const auto st = forge::nr::get_status();
		check(ok && st.active, "D3D12 bridge ran %d frames (%s): network '%s', model %.3f ms GPU%s%s", frames_per_run, st.backend,
			st.pass[0].network, st.gpu_ms, st.reason[0] ? ", " : "", st.reason);
		if (!ok)
			return 1;
		const std::vector<float> in = read_rgba16f(dev, ctx, forge::nr::debug_texture(1));
		const std::vector<float> out = read_rgba16f(dev, ctx, forge::nr::debug_texture(2));
		check(save_f32(L"nrvk-ref-in.f32", in) && save_f32(L"nrvk-ref-out.f32", out), "reference written: model input and answer, PSNR answer vs input %.2f dB",
			psnr_rgb(out, in));
		return 0;
	}

	// ---- Vulkan ----
	struct vk_api
	{
		PFN_vkGetInstanceProcAddr gipa = nullptr;
		PFN_vkGetDeviceProcAddr gdpa = nullptr;
		PFN_vkGetPhysicalDeviceProperties props = nullptr;
		PFN_vkEnumerateDeviceExtensionProperties dev_exts = nullptr;
		PFN_vkCreateCommandPool create_pool = nullptr;
		PFN_vkDestroyCommandPool destroy_pool = nullptr;
		PFN_vkAllocateCommandBuffers alloc_cmd = nullptr;
		PFN_vkBeginCommandBuffer begin = nullptr;
		PFN_vkEndCommandBuffer end = nullptr;
		PFN_vkResetCommandBuffer reset_cmd = nullptr;
		PFN_vkCmdPipelineBarrier barrier = nullptr;
		PFN_vkQueueSubmit submit = nullptr;
		PFN_vkCreateFence create_fence = nullptr;
		PFN_vkWaitForFences wait = nullptr;
		PFN_vkResetFences reset_fence = nullptr;
		PFN_vkDestroyFence destroy_fence = nullptr;
		PFN_vkCreateImageView create_view = nullptr;
		PFN_vkDestroyImageView destroy_view = nullptr;
		PFN_vkCreateQueryPool create_queries = nullptr;
		PFN_vkDestroyQueryPool destroy_queries = nullptr;
		PFN_vkCmdResetQueryPool reset_queries = nullptr;
		PFN_vkCmdWriteTimestamp timestamp = nullptr;
		PFN_vkGetQueryPoolResults query_results = nullptr;
		PFN_vkGetPhysicalDeviceMemoryProperties mem_props = nullptr;
		PFN_vkCreateBuffer create_buffer = nullptr;
		PFN_vkDestroyBuffer destroy_buffer = nullptr;
		PFN_vkGetBufferMemoryRequirements buffer_reqs = nullptr;
		PFN_vkAllocateMemory alloc_mem = nullptr;
		PFN_vkFreeMemory free_mem = nullptr;
		PFN_vkBindBufferMemory bind_buffer = nullptr;
		PFN_vkMapMemory map = nullptr;
		PFN_vkCmdCopyImageToBuffer copy_to_buffer = nullptr;
		PFN_vkCmdBindDescriptorSets2KHR bind_sets2 = nullptr;
	};

	struct vk_image
	{
		ID3D11Texture2D *tex = nullptr;
		VkImage image = VK_NULL_HANDLE;
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		VkImageView view = VK_NULL_HANDLE;
		resource_vk res = {};
	};

	const VkImageSubresourceRange color_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

	bool wrap(const vk_api &vk, VkDevice device, vk_image &img, ID3D11Texture2D *tex, const char *what)
	{
		img.tex = tex;
		IDXGIVkInteropSurface *s = nullptr;
		if (FAILED(tex->QueryInterface(iid_interop_surface, reinterpret_cast<void **>(&s))))
		{
			check(false, "%s: texture has no DXVK interop surface", what);
			return false;
		}
		const HRESULT hr = s->GetVulkanImageInfo(&img.image, &img.layout, &img.info);
		s->Release();
		VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		vi.image = img.image;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = img.info.format;
		vi.subresourceRange = color_range;
		const VkResult vr = SUCCEEDED(hr) ? vk.create_view(device, &vi, nullptr, &img.view) : VK_ERROR_UNKNOWN;
		FORGE_LOG("%s: VkImage %p format %d %ux%u usage 0x%X layout %d view %p (%d)", what, reinterpret_cast<void *>(img.image), img.info.format,
			img.info.extent.width, img.info.extent.height, img.info.usage, img.layout, reinterpret_cast<void *>(img.view), vr);
		if (FAILED(hr) || vr != VK_SUCCESS)
		{
			check(false, "%s: VkImage or view unavailable (hr 0x%08lX, vk %d)", what, static_cast<unsigned long>(hr), vr);
			return false;
		}
		img.res.res.image = { img.view, img.image, color_range, img.info.format, img.info.extent.width, img.info.extent.height };
		img.res.type = 0; // NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW
		img.res.read_write = (img.info.usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
		return true;
	}

	VkImageMemoryBarrier image_barrier(const vk_image &img, VkImageLayout from, VkImageLayout to)
	{
		VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		b.oldLayout = from;
		b.newLayout = to;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = img.image;
		b.subresourceRange = color_range;
		return b;
	}

	std::string ext_list(const VkExtensionProperties *p, uint32_t n)
	{
		std::string s;
		for (uint32_t i = 0; i < n; ++i)
		{
			if (!s.empty())
				s += ", ";
			s += p[i].extensionName;
		}
		return s.empty() ? "(none)" : s;
	}

	// DLSS SR the way the game runs it: NGX D3D11 on the D3D11 device, DLAA, evaluated on the immediate context
	struct sr_probe
	{
		PFN_Init_ProjectID init = nullptr;
		PFN_GetParams get_caps = nullptr, alloc = nullptr;
		PFN_CreateFeature create = nullptr;
		PFN_EvaluateFeature evaluate = nullptr;
		bool ready = false;
		Parameter *p = nullptr;
		Handle *h = nullptr;
		ID3D11Texture2D *out = nullptr;

		bool run(HMODULE core, ID3D11Device *dev, ID3D11DeviceContext *ctx, const scene &sc, const char *label)
		{
			if (!ready)
			{
				init = reinterpret_cast<PFN_Init_ProjectID>(GetProcAddress(core, "NVSDK_NGX_D3D11_Init_ProjectID"));
				get_caps = reinterpret_cast<PFN_GetParams>(GetProcAddress(core, "NVSDK_NGX_D3D11_GetCapabilityParameters"));
				alloc = reinterpret_cast<PFN_GetParams>(GetProcAddress(core, "NVSDK_NGX_D3D11_AllocateParameters"));
				create = reinterpret_cast<PFN_CreateFeature>(GetProcAddress(core, "NVSDK_NGX_D3D11_CreateFeature"));
				evaluate = reinterpret_cast<PFN_EvaluateFeature>(GetProcAddress(core, "NVSDK_NGX_D3D11_EvaluateFeature"));
				static const wchar_t *paths[1];
				paths[0] = g_dir;
				static feature_common_info info = {};
				info.paths = { paths, 1 };
				info.logging = { &ngx_log, 1, false };
				const Result r = init(project_id, 0, "1.0", g_dir, dev, sdk_version_api, &info);
				Parameter *caps = nullptr;
				int avail = -1;
				if (succeeded(r) && succeeded(get_caps(&caps)) && caps != nullptr)
					get_i(caps, name::ss_available, &avail);
				if (!succeeded(r) || avail != 1 || !succeeded(alloc(&p)) || p == nullptr)
				{
					check(false, "DLSS SR on D3D11: init 0x%08X, SuperSampling.Available=%d (nvngx_dlss.dll belongs next to the test)", r, avail);
					return false;
				}
				set_ui(p, name::width, W);
				set_ui(p, name::height, H);
				set_ui(p, name::out_width, W);
				set_ui(p, name::out_height, H);
				set_i(p, name::perf_quality, perf_dlaa);
				set_i(p, name::create_flags, flag_mv_low_res | flag_depth_inverted);
				set_ui(p, name::creation_node_mask, 1);
				set_ui(p, name::visibility_node_mask, 1);
				const Result cr = create(ctx, feature_super_sampling, p, &h);
				out = make_tex(dev, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
				if (!succeeded(cr) || h == nullptr || out == nullptr)
				{
					check(false, "DLSS SR feature on D3D11: 0x%08X", cr);
					return false;
				}
				ready = true;
			}
			Result r = result_success;
			for (int i = 0; i < 6 && succeeded(r); ++i)
			{
				set_d3d11(p, name::color, sc.color);
				set_d3d11(p, name::output, out);
				set_d3d11(p, name::depth, sc.depth);
				set_d3d11(p, name::motion_vectors, sc.mv);
				set_f(p, name::jitter_x, 0.0f);
				set_f(p, name::jitter_y, 0.0f);
				set_f(p, name::mv_scale_x, 1.0f);
				set_f(p, name::mv_scale_y, 1.0f);
				set_i(p, name::reset, i == 0 ? 1 : 0);
				set_ui(p, name::render_subrect_w, W);
				set_ui(p, name::render_subrect_h, H);
				r = evaluate(ctx, h, p, nullptr);
			}
			const double psnr = psnr_rgb(read_rgba16f(dev, ctx, out), sc.reference);
			check(succeeded(r) && psnr > 25.0, "DLSS SR (NGX D3D11, DLAA %ux%u) %s: 0x%08X, PSNR vs input %.2f dB", W, H, label, r, psnr);
			return succeeded(r);
		}
	};

	int vulkan_probe(ID3D11Device *dev, ID3D11DeviceContext *ctx, bool with_sr)
	{
		char msg[1024];

		// 1. DXVK's Vulkan objects
		IDXGIVkInteropDevice *interop = nullptr;
		if (FAILED(dev->QueryInterface(iid_interop_device, reinterpret_cast<void **>(&interop))))
		{
			check(false, "D3D11 device exposes IDXGIVkInteropDevice");
			return 1;
		}
		VkInstance instance = VK_NULL_HANDLE;
		VkPhysicalDevice pd = VK_NULL_HANDLE;
		VkDevice device = VK_NULL_HANDLE;
		VkQueue queue = VK_NULL_HANDLE;
		uint32_t family = ~0u;
		interop->GetVulkanHandles(&instance, &pd, &device);
		interop->GetSubmissionQueue(&queue, &family);
		check(instance && pd && device && queue, "DXVK interop: instance %p, physical device %p, device %p, queue %p (family %u)", reinterpret_cast<void *>(instance),
			reinterpret_cast<void *>(pd), reinterpret_cast<void *>(device), reinterpret_cast<void *>(queue), family);

		vk_api vk;
		HMODULE vulkan = GetModuleHandleW(L"winevulkan.dll");
		if (vulkan == nullptr)
			vulkan = LoadLibraryW(L"winevulkan.dll");
		if (vulkan != nullptr)
			vk.gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vulkan, "vkGetInstanceProcAddr"));
		check(vk.gipa != nullptr, "vkGetInstanceProcAddr from winevulkan.dll (the loader DXVK uses)");
		if (vk.gipa == nullptr)
			return 1;
		vk.gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(vk.gipa(instance, "vkGetDeviceProcAddr"));
		vk.props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(vk.gipa(instance, "vkGetPhysicalDeviceProperties"));
		vk.dev_exts = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(vk.gipa(instance, "vkEnumerateDeviceExtensionProperties"));
#define DEV(field, name) vk.field = reinterpret_cast<PFN_##name>(vk.gdpa(device, #name))
		DEV(create_pool, vkCreateCommandPool);
		DEV(destroy_pool, vkDestroyCommandPool);
		DEV(alloc_cmd, vkAllocateCommandBuffers);
		DEV(begin, vkBeginCommandBuffer);
		DEV(end, vkEndCommandBuffer);
		DEV(reset_cmd, vkResetCommandBuffer);
		DEV(barrier, vkCmdPipelineBarrier);
		DEV(submit, vkQueueSubmit);
		DEV(create_fence, vkCreateFence);
		DEV(wait, vkWaitForFences);
		DEV(reset_fence, vkResetFences);
		DEV(destroy_fence, vkDestroyFence);
		DEV(create_view, vkCreateImageView);
		DEV(destroy_view, vkDestroyImageView);
		DEV(create_queries, vkCreateQueryPool);
		DEV(destroy_queries, vkDestroyQueryPool);
		DEV(reset_queries, vkCmdResetQueryPool);
		DEV(timestamp, vkCmdWriteTimestamp);
		DEV(query_results, vkGetQueryPoolResults);
		DEV(create_buffer, vkCreateBuffer);
		DEV(destroy_buffer, vkDestroyBuffer);
		DEV(buffer_reqs, vkGetBufferMemoryRequirements);
		DEV(alloc_mem, vkAllocateMemory);
		DEV(free_mem, vkFreeMemory);
		DEV(bind_buffer, vkBindBufferMemory);
		DEV(map, vkMapMemory);
		DEV(copy_to_buffer, vkCmdCopyImageToBuffer);
		DEV(bind_sets2, vkCmdBindDescriptorSets2KHR);
		vk.mem_props = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(vk.gipa(instance, "vkGetPhysicalDeviceMemoryProperties"));
#undef DEV
		VkPhysicalDeviceProperties props = {};
		vk.props(pd, &props);
		FORGE_LOG("GPU %s, Vulkan %u.%u.%u, driver 0x%08X, timestamp period %.3f ns", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
			VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion, props.limits.timestampPeriod);

		uint32_t supported_n = 0;
		vk.dev_exts(pd, nullptr, &supported_n, nullptr);
		std::vector<VkExtensionProperties> supported(supported_n);
		vk.dev_exts(pd, nullptr, &supported_n, supported.data());
		const auto is_supported = [&](const char *name) {
			for (const auto &e : supported)
				if (std::strcmp(e.extensionName, name) == 0)
					return true;
			return false;
		};

		// What DXVK's device has enabled: device commands of extensions that are not enabled resolve to NULL
		struct ext_probe { const char *ext; const char *cmd; };
		const ext_probe probes[] = {
			{ "VK_NVX_binary_import", "vkCreateCuModuleNVX" },
			{ "VK_NVX_image_view_handle", "vkGetImageViewHandleNVX" },
			{ "VK_KHR_push_descriptor", "vkCmdPushDescriptorSetKHR" },
			{ "VK_KHR_buffer_device_address", "vkGetBufferDeviceAddressKHR" },
		};
		const auto enabled_on_dxvk = [&](const char *ext) -> int {
			for (const auto &p : probes)
				if (std::strcmp(p.ext, ext) == 0)
					return vk.gdpa(device, p.cmd) != nullptr ? 1 : 0;
			return -1;
		};
		for (const auto &p : probes)
			FORGE_LOG("DXVK device: %s supported by the GPU %s, enabled (%s resolves) %s", p.ext, is_supported(p.ext) ? "yes" : "no", p.cmd,
				vk.gdpa(device, p.cmd) != nullptr ? "yes" : "no");
		FORGE_LOG("DXVK device: vkGetBufferDeviceAddress (core 1.2) resolves %s", vk.gdpa(device, "vkGetBufferDeviceAddress") ? "yes" : "no");

		// 2. NVIDIA's documented query: the extensions NR needs on Vulkan
		HMODULE core = LoadLibraryW(L"_nvngx.dll");
		check(core != nullptr, "NGX core _nvngx.dll loaded");
		if (core == nullptr)
			return 1;
		wchar_t core_path[MAX_PATH] = {};
		GetModuleFileNameW(core, core_path, MAX_PATH);
		FORGE_LOG("NGX core: %ls", core_path);
		sr_probe sr;
		scene sr_scene;
		if (with_sr)
		{
			sr_scene = make_scene(dev);
			sr.run(core, dev, ctx, sr_scene, "before NGX Vulkan");
		}
		const auto get_feature_req = reinterpret_cast<PFN_GetFeatureRequirements>(GetProcAddress(core, "NVSDK_NGX_VULKAN_GetFeatureRequirements"));
		const auto get_inst_req = reinterpret_cast<PFN_GetInstanceExtReq>(GetProcAddress(core, "NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements"));
		const auto get_dev_req = reinterpret_cast<PFN_GetDeviceExtReq>(GetProcAddress(core, "NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements"));
		const auto required_exts = reinterpret_cast<PFN_RequiredExtensions>(GetProcAddress(core, "NVSDK_NGX_VULKAN_RequiredExtensions"));
		const auto init = reinterpret_cast<PFN_InitProjectIdExt>(GetProcAddress(core, "NVSDK_NGX_VULKAN_Init_ProjectID_Ext"));
		const auto get_caps = reinterpret_cast<PFN_VkGetParams>(GetProcAddress(core, "NVSDK_NGX_VULKAN_GetCapabilityParameters"));
		const auto alloc_params = reinterpret_cast<PFN_VkGetParams>(GetProcAddress(core, "NVSDK_NGX_VULKAN_AllocateParameters"));
		const auto destroy_params = reinterpret_cast<PFN_VkDestroyParams>(GetProcAddress(core, "NVSDK_NGX_VULKAN_DestroyParameters"));
		const auto create = reinterpret_cast<PFN_VkCreateFeature1>(GetProcAddress(core, "NVSDK_NGX_VULKAN_CreateFeature1"));
		const auto evaluate = reinterpret_cast<PFN_VkEvaluate>(GetProcAddress(core, "NVSDK_NGX_VULKAN_EvaluateFeature"));
		const auto release = reinterpret_cast<PFN_VkRelease>(GetProcAddress(core, "NVSDK_NGX_VULKAN_ReleaseFeature"));
		check(get_feature_req && get_inst_req && get_dev_req && init && get_caps && alloc_params && destroy_params && create && evaluate && release,
			"NGX core exports the Vulkan entry points");

		static const wchar_t *paths[1];
		paths[0] = g_dir;
		static feature_common_info common = {};
		common.paths = { paths, 1 };
		common.logging = { &ngx_log, getenv("FORGE_NGX_VERBOSE") ? 2 : 1, false };

		bool missing_on_dxvk = false;
		std::string missing;
		for (const int feature : { feature_super_sampling, feature_nr })
		{
			feature_discovery_info di = {};
			di.sdk_version = sdk_version_api;
			di.feature_id = feature;
			di.identifier.type = 1; // project id
			di.identifier.v.project = { project_id, 0, "1.0" };
			di.data_path = g_dir;
			di.feature_info = &common;
			const char *fname = feature == feature_nr ? "NR (18)" : "SR (1)";

			feature_requirement req = {};
			const Result rr = get_feature_req(instance, pd, &di, &req);
			FORGE_LOG("%s: GetFeatureRequirements 0x%08X, FeatureSupported 0x%X, MinHWArchitecture 0x%X, MinOSVersion '%s'", fname, rr, req.supported,
				req.min_hw_arch, req.min_os);

			uint32_t ni = 0;
			VkExtensionProperties *pi = nullptr;
			const Result ri = get_inst_req(&di, &ni, &pi);
			uint32_t nd = 0;
			VkExtensionProperties *pdx = nullptr;
			const Result rd = get_dev_req(instance, pd, &di, &nd, &pdx);
			snprintf(msg, sizeof(msg), "%s: NGX instance extensions (0x%08X): %s; device extensions (0x%08X): %s", fname, ri, ext_list(pi, ni).c_str(), rd,
				ext_list(pdx, nd).c_str());
			check(succeeded(ri) && succeeded(rd), "%s", msg);
			for (uint32_t i = 0; pdx != nullptr && i < nd; ++i)
			{
				const int on = enabled_on_dxvk(pdx[i].extensionName);
				FORGE_LOG("  %s: GPU %s, DXVK device %s", pdx[i].extensionName, is_supported(pdx[i].extensionName) ? "supports" : "lacks",
					on < 0 ? "(not probed)" : (on ? "enabled" : "NOT enabled"));
				if (feature == feature_nr && on == 0)
				{
					missing_on_dxvk = true;
					missing += missing.empty() ? "" : ", ";
					missing += pdx[i].extensionName;
				}
			}
		}
		if (required_exts != nullptr)
		{
			unsigned int ni = 0, nd = 0;
			const char **pi = nullptr, **pdx = nullptr;
			const Result r = required_exts(&ni, &pi, &nd, &pdx);
			std::string s;
			for (unsigned int i = 0; pdx != nullptr && i < nd; ++i)
				s += std::string(i ? ", " : "") + pdx[i];
			FORGE_LOG("RequiredExtensions (deprecated, all features) 0x%08X: %u instance, device: %s", r, ni, s.c_str());
		}
		check(!missing_on_dxvk, "every device extension NGX names for NR is enabled on DXVK's device%s%s", missing.empty() ? "" : "; missing: ", missing.c_str());

		// 3. NGX Vulkan on DXVK's device
		const Result ri = init(project_id, 0, "1.0", g_dir, instance, pd, device, vk.gipa, vk.gdpa, sdk_version_api, &common);
		check(succeeded(ri), "NGX Vulkan init on DXVK's instance/device: 0x%08X", ri);
		if (!succeeded(ri))
			return 1;
		Parameter *caps = nullptr;
		check(succeeded(get_caps(&caps)) && caps != nullptr, "NGX Vulkan capability parameters");
		if (caps == nullptr)
			return 1;
		int nr_avail = -1, nr_init = -1, ss_avail = -1, ss_init = -1;
		unsigned int nr_needs_update = 0;
		get_i(caps, "DLSSNR.Available", &nr_avail);
		get_i(caps, "DLSSNR.FeatureInitResult", &nr_init);
		get_ui(caps, "DLSSNR.NeedsUpdatedDriver", &nr_needs_update);
		get_i(caps, "SuperSampling.Available", &ss_avail);
		get_i(caps, "SuperSampling.FeatureInitResult", &ss_init);
		FORGE_LOG("Vulkan caps: SuperSampling.Available=%d (0x%08X), DLSSNR.NeedsUpdatedDriver=%u", ss_avail, static_cast<unsigned int>(ss_init), nr_needs_update);
		check(nr_avail == 1, "DLSSNR.Available=%d FeatureInitResult=0x%08X on Vulkan", nr_avail, static_cast<unsigned int>(nr_init));
		if (nr_avail != 1)
			return 1;

		// 4. Resources: D3D11 textures, addressed as DXVK's VkImages
		const std::vector<float> ref_in = load_f32(L"nrvk-ref-in.f32", static_cast<size_t>(W) * H * 4);
		const std::vector<float> ref_out = load_f32(L"nrvk-ref-out.f32", static_cast<size_t>(W) * H * 4);
		const bool have_ref = !ref_in.empty() && !ref_out.empty();
		scene sc = make_scene(dev);
		ID3D11Texture2D *source = have_ref ? upload_rgba16f(dev, ref_in, D3D11_BIND_SHADER_RESOURCE) : sc.color;
		const std::vector<float> &input_px = have_ref ? ref_in : sc.reference;
		FORGE_LOG("model input: %s", have_ref ? "the D3D12 bridge's model input (nrvk-ref-in.f32)" : "the synthetic picture");

		vk_image color, depth, motion, output;
		ID3D11Texture2D *color_tex = make_tex(dev, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
		ID3D11Texture2D *output_tex = make_tex(dev, W, H, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
		ctx->CopyResource(color_tex, source);
		// A marker value in the output tells an untouched output from one the model wrote
		ID3D11UnorderedAccessView *output_uav = nullptr;
		dev->CreateUnorderedAccessView(output_tex, nullptr, &output_uav);
		const float marker[4] = { 0.25f, 0.25f, 0.25f, 0.25f };
		if (output_uav != nullptr)
			ctx->ClearUnorderedAccessViewFloat(output_uav, marker);
		// Everything DXVK has recorded so far completes on the GPU before NGX touches the images
		const auto gpu_idle = [&] {
			D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
			ID3D11Query *q = nullptr;
			dev->CreateQuery(&qd, &q);
			ctx->End(q);
			ctx->Flush();
			BOOL done = FALSE;
			while (ctx->GetData(q, &done, sizeof(done), 0) == S_FALSE)
				Sleep(0);
			q->Release();
		};
		gpu_idle();
		if (!wrap(vk, device, color, color_tex, "color") || !wrap(vk, device, depth, sc.depth, "depth") || !wrap(vk, device, motion, sc.mv, "motion") ||
			!wrap(vk, device, output, output_tex, "output"))
			return 1;

		VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = family;
		VkCommandPool pool = VK_NULL_HANDLE;
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		VkQueryPool queries = VK_NULL_HANDLE;
		VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		VkQueryPoolCreateInfo qci = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
		qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
		qci.queryCount = 2;
		bool ok = vk.create_pool(device, &pci, nullptr, &pool) == VK_SUCCESS;
		cai.commandPool = pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		ok = ok && vk.alloc_cmd(device, &cai, &cmd) == VK_SUCCESS && vk.create_fence(device, &fci, nullptr, &fence) == VK_SUCCESS &&
			vk.create_queries(device, &qci, nullptr, &queries) == VK_SUCCESS;
		check(ok, "command pool, command buffer, fence and timestamp queries on DXVK's device (queue family %u)", family);
		if (!ok)
			return 1;

		// DXVK runs NVIDIA drivers in VK_EXT_descriptor_heap mode, where a command buffer starts without the
		// driver-managed heap the CUDA kernels address their image-view handles in. DXVK's own cubin launches
		// rebind it with an empty legacy vkCmdBindDescriptorSets2 call (dxvk_context.cpp, launchCuKernelNVX).
		const char *no_bind = getenv("FORGE_NRVK_NO_LEGACY_BIND");
		const bool legacy_bind = vk.bind_sets2 != nullptr && !(no_bind && no_bind[0] == '1');
		FORGE_LOG("driver-managed descriptor heap rebind before NGX: %s", legacy_bind ? "on" : (vk.bind_sets2 ? "off" : "vkCmdBindDescriptorSets2KHR unavailable"));

		// Records `body` between barriers into the layouts NGX expects, submits it on DXVK's queue after
		// DXVK's own work, and waits. Returns the GPU time between the two timestamps.
		const auto run = [&](auto body, bool timed) -> double {
			interop->FlushRenderingCommands();
			vk.reset_cmd(cmd, 0);
			VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
			bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
			vk.begin(cmd, &bi);
			if (timed)
				vk.reset_queries(cmd, queries, 0, 2);
			const VkImageMemoryBarrier to_ngx[4] = {
				image_barrier(color, color.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
				image_barrier(depth, depth.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
				image_barrier(motion, motion.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
				image_barrier(output, output.layout, VK_IMAGE_LAYOUT_GENERAL),
			};
			vk.barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 4, to_ngx);
			if (timed)
				vk.timestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
			if (legacy_bind)
			{
				VkBindDescriptorSetsInfoKHR bind = { VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO_KHR };
				bind.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
				vk.bind_sets2(cmd, &bind);
			}
			const Result r = body();
			if (timed)
				vk.timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
			const VkImageMemoryBarrier back[4] = {
				image_barrier(color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color.layout),
				image_barrier(depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, depth.layout),
				image_barrier(motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, motion.layout),
				image_barrier(output, VK_IMAGE_LAYOUT_GENERAL, output.layout),
			};
			vk.barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 4, back);
			vk.end(cmd);
			VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
			si.commandBufferCount = 1;
			si.pCommandBuffers = &cmd;
			vk.reset_fence(device, 1, &fence);
			interop->LockSubmissionQueue();
			const VkResult sr = vk.submit(queue, 1, &si, fence);
			interop->ReleaseSubmissionQueue();
			const VkResult wr = sr == VK_SUCCESS ? vk.wait(device, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000) : sr;
			if (!succeeded(r) || wr != VK_SUCCESS)
			{
				FORGE_LOG("submission: NGX 0x%08X, submit %d, wait %d", r, sr, wr);
				return -2.0;
			}
			if (!timed)
				return 0.0;
			uint64_t ts[2] = {};
			if (vk.query_results(device, queries, 0, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
				return -1.0;
			return static_cast<double>(ts[1] - ts[0]) * props.limits.timestampPeriod / 1.0e6;
		};

		// 5. Creation, exactly the parameters the D3D12 bridge writes for pass 1 at defaults
		Parameter *p = nullptr;
		check(succeeded(alloc_params(&p)) && p != nullptr, "NGX Vulkan parameter block");
		if (p == nullptr)
			return 1;
		const auto tuning = [&] {
			set_f(p, "DLSSNR.Intensity", 1.0f);
			set_ui(p, "DLSSNR.Style", 0);
			set_f(p, "DLSSNR.LocalStructureStrength", 1.0f);
			set_f(p, "DLSSNR.LocalToneStrength", 1.0f);
			set_f(p, "DLSSNR.SkinStructureStrength", -1.0f);
			set_ptr(p, "DLSSNR.ControlMask", nullptr);
			set_ui(p, "DLSSNR.UseAutoMask", 1);
		};
		set_ui(p, "DLSSNR.Enabled", 1);
		set_ui(p, "DLSSNR.Width", W);
		set_ui(p, "DLSSNR.Height", H);
		set_ui(p, name::creation_node_mask, 1);
		set_ui(p, name::visibility_node_mask, 1);
		set_ui(p, "DLSSNR.Hint.Render.Preset", 0);
		set_ui(p, "DLSSNR.UICorrection", 1);
		tuning();
		Handle *handle = nullptr;
		Result cr = result_fail;
		const ULONGLONG t0 = GetTickCount64();
		const double created = run([&] { return cr = create(device, cmd, feature_nr, p, &handle); }, false);
		check(succeeded(cr) && handle != nullptr && created >= 0.0, "NR feature created on Vulkan: 0x%08X in %llu ms, network '%s'", cr,
			static_cast<unsigned long long>(GetTickCount64() - t0), g_network.c_str());
		if (handle == nullptr)
			return 1;

		// 6. Evaluation: the static picture for the same number of frames the reference ran
		double gpu_total = 0.0;
		int gpu_n = 0;
		Result er = result_success;
		bool eval_ok = true;
		for (int i = 0; i < frames_per_run && eval_ok; ++i)
		{
			ctx->CopyResource(color_tex, source);
			gpu_idle();
			set_ptr(p, "DLSSNR.Color", &color.res);
			set_ptr(p, "DLSSNR.Depth", &depth.res);
			set_ptr(p, "DLSSNR.MVec", &motion.res);
			set_ptr(p, "DLSSNR.Output", &output.res);
			set_ui(p, "DLSSNR.Enabled", 1);
			set_ui(p, "DLSSNR.Width", W);
			set_ui(p, "DLSSNR.Height", H);
			set_ui(p, "DLSSNR.DepthInverted", 1);
			set_ui(p, "DLSSNR.Reset", i == 0 ? 1u : 0u);
			for (const char *n : { "Color", "Output", "Depth", "MVec" })
			{
				char k[64];
				snprintf(k, sizeof(k), "DLSSNR.%sSubrectBaseX", n);
				set_ui(p, k, 0);
				snprintf(k, sizeof(k), "DLSSNR.%sSubrectBaseY", n);
				set_ui(p, k, 0);
				snprintf(k, sizeof(k), "DLSSNR.%sSubrectWidth", n);
				set_ui(p, k, W);
				snprintf(k, sizeof(k), "DLSSNR.%sSubrectHeight", n);
				set_ui(p, k, H);
			}
			set_f(p, "DLSSNR.MVecScaleX", 1.0f);
			set_f(p, "DLSSNR.MVecScaleY", 1.0f);
			tuning();
			const double ms = run([&] { return er = evaluate(cmd, handle, p, nullptr); }, true);
			eval_ok = succeeded(er) && ms > -1.5;
			if (ms >= 0.0 && i >= 2)
			{
				gpu_total += ms;
				gpu_n++;
			}
		}
		// The output image read straight from Vulkan into a host buffer of our own, past DXVK's tracking
		{
			const VkDeviceSize bytes = static_cast<VkDeviceSize>(W) * H * 8;
			VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
			bci.size = bytes;
			bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			VkBuffer buf = VK_NULL_HANDLE;
			VkDeviceMemory mem = VK_NULL_HANDLE;
			VkMemoryRequirements req = {};
			VkPhysicalDeviceMemoryProperties mp = {};
			vk.mem_props(pd, &mp);
			bool ok_buf = vk.create_buffer(device, &bci, nullptr, &buf) == VK_SUCCESS;
			if (ok_buf)
			{
				vk.buffer_reqs(device, buf, &req);
				uint32_t type = ~0u;
				for (uint32_t t = 0; t < mp.memoryTypeCount && type == ~0u; ++t)
					if ((req.memoryTypeBits & (1u << t)) &&
						(mp.memoryTypes[t].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
							(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
						type = t;
				VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
				mai.allocationSize = req.size;
				mai.memoryTypeIndex = type;
				ok_buf = type != ~0u && vk.alloc_mem(device, &mai, nullptr, &mem) == VK_SUCCESS && vk.bind_buffer(device, buf, mem, 0) == VK_SUCCESS;
			}
			if (ok_buf)
			{
				VkBufferImageCopy region = {};
				region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
				region.imageExtent = { W, H, 1 };
				run([&] {
					vk.copy_to_buffer(cmd, output.image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &region);
					return result_success;
				}, false);
				void *mapped = nullptr;
				vk.map(device, mem, 0, bytes, 0, &mapped);
				size_t marker_px = 0, n = static_cast<size_t>(W) * H;
				const auto *px = static_cast<const uint16_t *>(mapped);
				std::vector<float> direct(n * 4);
				for (size_t i = 0; i < n * 4; ++i)
					direct[i] = from_half(px[i]);
				for (size_t i = 0; i < n; ++i)
					marker_px += direct[i * 4] == 0.25f && direct[i * 4 + 1] == 0.25f && direct[i * 4 + 2] == 0.25f;
				FORGE_LOG("output VkImage read directly: %.2f %% marker, PSNR vs input %.2f dB%s", 100.0 * marker_px / n, psnr_rgb(direct, input_px),
					!ref_out.empty() ? "" : "");
				if (!ref_out.empty())
					FORGE_LOG("output VkImage read directly vs D3D12 answer: PSNR %.2f dB", psnr_rgb(direct, ref_out));
			}
			else
				FORGE_LOG("direct readback buffer unavailable");
			if (buf != VK_NULL_HANDLE)
				vk.destroy_buffer(device, buf, nullptr);
			if (mem != VK_NULL_HANDLE)
				vk.free_mem(device, mem, nullptr);
		}
		const std::vector<float> out = read_rgba16f(dev, ctx, output_tex);
		size_t untouched = 0, zero = 0;
		for (size_t i = 0; i + 3 < out.size(); i += 4)
		{
			untouched += out[i] == 0.25f && out[i + 1] == 0.25f && out[i + 2] == 0.25f && out[i + 3] == 0.25f;
			zero += out[i] == 0.0f && out[i + 1] == 0.0f && out[i + 2] == 0.0f;
		}
		FORGE_LOG("output after evaluation: %.2f %% still the marker, %.2f %% black", 100.0 * untouched / (out.size() / 4), 100.0 * zero / (out.size() / 4));
		const double vs_input = psnr_rgb(out, input_px);
		check(eval_ok && vs_input > 18.0 && vs_input < 90.0, "NR evaluated on Vulkan %d frames: 0x%08X, PSNR answer vs input %.2f dB, model %.3f ms GPU", frames_per_run, er,
			vs_input, gpu_n ? gpu_total / gpu_n : -1.0);
		save_f32(L"nrvk-vk-out.f32", out);
		if (have_ref)
		{
			const double vs_ref = psnr_rgb(out, ref_out);
			check(vs_ref > 40.0, "Vulkan answer vs D3D12 bridge answer on the same input: PSNR %.2f dB (D3D12 answer vs input %.2f dB)", vs_ref,
				psnr_rgb(ref_out, ref_in));
		}

		if (with_sr && sr.ready)
			sr.run(core, dev, ctx, sr_scene, "after Vulkan NR");

		// 7. A live parameter change and a rebuild, as Forge does at runtime
		release(handle);
		handle = nullptr;
		set_ui(p, "DLSSNR.Hint.Render.Preset", 2);
		cr = result_fail;
		run([&] { return cr = create(device, cmd, feature_nr, p, &handle); }, false);
		check(succeeded(cr) && handle != nullptr, "NR rebuilt with preset 2 on Vulkan: 0x%08X, network '%s'", cr, g_network.c_str());
		if (handle != nullptr)
			release(handle);
		destroy_params(p);

		vk.destroy_queries(device, queries, nullptr);
		vk.destroy_fence(device, fence, nullptr);
		vk.destroy_pool(device, pool, nullptr);
		for (vk_image *img : { &color, &depth, &motion, &output })
			vk.destroy_view(device, img->view, nullptr);
		interop->Release();
		return 0;
	}
}

int main(int argc, char **argv)
{
	GetModuleFileNameW(nullptr, g_dir, MAX_PATH);
	*wcsrchr(g_dir, L'\\') = 0;
	const bool ref = argc > 1 && std::strcmp(argv[1], "--d3d12-ref") == 0;
	bool push = false, with_sr = false;
	for (int i = 1; i < argc; ++i)
	{
		push |= std::strcmp(argv[i], "--push") == 0;
		with_sr |= std::strcmp(argv[i], "--with-sr") == 0;
	}
	if (const char *size = getenv("FORGE_NRVK_SIZE"))
		sscanf(size, "%ux%u", &W, &H);
	wchar_t log_path[MAX_PATH] = {};
	swprintf(log_path, MAX_PATH, L"%ls\\forge-nrvk%ls.log", g_dir, ref ? L"-d3d12" : (push ? L"-push" : L""));
	forge::log::open(log_path);
	setvbuf(stdout, nullptr, _IONBF, 0);
	if (push)
	{
		bool any = false;
		for (const wchar_t *m : { L"dxgi.dll", L"d3d11.dll" })
			if (HMODULE h = LoadLibraryW(m))
				any = patch_gpa_import(h) || any;
		check(any, "GetProcAddress imports of DXVK's dxgi.dll and d3d11.dll routed");
	}

	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
	const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
	check(SUCCEEDED(hr), "D3D11 device (DXVK) created");
	if (FAILED(hr))
		return 2;

	if (push)
		check(g_push_added, "NGX device extensions added to DXVK's vkCreateDevice");
	const int rc = ref ? d3d12_reference(dev, ctx) : vulkan_probe(dev, ctx, with_sr);
	printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
	FORGE_LOG("%d failure(s), exit %d", g_failures, rc);
	forge::log::close();
	// NGX's shutdown faults under Proton (see the harness); the process exits with NGX up
	TerminateProcess(GetCurrentProcess(), g_failures ? 1 : 0);
	return 0;
}
