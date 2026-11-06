// Probe for the D3D11 (DXVK) <-> D3D12 (vkd3d-proton) interop that a DLSS Neural Rendering
// bridge for a D3D11 game needs: a D3D12 device on DXVK's adapter, NT-handle shared textures
// and shared fences, with data crossing in both directions.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>

namespace
{
	FILE *g_log = nullptr;
	int g_failures = 0;

	void say(const char *fmt, ...)
	{
		va_list a;
		va_start(a, fmt);
		vfprintf(g_log, fmt, a);
		va_end(a);
		fputc('\n', g_log);
		fflush(g_log);
	}

	void check(bool ok, const char *what, HRESULT hr = S_OK)
	{
		say("[%s] %s (hr 0x%08lX)", ok ? "PASS" : "FAIL", what, static_cast<unsigned long>(hr));
		if (!ok)
			g_failures++;
	}

	float from_half(uint16_t h)
	{
		const uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
		uint32_t x = exp == 0 ? sign : (sign | ((exp - 15 + 127) << 23) | (mant << 13));
		float f;
		std::memcpy(&f, &x, 4);
		return f;
	}

	uint16_t to_half(float f)
	{
		uint32_t x;
		std::memcpy(&x, &f, 4);
		const uint32_t sign = (x >> 16) & 0x8000;
		const int32_t exp = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
		return exp <= 0 ? static_cast<uint16_t>(sign) : static_cast<uint16_t>(sign | (exp << 10) | ((x & 0x7FFFFF) >> 13));
	}

	D3D12_RESOURCE_BARRIER transition(ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
	{
		D3D12_RESOURCE_BARRIER br = {};
		br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		br.Transition.pResource = r;
		br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		br.Transition.StateBefore = a;
		br.Transition.StateAfter = b;
		return br;
	}

	ID3D12Resource *make_buffer(ID3D12Device *dev, D3D12_HEAP_TYPE heap, UINT64 size)
	{
		D3D12_HEAP_PROPERTIES hp = {};
		hp.Type = heap;
		D3D12_RESOURCE_DESC d = {};
		d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		d.Width = size;
		d.Height = 1;
		d.DepthOrArraySize = 1;
		d.MipLevels = 1;
		d.SampleDesc.Count = 1;
		d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ID3D12Resource *r = nullptr;
		dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, heap == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&r));
		return r;
	}
}

int main()
{
	wchar_t path[MAX_PATH];
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	wcscpy(wcsrchr(path, L'.'), L".log");
	g_log = _wfopen(path, L"w");
	if (g_log == nullptr)
		return 3;

	IDXGIFactory1 *factory = nullptr;
	CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory));
	IDXGIAdapter1 *adapter = nullptr;
	factory->EnumAdapters1(0, &adapter);

	ID3D11Device *dev11 = nullptr;
	ID3D11DeviceContext *ctx11 = nullptr;
	const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
	HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev11, nullptr, &ctx11);
	check(SUCCEEDED(hr), "D3D11 device on the DXVK adapter", hr);

	// FORGE_HOG_MB=<n>: occupy that much VRAM on the D3D11 device first, the way a running game does
	char hog_env[32] = {};
	std::vector<ID3D11Texture2D *> hog;
	if (GetEnvironmentVariableA("FORGE_HOG_MB", hog_env, sizeof(hog_env)) != 0)
	{
		const int mb = atoi(hog_env);
		D3D11_TEXTURE2D_DESC hd = {};
		hd.Width = 4096;
		hd.Height = 2048; // 64 MiB at RGBA16F
		hd.MipLevels = 1;
		hd.ArraySize = 1;
		hd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		hd.SampleDesc.Count = 1;
		hd.BindFlags = D3D11_BIND_RENDER_TARGET;
		const float clear[4] = { 1, 0, 0, 1 };
		for (int i = 0; i < mb / 64; ++i)
		{
			ID3D11Texture2D *t = nullptr;
			ID3D11RenderTargetView *rtv = nullptr;
			if (FAILED(dev11->CreateTexture2D(&hd, nullptr, &t)))
				break;
			dev11->CreateRenderTargetView(t, nullptr, &rtv);
			if (rtv != nullptr)
			{
				ctx11->ClearRenderTargetView(rtv, clear);
				rtv->Release();
			}
			hog.push_back(t);
		}
		ctx11->Flush();
		say("occupied %zu MiB of VRAM on the D3D11 device", hog.size() * 64);
	}

	HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
	const bool wine_builtin = GetModuleHandleW(L"libvkd3d-1.dll") != nullptr;
	const bool vkd3d_proton = GetModuleHandleW(L"d3d12core.dll") != nullptr || GetProcAddress(d3d12, "D3D12GetInterface") != nullptr;
	say("d3d12.dll implementation: %s", wine_builtin ? "Wine builtin (upstream vkd3d, libvkd3d-1.dll)" : (vkd3d_proton ? "vkd3d-proton" : "unknown"));

	auto create12 = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(d3d12, "D3D12CreateDevice"));
	ID3D12Device *dev12 = nullptr;
	hr = create12 != nullptr ? create12(adapter, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), reinterpret_cast<void **>(&dev12)) : E_FAIL;
	check(SUCCEEDED(hr) && dev12 != nullptr, "D3D12CreateDevice on DXVK's adapter", hr);
	if (dev12 == nullptr)
	{
		say("%d failure(s)", g_failures);
		return 1;
	}

	// Shared texture created on D3D11, opened on D3D12
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = 256;
	td.Height = 256;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	ID3D11Texture2D *tex11 = nullptr;
	hr = dev11->CreateTexture2D(&td, nullptr, &tex11);
	check(SUCCEEDED(hr), "D3D11 NT-handle shared texture", hr);
	IDXGIResource1 *dxgi_res = nullptr;
	HANDLE tex_handle = nullptr;
	if (tex11 != nullptr && SUCCEEDED(tex11->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void **>(&dxgi_res))))
		hr = dxgi_res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &tex_handle);
	check(tex_handle != nullptr, "texture shared handle", hr);
	ID3D12Resource *tex12 = nullptr;
	hr = tex_handle != nullptr ? dev12->OpenSharedHandle(tex_handle, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&tex12)) : E_FAIL;
	check(SUCCEEDED(hr) && tex12 != nullptr, "D3D12 opens the D3D11 texture", hr);

	// Shared fence created on D3D11, opened on D3D12
	ID3D11Device5 *dev11_5 = nullptr;
	ID3D11DeviceContext4 *ctx11_4 = nullptr;
	dev11->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&dev11_5));
	ctx11->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void **>(&ctx11_4));
	ID3D11Fence *fence11 = nullptr;
	hr = dev11_5 != nullptr ? dev11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), reinterpret_cast<void **>(&fence11)) : E_NOINTERFACE;
	check(SUCCEEDED(hr) && fence11 != nullptr, "D3D11 shared fence", hr);
	HANDLE fence_handle = nullptr;
	if (fence11 != nullptr)
		hr = fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fence_handle);
	ID3D12Fence *fence12 = nullptr;
	hr = fence_handle != nullptr ? dev12->OpenSharedHandle(fence_handle, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&fence12)) : E_FAIL;
	check(SUCCEEDED(hr) && fence12 != nullptr, "D3D12 opens the D3D11 fence", hr);
	if (tex12 == nullptr || fence12 == nullptr || ctx11_4 == nullptr)
	{
		say("%d failure(s)", g_failures);
		return 1;
	}

	ID3D12CommandQueue *queue = nullptr;
	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	dev12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&queue));
	ID3D12CommandAllocator *alloc = nullptr;
	dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void **>(&alloc));
	ID3D12GraphicsCommandList *list = nullptr;
	dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&list));

	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rd.Width = 256;
	rd.Height = 256;
	rd.DepthOrArraySize = 1;
	rd.MipLevels = 1;
	rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	rd.SampleDesc.Count = 1;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
	UINT64 total = 0;
	dev12->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
	ID3D12Resource *readback = make_buffer(dev12, D3D12_HEAP_TYPE_READBACK, total);
	ID3D12Resource *upload = make_buffer(dev12, D3D12_HEAP_TYPE_UPLOAD, total);
	HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

	// D3D11 -> D3D12: clear on D3D11, signal, D3D12 waits and copies out
	ID3D11RenderTargetView *rtv = nullptr;
	dev11->CreateRenderTargetView(tex11, nullptr, &rtv);
	const float clear[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
	ctx11->ClearRenderTargetView(rtv, clear);
	ctx11_4->Signal(fence11, 1);
	ctx11->Flush();

	D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
	src.pResource = tex12;
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	dst.pResource = readback;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint = fp;
	D3D12_RESOURCE_BARRIER b = transition(tex12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
	list->ResourceBarrier(1, &b);
	list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	b = transition(tex12, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
	list->ResourceBarrier(1, &b);
	list->Close();
	queue->Wait(fence12, 1);
	ID3D12CommandList *lists[] = { list };
	queue->ExecuteCommandLists(1, lists);
	queue->Signal(fence12, 2);
	fence12->SetEventOnCompletion(2, event);
	const bool d12_done = WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
	check(d12_done, "D3D12 waited on the D3D11 signal and finished");
	void *mapped = nullptr;
	D3D12_RANGE range = { 0, static_cast<SIZE_T>(total) };
	float px[4] = {};
	if (d12_done && SUCCEEDED(readback->Map(0, &range, &mapped)))
	{
		const auto *h = static_cast<const uint16_t *>(mapped) + (fp.Footprint.RowPitch / 2) * 128 + 128 * 4;
		for (int i = 0; i < 4; ++i)
			px[i] = from_half(h[i]);
		readback->Unmap(0, nullptr);
	}
	char msg[160];
	snprintf(msg, sizeof(msg), "D3D11 -> D3D12 pixel %.3f %.3f %.3f %.3f (expect 0.25 0.5 0.75 1)", px[0], px[1], px[2], px[3]);
	check(px[0] > 0.24f && px[0] < 0.26f && px[2] > 0.74f && px[2] < 0.76f, msg);

	// D3D12 -> D3D11: D3D12 uploads 0.9, signals, D3D11 waits and reads back
	if (SUCCEEDED(upload->Map(0, nullptr, &mapped)))
	{
		auto *bytes = static_cast<uint8_t *>(mapped);
		for (UINT y = 0; y < 256; ++y)
		{
			auto *row = reinterpret_cast<uint16_t *>(bytes + static_cast<size_t>(y) * fp.Footprint.RowPitch);
			for (UINT i = 0; i < 256 * 4; ++i)
				row[i] = to_half(0.875f);
		}
		upload->Unmap(0, nullptr);
	}
	alloc->Reset();
	list->Reset(alloc, nullptr);
	src = {};
	src.pResource = upload;
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint = fp;
	dst = {};
	dst.pResource = tex12;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	b = transition(tex12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
	list->ResourceBarrier(1, &b);
	list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	b = transition(tex12, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
	list->ResourceBarrier(1, &b);
	list->Close();
	queue->ExecuteCommandLists(1, lists);
	queue->Signal(fence12, 3);
	ctx11_4->Wait(fence11, 3);

	td.MiscFlags = 0;
	td.BindFlags = 0;
	td.Usage = D3D11_USAGE_STAGING;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ID3D11Texture2D *staging = nullptr;
	dev11->CreateTexture2D(&td, nullptr, &staging);
	ctx11->CopyResource(staging, tex11);
	D3D11_MAPPED_SUBRESOURCE m = {};
	float back = 0.0f;
	if (SUCCEEDED(ctx11->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
	{
		back = from_half(static_cast<const uint16_t *>(m.pData)[(m.RowPitch / 2) * 64 + 64 * 4]);
		ctx11->Unmap(staging, 0);
	}
	snprintf(msg, sizeof(msg), "D3D12 -> D3D11 pixel %.3f (expect 0.875)", back);
	check(back > 0.87f && back < 0.88f, msg);

	say("%d failure(s)", g_failures);
	fclose(g_log);
	return g_failures ? 1 : 0;
}
