#include "nr_bridge.h"
#include "nr_dll.h"
#include "nr_contract.h"
#include "nr_shader_params.h"
#include "log.h"
#include "ngx_hook.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include "nr_shaders.h"
#include "vk_interop.h"
#include <dxgi1_4.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <cwchar>
#include <initializer_list>
#include <utility>
#include <vector>

using namespace ngx;
using forge::nr::max_passes;

namespace
{
	constexpr int feature_nr = 18;
	constexpr unsigned int ring = 3; // submission slots, one for each frame
	constexpr ULONGLONG settle_ms = 500;

	using PFN_D3D12_Init_ProjectID = Result (*)(const char *, int, const char *, const wchar_t *, ID3D12Device *, int, const void *);
	using PFN_D3D12_GetParams = Result (*)(Parameter **);
	using PFN_D3D12_CreateFeature = Result (*)(ID3D12GraphicsCommandList *, int, Parameter *, Handle **);
	using PFN_D3D12_EvaluateFeature = Result (*)(ID3D12GraphicsCommandList *, const Handle *, const Parameter *, void *);
	using PFN_D3D12_ReleaseFeature = Result (*)(Handle *);

	struct path_list { const wchar_t *const *path; unsigned int length; };
	struct logging_info { void (*callback)(const char *, int, int); int min_level; bool disable_other_sinks; };
	struct feature_common_info { path_list paths; void *internal; logging_info logging; };

	// D3D12's device factory (D3D12GetInterface). ReShade wraps only what D3D12CreateDevice returns, so a device
	// from the factory gets no ReShade proxy or add-on events. vkd3d-proton and current Windows runtimes have it.
	constexpr GUID clsid_d3d12_device_factory = { 0x114863bf, 0xc386, 0x4aee, { 0xb3, 0x9d, 0x8f, 0x0b, 0xbb, 0x06, 0x29, 0x55 } };
	using PFN_D3D12GetInterface = HRESULT(WINAPI *)(REFCLSID, REFIID, void **);
	bool g_skip_device_factory = false; // test hook



	using cb_data = forge::nr::shader_params;

	// ControlMask: red weights Intensity, green multiplies tone, blue multiplies structure.
	constexpr DXGI_FORMAT mask_format = DXGI_FORMAT_R8G8B8A8_UNORM;

	// Packed motion vectors, in the format of the game's own vectors. Windows rejects a shared R32G32_FLOAT texture
	// (E_INVALIDARG). A shared R16G16_FLOAT texture passed the same test, with the D3D12 import.
	constexpr DXGI_FORMAT motion_format = DXGI_FORMAT_R16G16_FLOAT;

	uint16_t to_half(float f)
	{
		uint32_t x;
		std::memcpy(&x, &f, 4);
		const uint32_t sign = (x >> 16) & 0x8000u;
		int exp = static_cast<int>((x >> 23) & 0xFF) - 127 + 15;
		uint32_t mant = x & 0x7FFFFFu;
		if (((x >> 23) & 0xFF) == 0xFF)
			return static_cast<uint16_t>(sign | 0x7C00u | (mant ? 0x200u : 0u));
		if (exp >= 31)
			return static_cast<uint16_t>(sign | 0x7C00u);
		if (exp <= 0)
		{
			if (exp < -10)
				return static_cast<uint16_t>(sign);
			mant |= 0x800000u;
			const uint32_t shift = static_cast<uint32_t>(14 - exp);
			uint32_t h = mant >> shift;
			if ((mant >> (shift - 1)) & 1u)
				h++;
			return static_cast<uint16_t>(sign | h);
		}
		uint32_t h = static_cast<uint32_t>(exp << 10) | (mant >> 13);
		if (mant & 0x1000u)
			h++;
		return static_cast<uint16_t>(sign | h);
	}

	template <typename T>
	void safe_release(T *&p)
	{
		if (p != nullptr)
			p->Release();
		p = nullptr;
	}

	// Destroy what the Vulkan backend made on DXVK's device
	PFN_vkDestroyImageView s_destroy_view = nullptr;
	PFN_vkDestroyImage s_destroy_image = nullptr;
	PFN_vkFreeMemory s_free_memory = nullptr;
	VkDevice s_view_device = VK_NULL_HANDLE;

	// A texture shared by the D3D11 shaders and the model: a D3D12 resource opened from its NT handle, or a
	// VkImage created here that DXVK wraps as a D3D11 texture.
	struct shared_tex
	{
		ID3D11Texture2D *t11 = nullptr;
		ID3D11ShaderResourceView *srv = nullptr;
		ID3D11UnorderedAccessView *uav = nullptr;
		ID3D12Resource *t12 = nullptr;
		VkImage image = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
		VkImageView view = VK_NULL_HANDLE;
		ngxvk::resource res = {};
		unsigned int w = 0, h = 0;
		DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;

		void release()
		{
			if (view != VK_NULL_HANDLE && s_destroy_view != nullptr)
				s_destroy_view(s_view_device, view, nullptr);
			view = VK_NULL_HANDLE;
			res = {};
			safe_release(srv);
			safe_release(uav);
			safe_release(t12);
			safe_release(t11);
			// The caller drained DXVK's queue; DXVK leaves imported images to their owner
			if (image != VK_NULL_HANDLE && s_destroy_image != nullptr)
				s_destroy_image(s_view_device, image, nullptr);
			if (memory != VK_NULL_HANDLE && s_free_memory != nullptr)
				s_free_memory(s_view_device, memory, nullptr);
			image = VK_NULL_HANDLE;
			memory = VK_NULL_HANDLE;
			w = h = 0;
		}
	};

	// Typed view format for a typeless or sRGB colour resource (UAVs take no sRGB formats)
	DXGI_FORMAT typed_uav_format(DXGI_FORMAT f)
	{
		switch (f)
		{
		case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
		case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
		case DXGI_FORMAT_R8G8B8A8_TYPELESS:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_TYPELESS:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
		case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
		default: return f;
		}
	}

	// SRV format for a resource the game created, which may be typeless or a depth format
	DXGI_FORMAT readable_format(DXGI_FORMAT f)
	{
		switch (f)
		{
		case DXGI_FORMAT_R24G8_TYPELESS:
		case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
		case DXGI_FORMAT_R32_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
		case DXGI_FORMAT_R32G8X24_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
		case DXGI_FORMAT_R16_TYPELESS:
		case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
		case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
		case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
		default: return typed_uav_format(f);
		}
	}

	struct view_cache
	{
		ID3D11Resource *res = nullptr;
		ID3D11ShaderResourceView *srv = nullptr;
		ID3D11UnorderedAccessView *uav = nullptr;
		unsigned int w = 0, h = 0;
		uint64_t used = 0;

		void release()
		{
			safe_release(srv);
			safe_release(uav);
			safe_release(res);
			w = h = 0;
			used = 0;
		}
	};

	// Views on one kind of game texture. A few entries, since a game may alternate between buffers.
	struct view_set
	{
		view_cache entry[4];
		view_cache *current = nullptr;
		uint64_t tick = 0;
		const void *seen[8] = {}; // resources already described in the log; survives release()
		unsigned int seen_next = 0;

		view_cache &operator*() const { return *current; }
		view_cache *operator->() const { return current; }
		void release()
		{
			for (view_cache &e : entry)
				e.release();
			current = nullptr;
		}
	};

	struct pass_feature
	{
		Handle *handle = nullptr;
		forge::nr::tuning built;
		forge::nr::tuning evaluated;
		forge::nr::tuning pending;
		ULONGLONG pending_since = 0;
		bool reset = true;
		char network[64] = {};
	};

	// NGX's Vulkan API on DXVK's VkDevice and queue. The shared textures are D3D11 textures addressed through their
	// VkImages; the model's command buffers follow everything DXVK has recorded.
	struct vk_backend
	{
		bool on = false;
		dxvk_interop::IDXGIVkInteropDevice1 *interop = nullptr; // of the attached D3D11 device
		VkInstance instance = VK_NULL_HANDLE;
		VkPhysicalDevice pd = VK_NULL_HANDLE;
		VkDevice dev = VK_NULL_HANDLE;
		VkQueue queue = VK_NULL_HANDLE;
		uint32_t family = 0;
		float ts_period = 0.0f;
		PFN_vkGetInstanceProcAddr gipa = nullptr;
		PFN_vkGetDeviceProcAddr gdpa = nullptr;
		PFN_vkCreateCommandPool create_pool = nullptr;
		PFN_vkAllocateCommandBuffers alloc_cmd = nullptr;
		PFN_vkBeginCommandBuffer begin = nullptr;
		PFN_vkEndCommandBuffer end = nullptr;
		PFN_vkResetCommandBuffer reset_cmd = nullptr;
		PFN_vkCmdPipelineBarrier barrier = nullptr;
		PFN_vkQueueSubmit submit = nullptr;
		PFN_vkCreateFence create_fence = nullptr;
		PFN_vkWaitForFences wait = nullptr;
		PFN_vkResetFences reset_fence = nullptr;
		PFN_vkCreateImageView create_view = nullptr;
		PFN_vkDestroyImageView destroy_view = nullptr;
		PFN_vkCreateQueryPool create_queries = nullptr;
		PFN_vkCmdResetQueryPool reset_queries = nullptr;
		PFN_vkCmdWriteTimestamp timestamp = nullptr;
		PFN_vkGetQueryPoolResults query_results = nullptr;
		PFN_vkCmdBindDescriptorSets2KHR bind_sets2 = nullptr;
		PFN_vkCreateImage create_image = nullptr;
		PFN_vkDestroyImage destroy_image = nullptr;
		PFN_vkGetImageMemoryRequirements image_reqs = nullptr;
		PFN_vkAllocateMemory alloc_mem = nullptr;
		PFN_vkFreeMemory free_mem = nullptr;
		PFN_vkBindImageMemory bind_image = nullptr;
		PFN_vkQueueWaitIdle queue_idle = nullptr;
		VkPhysicalDeviceMemoryProperties mem_props = {};
		VkCommandPool pool = VK_NULL_HANDLE;
		VkCommandBuffer cmd[ring] = {};
		VkFence fence[ring] = {};
		bool pending[ring] = {};
		bool timed[ring] = {};
		VkQueryPool queries = VK_NULL_HANDLE;
		ngxvk::PFN_Init_ProjectID_Ext init = nullptr;
		ngxvk::PFN_CreateFeature1 create = nullptr;
		ngxvk::PFN_EvaluateFeature evaluate = nullptr;
		double sync_ms = -1.0; // CPU time of the per-frame DXVK flush
	};

	// Frame log (config::frame_log): stats_rows x stats_cols floats per frame, read from a ring of staging textures.
	// Rows: 0 model chain, 1 frame against the game frame, 2 motion vectors.
	constexpr unsigned int stats_ring = 8;
	constexpr unsigned int stats_rows = 3;
	constexpr unsigned int stats_cols = 8;
	constexpr unsigned int row_chain = 0, row_frame = 1, row_motion = 2;

	struct frame_record
	{
		bool pending = false;
		uint64_t frame = 0;
		double time_ms = 0.0;
		unsigned int reset_bits = 0; // 1 game, 2 first/failed, 4 gap, 8 inputs, 16 feature, 32 controls
		unsigned int mode = 0;       // 0 linear HDR, 1 display-referred
		unsigned int passes = 0;
	};

	struct state
	{
		SRWLOCK lock = SRWLOCK_INIT;
		forge::nr::status st;
		char logged_reason[200] = {};

		ID3D11Device *dev11 = nullptr;
		ID3D11Fence *fence11 = nullptr;
		ID3D12Device *dev12 = nullptr;
		ID3D12CommandQueue *queue = nullptr;
		ID3D12CommandAllocator *alloc[ring] = {};
		uint64_t alloc_done[ring] = {};
		unsigned int slot = 0;
		ID3D12GraphicsCommandList *list = nullptr;
		ID3D12Fence *fence12 = nullptr;
		uint64_t fence_value = 0;
		HANDLE event = nullptr;
		ID3D12QueryHeap *queries = nullptr;
		ID3D12Resource *query_readback = nullptr;
		uint64_t query_done[ring] = {};
		uint64_t ts_freq = 0;

		PFN_D3D12_Init_ProjectID init = nullptr;
		PFN_D3D12_GetParams get_caps = nullptr;
		PFN_D3D12_GetParams alloc_params = nullptr;
		PFN_D3D12_CreateFeature create = nullptr;
		PFN_D3D12_EvaluateFeature evaluate = nullptr;
		PFN_D3D12_ReleaseFeature release_feature = nullptr;
		bool ngx_ready = false;
		Parameter *caps = nullptr;
		Parameter *params = nullptr;

		pass_feature pass[max_passes];
		unsigned int feature_w = 0, feature_h = 0;
		unsigned int size_w = 0, size_h = 0; // requested working size and since when
		ULONGLONG size_since = 0;
		bool probe = false;
		bool history_valid = false;
		forge::nr::history_key history;
		std::array<float, 2> history_jitter = {};
		uint64_t history_present = 0, probe_revision = 0;

		// Model input at work size, ping-pong model answers, guides, control mask; all shared with the D3D12 device
		shared_tex proxy, model[2], depth, motion, mask;
		// D3D11 only: which developer-mask group each pixel of the mask belongs to, for the overlay
		ID3D11Texture2D *groups = nullptr;
		ID3D11ShaderResourceView *groups_srv = nullptr;
		ID3D11UnorderedAccessView *groups_uav = nullptr;
		unsigned int groups_w = 0, groups_h = 0;
		// Test hook: an explicit mask picture instead of the painted one
		std::vector<float> probe_mask;
		unsigned int probe_w = 0, probe_h = 0;
		DXGI_FORMAT probe_format = DXGI_FORMAT_UNKNOWN;
		bool probe_dirty = false;
		bool mask_logged = false;
		// D3D11 only: the untouched DLSS output region
		ID3D11Texture2D *keep = nullptr;
		ID3D11ShaderResourceView *keep_srv = nullptr;
		ID3D11Texture2D *output = nullptr;
		ID3D11RenderTargetView *output_rtv = nullptr;
		ID3D11ShaderResourceView *output_srv = nullptr;
		unsigned int keep_w = 0, keep_h = 0;
		DXGI_FORMAT keep_format = DXGI_FORMAT_UNKNOWN;

		ID3D11ComputeShader *pack_color_cs = nullptr, *pack_guides_cs = nullptr, *pack_work_cs = nullptr, *pack_mask_cs = nullptr;
		ID3D11PixelShader *resolve_ps = nullptr;
		ID3D11VertexShader *output_vs = nullptr;
		ID3D11RasterizerState *rasterizer = nullptr;
		ID3DDeviceContextState *context_state = nullptr;
		// Frame log
		ID3D11ComputeShader *stats_cs = nullptr;
		ID3D11Texture2D *stats = nullptr;
		ID3D11UnorderedAccessView *stats_uav = nullptr;
		ID3D11Buffer *stats_cb = nullptr;
		ID3D11Texture2D *stats_read[stats_ring] = {};
		frame_record records[stats_ring];
		uint64_t stats_next = 0, stats_done = 0; // frames: the next record to write, the oldest record to read
		bool frame_log = false;
		LARGE_INTEGER log_start = {};
		ID3D11Buffer *cb = nullptr;
		view_set depth_in, motion_in;
		bool shader_failed = false;

		vk_backend v;
	} g;

	wchar_t g_dir[MAX_PATH] = {};
	char g_last_network[64] = {};
	std::atomic<bool> g_retry { false };
	std::atomic<uint64_t> g_present { 0 };

	struct present_capture
	{
		forge::nr::frame metadata;
		ID3D11Device *device = nullptr;
		ID3D11Texture2D *depth = nullptr, *motion = nullptr;
		uint64_t serial = 0;
		bool ready = false;

		void release()
		{
			ready = false;
			safe_release(depth);
			safe_release(motion);
			safe_release(device);
			metadata = {};
		}
	} g_capture;

	// Parameters the snippet reads at every evaluate (measured: a value written only at evaluate matches a
	// feature built with it). The preset picks the network and is fixed when the feature is built.
	const char *const s_live[] = { "DLSSNR.Style", "DLSSNR.Intensity", "DLSSNR.LocalStructureStrength", "DLSSNR.LocalToneStrength",
		"DLSSNR.SkinStructureStrength", "DLSSNR.UseAutoMask", "DLSSNR.ControlMask", "DLSSNR.MaskBlendMode" };

	bool live(const char *name)
	{
		if (g.probe)
			return true;
		for (const char *n : s_live)
			if (std::strcmp(n, name) == 0)
				return true;
		return false;
	}

	void set_reason(const char *fmt, ...)
	{
		char buf[200];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		AcquireSRWLockExclusive(&g.lock);
		std::memcpy(g.st.reason, buf, sizeof(buf));
		ReleaseSRWLockExclusive(&g.lock);
		if (std::strcmp(buf, g.logged_reason) != 0)
		{
			std::memcpy(g.logged_reason, buf, sizeof(buf));
			if (buf[0] != 0)
				FORGE_LOG("DLSS NR: %s", buf);
		}
	}

	void fail(const char *fmt, ...)
	{
		char buf[200];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		set_reason("%s", buf);
		AcquireSRWLockExclusive(&g.lock);
		g.st.failed = true;
		g.st.active = false;
		ReleaseSRWLockExclusive(&g.lock);
	}

	// NGX log sink. The snippet names the network it builds for the requested preset.
	void ngx_log(const char *msg, int level, int)
	{
		if (msg == nullptr)
			return;
		const char *built = std::strstr(msg, "Created feature");
		if (built != nullptr)
		{
			const char *arrow = std::strstr(built, "-> ");
			if (arrow != nullptr)
			{
				char name[64] = {};
				size_t n = 0;
				for (const char *c = arrow + 3; *c != 0 && *c != ')' && *c != '\n' && n + 1 < sizeof(name); ++c)
					name[n++] = *c;
				std::memcpy(g_last_network, name, sizeof(name));
			}
		}
		if (built != nullptr || std::strstr(msg, "rror") != nullptr || std::strstr(msg, "ailed") != nullptr || std::strstr(msg, "CG2R") != nullptr)
		{
			size_t len = std::strlen(msg);
			while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\r'))
				--len;
			FORGE_LOG("NGX[%d]: %.*s", level, static_cast<int>(len), msg);
		}
	}

	bool wait_value(uint64_t value, DWORD timeout_ms)
	{
		if (g.fence12->GetCompletedValue() >= value)
			return true;
		g.fence12->SetEventOnCompletion(value, g.event);
		return WaitForSingleObject(g.event, timeout_ms) == WAIT_OBJECT_0;
	}

	void vk_read_timing(unsigned int s)
	{
		if (!g.v.timed[s] || g.v.queries == VK_NULL_HANDLE || g.v.ts_period <= 0.0f)
			return;
		g.v.timed[s] = false;
		uint64_t ts[2] = {};
		if (g.v.query_results(g.v.dev, g.v.queries, 2 * s, 2, sizeof(ts), ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS || ts[1] <= ts[0])
			return;
		const double ms = static_cast<double>(ts[1] - ts[0]) * g.v.ts_period / 1.0e6;
		AcquireSRWLockExclusive(&g.lock);
		g.st.gpu_ms = g.st.gpu_ms < 0.0 ? ms : g.st.gpu_ms * 0.9 + ms * 0.1;
		ReleaseSRWLockExclusive(&g.lock);
	}

	// Waits for the slot's last submission and reads its timestamps
	bool vk_slot_ready(unsigned int s, DWORD timeout_ms)
	{
		if (!g.v.pending[s])
			return true;
		if (g.v.wait(g.v.dev, 1, &g.v.fence[s], VK_TRUE, static_cast<uint64_t>(timeout_ms) * 1000000ull) != VK_SUCCESS)
			return false;
		g.v.pending[s] = false;
		vk_read_timing(s);
		return true;
	}

	// Submits the slot's command buffer on DXVK's queue. DXVK first submits everything it has recorded, so the
	// model runs after the packing and before anything the game records afterwards.
	bool vk_submit(unsigned int s)
	{
		LARGE_INTEGER t0, t1, freq;
		QueryPerformanceCounter(&t0);
		g.v.interop->FlushRenderingCommands();
		QueryPerformanceCounter(&t1);
		QueryPerformanceFrequency(&freq);
		const double ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart);
		g.v.sync_ms = g.v.sync_ms < 0.0 ? ms : g.v.sync_ms * 0.9 + ms * 0.1;
		VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
		si.commandBufferCount = 1;
		si.pCommandBuffers = &g.v.cmd[s];
		g.v.reset_fence(g.v.dev, 1, &g.v.fence[s]);
		g.v.interop->LockSubmissionQueue();
		const VkResult r = g.v.submit(g.v.queue, 1, &si, g.v.fence[s]);
		g.v.interop->ReleaseSubmissionQueue();
		g.v.pending[s] = r == VK_SUCCESS;
		return r == VK_SUCCESS;
	}

	// Starts the slot's command buffer. NGX's CUDA kernels address their image-view handles in the driver-managed
	// descriptor heap, which DXVK's descriptor-heap mode leaves unbound in a new command buffer; an empty legacy
	// bind brings it back, as DXVK does before its own cubin launches (dxvk_context.cpp, launchCuKernelNVX).
	void vk_begin(unsigned int s)
	{
		g.v.reset_cmd(g.v.cmd[s], 0);
		VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		g.v.begin(g.v.cmd[s], &bi);
		if (g.v.bind_sets2 != nullptr)
		{
			VkBindDescriptorSetsInfoKHR bind = { VK_STRUCTURE_TYPE_BIND_DESCRIPTOR_SETS_INFO_KHR };
			bind.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
			g.v.bind_sets2(g.v.cmd[s], &bind);
		}
	}

	const VkImageSubresourceRange vk_color_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

	// Between DXVK's layout and GENERAL, where NGX samples and writes; also orders the access with DXVK's work
	void vk_image_barrier(VkCommandBuffer cmd, const shared_tex &t, bool to_model)
	{
		if (t.image == VK_NULL_HANDLE)
			return;
		VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		b.oldLayout = to_model ? t.layout : VK_IMAGE_LAYOUT_GENERAL;
		b.newLayout = to_model ? VK_IMAGE_LAYOUT_GENERAL : t.layout;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = t.image;
		b.subresourceRange = vk_color_range;
		g.v.barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
	}

	void vk_memory_barrier(VkCommandBuffer cmd)
	{
		VkMemoryBarrier b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		g.v.barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
	}

	// Waits for everything DXVK has recorded and queued; precedes destroying the shared images
	void vk_drain()
	{
		if (g.v.interop == nullptr)
			return;
		g.v.interop->FlushRenderingCommands();
		g.v.interop->LockSubmissionQueue();
		g.v.queue_idle(g.v.queue);
		g.v.interop->ReleaseSubmissionQueue();
		for (unsigned int s = 0; s < ring; ++s)
			vk_slot_ready(s, 0);
	}

	VkFormat vk_format(DXGI_FORMAT f)
	{
		switch (f)
		{
		case DXGI_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
		case DXGI_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
		case DXGI_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
		case DXGI_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
		case DXGI_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
		case DXGI_FORMAT_R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
		case DXGI_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
		case DXGI_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_UNORM;
		case DXGI_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
		default: return VK_FORMAT_UNDEFINED;
		}
	}

	// A VkImage created on DXVK's device and wrapped as a D3D11 texture (CreateTexture2DFromVkImage), with a view
	// and its NGX resource. Owning the image avoids asking a resource for its device, which ReShade answers with
	// its proxy device.
	bool vk_create_tex(shared_tex &t, unsigned int w, unsigned int h, DXGI_FORMAT fmt)
	{
		const VkFormat format = vk_format(fmt);
		if (format == VK_FORMAT_UNDEFINED)
			return false;
		VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
		ci.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; // DXVK may view R32-compatible UAV formats as R32
		ci.imageType = VK_IMAGE_TYPE_2D;
		ci.format = format;
		ci.extent = { w, h, 1 };
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.samples = VK_SAMPLE_COUNT_1_BIT;
		ci.tiling = VK_IMAGE_TILING_OPTIMAL;
		ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
		ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (g.v.create_image(g.v.dev, &ci, nullptr, &t.image) != VK_SUCCESS)
		{
			t.image = VK_NULL_HANDLE;
			return false;
		}
		VkMemoryRequirements req = {};
		g.v.image_reqs(g.v.dev, t.image, &req);
		uint32_t type = ~0u;
		for (uint32_t i = 0; i < g.v.mem_props.memoryTypeCount && type == ~0u; ++i)
			if ((req.memoryTypeBits & (1u << i)) && (g.v.mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
				type = i;
		VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		mai.allocationSize = req.size;
		mai.memoryTypeIndex = type;
		if (type == ~0u || g.v.alloc_mem(g.v.dev, &mai, nullptr, &t.memory) != VK_SUCCESS)
		{
			t.memory = VK_NULL_HANDLE;
			return false;
		}
		if (g.v.bind_image(g.v.dev, t.image, t.memory, 0) != VK_SUCCESS)
			return false;

		// GENERAL is DXVK's layout for these textures; the image enters it before DXVK first uses it
		const unsigned int s = g.slot;
		if (!vk_slot_ready(s, 5000))
			return false;
		vk_begin(s);
		VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
		b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
		b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = t.image;
		b.subresourceRange = vk_color_range;
		g.v.barrier(g.v.cmd[s], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
		g.v.end(g.v.cmd[s]);
		g.slot = (s + 1) % ring;
		if (!vk_submit(s) || !vk_slot_ready(s, 5000))
			return false;
		t.layout = VK_IMAGE_LAYOUT_GENERAL;

		D3D11_TEXTURE2D_DESC1 d = {};
		d.Width = w;
		d.Height = h;
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.Format = fmt;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		d.TextureLayout = D3D11_TEXTURE_LAYOUT_UNDEFINED;
		if (FAILED(g.v.interop->CreateTexture2DFromVkImage(&d, t.image, &t.t11)) || t.t11 == nullptr)
			return false;

		VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		vi.image = t.image;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = format;
		vi.subresourceRange = vk_color_range;
		if (g.v.create_view(g.v.dev, &vi, nullptr, &t.view) != VK_SUCCESS)
		{
			t.view = VK_NULL_HANDLE;
			return false;
		}
		t.res.res.image = { t.view, t.image, vk_color_range, format, w, h };
		t.res.type = 0; // image view
		t.res.read_write = true;
		return true;
	}

	bool wait_idle()
	{
		if (g.v.on)
		{
			bool ok = true;
			for (unsigned int s = 0; s < ring; ++s)
				ok = vk_slot_ready(s, 5000) && ok;
			vk_drain();
			return ok;
		}
		if (g.queue == nullptr || g.fence12 == nullptr)
			return true;
		g.queue->Signal(g.fence12, ++g.fence_value);
		return wait_value(g.fence_value, 5000);
	}

	void publish_pass(unsigned int i)
	{
		AcquireSRWLockExclusive(&g.lock);
		g.st.pass[i].live = g.pass[i].handle != nullptr;
		g.st.pass[i].built = g.pass[i].built;
		std::memcpy(g.st.pass[i].network, g.pass[i].network, sizeof(g.st.pass[i].network));
		ReleaseSRWLockExclusive(&g.lock);
	}

	void release_pass(unsigned int i)
	{
		pass_feature &p = g.pass[i];
		if (p.handle != nullptr && g.release_feature != nullptr)
		{
			wait_idle();
			g.release_feature(p.handle);
		}
		p = pass_feature {};
		publish_pass(i);
	}

	void release_features()
	{
		g.history_valid = false;
		for (unsigned int i = 0; i < max_passes; ++i)
			if (g.pass[i].handle != nullptr)
				release_pass(i);
		g.feature_w = g.feature_h = 0;
	}

	void release_textures()
	{
		wait_idle();
		g.proxy.release();
		g.model[0].release();
		g.model[1].release();
		g.depth.release();
		g.motion.release();
		g.mask.release();
		safe_release(g.groups_srv);
		safe_release(g.groups_uav);
		safe_release(g.groups);
		g.groups_w = g.groups_h = 0;
		g.probe_dirty = !g.probe_mask.empty();
		safe_release(g.keep_srv);
		safe_release(g.keep);
		safe_release(g.output_rtv);
		safe_release(g.output_srv);
		safe_release(g.output);
		g.keep_w = g.keep_h = 0;
		g.keep_format = DXGI_FORMAT_UNKNOWN;
		g.depth_in.release();
		g.motion_in.release();
	}

	// Everything tied to the game's D3D11 device. The D3D12 device and NGX stay.
	void release_device_objects()
	{
		release_features();
		release_textures();
		safe_release(g.pack_color_cs);
		safe_release(g.pack_guides_cs);
		safe_release(g.pack_work_cs);
		safe_release(g.pack_mask_cs);
		safe_release(g.resolve_ps);
		safe_release(g.output_vs);
		safe_release(g.rasterizer);
		safe_release(g.context_state);
		safe_release(g.stats_cs);
		safe_release(g.stats_uav);
		safe_release(g.stats);
		safe_release(g.stats_cb);
		for (unsigned int k = 0; k < stats_ring; ++k)
		{
			safe_release(g.stats_read[k]);
			g.records[k].pending = false;
		}
		g.stats_done = g.stats_next;
		safe_release(g.cb);
		g.shader_failed = false;
	}

	// The Vulkan backend on DXVK's device. Without DXVK's interop (native D3D11 on Windows) or a loaded Vulkan
	// loader, the D3D12 bridge runs the model instead.
	bool create_vk(ID3D11Device *dev11)
	{
		dxvk_interop::IDXGIVkInteropDevice1 *interop = nullptr;
		if (FAILED(dev11->QueryInterface(dxvk_interop::iid_device1, reinterpret_cast<void **>(&interop))) || interop == nullptr)
			return false;
		HMODULE lib = GetModuleHandleW(L"winevulkan.dll");
		if (lib == nullptr)
			lib = GetModuleHandleW(L"vulkan-1.dll");
		vk_backend &v = g.v;
		v.gipa = lib != nullptr ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib, "vkGetInstanceProcAddr")) : nullptr;
		if (v.gipa == nullptr)
		{
			interop->Release();
			v = vk_backend {};
			FORGE_LOG("DLSS NR: DXVK interop without a loaded Vulkan loader; the D3D12 bridge runs the model");
			return false;
		}
		interop->GetVulkanHandles(&v.instance, &v.pd, &v.dev);
		interop->GetSubmissionQueue(&v.queue, &v.family);
		v.gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(v.gipa(v.instance, "vkGetDeviceProcAddr"));
		const auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(v.gipa(v.instance, "vkGetPhysicalDeviceProperties"));
		bool ok = v.dev != VK_NULL_HANDLE && v.queue != VK_NULL_HANDLE && v.gdpa != nullptr && props != nullptr;
#define VKFN(field, name) if (ok) { v.field = reinterpret_cast<PFN_##name>(v.gdpa(v.dev, #name)); ok = v.field != nullptr; }
		VKFN(create_pool, vkCreateCommandPool)
		VKFN(alloc_cmd, vkAllocateCommandBuffers)
		VKFN(begin, vkBeginCommandBuffer)
		VKFN(end, vkEndCommandBuffer)
		VKFN(reset_cmd, vkResetCommandBuffer)
		VKFN(barrier, vkCmdPipelineBarrier)
		VKFN(submit, vkQueueSubmit)
		VKFN(create_fence, vkCreateFence)
		VKFN(wait, vkWaitForFences)
		VKFN(reset_fence, vkResetFences)
		VKFN(create_view, vkCreateImageView)
		VKFN(destroy_view, vkDestroyImageView)
		VKFN(create_queries, vkCreateQueryPool)
		VKFN(reset_queries, vkCmdResetQueryPool)
		VKFN(timestamp, vkCmdWriteTimestamp)
		VKFN(query_results, vkGetQueryPoolResults)
		VKFN(create_image, vkCreateImage)
		VKFN(destroy_image, vkDestroyImage)
		VKFN(image_reqs, vkGetImageMemoryRequirements)
		VKFN(alloc_mem, vkAllocateMemory)
		VKFN(free_mem, vkFreeMemory)
		VKFN(bind_image, vkBindImageMemory)
		VKFN(queue_idle, vkQueueWaitIdle)
#undef VKFN
		const auto mem_props = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(v.gipa(v.instance, "vkGetPhysicalDeviceMemoryProperties"));
		ok = ok && mem_props != nullptr;
		if (ok)
			mem_props(v.pd, &v.mem_props);
		if (ok)
		{
			v.bind_sets2 = reinterpret_cast<PFN_vkCmdBindDescriptorSets2KHR>(v.gdpa(v.dev, "vkCmdBindDescriptorSets2KHR"));
			if (v.bind_sets2 == nullptr)
				v.bind_sets2 = reinterpret_cast<PFN_vkCmdBindDescriptorSets2KHR>(v.gdpa(v.dev, "vkCmdBindDescriptorSets2"));
			VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
			pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
			pci.queueFamilyIndex = v.family;
			ok = v.create_pool(v.dev, &pci, nullptr, &v.pool) == VK_SUCCESS;
			VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
			cai.commandPool = v.pool;
			cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			cai.commandBufferCount = ring;
			ok = ok && v.alloc_cmd(v.dev, &cai, v.cmd) == VK_SUCCESS;
			VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
			for (VkFence &fence : v.fence)
				ok = ok && v.create_fence(v.dev, &fci, nullptr, &fence) == VK_SUCCESS;
			VkQueryPoolCreateInfo qci = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
			qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
			qci.queryCount = 2 * ring;
			if (ok && v.create_queries(v.dev, &qci, nullptr, &v.queries) != VK_SUCCESS)
				v.queries = VK_NULL_HANDLE;
		}
		if (!ok)
		{
			interop->Release();
			v = vk_backend {};
			FORGE_LOG("DLSS NR: DXVK's Vulkan device lacks an entry point or object the model needs; the D3D12 bridge runs the model");
			return false;
		}
		VkPhysicalDeviceProperties pp = {};
		props(v.pd, &pp);
		v.ts_period = pp.limits.timestampPeriod;
		s_destroy_view = v.destroy_view;
		s_destroy_image = v.destroy_image;
		s_free_memory = v.free_mem;
		s_view_device = v.dev;
		v.interop = interop;
		v.on = true;
		std::strcpy(g.st.backend, "Vulkan (DXVK's device)");
		FORGE_LOG("DLSS NR: Vulkan on DXVK's device (%s, queue family %u, driver heap rebind %s)", pp.deviceName, v.family,
			v.bind_sets2 != nullptr ? "on" : "unavailable");
		return true;
	}

	bool create_bridge(ID3D11Device *dev11, int device)
	{
		g.st.device = device;
		if (device == 0 && create_vk(dev11))
			return true;
		HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
		if (d3d12 == nullptr)
		{
			fail("d3d12.dll could not be loaded");
			return false;
		}
		// Wine stamps its own PE modules with this signature in the DOS stub; vkd3d-proton's d3d12.dll carries none.
		// A D3D12 choice without vkd3d-proton runs the model on DXVK's device, so the game keeps its picture.
		if (std::memcmp(reinterpret_cast<const char *>(d3d12) + 0x40, "Wine builtin DLL", 16) == 0)
		{
			FreeLibrary(d3d12);
			if (device == 1 && create_vk(dev11))
			{
				g.st.device = 0;
				g.st.d3d12_builtin = true;
				FORGE_LOG("DLSS NR: D3D12 needs vkd3d-proton (DLL overrides d3d12,d3d12core=n); the model runs on Vulkan");
				return true;
			}
			fail("D3D12 under Wine needs vkd3d-proton. Set the DLL overrides d3d12,d3d12core=n.");
			return false;
		}
		const auto create_device = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice"));
		const auto get_interface = reinterpret_cast<PFN_D3D12GetInterface>(GetProcAddress(d3d12, "D3D12GetInterface"));

		IDXGIDevice *dxgi_dev = nullptr;
		IDXGIAdapter *adapter = nullptr;
		if (FAILED(dev11->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dxgi_dev))) || FAILED(dxgi_dev->GetAdapter(&adapter)))
		{
			safe_release(dxgi_dev);
			fail("the game's D3D11 device has no DXGI adapter");
			return false;
		}
		safe_release(dxgi_dev);

		// A device of its own from the factory; it never becomes the adapter's shared D3D12 device. Without the
		// factory, D3D12CreateDevice: ReShade then wraps the device, as it does for every D3D12 game.
		HRESULT hr = E_NOINTERFACE;
		ID3D12DeviceFactory *factory = nullptr;
		if (!g_skip_device_factory && get_interface != nullptr &&
			SUCCEEDED(get_interface(clsid_d3d12_device_factory, __uuidof(ID3D12DeviceFactory), reinterpret_cast<void **>(&factory))) && factory != nullptr)
		{
			factory->SetFlags(D3D12_DEVICE_FACTORY_FLAG_DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON);
			hr = factory->CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), reinterpret_cast<void **>(&g.dev12));
			safe_release(factory);
			std::strcpy(g.st.backend, "D3D12 (own device)");
		}
		if ((FAILED(hr) || g.dev12 == nullptr) && create_device != nullptr)
		{
			safe_release(g.dev12);
			hr = create_device(adapter, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), reinterpret_cast<void **>(&g.dev12));
			std::strcpy(g.st.backend, "D3D12");
		}
		if (FAILED(hr) || g.dev12 == nullptr)
		{
			char vram[96] = "";
			IDXGIAdapter3 *a3 = nullptr;
			DXGI_QUERY_VIDEO_MEMORY_INFO mi = {};
			if (SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void **>(&a3))) && SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &mi)))
				snprintf(vram, sizeof(vram), "; VRAM budget %llu MiB, %llu MiB in use by the game",
					static_cast<unsigned long long>(mi.Budget >> 20), static_cast<unsigned long long>(mi.CurrentUsage >> 20));
			safe_release(a3);
			safe_release(adapter);
			fail("D3D12 device creation failed (0x%08lX)%s", static_cast<unsigned long>(hr), vram);
			return false;
		}
		safe_release(adapter);

		D3D12_COMMAND_QUEUE_DESC qd = {};
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(g.dev12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&g.queue))))
		{
			fail("D3D12 command queue creation failed");
			return false;
		}
		for (auto *&a : g.alloc)
		{
			if (FAILED(g.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void **>(&a))))
			{
				fail("D3D12 command allocator creation failed");
				return false;
			}
		}
		if (FAILED(g.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0], nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&g.list))))
		{
			fail("D3D12 command list creation failed");
			return false;
		}
		g.list->Close();
		g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

		D3D12_QUERY_HEAP_DESC qh = {};
		qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
		qh.Count = 2 * ring;
		g.dev12->CreateQueryHeap(&qh, __uuidof(ID3D12QueryHeap), reinterpret_cast<void **>(&g.queries));
		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC rd = {};
		rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		rd.Width = sizeof(uint64_t) * 2 * ring;
		rd.Height = 1;
		rd.DepthOrArraySize = 1;
		rd.MipLevels = 1;
		rd.SampleDesc.Count = 1;
		rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		g.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&g.query_readback));
		g.queue->GetTimestampFrequency(&g.ts_freq);
		FORGE_LOG("DLSS NR: bridge device ready (%s)", g.st.backend);
		return true;
	}

	// The shared fence between the game's D3D11 device and the bridge
	bool attach_d3d11(ID3D11Device *dev11)
	{
		if (g.v.on)
		{
			if (g.v.interop == nullptr)
			{
				dxvk_interop::IDXGIVkInteropDevice1 *interop = nullptr;
				VkDevice dev = VK_NULL_HANDLE;
				if (SUCCEEDED(dev11->QueryInterface(dxvk_interop::iid_device1, reinterpret_cast<void **>(&interop))) && interop != nullptr)
					interop->GetVulkanHandles(nullptr, nullptr, &dev);
				if (dev == VK_NULL_HANDLE || dev != g.v.dev)
				{
					safe_release(interop);
					fail("The game made a new D3D11 device. Restart the game for DLSS 5.");
					return false;
				}
				g.v.interop = interop;
			}
			g.dev11 = dev11;
			g.dev11->AddRef();
			return true;
		}
		ID3D11Device5 *dev11_5 = nullptr;
		if (FAILED(dev11->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&dev11_5))) ||
			FAILED(dev11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), reinterpret_cast<void **>(&g.fence11))))
		{
			safe_release(dev11_5);
			fail("shared D3D11 fence unavailable");
			return false;
		}
		safe_release(dev11_5);
		HANDLE fh = nullptr;
		ID3D12Fence *f12 = nullptr;
		if (FAILED(g.fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fh)) ||
			FAILED(g.dev12->OpenSharedHandle(fh, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&f12))))
		{
			if (fh != nullptr)
				CloseHandle(fh);
			safe_release(g.fence11);
			fail("the D3D12 device could not open the shared fence");
			return false;
		}
		CloseHandle(fh);
		// A fresh fence starts at 0; every wait value from here on counts up from it
		if (g.fence12 != nullptr)
			wait_value(g.fence_value, 5000);
		safe_release(g.fence12);
		g.fence12 = f12;
		g.fence_value = 0;
		for (unsigned int i = 0; i < ring; ++i)
			g.alloc_done[i] = g.query_done[i] = 0;
		g.dev11 = dev11;
		g.dev11->AddRef();
		return true;
	}

	void detach_d3d11()
	{
		release_device_objects();
		safe_release(g.fence11);
		safe_release(g.v.interop);
		safe_release(g.dev11);
	}

	const feature_common_info *ngx_info()
	{
		GetModuleFileNameW(nullptr, g_dir, MAX_PATH);
		if (wchar_t *slash = wcsrchr(g_dir, L'\\'))
			*slash = 0;
		static const wchar_t *paths[1];
		paths[0] = g_dir;
		static feature_common_info info = {};
		info.paths = { paths, 1 };
		const char *verbose = getenv("FORGE_NGX_VERBOSE");
		info.logging = { &ngx_log, verbose != nullptr && verbose[0] == '2' ? 2 : 1, false };
		return &info;
	}

	// The caps check and parameter block both APIs share once NGX is up
	bool finish_ngx_init(const char *api)
	{
		g.ngx_ready = true;
		if (!succeeded(g.get_caps(&g.caps)) || g.caps == nullptr)
		{
			fail("NGX %s capability parameters unavailable", api);
			return false;
		}
		int available = 0, init_result = 0;
		get_i(g.caps, "DLSSNR.Available", &available);
		get_i(g.caps, "DLSSNR.FeatureInitResult", &init_result);
		if (available != 1)
		{
			fail("DLSS NR unavailable on %s (FeatureInitResult 0x%08X). Put nvngx_dlssnr.dll next to ffxiv_dx11.exe.", api, static_cast<unsigned int>(init_result));
			return false;
		}
		if (!succeeded(g.alloc_params(&g.params)) || g.params == nullptr)
		{
			fail("NGX %s parameter allocation failed", api);
			return false;
		}
		FORGE_LOG("DLSS NR: NGX %s ready, DLSSNR.Available=1", api);
		return true;
	}

	bool init_ngx_vk()
	{
		g.v.init = reinterpret_cast<ngxvk::PFN_Init_ProjectID_Ext>(forge::ngx_hook::core_proc("NVSDK_NGX_VULKAN_Init_ProjectID_Ext"));
		g.get_caps = reinterpret_cast<PFN_D3D12_GetParams>(forge::ngx_hook::core_proc("NVSDK_NGX_VULKAN_GetCapabilityParameters"));
		g.alloc_params = reinterpret_cast<PFN_D3D12_GetParams>(forge::ngx_hook::core_proc("NVSDK_NGX_VULKAN_AllocateParameters"));
		g.v.create = reinterpret_cast<ngxvk::PFN_CreateFeature1>(forge::ngx_hook::core_proc("NVSDK_NGX_VULKAN_CreateFeature1"));
		g.v.evaluate = reinterpret_cast<ngxvk::PFN_EvaluateFeature>(forge::ngx_hook::core_proc("NVSDK_NGX_VULKAN_EvaluateFeature"));
		g.release_feature = reinterpret_cast<PFN_D3D12_ReleaseFeature>(forge::ngx_hook::core_proc("NVSDK_NGX_VULKAN_ReleaseFeature"));
		if (g.v.init == nullptr || g.get_caps == nullptr || g.alloc_params == nullptr || g.v.create == nullptr || g.v.evaluate == nullptr || g.release_feature == nullptr)
		{
			fail("the NGX core exports no Vulkan entry points");
			return false;
		}
		const Result r = g.v.init("a0f57b54-1daf-4934-90ae-c4035c19df04", 0, "1.0", g_dir, g.v.instance, g.v.pd, g.v.dev, g.v.gipa, g.v.gdpa,
			sdk_version_api, ngx_info());
		if (!succeeded(r))
		{
			fail("NGX Vulkan init failed (0x%08X). Set Model device to D3D12.", r);
			return false;
		}
		return finish_ngx_init("Vulkan");
	}

	bool init_ngx()
	{
		if (g.v.on)
			return init_ngx_vk();
		g.init = reinterpret_cast<PFN_D3D12_Init_ProjectID>(forge::ngx_hook::core_proc("NVSDK_NGX_D3D12_Init_ProjectID"));
		g.get_caps = reinterpret_cast<PFN_D3D12_GetParams>(forge::ngx_hook::core_proc("NVSDK_NGX_D3D12_GetCapabilityParameters"));
		g.alloc_params = reinterpret_cast<PFN_D3D12_GetParams>(forge::ngx_hook::core_proc("NVSDK_NGX_D3D12_AllocateParameters"));
		g.create = reinterpret_cast<PFN_D3D12_CreateFeature>(forge::ngx_hook::core_proc("NVSDK_NGX_D3D12_CreateFeature"));
		g.evaluate = reinterpret_cast<PFN_D3D12_EvaluateFeature>(forge::ngx_hook::core_proc("NVSDK_NGX_D3D12_EvaluateFeature"));
		g.release_feature = reinterpret_cast<PFN_D3D12_ReleaseFeature>(forge::ngx_hook::core_proc("NVSDK_NGX_D3D12_ReleaseFeature"));
		if (g.init == nullptr || g.get_caps == nullptr || g.alloc_params == nullptr || g.create == nullptr || g.evaluate == nullptr || g.release_feature == nullptr)
		{
			fail("the NGX core exports no D3D12 entry points");
			return false;
		}

		const feature_common_info *info = ngx_info();
		const Result r = g.init("a0f57b54-1daf-4934-90ae-c4035c19df04", 0, "1.0", g_dir, g.dev12, sdk_version_api, info);
		if (!succeeded(r))
		{
			fail("NGX D3D12 init on the bridge device failed (0x%08X)", r);
			return false;
		}
		return finish_ngx_init("D3D12");
	}

	bool ensure_shaders()
	{
		if (g.shader_failed)
			return false;
		if (g.context_state != nullptr)
			return true;
		struct kernel { const unsigned char *code; size_t size; ID3D11ComputeShader **out; };
		for (const kernel &k : { kernel { nr_pack_color, sizeof(nr_pack_color), &g.pack_color_cs }, kernel { nr_pack_guides, sizeof(nr_pack_guides), &g.pack_guides_cs },
				 kernel { nr_pack_work, sizeof(nr_pack_work), &g.pack_work_cs }, kernel { nr_pack_mask, sizeof(nr_pack_mask), &g.pack_mask_cs },
				 kernel { nr_stats, sizeof(nr_stats), &g.stats_cs } })
		{
			if (FAILED(g.dev11->CreateComputeShader(k.code, k.size, nullptr, k.out)))
			{
				g.shader_failed = true;
				fail("input shader creation failed");
				return false;
			}
		}
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = sizeof(cb_data);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		D3D11_RASTERIZER_DESC rd = {};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;
		rd.DepthClipEnable = TRUE;
		ID3D11Device1 *device = nullptr;
		const D3D_FEATURE_LEVEL level = g.dev11->GetFeatureLevel();
		bool ok = SUCCEEDED(g.dev11->CreateBuffer(&bd, nullptr, &g.cb)) &&
			SUCCEEDED(g.dev11->CreatePixelShader(nr_resolve, sizeof(nr_resolve), nullptr, &g.resolve_ps)) &&
			SUCCEEDED(g.dev11->CreateVertexShader(nr_output_vs, sizeof(nr_output_vs), nullptr, &g.output_vs)) &&
			SUCCEEDED(g.dev11->CreateRasterizerState(&rd, &g.rasterizer)) &&
			SUCCEEDED(g.dev11->QueryInterface(__uuidof(ID3D11Device1), reinterpret_cast<void **>(&device)));
		ok = ok && SUCCEEDED(device->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &g.context_state));
		safe_release(device);
		if (!ok)
		{
			g.shader_failed = true;
			fail("output shaders or D3D11.1 context state could not be created");
		}
		return ok;
	}

	bool ensure_shared(shared_tex &t, unsigned int w, unsigned int h, DXGI_FORMAT fmt, const char *what)
	{
		if (t.t11 != nullptr && t.w == w && t.h == h && t.fmt == fmt)
			return true;
		wait_idle();
		t.release();
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = fmt;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		// The log names the first step that fails and gives its HRESULT
		const char *step = g.v.on ? "Vulkan image" : "CreateTexture2D";
		HRESULT hr = g.v.on ? (vk_create_tex(t, w, h, fmt) ? S_OK : E_FAIL) : g.dev11->CreateTexture2D(&td, nullptr, &t.t11);
		const auto next = [&](const char *name) { step = SUCCEEDED(hr) ? name : step; return SUCCEEDED(hr); };
		if (next("CreateShaderResourceView"))
			hr = g.dev11->CreateShaderResourceView(t.t11, nullptr, &t.srv);
		if (next("CreateUnorderedAccessView"))
			hr = g.dev11->CreateUnorderedAccessView(t.t11, nullptr, &t.uav);
		if (!g.v.on)
		{
			HANDLE handle = nullptr;
			IDXGIResource1 *res = nullptr;
			if (next("IDXGIResource1"))
				hr = t.t11->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void **>(&res));
			if (next("CreateSharedHandle"))
				hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle);
			if (next("OpenSharedHandle"))
				hr = g.dev12->OpenSharedHandle(handle, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&t.t12));
			safe_release(res);
			if (handle != nullptr)
				CloseHandle(handle);
		}
		if (FAILED(hr))
		{
			t.release();
			fail("shared %s texture %ux%u format %d: %s failed (0x%08lX)", what, w, h, static_cast<int>(fmt), step, static_cast<unsigned long>(hr));
			return false;
		}
		t.w = w;
		t.h = h;
		t.fmt = fmt;
		return true;
	}

	// Same-format copies support SR inputs and swapchain buffers with any bind flags.
	bool ensure_keep(unsigned int w, unsigned int h, DXGI_FORMAT format)
	{
		format = typed_uav_format(format);
		if (g.keep != nullptr && g.output_rtv != nullptr && g.keep_w == w && g.keep_h == h && g.keep_format == format)
			return true;
		safe_release(g.keep_srv);
		safe_release(g.keep);
		safe_release(g.output_rtv);
		safe_release(g.output_srv);
		safe_release(g.output);
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = format;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bool ok = SUCCEEDED(g.dev11->CreateTexture2D(&td, nullptr, &g.keep)) &&
			SUCCEEDED(g.dev11->CreateShaderResourceView(g.keep, nullptr, &g.keep_srv));
		td.BindFlags |= D3D11_BIND_RENDER_TARGET;
		ok = ok && SUCCEEDED(g.dev11->CreateTexture2D(&td, nullptr, &g.output)) &&
			SUCCEEDED(g.dev11->CreateRenderTargetView(g.output, nullptr, &g.output_rtv)) &&
			SUCCEEDED(g.dev11->CreateShaderResourceView(g.output, nullptr, &g.output_srv));
		if (!ok)
		{
			fail("frame copies %ux%u format %d could not be created", w, h, static_cast<int>(format));
			return false;
		}
		g.keep_w = w;
		g.keep_h = h;
		g.keep_format = format;
		return true;
	}

	// Frame log: the probe texture, the probe's constant buffer and the read-back ring
	bool ensure_stats()
	{
		if (g.stats != nullptr)
			return true;
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = stats_cols;
		td.Height = stats_rows;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R32_FLOAT;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		const float zero[stats_rows * stats_cols] = {};
		const D3D11_SUBRESOURCE_DATA init = { zero, stats_cols * sizeof(float), 0 };
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = 64;
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		bool ok = SUCCEEDED(g.dev11->CreateTexture2D(&td, &init, &g.stats)) &&
			SUCCEEDED(g.dev11->CreateUnorderedAccessView(g.stats, nullptr, &g.stats_uav)) &&
			SUCCEEDED(g.dev11->CreateBuffer(&bd, nullptr, &g.stats_cb));
		td.Usage = D3D11_USAGE_STAGING;
		td.BindFlags = 0;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (unsigned int k = 0; ok && k < stats_ring; ++k)
			ok = SUCCEEDED(g.dev11->CreateTexture2D(&td, nullptr, &g.stats_read[k]));
		if (!ok)
		{
			safe_release(g.stats_uav);
			safe_release(g.stats);
			safe_release(g.stats_cb);
			for (ID3D11Texture2D *&t : g.stats_read)
				safe_release(t);
			FORGE_LOG("frame log: probe textures could not be created");
			return false;
		}
		return true;
	}

	// Starts or stops the per-frame lines in the add-on's log
	void frame_log_switch(bool on)
	{
		if (on == g.frame_log)
			return;
		for (frame_record &r : g.records)
			r.pending = false;
		g.stats_done = g.stats_next;
		g.frame_log = on;
		QueryPerformanceCounter(&g.log_start);
		FORGE_LOG("frame log %s", on ? "on: frame ms reset mode passes | motion px max | game out stops abs | chain stops abs" : "off");
	}

	// Writes the records that the GPU completed, oldest first, and returns at the first record in progress
	void harvest(ID3D11DeviceContext *ctx)
	{
		const auto stops = [](float a, float b) { return a > 0.0f && b > 0.0f ? std::log2(b / a) : 0.0f; };
		while (g.stats_done < g.stats_next)
		{
			const unsigned int slot = static_cast<unsigned int>(g.stats_done % stats_ring);
			frame_record &r = g.records[slot];
			if (r.pending)
			{
				D3D11_MAPPED_SUBRESOURCE m = {};
				const HRESULT hr = ctx->Map(g.stats_read[slot], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
				if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
					return;
				if (SUCCEEDED(hr))
				{
					float v[stats_rows][stats_cols];
					for (unsigned int y = 0; y < stats_rows; ++y)
						std::memcpy(v[y], static_cast<const uint8_t *>(m.pData) + static_cast<size_t>(y) * m.RowPitch, sizeof(v[y]));
					ctx->Unmap(g.stats_read[slot], 0);
					if (g.frame_log)
					{
						const float *ch = v[row_chain], *fr = v[row_frame], *mo = v[row_motion];
						FORGE_LOG("F %llu %.1f %u %u %u | %.1f %.1f | %.4f %.4f %+.3f %.3f | %+.3f %.3f",
							static_cast<unsigned long long>(r.frame), r.time_ms, r.reset_bits, r.mode, r.passes,
							mo[0], mo[4], fr[0], fr[1], stops(fr[0], fr[1]), fr[3], stops(ch[0], ch[1]), ch[3]);
					}
				}
				r.pending = false;
			}
			g.stats_done++;
		}
	}

	// Which developer-mask group each pixel of the mask belongs to (R8, 0 / 0.25 / 0.5 / 0.75), for the overlay
	bool ensure_groups(unsigned int w, unsigned int h)
	{
		if (g.groups != nullptr && g.groups_w == w && g.groups_h == h)
			return true;
		safe_release(g.groups_srv);
		safe_release(g.groups_uav);
		safe_release(g.groups);
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		if (FAILED(g.dev11->CreateTexture2D(&td, nullptr, &g.groups)) ||
			FAILED(g.dev11->CreateShaderResourceView(g.groups, nullptr, &g.groups_srv)) ||
			FAILED(g.dev11->CreateUnorderedAccessView(g.groups, nullptr, &g.groups_uav)))
		{
			safe_release(g.groups_srv);
			safe_release(g.groups_uav);
			safe_release(g.groups);
			fail("mask group texture %ux%u could not be created", w, h);
			return false;
		}
		g.groups_w = w;
		g.groups_h = h;
		return true;
	}

	// The probe picture (four floats per pixel) in the mask texture's format
	unsigned int mask_pixel_bytes(DXGI_FORMAT f)
	{
		switch (f)
		{
		case DXGI_FORMAT_R8_UNORM: return 1;
		case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R16_FLOAT: return 2;
		case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R8G8B8A8_UNORM: return 4;
		case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R32G32_FLOAT: return 8;
		default: return 0;
		}
	}

	void upload_probe_mask(ID3D11DeviceContext *ctx)
	{
		const unsigned int bpp = mask_pixel_bytes(g.probe_format);
		if (bpp == 0 || g.mask.t11 == nullptr)
			return;
		std::vector<uint8_t> bytes(static_cast<size_t>(g.probe_w) * g.probe_h * bpp);
		const auto unorm = [](float v) { v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); return static_cast<uint8_t>(v * 255.0f + 0.5f); };
		for (size_t i = 0; i < static_cast<size_t>(g.probe_w) * g.probe_h; ++i)
		{
			const float *px = &g.probe_mask[i * 4];
			uint8_t *out = &bytes[i * bpp];
			switch (g.probe_format)
			{
			case DXGI_FORMAT_R8_UNORM: out[0] = unorm(px[0]); break;
			case DXGI_FORMAT_R8G8_UNORM: out[0] = unorm(px[0]); out[1] = unorm(px[1]); break;
			case DXGI_FORMAT_R8G8B8A8_UNORM: for (int k = 0; k < 4; ++k) out[k] = unorm(px[k]); break;
			case DXGI_FORMAT_R16_FLOAT: { const uint16_t h = to_half(px[0]); std::memcpy(out, &h, 2); break; }
			case DXGI_FORMAT_R16G16_FLOAT: { const uint16_t h[2] = { to_half(px[0]), to_half(px[1]) }; std::memcpy(out, h, 4); break; }
			case DXGI_FORMAT_R16G16B16A16_FLOAT: { const uint16_t h[4] = { to_half(px[0]), to_half(px[1]), to_half(px[2]), to_half(px[3]) }; std::memcpy(out, h, 8); break; }
			case DXGI_FORMAT_R32_FLOAT: std::memcpy(out, px, 4); break;
			case DXGI_FORMAT_R32G32_FLOAT: std::memcpy(out, px, 8); break;
			default: break;
			}
		}
		ctx->UpdateSubresource(g.mask.t11, 0, nullptr, bytes.data(), g.probe_w * bpp, 0);
		g.probe_dirty = false;
	}

	bool ensure_view(view_set &set, ID3D11Resource *res, bool uav, const char *what)
	{
		++set.tick;
		view_cache *slot = &set.entry[0];
		for (view_cache &e : set.entry)
		{
			if (e.res == res && e.srv != nullptr && (!uav || e.uav != nullptr))
			{
				e.used = set.tick;
				set.current = &e;
				return true;
			}
			if (e.used < slot->used)
				slot = &e;
		}
		view_cache &v = *slot;
		v.release();
		set.current = nullptr;
		ID3D11Texture2D *tex = nullptr;
		if (res == nullptr || FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))))
		{
			set_reason("the game's %s is not a 2D texture", what);
			return false;
		}
		D3D11_TEXTURE2D_DESC d;
		tex->GetDesc(&d);
		bool seen = false;
		for (const void *r : set.seen)
			seen |= r == res;
		if (!seen)
		{
			set.seen[set.seen_next++ % 8] = res;
			FORGE_LOG("DLSS NR input: %s %ux%u format %d bind 0x%X samples %u", what, d.Width, d.Height, static_cast<int>(d.Format), d.BindFlags, d.SampleDesc.Count);
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
		sd.Format = readable_format(d.Format);
		sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		sd.Texture2D.MipLevels = 1;
		bool ok = SUCCEEDED(g.dev11->CreateShaderResourceView(tex, &sd, &v.srv));
		if (ok && uav)
		{
			D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
			ud.Format = typed_uav_format(d.Format);
			ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			ok = (d.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0 && SUCCEEDED(g.dev11->CreateUnorderedAccessView(tex, &ud, &v.uav));
		}
		tex->Release();
		if (!ok)
		{
			v.release();
			set_reason("views on the game's %s (format %d) could not be created", what, static_cast<int>(d.Format));
			return false;
		}
		v.res = res;
		v.res->AddRef();
		v.w = d.Width;
		v.h = d.Height;
		v.used = set.tick;
		set.current = &v;
		return true;
	}

	struct cs_state
	{
		ID3D11DeviceContext1 *context = nullptr;
		ID3DDeviceContextState *previous = nullptr;

		bool save(ID3D11DeviceContext *ctx)
		{
			if (FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void **>(&context))))
				return false;
			context->SwapDeviceContextState(g.context_state, &previous);
			context->ClearState();
			return true;
		}
		void restore(ID3D11DeviceContext *)
		{
			if (context == nullptr) return;
			context->ClearState();
			context->SwapDeviceContextState(previous, nullptr);
			safe_release(previous);
			safe_release(context);
		}
		~cs_state() { restore(nullptr); }
	};

	void dispatch(ID3D11DeviceContext *ctx, ID3D11ComputeShader *cs, std::initializer_list<ID3D11ShaderResourceView *> srvs, std::initializer_list<std::pair<UINT, ID3D11UnorderedAccessView *>> uavs, unsigned int w, unsigned int h)
	{
		ID3D11UnorderedAccessView *u[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
		for (const auto &p : uavs)
			u[p.first] = p.second;
		ctx->CSSetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, u, nullptr);
		ID3D11ShaderResourceView *s[6] = {};
		UINT i = 0;
		for (ID3D11ShaderResourceView *v : srvs)
			s[i++] = v;
		ctx->CSSetShaderResources(0, 6, s);
		ctx->CSSetShader(cs, nullptr, 0);
		ctx->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
		ID3D11UnorderedAccessView *nu[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
		ctx->CSSetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, nu, nullptr);
		ID3D11ShaderResourceView *ns[6] = {};
		ctx->CSSetShaderResources(0, 6, ns);
	}

	void write_tuning(Parameter *p, const forge::nr::tuning &t)
	{
		set_f(p, "DLSSNR.Intensity", t.intensity);
		set_ui(p, "DLSSNR.Style", t.style);
		set_f(p, "DLSSNR.LocalStructureStrength", t.structure);
		set_f(p, "DLSSNR.LocalToneStrength", t.tone);
		set_f(p, "DLSSNR.SkinStructureStrength", t.skin);
		set_ui(p, "DLSSNR.UseAutoMask", t.auto_mask ? 1u : 0u);
	}

	// The control mask (an ID3D12Resource or NGX Vulkan resource, nullptr for none) and how it meets the
	// automask. Both are read at every evaluate; the snippet resets its history when the blend mode changes
	// and switches the automask off while a mask comes with blend mode 0.
	void write_mask_params(Parameter *p, void *mask, unsigned int mw, unsigned int mh, unsigned int blend)
	{
		set_ptr(p, "DLSSNR.ControlMask", mask);
		set_ui(p, "DLSSNR.ControlMaskSubrectBaseX", 0);
		set_ui(p, "DLSSNR.ControlMaskSubrectBaseY", 0);
		set_ui(p, "DLSSNR.ControlMaskSubrectWidth", mask != nullptr ? mw : 0u);
		set_ui(p, "DLSSNR.ControlMaskSubrectHeight", mask != nullptr ? mh : 0u);
		set_ui(p, "DLSSNR.MaskBlendMode", blend > 3 ? 0u : blend);
	}

	// What the evaluate writes: live parameters from the current settings, latched ones as the feature was built
	forge::nr::tuning evaluate_tuning(const forge::nr::tuning &built, const forge::nr::tuning &now)
	{
		forge::nr::tuning t = built;
		if (live("DLSSNR.Style")) t.style = now.style;
		if (live("DLSSNR.Intensity")) t.intensity = now.intensity;
		if (live("DLSSNR.LocalStructureStrength")) t.structure = now.structure;
		if (live("DLSSNR.LocalToneStrength")) t.tone = now.tone;
		if (live("DLSSNR.SkinStructureStrength")) t.skin = now.skin;
		if (live("DLSSNR.UseAutoMask")) t.auto_mask = now.auto_mask;
		if (live("DLSSNR.Hint.Render.Preset")) t.preset = now.preset;
		return t;
	}

	// True when building with `want` gives a different feature than `built`
	bool needs_rebuild(const forge::nr::tuning &built, const forge::nr::tuning &want)
	{
		return evaluate_tuning(built, want) != want;
	}

	// The part of a pass that is fixed when its feature is built
	forge::nr::tuning latched(const forge::nr::tuning &t)
	{
		return evaluate_tuning(t, forge::nr::tuning {});
	}

	// Submits `g.list` (recorded and closed) after the D3D11 work up to `after`, returns the value it signals
	uint64_t submit(uint64_t after)
	{
		g.queue->Wait(g.fence12, after);
		ID3D12CommandList *lists[] = { g.list };
		g.queue->ExecuteCommandLists(1, lists);
		g.queue->Signal(g.fence12, ++g.fence_value);
		return g.fence_value;
	}

	// Builds the feature for one pass on its own submission and waits for it, so the first evaluate never
	// shares a command list with the creation
	bool create_pass(unsigned int i, unsigned int w, unsigned int h, const forge::nr::tuning &t)
	{
		release_pass(i);
		set_ui(g.params, "DLSSNR.Enabled", 1);
		set_ui(g.params, "DLSSNR.Width", w);
		set_ui(g.params, "DLSSNR.Height", h);
		set_ui(g.params, name::creation_node_mask, 1);
		set_ui(g.params, name::visibility_node_mask, 1);
		set_ui(g.params, "DLSSNR.Hint.Render.Preset", t.preset);
		set_ui(g.params, "DLSSNR.UICorrection", 0); // the frame carries no UI yet
		write_tuning(g.params, t);

		const unsigned int s = g.slot;
		g_last_network[0] = 0;
		Handle *h_new = nullptr;
		Result r = result_fail;
		if (g.v.on)
		{
			if (!vk_slot_ready(s, 5000))
			{
				fail("DXVK's queue stopped responding");
				return false;
			}
			vk_begin(s);
			r = g.v.create(g.v.dev, g.v.cmd[s], feature_nr, g.params, &h_new);
			g.v.end(g.v.cmd[s]);
			g.slot = (s + 1) % ring;
			if (!vk_submit(s) || !vk_slot_ready(s, 10000))
			{
				fail("DLSS NR creation did not finish on the GPU");
				return false;
			}
		}
		else
		{
			if (!wait_value(g.alloc_done[s], 5000))
			{
				fail("the D3D12 queue stopped responding");
				return false;
			}
			g.alloc[s]->Reset();
			g.list->Reset(g.alloc[s], nullptr);
			r = g.create(g.list, feature_nr, g.params, &h_new);
			g.list->Close();
			const uint64_t done = submit(g.fence_value);
			g.alloc_done[s] = done;
			g.slot = (s + 1) % ring;
			if (!wait_value(done, 10000))
			{
				fail("DLSS NR creation did not finish on the GPU");
				return false;
			}
		}
		AcquireSRWLockExclusive(&g.lock);
		g.st.creates++;
		g.st.last_result = r;
		ReleaseSRWLockExclusive(&g.lock);
		if (!succeeded(r) || h_new == nullptr)
		{
			fail("DLSS NR feature creation failed (0x%08X) at %ux%u, pass %u, preset %u", r, w, h, i + 1, t.preset);
			return false;
		}
		pass_feature &p = g.pass[i];
		p.handle = h_new;
		p.built = t;
		p.pending = t;
		p.pending_since = GetTickCount64();
		p.reset = true;
		std::memcpy(p.network, g_last_network, sizeof(p.network));
		if (p.network[0] == 0)
		{
			// The snippet names the network only at verbose NGX logging; the file's table gives the same name
			const forge::nr::dll_info &d = forge::nr::snippet_info();
			for (unsigned int k = 0; k < d.count; ++k)
				if (d.table && d.networks[k].preset == static_cast<int>(t.preset))
					snprintf(p.network, sizeof(p.network), "%s", d.networks[k].name);
		}
		publish_pass(i);
		FORGE_LOG("DLSS NR pass %u: %ux%u preset %u style %u intensity %.2f structure %.2f tone %.2f skin %.2f mask %d -> %s",
			i + 1, w, h, t.preset, t.style, t.intensity, t.structure, t.tone, t.skin, t.auto_mask ? 1 : 0, p.network[0] ? p.network : (t.preset == 0 ? "the DLL's default" : "(not in the DLL's table)"));
		return true;
	}

	void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
	{
		if (r == nullptr || from == to)
			return;
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = r;
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = from;
		b.Transition.StateAfter = to;
		g.list->ResourceBarrier(1, &b);
	}

	void read_timing(unsigned int s)
	{
		if (g.query_done[s] == 0 || g.fence12->GetCompletedValue() < g.query_done[s] || g.query_readback == nullptr || g.ts_freq == 0)
			return;
		void *mapped = nullptr;
		D3D12_RANGE range = { sizeof(uint64_t) * 2 * s, sizeof(uint64_t) * 2 * (s + 1) };
		if (SUCCEEDED(g.query_readback->Map(0, &range, &mapped)))
		{
			const auto *ts = static_cast<const uint64_t *>(mapped) + 2 * s;
			if (ts[1] > ts[0])
			{
				const double ms = 1000.0 * static_cast<double>(ts[1] - ts[0]) / static_cast<double>(g.ts_freq);
				AcquireSRWLockExclusive(&g.lock);
				g.st.gpu_ms = g.st.gpu_ms < 0.0 ? ms : g.st.gpu_ms * 0.9 + ms * 0.1;
				ReleaseSRWLockExclusive(&g.lock);
			}
			D3D12_RANGE none = { 0, 0 };
			g.query_readback->Unmap(0, &none);
		}
		g.query_done[s] = 0;
	}

	// The per-evaluate parameters of one pass; the resources are ID3D12Resource pointers or NGX Vulkan resources
	void write_pass_params(Parameter *p, void *color, void *mvec, void *output, unsigned int ww, unsigned int wh,
		const forge::nr::frame &f, bool reset, float mv_x, float mv_y)
	{
		set_ptr(p, "DLSSNR.Color", color);
		set_ptr(p, "DLSSNR.Depth", nullptr);
		set_ptr(p, "DLSSNR.MVec", mvec);
		set_ptr(p, "DLSSNR.Output", output);
		set_ui(p, "DLSSNR.Enabled", 1);
		set_ui(p, "DLSSNR.Width", ww);
		set_ui(p, "DLSSNR.Height", wh);
		set_ui(p, "DLSSNR.DepthInverted", f.depth_inverted ? 1u : 0u);
		set_ui(p, "DLSSNR.Reset", reset ? 1u : 0u);
		struct rect { const char *n; unsigned int w, h; };
		for (const rect &rr : { rect { "Color", ww, wh }, rect { "Output", ww, wh }, rect { "Depth", 0, 0 }, rect { "MVec", mvec != nullptr ? ww : 0, mvec != nullptr ? wh : 0 } })
		{
			char k[64];
			snprintf(k, sizeof(k), "DLSSNR.%sSubrectBaseX", rr.n);
			set_ui(p, k, 0);
			snprintf(k, sizeof(k), "DLSSNR.%sSubrectBaseY", rr.n);
			set_ui(p, k, 0);
			snprintf(k, sizeof(k), "DLSSNR.%sSubrectWidth", rr.n);
			set_ui(p, k, rr.w);
			snprintf(k, sizeof(k), "DLSSNR.%sSubrectHeight", rr.n);
			set_ui(p, k, rr.h);
		}
		set_f(p, "DLSSNR.MVecScaleX", mv_x);
		set_f(p, "DLSSNR.MVecScaleY", mv_y);
	}

	void fill_cb(cb_data &cb, const forge::nr::frame &f, const forge::nr::config &c, unsigned int ww, unsigned int wh, bool show_groups, bool show_estimate)
	{
		const auto clamp = [](float v, float lo, float hi, float d) { return std::isfinite(v) ? (v < lo ? lo : (v > hi ? hi : v)) : d; };
		cb = {};
		cb.rect[0] = f.x;
		cb.rect[1] = f.y;
		cb.rect[2] = f.w;
		cb.rect[3] = f.h;
		cb.guides[0] = f.depth_x;
		cb.guides[1] = f.depth_y;
		cb.guides[2] = f.motion_x;
		cb.guides[3] = f.motion_y;
		cb.sizes[0] = f.guide_w;
		cb.sizes[1] = f.guide_h;
		cb.sizes[2] = ww;
		cb.sizes[3] = wh;
		cb.motion_size[0] = f.motion_w;
		cb.motion_size[1] = f.motion_h;
		cb.motion_size[2] = f.motion != nullptr ? 1u : 0u;
		cb.motion_size[3] = f.depth != nullptr ? 1u : 0u;
		cb.encoding[0] = clamp(c.paper_white, 0.01f, 4096.0f, 1.0f);
		cb.encoding[1] = forge::nr::motion_to_uv(f.mv_scale_x, f.render_w);
		cb.encoding[2] = forge::nr::motion_to_uv(f.mv_scale_y, f.render_h);
		cb.modes[0] = f.hdr ? 0u : 1u;
		cb.modes[1] = static_cast<uint32_t>(c.debug_view < 0 || c.debug_view > 3 ? 0 : c.debug_view);
		cb.modes[2] = c.apply ? 1u : 0u;
		cb.modes[3] = static_cast<uint32_t>(c.compare < 0 || c.compare > 2 ? 0 : c.compare);
		cb.compare[0] = clamp(c.split, 0.0f, 1.0f, 0.5f);
		cb.compare[1] = c.swap ? 1.0f : 0.0f;
		const forge::nr::masking &m = c.mask;
		const auto group = [&](float *out, const forge::nr::mask_group &gr, float extra) {
			out[0] = clamp(gr.structure, 0.0f, 1.0f, 1.0f);
			out[1] = clamp(gr.tone, 0.0f, 1.0f, 1.0f);
			out[2] = gr.on ? 1.0f : 0.0f;
			out[3] = extra;
		};
		group(cb.mask_env, m.environment, 0.0f);
		cb.mask_env[2] = show_groups ? 1.0f : 0.0f;
		cb.mask_env[3] = show_estimate ? 1.0f : 0.0f;
		group(cb.mask_near, m.near_band, std::log2(clamp(m.near_end, 1.0f, 10000.0f, 20.0f)));
		group(cb.mask_far, m.far_band, std::log2(clamp(m.far_start, 1.0f, 10000.0f, 400.0f)));
		cb.mask_depth[0] = clamp(m.feather, 0.0f, 2.0f, 0.5f);
		cb.mask_depth[1] = f.depth_inverted ? 1.0f : 0.0f;
	}
}

bool forge::nr::run(const frame &in, const config &c)
{
	const bool previous_valid = g.history_valid;
	g.history_valid = false;
	if (in.ctx == nullptr || in.image == nullptr || in.w == 0 || in.h == 0)
		return false;
	if (g_retry.exchange(false))
	{
		AcquireSRWLockExclusive(&g.lock);
		g.st.failed = false;
		g.st.reason[0] = 0;
		ReleaseSRWLockExclusive(&g.lock);
		g.logged_reason[0] = 0;
		g.shader_failed = false;
	}
	{
		AcquireSRWLockShared(&g.lock);
		const bool failed = g.st.failed;
		ReleaseSRWLockShared(&g.lock);
		if (failed)
			return false;
	}

	ID3D11Device *dev = nullptr;
	in.ctx->GetDevice(&dev);
	if (dev == nullptr)
		return false;
	if (g.dev11 != nullptr && g.dev11 != dev)
		detach_d3d11();
	if (!g.v.on && g.dev12 == nullptr && !create_bridge(dev, c.device == 1 ? 1 : 0))
	{
		dev->Release();
		return false;
	}
	if (g.dev11 == nullptr && !attach_d3d11(dev))
	{
		dev->Release();
		return false;
	}
	dev->Release();
	if (!g.ngx_ready && !init_ngx())
		return false;
	if (!ensure_shaders())
		return false;
	frame_log_switch(c.frame_log);
	AcquireSRWLockExclusive(&g.lock);
	g.st.ready = true;
	ReleaseSRWLockExclusive(&g.lock);
	frame f = in;
	ID3D11Texture2D *image = nullptr;
	if (FAILED(f.image->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&image))))
		return false;
	D3D11_TEXTURE2D_DESC image_desc = {};
	image->GetDesc(&image_desc);
	image->Release();
	if (image_desc.SampleDesc.Count != 1 || f.x >= image_desc.Width || f.y >= image_desc.Height)
	{
		set_reason("DLSS 5 requires a single-sample image and an in-bounds rectangle");
		return false;
	}
	f.w = (std::min)(f.w, image_desc.Width - f.x);
	f.h = (std::min)(f.h, image_desc.Height - f.y);
	f.motion_w = f.motion_w != 0 ? f.motion_w : f.guide_w;
	f.motion_h = f.motion_h != 0 ? f.motion_h : f.guide_h;
	f.render_w = f.render_w != 0 ? f.render_w : f.motion_w;
	f.render_h = f.render_h != 0 ? f.render_h : f.motion_h;
	const bool finite_jitter = std::isfinite(f.jitter_x) && std::isfinite(f.jitter_y);
	if (!c.temporal || !std::isfinite(f.mv_scale_x) || !std::isfinite(f.mv_scale_y) || !finite_jitter ||
		f.motion_w == 0 || f.motion_h == 0 || f.render_w == 0 || f.render_h == 0)
		f.motion = nullptr;
	const std::array<float, 2> jitter = { motion_to_uv(f.jitter_x, f.render_w), motion_to_uv(f.jitter_y, f.render_h) };
	if (!c.mask.enabled || !g.probe_mask.empty() || f.guide_w == 0 || f.guide_h == 0)
		f.depth = nullptr;
	const auto guide = [&](view_set &views, ID3D11Resource *&resource, unsigned int x, unsigned int y, unsigned int &w, unsigned int &h, const char *name) {
		if (resource == nullptr) { w = h = 0; return true; }
		if (!ensure_view(views, resource, false, name)) return false;
		if (x >= views->w || y >= views->h) return false;
		w = (std::min)(w, views->w - x);
		h = (std::min)(h, views->h - y);
		return true;
	};
	if (!guide(g.depth_in, f.depth, f.depth_x, f.depth_y, f.guide_w, f.guide_h, "depth") ||
		!guide(g.motion_in, f.motion, f.motion_x, f.motion_y, f.motion_w, f.motion_h, "motion vectors"))
	{
		set_reason("DLSS 5 requires readable guides with in-bounds rectangles");
		return false;
	}

	// Working size of the model. A new size applies after it holds for half a second, so a slider drag rebuilds one time.
	const ULONGLONG now = GetTickCount64();
	bool rebuilding = false;
	float scale = std::isfinite(c.work_scale) ? c.work_scale : 1.0f;
	scale = scale < 0.25f ? 0.25f : (scale > 2.0f ? 2.0f : scale);
	unsigned int ww = static_cast<unsigned int>(f.w * scale + 0.5f), wh = static_cast<unsigned int>(f.h * scale + 0.5f);
	ww = ww < 64 ? 64 : ww;
	wh = wh < 64 ? 64 : wh;
	if (ww != g.size_w || wh != g.size_h)
	{
		g.size_w = ww;
		g.size_h = wh;
		g.size_since = now;
	}
	if (g.pass[0].handle != nullptr && (ww != g.feature_w || wh != g.feature_h) && now - g.size_since < settle_ms)
	{
		ww = g.feature_w;
		wh = g.feature_h;
		rebuilding = true;
	}
	const bool reduced = ww != f.w || wh != f.h;
	if (g.feature_w != ww || g.feature_h != wh)
	{
		release_features();
		g.feature_w = ww;
		g.feature_h = wh;
	}

	// Automask estimate: pass 1 again on the same input, with the other base character value. The difference of the
	// two outputs marks the model's character mask. It replaces the later passes.
	const bool estimate = c.show_automask && c.resolve(0).auto_mask;
	const auto want_tuning = [&](unsigned int i) {
		tuning t = c.resolve(estimate ? 0 : i);
		if (estimate && i == 1)
		{
			const float skin = t.skin < 0.0f ? t.structure : t.skin;
			t.skin = skin > 0.5f ? 0.0f : 1.0f;
		}
		return t;
	};

	// Features: pass 1 always, and at most one more pass feature for each frame. A creation value applies after it
	// holds for half a second.
	const unsigned int want = estimate ? 2 : c.pass_count();
	bool created_this_frame = false;
	for (unsigned int i = 0; i < max_passes; ++i)
	{
		pass_feature &p = g.pass[i];
		if (i >= want)
		{
			if (p.handle != nullptr)
				release_pass(i);
			continue;
		}
		const tuning t = want_tuning(i);
		if (latched(t) != latched(p.pending))
			p.pending_since = now;
		p.pending = t;
		const bool missing = p.handle == nullptr;
		const bool stale = !missing && needs_rebuild(p.built, t);
		if (!missing && !stale)
			continue;
		if (stale && now - p.pending_since < settle_ms)
		{
			rebuilding = true;
			continue;
		}
		if (created_this_frame && i > 0)
		{
			rebuilding = true;
			break;
		}
		if (!create_pass(i, ww, wh, t))
			return false;
		created_this_frame = true;
	}
	unsigned int passes = 0;
	while (passes < want && g.pass[passes].handle != nullptr)
		++passes;
	if (passes == 0)
		return false;

	// The control mask: the probe picture when the test hook set one, otherwise painted at the working size
	const bool probe_mask = !g.probe_mask.empty();
	const bool mask_on = c.mask.enabled || probe_mask;
	const unsigned int mw = probe_mask ? g.probe_w : ww, mh = probe_mask ? g.probe_h : wh;
	const DXGI_FORMAT mf = probe_mask ? g.probe_format : mask_format;
	const bool show_groups = mask_on && !probe_mask && c.mask.show;
	if (!ensure_shared(g.proxy, ww, wh, DXGI_FORMAT_R16G16B16A16_FLOAT, "model input") ||
		!ensure_shared(g.model[0], ww, wh, DXGI_FORMAT_R16G16B16A16_FLOAT, "model output") ||
		(passes > 1 && !ensure_shared(g.model[1], ww, wh, DXGI_FORMAT_R16G16B16A16_FLOAT, "second model output")) ||
		(f.depth != nullptr && !ensure_shared(g.depth, f.guide_w, f.guide_h, DXGI_FORMAT_R32_FLOAT, "depth")) ||
		(f.motion != nullptr && !ensure_shared(g.motion, ww, wh, motion_format, "motion vector")) ||
		(mask_on && !ensure_shared(g.mask, mw, mh, mf, "control mask")) ||
		(show_groups && !ensure_groups(ww, wh)) ||
		!ensure_keep(f.w, f.h, image_desc.Format))
		return false;
	if (!mask_on && g.mask.t11 != nullptr)
	{
		wait_idle();
		g.mask.release();
	}
	if (mask_on && !g.mask_logged)
	{
		g.mask_logged = true;
		FORGE_LOG("DLSS NR: control mask %ux%u format %d (%s), blend mode %d", mw, mh, static_cast<int>(mf), probe_mask ? "probe picture" : "painted", c.mask.blend);
	}
	if (!mask_on)
		g.mask_logged = false;

	ID3D11DeviceContext4 *ctx4 = nullptr;
	if (!g.v.on && FAILED(f.ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void **>(&ctx4))))
	{
		fail("the game's context has no fence support");
		return false;
	}

	// 1. Game context: the frame, depth and vectors into the shared textures, then the mask from them
	cb_data cb;
	fill_cb(cb, f, c, ww, wh, show_groups, estimate && passes > 1);
	const unsigned int blend = c.mask.blend < 0 || c.mask.blend > 3 ? 0u : static_cast<unsigned int>(c.mask.blend);
	history_key key;
	key.source = reinterpret_cast<uintptr_t>(f.source);
	key.context = reinterpret_cast<uintptr_t>(f.ctx);
	key.image = { f.x, f.y, f.w, f.h };
	key.depth = { f.depth_x, f.depth_y, f.guide_w, f.guide_h };
	key.motion = { f.motion_x, f.motion_y, f.motion_w, f.motion_h };
	key.render = { f.render_w, f.render_h };
	key.motion_jittered = f.motion_jittered;
	key.motion_scale = { cb.encoding[1], cb.encoding[2] };
	key.format = static_cast<unsigned int>(image_desc.Format);
	key.position = c.selected_stage();
	key.hdr = f.hdr;
	key.white = f.hdr ? cb.encoding[0] : 1.0f;
	key.temporal = f.motion != nullptr;
	key.estimate = estimate;
	key.mask = mask_on;
	if (mask_on)
	{
		key.mask_blend = blend;
		key.mask_revision = g.probe_revision;
		key.mask_values = { cb.mask_env[0], cb.mask_env[1], cb.mask_near[0], cb.mask_near[1], cb.mask_near[2], cb.mask_near[3],
			cb.mask_far[0], cb.mask_far[1], cb.mask_far[2], cb.mask_far[3], cb.mask_depth[0], cb.mask_depth[1] };
	}
	const uint64_t present = g_present.load(std::memory_order_relaxed);
	unsigned int reset_bits = history_reset(f.reset, previous_valid, present, g.history_present, key, g.history);
	bool chain_reset = reset_bits != 0;
	for (unsigned int axis = 0; axis < 2; ++axis)
	{
		cb.motion_map[axis] = c.selected_stage() == stage::before_upscaling ? 0.0f : jitter[axis];
		if (!chain_reset)
			cb.motion_map[axis + 2] = motion_jitter_offset(c.selected_stage(), f.motion_jittered, jitter[axis], g.history_jitter[axis]);
	}
	cs_state saved;
	if (!saved.save(f.ctx))
	{
		safe_release(ctx4);
		set_reason("DLSS 5 requires a D3D11.1 immediate context");
		return false;
	}
	const D3D11_BOX source_box = { f.x, f.y, 0, f.x + f.w, f.y + f.h, 1 };
	f.ctx->CopySubresourceRegion(g.keep, 0, 0, 0, 0, f.image, 0, &source_box);
	f.ctx->UpdateSubresource(g.cb, 0, nullptr, &cb, 0, 0);
	f.ctx->CSSetConstantBuffers(0, 1, &g.cb);
	dispatch(f.ctx, reduced ? g.pack_work_cs : g.pack_color_cs, { g.keep_srv }, { { 0, g.proxy.uav } }, ww, wh);
	if (f.depth != nullptr || f.motion != nullptr)
		dispatch(f.ctx, g.pack_guides_cs, { f.depth != nullptr ? g.depth_in->srv : nullptr, f.motion != nullptr ? g.motion_in->srv : nullptr },
			{ { 0, f.depth != nullptr ? g.depth.uav : nullptr }, { 1, f.motion != nullptr ? g.motion.uav : nullptr } },
			(std::max)(f.guide_w, ww), (std::max)(f.guide_h, wh));
	// Frame log probe: image a at the origin with size (aw, ah), image b at (bx, by) with size (bw, bh)
	const bool logging = g.frame_log && g.stats_cs != nullptr && ensure_stats();
	const auto probe = [&](unsigned int row, ID3D11ShaderResourceView *a, unsigned int aw, unsigned int ah,
		ID3D11ShaderResourceView *b, unsigned int bx, unsigned int by, unsigned int bw, unsigned int bh, bool motion) {
		struct { uint32_t a[4], b[4], mode[4]; float scale[4]; } params = {
			{ 0, 0, aw, ah }, { bx, by, bw, bh }, { row, cb.modes[0] != 0 ? 1u : 0u, motion ? 1u : 0u, 0 }, { static_cast<float>(f.w), static_cast<float>(f.h), 0.0f, 0.0f } };
		f.ctx->UpdateSubresource(g.stats_cb, 0, nullptr, &params, 0, 0);
		f.ctx->CSSetConstantBuffers(1, 1, &g.stats_cb);
		dispatch(f.ctx, g.stats_cs, { a, b }, { { 0, g.stats_uav } }, 1, 1);
	};
	if (logging)
		probe(row_motion, f.motion != nullptr ? g.motion.srv : nullptr, ww, wh, nullptr, 0, 0, 1, 1, true);
	if (mask_on && probe_mask)
	{
		if (g.probe_dirty)
			upload_probe_mask(f.ctx);
	}
	else if (mask_on)
		dispatch(f.ctx, g.pack_mask_cs, { f.depth != nullptr ? g.depth.srv : nullptr }, { { 0, g.mask.uav }, { 1, show_groups ? g.groups_uav : nullptr } }, ww, wh);
	uint64_t packed = 0;
	if (ctx4 != nullptr)
	{
		packed = ++g.fence_value;
		ctx4->Signal(g.fence11, packed);
	}

	// 2. The model passes in one submission. Each pass reads the output of the pass before it.
	// Packed motion is UV displacement. NGX divides these scales by the guide rectangle.
	const float mv_x = static_cast<float>(ww);
	const float mv_y = static_cast<float>(wh);
	Result r = result_success;
	unsigned int ran = 0;
	uint64_t evaluated = 0;
	if (g.v.on)
	{
		// Vulkan: a command buffer on DXVK's queue, behind what the game context recorded so far
		const unsigned int s = g.slot;
		if (!vk_slot_ready(s, 5000))
		{
			saved.restore(f.ctx);
			fail("DXVK's queue stopped responding");
			return false;
		}
		vk_begin(s);
		const VkCommandBuffer cmd = g.v.cmd[s];
		if (g.v.queries != VK_NULL_HANDLE)
		{
			g.v.reset_queries(cmd, g.v.queries, 2 * s, 2);
			g.v.timestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g.v.queries, 2 * s);
		}
		for (const shared_tex *t : { &g.proxy, &g.depth, &g.motion, &g.mask, &g.model[0], &g.model[1] })
			vk_image_barrier(cmd, *t, true);
		for (unsigned int i = 0; i < passes; ++i)
		{
			pass_feature &pf = g.pass[i];
			const unsigned int out = i & 1;
			shared_tex &input = i == 0 || estimate ? g.proxy : g.model[out ^ 1];
			if (i > 0)
				vk_memory_barrier(cmd);
			const tuning active = evaluate_tuning(pf.built, want_tuning(i));
			const bool pass_reset = chain_reset || pf.reset;
			reset_bits |= pf.reset ? 16u : 0u;
			write_pass_params(g.params, &input.res, f.motion != nullptr ? &g.motion.res : nullptr, &g.model[out].res, ww, wh, f, pass_reset, mv_x, mv_y);
			write_mask_params(g.params, mask_on ? &g.mask.res : nullptr, mw, mh, blend);
			write_tuning(g.params, active);
			reset_bits |= output_changed(pf.evaluated, active) ? 32u : 0u;
			r = g.v.evaluate(cmd, pf.handle, g.params, nullptr);
			if (!succeeded(r))
				break;
			chain_reset = chain_reset || (!estimate && (pf.reset || output_changed(pf.evaluated, active)));
			pf.evaluated = active;
			pf.reset = false;
			ran = i + 1;
		}
		if (g.v.queries != VK_NULL_HANDLE)
			g.v.timestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g.v.queries, 2 * s + 1);
		for (const shared_tex *t : { &g.proxy, &g.depth, &g.motion, &g.mask, &g.model[0], &g.model[1] })
			vk_image_barrier(cmd, *t, false);
		g.v.end(cmd);
		g.v.timed[s] = g.v.queries != VK_NULL_HANDLE;
		g.slot = (s + 1) % ring;
		if (!vk_submit(s))
		{
			saved.restore(f.ctx);
			fail("the submission to DXVK's queue failed");
			return false;
		}
	}
	else
	{
		// D3D12 bridge queue: waits for the game context's work through the shared fence
		const unsigned int s = g.slot;
		read_timing(s);
		if (!wait_value(g.alloc_done[s], 5000))
		{
			saved.restore(f.ctx);
			ctx4->Release();
			fail("the D3D12 queue stopped responding");
			return false;
		}
		g.alloc[s]->Reset();
		g.list->Reset(g.alloc[s], nullptr);
		constexpr D3D12_RESOURCE_STATES common = D3D12_RESOURCE_STATE_COMMON;
		constexpr D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		constexpr D3D12_RESOURCE_STATES write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		D3D12_RESOURCE_STATES model_state[2] = { common, common };
		barrier(g.proxy.t12, common, read);
		barrier(g.depth.t12, common, read);
		barrier(g.motion.t12, common, read);
		if (mask_on)
			barrier(g.mask.t12, common, read);
		if (g.queries != nullptr)
			g.list->EndQuery(g.queries, D3D12_QUERY_TYPE_TIMESTAMP, 2 * s);
		for (unsigned int i = 0; i < passes; ++i)
		{
			pass_feature &pf = g.pass[i];
			const unsigned int out = i & 1;
			const bool chained = i > 0 && !estimate;
			ID3D12Resource *input = chained ? g.model[out ^ 1].t12 : g.proxy.t12;
			if (chained)
			{
				barrier(g.model[out ^ 1].t12, model_state[out ^ 1], read);
				model_state[out ^ 1] = read;
			}
			barrier(g.model[out].t12, model_state[out], write);
			model_state[out] = write;
			const tuning active = evaluate_tuning(pf.built, want_tuning(i));
			const bool pass_reset = chain_reset || pf.reset;
			reset_bits |= pf.reset ? 16u : 0u;
			write_pass_params(g.params, input, f.motion != nullptr ? g.motion.t12 : nullptr, g.model[out].t12, ww, wh, f, pass_reset, mv_x, mv_y);
			write_mask_params(g.params, mask_on ? static_cast<void *>(g.mask.t12) : nullptr, mw, mh, blend);
			write_tuning(g.params, active);
			reset_bits |= output_changed(pf.evaluated, active) ? 32u : 0u;
			r = g.evaluate(g.list, pf.handle, g.params, nullptr);
			if (!succeeded(r))
				break;
			chain_reset = chain_reset || (!estimate && (pf.reset || output_changed(pf.evaluated, active)));
			pf.evaluated = active;
			pf.reset = false;
			ran = i + 1;
		}
		if (g.queries != nullptr)
		{
			g.list->EndQuery(g.queries, D3D12_QUERY_TYPE_TIMESTAMP, 2 * s + 1);
			g.list->ResolveQueryData(g.queries, D3D12_QUERY_TYPE_TIMESTAMP, 2 * s, 2, g.query_readback, sizeof(uint64_t) * 2 * s);
		}
		barrier(g.proxy.t12, read, common);
		barrier(g.depth.t12, read, common);
		barrier(g.motion.t12, read, common);
		if (mask_on)
			barrier(g.mask.t12, read, common);
		barrier(g.model[0].t12, model_state[0], common);
		if (g.model[1].t12 != nullptr)
			barrier(g.model[1].t12, model_state[1], common);
		g.list->Close();
		evaluated = submit(packed);
		g.alloc_done[s] = evaluated;
		g.query_done[s] = g.queries != nullptr ? evaluated : 0;
		g.slot = (s + 1) % ring;
	}
	const unsigned int final_out = ran > 0 && !estimate ? (ran - 1) & 1 : 0;

	// 3. Game context: (D3D12) waits for the model on the GPU, then composes the final answer into the DLSS output
	if (ctx4 != nullptr)
	{
		ctx4->Wait(g.fence11, evaluated);
		ctx4->Release();
	}
	if (logging && ran > 0)
		probe(row_chain, g.proxy.srv, ww, wh, g.model[final_out].srv, 0, 0, ww, wh, false);
	if (ran > 0)
	{
		ID3D11ShaderResourceView *views[] = { g.keep_srv, g.model[final_out].srv, g.proxy.srv,
			show_groups ? g.groups_srv : nullptr, estimate && ran > 1 ? g.model[1].srv : nullptr };
		const D3D11_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(f.w), static_cast<float>(f.h), 0.0f, 1.0f };
		f.ctx->OMSetRenderTargets(1, &g.output_rtv, nullptr);
		f.ctx->RSSetState(g.rasterizer);
		f.ctx->RSSetViewports(1, &viewport);
		f.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		f.ctx->VSSetShader(g.output_vs, nullptr, 0);
		f.ctx->PSSetShader(g.resolve_ps, nullptr, 0);
		f.ctx->PSSetConstantBuffers(0, 1, &g.cb);
		f.ctx->PSSetShaderResources(0, 5, views);
		f.ctx->Draw(3, 0);
		f.ctx->OMSetRenderTargets(0, nullptr, nullptr);
		ID3D11ShaderResourceView *empty[5] = {};
		f.ctx->PSSetShaderResources(0, 5, empty);
		f.ctx->CopySubresourceRegion(f.image, 0, f.x, f.y, 0, g.output, 0, nullptr);
	}
	if (logging && ran > 0)
	{
		probe(row_frame, g.keep_srv, f.w, f.h, g.output_srv, 0, 0, f.w, f.h, false);
		if (g.stats_next - g.stats_done >= stats_ring)
		{
			// The GPU is too far behind. The log skips the oldest record.
			g.records[g.stats_done % stats_ring].pending = false;
			g.stats_done = g.stats_next - stats_ring + 1;
		}
		const unsigned int slot = static_cast<unsigned int>(g.stats_next % stats_ring);
		f.ctx->CopyResource(g.stats_read[slot], g.stats);
		LARGE_INTEGER tick = {}, freq = {};
		QueryPerformanceCounter(&tick);
		QueryPerformanceFrequency(&freq);
		frame_record &rec = g.records[slot];
		rec.pending = true;
		rec.frame = g.stats_next;
		rec.time_ms = freq.QuadPart != 0 ? 1000.0 * static_cast<double>(tick.QuadPart - g.log_start.QuadPart) / static_cast<double>(freq.QuadPart) : 0.0;
		rec.reset_bits = reset_bits;
		rec.mode = cb.modes[0];
		rec.passes = ran;
		g.stats_next++;
	}
	saved.restore(f.ctx);
	if (g.frame_log && g.stats_read[0] != nullptr)
		harvest(f.ctx);

	AcquireSRWLockExclusive(&g.lock);
	g.st.last_result = r;
	g.st.active = ran > 0;
	g.st.passes = ran;
	g.st.rebuilding = rebuilding;
	g.st.work_w = ww;
	g.st.work_h = wh;
	g.st.frame_w = f.w;
	g.st.frame_h = f.h;
	g.st.guide_w = f.motion != nullptr ? ww : 0;
	g.st.guide_h = f.motion != nullptr ? wh : 0;
	g.st.hdr = f.hdr;
	g.st.position = c.selected_stage();
	g.st.temporal = f.motion != nullptr;
	g.st.reset_bits = reset_bits;
	g.st.mask = mask_on;
	g.st.mask_w = mask_on ? mw : 0;
	g.st.mask_h = mask_on ? mh : 0;
	g.st.sync_ms = g.v.on ? g.v.sync_ms : -1.0;
	if (ran > 0)
		g.st.frames++;
	ReleaseSRWLockExclusive(&g.lock);
	if (!succeeded(r))
	{
		set_reason("DLSS NR evaluate failed (0x%08X) on pass %u", r, ran + 1);
		return ran > 0;
	}
	g.history = key;
	g.history_jitter = jitter;
	g.history_present = present;
	g.history_valid = ran == passes;
	if (g.st.reason[0] != 0 && !g.st.failed)
		set_reason("%s", "");
	return true;
}

void forge::nr::capture_present(const frame &f)
{
	g_capture.ready = false;
	if (f.ctx == nullptr) return;
	ID3D11Device *device = nullptr;
	f.ctx->GetDevice(&device);
	if (device == nullptr) return;
	if (device != g_capture.device)
	{
		g_capture.release();
		g_capture.device = device;
	}
	else
		device->Release();
	const auto copy = [&](ID3D11Resource *source, ID3D11Texture2D *&target) {
		if (source == nullptr) return false;
		ID3D11Texture2D *texture = nullptr;
		if (FAILED(source->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture)))) return false;
		D3D11_TEXTURE2D_DESC desc = {}, old = {};
		texture->GetDesc(&desc);
		texture->Release();
		if (desc.SampleDesc.Count != 1) return false;
		if (desc.Format == DXGI_FORMAT_D32_FLOAT) desc.Format = DXGI_FORMAT_R32_TYPELESS;
		if (desc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT) desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
		if (desc.Format == DXGI_FORMAT_D16_UNORM) desc.Format = DXGI_FORMAT_R16_TYPELESS;
		if (target != nullptr) target->GetDesc(&old);
		if (target == nullptr || old.Width != desc.Width || old.Height != desc.Height || old.Format != desc.Format)
		{
			safe_release(target);
			desc.MipLevels = desc.ArraySize = 1;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.CPUAccessFlags = desc.MiscFlags = 0;
			if (FAILED(g_capture.device->CreateTexture2D(&desc, nullptr, &target))) return false;
		}
		f.ctx->CopySubresourceRegion(target, 0, 0, 0, 0, source, 0, nullptr);
		return true;
	};
	g_capture.metadata = f;
	g_capture.metadata.image = nullptr;
	g_capture.metadata.ctx = nullptr;
	g_capture.metadata.depth = copy(f.depth, g_capture.depth) ? g_capture.depth : nullptr;
	g_capture.metadata.motion = copy(f.motion, g_capture.motion) ? g_capture.motion : nullptr;
	g_capture.serial = g_present.load(std::memory_order_relaxed);
	g_capture.ready = true;
}

bool forge::nr::run_present(ID3D11DeviceContext *ctx, ID3D11Resource *image, const config &c, bool linear_hdr)
{
	const bool fresh = g_capture.ready && g_capture.serial == g_present.load(std::memory_order_relaxed);
	frame f = fresh ? g_capture.metadata : frame {};
	g_capture.ready = false;
	if (image == nullptr || ctx == nullptr) return false;
	ID3D11Device *device = nullptr;
	ctx->GetDevice(&device);
	if (fresh && device != g_capture.device) f = frame {};
	safe_release(device);
	ID3D11Texture2D *texture = nullptr;
	if (FAILED(image->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture)))) return false;
	D3D11_TEXTURE2D_DESC desc = {};
	texture->GetDesc(&desc);
	texture->Release();
	f.ctx = ctx;
	f.image = image;
	f.x = f.y = 0;
	f.w = desc.Width;
	f.h = desc.Height;
	f.hdr = linear_hdr;
	return run(f, c);
}

void forge::nr::invalidate()
{
	g.history_valid = false;
	g_capture.ready = false;
}

void forge::nr::end_frame()
{
	g_present.fetch_add(1, std::memory_order_relaxed);
}

void forge::nr::release()
{
	g_capture.release();
	g.history_valid = false;
	if (g.dev12 == nullptr && g.dev11 == nullptr && !g.v.on)
		return;
	release_features();
	release_textures();
	AcquireSRWLockExclusive(&g.lock);
	g.st.active = false;
	g.st.passes = 0;
	ReleaseSRWLockExclusive(&g.lock);
	FORGE_LOG("DLSS NR: model and textures released; bridge device kept");
}

void forge::nr::detach()
{
	g_capture.release();
	g.history_valid = false;
	if (g.dev11 == nullptr)
		return;
	detach_d3d11();
	AcquireSRWLockExclusive(&g.lock);
	g.st.active = false;
	g.st.passes = 0;
	ReleaseSRWLockExclusive(&g.lock);
	FORGE_LOG("DLSS NR: detached from the game's D3D11 device; bridge device kept");
}

void forge::nr::retry()
{
	g_retry.store(true);
}

forge::nr::status forge::nr::get_status()
{
	AcquireSRWLockShared(&g.lock);
	status copy = g.st;
	ReleaseSRWLockShared(&g.lock);
	return copy;
}

void forge::nr::set_skip_device_factory(bool skip)
{
	g_skip_device_factory = skip;
}

void forge::nr::set_probe(bool all_live)
{
	g.probe = all_live;
}

bool forge::nr::is_live_parameter(const char *name)
{
	return live(name);
}

ID3D11Texture2D *forge::nr::debug_texture(int which)
{
	switch (which)
	{
	case 0: return g.keep;
	case 1: return g.proxy.t11;
	case 2: return g.model[0].t11;
	case 3: return g.model[1].t11;
	case 4: return g.mask.t11;
	case 5: return g.motion.t11;
	default: return nullptr;
	}
}

void forge::nr::set_probe_mask(const float *rgba, unsigned int w, unsigned int h, DXGI_FORMAT format)
{
	++g.probe_revision;
	if (rgba == nullptr || w == 0 || h == 0 || mask_pixel_bytes(format) == 0)
	{
		g.probe_mask.clear();
		g.probe_w = g.probe_h = 0;
		g.probe_format = DXGI_FORMAT_UNKNOWN;
		g.probe_dirty = false;
		return;
	}
	g.probe_mask.assign(rgba, rgba + static_cast<size_t>(w) * h * 4);
	g.probe_w = w;
	g.probe_h = h;
	g.probe_format = format;
	g.probe_dirty = true;
}
