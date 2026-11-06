#include "nr_bridge.h"
#include "nr_dll.h"
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
#include <cwchar>
#include <initializer_list>
#include <utility>
#include <vector>

using namespace ngx;
using forge::nr::max_passes;

namespace
{
	constexpr int feature_nr = 18;
	constexpr unsigned int ring = 8; // submission slots: one per frame, or one per pass with per-pass chaining
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



	struct cb_data
	{
		uint32_t rect[4];
		uint32_t guides[4];
		uint32_t sizes[4];
		float strength[4];
		uint32_t modes[4];
		float compare[4];
		float regions[4];
		float mask_env[4];
		float mask_near[4];
		float mask_far[4];
		float mask_depth[4];
		uint32_t motion_size[4];
		float jitter[4];
		float motion_scale[4];
		float codec[4];
		uint32_t codec_state[4];
	};
	static_assert(sizeof(cb_data) == 256, "constant buffer layout");

	// Control mask pixels (310.9.16): red is the weight, green the tone value, blue the structure value, alpha is
	// ignored. The local strengths become weight * value, so every pixel carries weight 1. The snippet samples the
	// mask normalised, so it may have any size.
	constexpr DXGI_FORMAT mask_format = DXGI_FORMAT_R8G8B8A8_UNORM;

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
		forge::nr::tuning evaluated; // tuning used by the last successful evaluation
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
		bool last_part[ring] = {}; // the slot holds the frame's last submission: its time completes the frame's
		double part_ms = 0.0;      // GPU time of the frame's earlier submissions
		VkQueryPool queries = VK_NULL_HANDLE;
		ngxvk::PFN_Init_ProjectID_Ext init = nullptr;
		ngxvk::PFN_CreateFeature1 create = nullptr;
		ngxvk::PFN_EvaluateFeature evaluate = nullptr;
		double sync_ms = -1.0; // CPU time of the per-frame DXVK flush
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
		forge::nr::frame history_frame;
		uint64_t history_present = 0;
		float history_mask[12] = {};
		bool history_mask_on = false, history_estimate = false;
		unsigned int history_blend = 0;
		uint64_t probe_revision = 0, history_probe_revision = 0;

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
		ID3D11UnorderedAccessView *keep_uav = nullptr;
		unsigned int keep_w = 0, keep_h = 0;
		DXGI_FORMAT keep_format = DXGI_FORMAT_UNKNOWN;

		ID3D11ComputeShader *pack_color_cs = nullptr, *pack_guides_cs = nullptr, *pack_work_cs = nullptr, *pack_mask_cs = nullptr, *resolve_cs = nullptr;
		ID3D11ComputeShader *meter_cs = nullptr, *transfer_cs = nullptr;
		// Per-pass chaining: the running image after each pass (D3D11 only, frame size, the frame copy's format)
		ID3D11Texture2D *work[2] = {};
		ID3D11ShaderResourceView *work_srv[2] = {};
		ID3D11UnorderedAccessView *work_uav[2] = {};
		unsigned int work_w = 0, work_h = 0;
		DXGI_FORMAT work_format = DXGI_FORMAT_UNKNOWN;
		// Linear codec: the applied divisor and whether it is moving (R32_FLOAT, 2 x 1)
		ID3D11Texture2D *meter = nullptr;
		ID3D11ShaderResourceView *meter_srv = nullptr;
		ID3D11UnorderedAccessView *meter_uav = nullptr;
		LARGE_INTEGER meter_tick = {};
		bool history_codec = false;
		ID3D11Buffer *cb = nullptr;
		ID3D11SamplerState *sampler = nullptr;
		view_set image, depth_in, motion_in;
		bool shader_failed = false;

		vk_backend v;
	} g;

	wchar_t g_dir[MAX_PATH] = {};
	char g_last_network[64] = {};
	std::atomic<bool> g_retry { false };
	std::atomic<uint64_t> g_present { 0 };

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
		double ms = static_cast<double>(ts[1] - ts[0]) * g.v.ts_period / 1.0e6;
		// Slots are read in submission order, so a frame's parts add up before its last one
		g.v.part_ms += ms;
		if (!g.v.last_part[s])
			return;
		ms = g.v.part_ms;
		g.v.part_ms = 0.0;
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
		safe_release(g.keep_uav);
		safe_release(g.keep);
		g.keep_w = g.keep_h = 0;
		g.keep_format = DXGI_FORMAT_UNKNOWN;
		for (unsigned int k = 0; k < 2; ++k)
		{
			safe_release(g.work_srv[k]);
			safe_release(g.work_uav[k]);
			safe_release(g.work[k]);
		}
		g.work_w = g.work_h = 0;
		g.work_format = DXGI_FORMAT_UNKNOWN;
		g.image.release();
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
		safe_release(g.resolve_cs);
		safe_release(g.meter_cs);
		safe_release(g.transfer_cs);
		safe_release(g.meter_srv);
		safe_release(g.meter_uav);
		safe_release(g.meter);
		safe_release(g.cb);
		safe_release(g.sampler);
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
		if (g.resolve_cs != nullptr)
			return true;
		if (g.shader_failed)
			return false;
		struct kernel { const unsigned char *code; size_t size; ID3D11ComputeShader **out; };
		for (const kernel &k : { kernel { nr_pack_color, sizeof(nr_pack_color), &g.pack_color_cs }, kernel { nr_pack_guides, sizeof(nr_pack_guides), &g.pack_guides_cs },
				 kernel { nr_pack_work, sizeof(nr_pack_work), &g.pack_work_cs }, kernel { nr_pack_mask, sizeof(nr_pack_mask), &g.pack_mask_cs },
				 kernel { nr_resolve, sizeof(nr_resolve), &g.resolve_cs }, kernel { nr_meter, sizeof(nr_meter), &g.meter_cs },
				 kernel { nr_transfer, sizeof(nr_transfer), &g.transfer_cs } })
		{
			if (FAILED(g.dev11->CreateComputeShader(k.code, k.size, nullptr, k.out)))
			{
				g.shader_failed = true;
				fail("composition shader creation failed");
				return false;
			}
		}
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = sizeof(cb_data);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		D3D11_SAMPLER_DESC sd = {};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(g.dev11->CreateBuffer(&bd, nullptr, &g.cb)) || FAILED(g.dev11->CreateSamplerState(&sd, &g.sampler)))
		{
			g.shader_failed = true;
			fail("composition constant buffer or sampler creation failed");
			return false;
		}
		return true;
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
		bool ok = g.v.on ? vk_create_tex(t, w, h, fmt) : SUCCEEDED(g.dev11->CreateTexture2D(&td, nullptr, &t.t11));
		ok = ok && SUCCEEDED(g.dev11->CreateShaderResourceView(t.t11, nullptr, &t.srv)) &&
			SUCCEEDED(g.dev11->CreateUnorderedAccessView(t.t11, nullptr, &t.uav));
		if (ok && !g.v.on)
		{
			HANDLE handle = nullptr;
			IDXGIResource1 *res = nullptr;
			ok = SUCCEEDED(t.t11->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void **>(&res))) &&
				SUCCEEDED(res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle)) &&
				SUCCEEDED(g.dev12->OpenSharedHandle(handle, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&t.t12)));
			safe_release(res);
			if (handle != nullptr)
				CloseHandle(handle);
		}
		if (!ok)
		{
			t.release();
			fail("shared %s texture %ux%u could not be created", what, w, h);
			return false;
		}
		t.w = w;
		t.h = h;
		t.fmt = fmt;
		return true;
	}

	// The exact copy of the frame: 32-bit float for 32-bit frames, otherwise 16-bit float holds every value the frame can carry
	bool ensure_keep(unsigned int w, unsigned int h, ID3D11Resource *image)
	{
		DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
		image->GetType(&dim);
		if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D)
		{
			D3D11_TEXTURE2D_DESC d = {};
			static_cast<ID3D11Texture2D *>(image)->GetDesc(&d);
			if (d.Format == DXGI_FORMAT_R32G32B32A32_TYPELESS || d.Format == DXGI_FORMAT_R32G32B32A32_FLOAT)
				format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		}
		if (g.keep != nullptr && g.keep_w == w && g.keep_h == h && g.keep_format == format)
			return true;
		safe_release(g.keep_srv);
		safe_release(g.keep_uav);
		safe_release(g.keep);
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = format;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		if (FAILED(g.dev11->CreateTexture2D(&td, nullptr, &g.keep)) ||
			FAILED(g.dev11->CreateShaderResourceView(g.keep, nullptr, &g.keep_srv)) ||
			FAILED(g.dev11->CreateUnorderedAccessView(g.keep, nullptr, &g.keep_uav)))
		{
			safe_release(g.keep_srv);
			safe_release(g.keep_uav);
			safe_release(g.keep);
			fail("frame copy %ux%u could not be created", w, h);
			return false;
		}
		g.keep_w = w;
		g.keep_h = h;
		g.keep_format = format;
		return true;
	}

	// The running images of per-pass chaining, in the frame copy's format
	bool ensure_work(unsigned int w, unsigned int h)
	{
		if (g.work[0] != nullptr && g.work_w == w && g.work_h == h && g.work_format == g.keep_format)
			return true;
		for (unsigned int k = 0; k < 2; ++k)
		{
			safe_release(g.work_srv[k]);
			safe_release(g.work_uav[k]);
			safe_release(g.work[k]);
		}
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = g.keep_format;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		for (unsigned int k = 0; k < 2; ++k)
		{
			if (FAILED(g.dev11->CreateTexture2D(&td, nullptr, &g.work[k])) ||
				FAILED(g.dev11->CreateShaderResourceView(g.work[k], nullptr, &g.work_srv[k])) ||
				FAILED(g.dev11->CreateUnorderedAccessView(g.work[k], nullptr, &g.work_uav[k])))
			{
				fail("per-pass image %ux%u could not be created", w, h);
				return false;
			}
		}
		g.work_w = w;
		g.work_h = h;
		g.work_format = g.keep_format;
		return true;
	}

	// The linear codec's divisor state, cleared to zero so the first frame snaps to its measurement
	bool ensure_meter()
	{
		if (g.meter != nullptr)
			return true;
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = 2;
		td.Height = 1;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R32_FLOAT;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		const float zero[2] = { 0.0f, 0.0f };
		D3D11_SUBRESOURCE_DATA init = { zero, sizeof(zero), 0 };
		if (FAILED(g.dev11->CreateTexture2D(&td, &init, &g.meter)) ||
			FAILED(g.dev11->CreateShaderResourceView(g.meter, nullptr, &g.meter_srv)) ||
			FAILED(g.dev11->CreateUnorderedAccessView(g.meter, nullptr, &g.meter_uav)))
		{
			safe_release(g.meter_srv);
			safe_release(g.meter_uav);
			safe_release(g.meter);
			fail("codec meter texture could not be created");
			return false;
		}
		return true;
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
		ID3D11ComputeShader *cs = nullptr;
		ID3D11ShaderResourceView *srv[6] = {};
		ID3D11UnorderedAccessView *uav[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
		ID3D11Buffer *cb = nullptr;
		ID3D11SamplerState *sampler = nullptr;

		void save(ID3D11DeviceContext *ctx)
		{
			ctx->CSGetShader(&cs, nullptr, nullptr);
			ctx->CSGetShaderResources(0, 6, srv);
			ctx->CSGetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, uav);
			ctx->CSGetConstantBuffers(0, 1, &cb);
			ctx->CSGetSamplers(0, 1, &sampler);
		}
		void restore(ID3D11DeviceContext *ctx)
		{
			ID3D11ShaderResourceView *none[6] = {};
			ctx->CSSetShaderResources(0, 6, none);
			UINT keep[D3D11_PS_CS_UAV_REGISTER_COUNT];
			for (UINT &k : keep)
				k = ~0u;
			ctx->CSSetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, uav, keep);
			ctx->CSSetShaderResources(0, 6, srv);
			ctx->CSSetConstantBuffers(0, 1, &cb);
			ctx->CSSetSamplers(0, 1, &sampler);
			ctx->CSSetShader(cs, nullptr, 0);
			safe_release(cs);
			for (auto *&s : srv)
				safe_release(s);
			for (auto *&u : uav)
				safe_release(u);
			safe_release(cb);
			safe_release(sampler);
		}
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
		if (from == to)
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
	void write_pass_params(Parameter *p, void *color, void *depth, void *mvec, void *output, unsigned int ww, unsigned int wh,
		const forge::nr::frame &f, bool reset, float mv_x, float mv_y)
	{
		set_ptr(p, "DLSSNR.Color", color);
		set_ptr(p, "DLSSNR.Depth", depth);
		set_ptr(p, "DLSSNR.MVec", mvec);
		set_ptr(p, "DLSSNR.Output", output);
		set_ui(p, "DLSSNR.Enabled", 1);
		set_ui(p, "DLSSNR.Width", ww);
		set_ui(p, "DLSSNR.Height", wh);
		set_ui(p, "DLSSNR.DepthInverted", f.depth_inverted ? 1u : 0u);
		set_ui(p, "DLSSNR.Reset", reset ? 1u : 0u);
		struct rect { const char *n; unsigned int w, h; };
		for (const rect &rr : { rect { "Color", ww, wh }, rect { "Output", ww, wh }, rect { "Depth", ww, wh }, rect { "MVec", ww, wh } })
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

	void fill_cb(cb_data &cb, const forge::nr::frame &f, const forge::nr::config &c, unsigned int ww, unsigned int wh, bool show_groups, bool show_estimate, bool previous_valid)
	{
		const auto clamp = [](float v, float lo, float hi, float d) { return v == v ? (v < lo ? lo : (v > hi ? hi : v)) : d; };
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
		cb.strength[0] = clamp(c.detail, 0.0f, 2.0f, 1.0f);
		cb.strength[1] = clamp(c.colour, 0.0f, 4.0f, 1.0f);
		cb.strength[2] = clamp(c.guard, 1.0f, 8.0f, 2.0f);
		cb.strength[3] = clamp(c.paper_white, 0.01f, 4096.0f, 1.0f);
		const bool use_codec = !f.hdr && c.codec;
		cb.modes[0] = f.hdr ? 0u : (use_codec ? 2u : 1u);
		cb.codec[0] = clamp(c.codec_luma, 0.0f, 4.0f, 2.0f);
		cb.codec[1] = clamp(c.codec_chroma, 0.0f, 2.0f, 1.0f);
		// The divisor rises at most 4 stops and falls at most 0.5 stops per second
		LARGE_INTEGER tick = {}, freq = {};
		QueryPerformanceCounter(&tick);
		QueryPerformanceFrequency(&freq);
		double dt = g.meter_tick.QuadPart != 0 && freq.QuadPart != 0 ? static_cast<double>(tick.QuadPart - g.meter_tick.QuadPart) / static_cast<double>(freq.QuadPart) : 0.0;
		dt = dt < 0.0 ? 0.0 : (dt > 0.1 ? 0.1 : dt);
		g.meter_tick = tick;
		cb.codec[2] = static_cast<float>(4.0 * dt);
		cb.codec[3] = static_cast<float>(0.5 * dt);
		cb.modes[1] = static_cast<uint32_t>(c.debug_view < 0 || c.debug_view > 3 ? 0 : c.debug_view);
		cb.modes[2] = c.apply ? 1u : 0u;
		cb.modes[3] = static_cast<uint32_t>(c.compare < 0 || c.compare > 2 ? 0 : c.compare);
		cb.compare[0] = clamp(c.split, 0.0f, 1.0f, 0.5f);
		cb.compare[1] = c.swap ? 1.0f : 0.0f;
		cb.compare[2] = c.regions ? 1.0f : 0.0f;
		cb.compare[3] = c.regions && c.show_mask ? 1.0f : 0.0f;
		cb.regions[0] = clamp(c.skin_detail, 0.0f, 1.0f, 1.0f);
		cb.regions[1] = c.skin_tone ? clamp(c.skin_colour, 0.0f, 1.0f, 1.0f) : 0.0f;
		cb.regions[2] = clamp(c.env_detail, 0.0f, 1.0f, 1.0f);
		cb.regions[3] = clamp(c.env_colour, 0.0f, 1.0f, 1.0f);
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
		cb.motion_size[0] = f.motion_w;
		cb.motion_size[1] = f.motion_h;
		cb.jitter[0] = f.jitter_x / static_cast<float>(f.guide_w);
		cb.jitter[1] = f.jitter_y / static_cast<float>(f.guide_h);
		cb.motion_scale[0] = f.mv_scale_x / static_cast<float>(f.motion_w);
		cb.motion_scale[1] = f.mv_scale_y / static_cast<float>(f.motion_h);
		const forge::nr::frame &previous = g.history_frame;
		if (f.motion_jittered && previous_valid && previous.motion_jittered &&
			f.source == previous.source && f.ctx == previous.ctx &&
			f.guide_w == previous.guide_w && f.guide_h == previous.guide_h)
		{
			// Stored vectors include previous jitter minus current jitter. Remove that
			// component to reproject the unjittered post-SR color in every NR pass.
			cb.jitter[2] = cb.jitter[0] - previous.jitter_x / static_cast<float>(previous.guide_w);
			cb.jitter[3] = cb.jitter[1] - previous.jitter_y / static_cast<float>(previous.guide_h);
		}
	}
}

bool forge::nr::run(const frame &in, const config &c)
{
	// Only a fully successful frame establishes continuity for the next evaluation.
	const bool previous_valid = g.history_valid;
	g.history_valid = false;
	if (in.ctx == nullptr || in.image == nullptr || in.depth == nullptr || in.motion == nullptr || in.w == 0 || in.h == 0 || in.guide_w == 0 || in.guide_h == 0)
		return false;
	if (!std::isfinite(in.jitter_x) || !std::isfinite(in.jitter_y) ||
		!std::isfinite(in.mv_scale_x) || !std::isfinite(in.mv_scale_y))
	{
		set_reason("the game's NR guide metadata contains non-finite jitter or motion scale");
		return false;
	}
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
	AcquireSRWLockExclusive(&g.lock);
	g.st.ready = true;
	ReleaseSRWLockExclusive(&g.lock);
	if (!ensure_view(g.image, in.image, true, "DLSS output") || !ensure_view(g.depth_in, in.depth, false, "depth") || !ensure_view(g.motion_in, in.motion, false, "motion vectors"))
		return false;

	// The regions, kept inside the textures they come from
	frame f = in;
	if (f.x >= g.image->w || f.y >= g.image->h || f.depth_x >= g.depth_in->w || f.depth_y >= g.depth_in->h || f.motion_x >= g.motion_in->w || f.motion_y >= g.motion_in->h)
	{
		set_reason("the DLSS regions lie outside their textures");
		return false;
	}
	f.w = f.w < g.image->w - f.x ? f.w : g.image->w - f.x;
	f.h = f.h < g.image->h - f.y ? f.h : g.image->h - f.y;
	f.motion_w = f.motion_w != 0 ? f.motion_w : f.guide_w;
	f.motion_h = f.motion_h != 0 ? f.motion_h : f.guide_h;
	const unsigned int gw = g.depth_in->w - f.depth_x, gh = g.depth_in->h - f.depth_y;
	const unsigned int mw_available = g.motion_in->w - f.motion_x, mh_available = g.motion_in->h - f.motion_y;
	f.guide_w = f.guide_w < gw ? f.guide_w : gw;
	f.guide_h = f.guide_h < gh ? f.guide_h : gh;
	f.motion_w = f.motion_w < mw_available ? f.motion_w : mw_available;
	f.motion_h = f.motion_h < mh_available ? f.motion_h : mh_available;

	// Working size of the model. A new size applies once it has held for half a second, so dragging the
	// slider rebuilds once; until then the model keeps its current size.
	const ULONGLONG now = GetTickCount64();
	bool rebuilding = false;
	float scale = c.work_scale == c.work_scale ? c.work_scale : 1.0f;
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

	// The automask estimate runs pass 1 a second time on the same input with the other base character
	// value; where the two answers differ, the model's own character mask applies. It takes the place of
	// further passes while it is shown.
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

	// Features: pass 1 always, later passes one creation per frame. Create-time changes apply once they
	// have held for half a second, so dragging a slider rebuilds once.
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
		!ensure_shared(g.depth, ww, wh, DXGI_FORMAT_R32_FLOAT, "depth") ||
		!ensure_shared(g.motion, ww, wh, DXGI_FORMAT_R16G16_FLOAT, "motion vector") ||
		(mask_on && !ensure_shared(g.mask, mw, mh, mf, "control mask")) ||
		(show_groups && !ensure_groups(ww, wh)) ||
		!ensure_keep(f.w, f.h, f.image) || !ensure_meter())
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
	fill_cb(cb, f, c, ww, wh, show_groups, estimate && passes > 1, previous_valid);
	const uint64_t present = g_present.load(std::memory_order_relaxed);
	const frame &previous = g.history_frame;
	const float mask_state[12] = {
		cb.mask_env[0], cb.mask_env[1],
		cb.mask_near[0], cb.mask_near[1], cb.mask_near[2], cb.mask_near[3],
		cb.mask_far[0], cb.mask_far[1], cb.mask_far[2], cb.mask_far[3],
		cb.mask_depth[0], cb.mask_depth[1]
	};
	const unsigned int blend = c.mask.blend < 0 || c.mask.blend > 3 ? 0u : static_cast<unsigned int>(c.mask.blend);
	// Texture objects may rotate every frame; continuity concerns their active grids and SR owner.
	bool chain_reset = f.reset || !previous_valid || estimate != g.history_estimate ||
		(present != g.history_present && present != g.history_present + 1) ||
		f.source != previous.source || f.ctx != previous.ctx ||
		f.x != previous.x || f.y != previous.y || f.w != previous.w || f.h != previous.h ||
		f.guide_w != previous.guide_w || f.guide_h != previous.guide_h ||
		f.motion_w != previous.motion_w || f.motion_h != previous.motion_h ||
		f.motion_jittered != previous.motion_jittered ||
		f.depth_x != previous.depth_x || f.depth_y != previous.depth_y ||
		f.motion_x != previous.motion_x || f.motion_y != previous.motion_y ||
		f.mv_scale_x != previous.mv_scale_x || f.mv_scale_y != previous.mv_scale_y ||
		f.depth_inverted != previous.depth_inverted || f.hdr != previous.hdr || c.codec != g.history_codec ||
		mask_on != g.history_mask_on ||
		(mask_on && (blend != g.history_blend || g.probe_revision != g.history_probe_revision ||
			std::memcmp(mask_state, g.history_mask, sizeof(mask_state)) != 0));
	// A cut, a size change or a switch of codec snaps the divisor to this frame's measurement
	cb.codec_state[0] = chain_reset ? 1u : 0u;
	cs_state saved;
	saved.save(f.ctx);
	f.ctx->UpdateSubresource(g.cb, 0, nullptr, &cb, 0, 0);
	f.ctx->CSSetConstantBuffers(0, 1, &g.cb);
	f.ctx->CSSetSamplers(0, 1, &g.sampler);
	const bool use_codec = cb.modes[0] == 2;
	if (use_codec)
		dispatch(f.ctx, g.meter_cs, { g.image->srv }, { { 0, g.meter_uav } }, 1, 1);
	dispatch(f.ctx, g.pack_color_cs, { g.image->srv, use_codec ? g.meter_srv : nullptr }, { { 0, g.keep_uav }, { 1, reduced ? nullptr : g.proxy.uav } }, f.w, f.h);
	if (reduced)
		dispatch(f.ctx, g.pack_work_cs, { g.keep_srv, use_codec ? g.meter_srv : nullptr }, { { 0, g.proxy.uav } }, ww, wh);
	dispatch(f.ctx, g.pack_guides_cs, { g.depth_in->srv, g.motion_in->srv }, { { 0, g.depth.uav }, { 1, g.motion.uav } }, ww, wh);
	if (mask_on && probe_mask)
	{
		if (g.probe_dirty)
			upload_probe_mask(f.ctx);
	}
	else if (mask_on)
		dispatch(f.ctx, g.pack_mask_cs, { g.depth.srv }, { { 0, g.mask.uav }, { 1, show_groups ? g.groups_uav : nullptr } }, ww, wh);
	uint64_t packed = 0;
	if (ctx4 != nullptr)
	{
		packed = ++g.fence_value;
		ctx4->Signal(g.fence11, packed);
	}

	// 2. The model passes. Chained: each pass reads the previous pass's raw answer in one submission, and the
	// resolve composes the last answer. Per pass (linear codec, more than one pass): after each pass the game
	// context applies that pass's transfer to the running image and encodes the result again, with the frame's
	// divisor, as the next pass's input; every pass is its own submission.
	// Prepared motion is in unjittered UV units; NGX scales it into model-grid pixels.
	const float mv_x = static_cast<float>(ww);
	const float mv_y = static_cast<float>(wh);
	const bool per_pass = use_codec && c.per_pass && passes > 1 && !estimate;
	if (per_pass && !ensure_work(f.w, f.h))
	{
		saved.restore(f.ctx);
		if (ctx4 != nullptr)
			ctx4->Release();
		return false;
	}
	Result r = result_success;
	unsigned int ran = 0;
	uint64_t evaluated = 0;
	const auto abort_frame = [&](const char *why) {
		saved.restore(f.ctx);
		if (ctx4 != nullptr)
			ctx4->Release();
		fail("%s", why);
		return false;
	};
	// Passes [first, last) in one submission behind the game context's work up to `after` (D3D12)
	const auto run_passes = [&](unsigned int first, unsigned int last, bool first_part, bool last_part, uint64_t after) -> bool {
		if (g.v.on)
		{
			// Vulkan: a command buffer on DXVK's queue, behind what the game context recorded so far
			const unsigned int s = g.slot;
			if (!vk_slot_ready(s, 5000))
				return abort_frame("DXVK's queue stopped responding");
			vk_begin(s);
			const VkCommandBuffer cmd = g.v.cmd[s];
			if (g.v.queries != VK_NULL_HANDLE)
			{
				g.v.reset_queries(cmd, g.v.queries, 2 * s, 2);
				g.v.timestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g.v.queries, 2 * s);
			}
			for (const shared_tex *t : { &g.proxy, &g.depth, &g.motion, &g.mask, &g.model[0], &g.model[1] })
				vk_image_barrier(cmd, *t, true);
			for (unsigned int i = first; i < last; ++i)
			{
				pass_feature &pf = g.pass[i];
				const unsigned int out = per_pass ? 0 : (i & 1);
				shared_tex &input = per_pass || i == 0 || estimate ? g.proxy : g.model[out ^ 1];
				if (i > first)
					vk_memory_barrier(cmd);
				const tuning active = evaluate_tuning(pf.built, want_tuning(i));
				chain_reset = chain_reset || pf.reset || active != pf.evaluated;
				write_pass_params(g.params, &input.res, &g.depth.res, &g.motion.res, &g.model[out].res, ww, wh, f, chain_reset, mv_x, mv_y);
				write_mask_params(g.params, mask_on ? &g.mask.res : nullptr, mw, mh, blend);
				write_tuning(g.params, active);
				r = g.v.evaluate(cmd, pf.handle, g.params, nullptr);
				if (!succeeded(r))
					break;
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
			g.v.last_part[s] = last_part || ran < last;
			g.slot = (s + 1) % ring;
			if (!vk_submit(s))
				return abort_frame("the submission to DXVK's queue failed");
			return true;
		}

		// D3D12 bridge queue: waits for the game context's work through the shared fence. One slot per frame:
		// its allocator is reset by the frame's first submission and released by its last.
		const unsigned int s = g.slot;
		if (first_part)
		{
			read_timing(s);
			if (!wait_value(g.alloc_done[s], 5000))
				return abort_frame("the D3D12 queue stopped responding");
			g.alloc[s]->Reset();
		}
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
		if (g.queries != nullptr && first_part)
			g.list->EndQuery(g.queries, D3D12_QUERY_TYPE_TIMESTAMP, 2 * s);
		for (unsigned int i = first; i < last; ++i)
		{
			pass_feature &pf = g.pass[i];
			const unsigned int out = per_pass ? 0 : (i & 1);
			const bool chained = !per_pass && i > 0 && !estimate;
			ID3D12Resource *input = chained ? g.model[out ^ 1].t12 : g.proxy.t12;
			if (chained)
			{
				barrier(g.model[out ^ 1].t12, model_state[out ^ 1], read);
				model_state[out ^ 1] = read;
			}
			barrier(g.model[out].t12, model_state[out], write);
			model_state[out] = write;
			const tuning active = evaluate_tuning(pf.built, want_tuning(i));
			chain_reset = chain_reset || pf.reset || active != pf.evaluated;
			write_pass_params(g.params, input, g.depth.t12, g.motion.t12, g.model[out].t12, ww, wh, f, chain_reset, mv_x, mv_y);
			write_mask_params(g.params, mask_on ? static_cast<void *>(g.mask.t12) : nullptr, mw, mh, blend);
			write_tuning(g.params, active);
			r = g.evaluate(g.list, pf.handle, g.params, nullptr);
			if (!succeeded(r))
				break;
			pf.evaluated = active;
			pf.reset = false;
			ran = i + 1;
		}
		const bool frame_done = last_part || ran < last;
		if (g.queries != nullptr && frame_done)
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
		evaluated = submit(after);
		if (frame_done)
		{
			g.alloc_done[s] = evaluated;
			g.query_done[s] = g.queries != nullptr ? evaluated : 0;
			g.slot = (s + 1) % ring;
		}
		return true;
	};

	if (!per_pass)
	{
		if (!run_passes(0, passes, true, true, packed))
			return false;
	}
	else
	{
		for (unsigned int i = 0; i < passes; ++i)
		{
			if (i > 0)
			{
				if (ran != i)
					break;
				// The game context waits for pass i, applies its transfer to the running image and encodes the
				// result as the next pass's input
				if (ctx4 != nullptr)
					ctx4->Wait(g.fence11, evaluated);
				ID3D11ShaderResourceView *src = i == 1 ? g.keep_srv : g.work_srv[i & 1];
				dispatch(f.ctx, g.transfer_cs, { src, g.model[0].srv, g.proxy.srv }, { { 0, g.work_uav[(i - 1) & 1] } }, f.w, f.h);
				dispatch(f.ctx, g.pack_work_cs, { g.work_srv[(i - 1) & 1], g.meter_srv }, { { 0, g.proxy.uav } }, ww, wh);
				if (ctx4 != nullptr)
				{
					packed = ++g.fence_value;
					ctx4->Signal(g.fence11, packed);
				}
			}
			if (!run_passes(i, i + 1, i == 0, i == passes - 1, packed))
				return false;
		}
	}
	const unsigned int final_out = per_pass ? 0 : (ran > 0 && !estimate ? (ran - 1) & 1 : 0);
	// The image the last pass enhanced: the game's frame, or the running image of the passes before it
	ID3D11ShaderResourceView *last_src = per_pass && ran > 1 ? g.work_srv[ran & 1] : g.keep_srv;

	// 3. Game context: (D3D12) waits for the model on the GPU, then composes the final answer into the DLSS output
	if (ctx4 != nullptr)
	{
		ctx4->Wait(g.fence11, evaluated);
		ctx4->Release();
	}
	if (ran > 0)
		dispatch(f.ctx, g.resolve_cs, { g.keep_srv, g.model[final_out].srv, g.proxy.srv, show_groups ? g.groups_srv : nullptr, estimate && ran > 1 ? g.model[1].srv : nullptr, last_src },
			{ { 0, g.image->uav } }, f.w, f.h);
	saved.restore(f.ctx);

	AcquireSRWLockExclusive(&g.lock);
	g.st.last_result = r;
	g.st.active = ran > 0;
	g.st.passes = ran;
	g.st.rebuilding = rebuilding;
	g.st.work_w = ww;
	g.st.work_h = wh;
	g.st.frame_w = f.w;
	g.st.frame_h = f.h;
	g.st.guide_w = f.guide_w;
	g.st.guide_h = f.guide_h;
	g.st.hdr = f.hdr;
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
	g.history_frame = f;
	g.history_present = present;
	g.history_mask_on = mask_on;
	g.history_estimate = estimate;
	g.history_codec = c.codec;
	g.history_blend = blend;
	g.history_probe_revision = g.probe_revision;
	std::memcpy(g.history_mask, mask_state, sizeof(mask_state));
	g.history_valid = ran == passes;
	if (g.st.reason[0] != 0 && !g.st.failed)
		set_reason("%s", "");
	return true;
}

void forge::nr::release()
{
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

void forge::nr::end_frame()
{
	g_present.fetch_add(1, std::memory_order_relaxed);
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
