#include "motion_vectors.h"
#include "log.h"
#include <cstring>
#include <vector>

namespace
{
	template <typename T>
	void safe_release(T *&p)
	{
		if (p != nullptr)
		{
			p->Release();
			p = nullptr;
		}
	}

	struct state
	{
		SRWLOCK lock = SRWLOCK_INIT;
		ID3D11DeviceContext *ctx = nullptr; // no reference: the immediate context of the game's device
		ID3D11Resource *source = nullptr;   // identity of the game's vector texture, never dereferenced
		ID3D11ShaderResourceView *vectors = nullptr;
		ID3D11Texture2D *info = nullptr;
		ID3D11ShaderResourceView *info_view = nullptr;
		std::vector<ID3D11ShaderResourceView *> retired; // replaced views; ReShade still binds them until the next binding
		float values[8] = {};
		bool valid = false;     // the info texture says valid
		bool published = false; // a DLSS evaluate since the last end_frame
		uint64_t generation = 0;
	} g;

	struct guard
	{
		guard() { AcquireSRWLockExclusive(&g.lock); }
		~guard() { ReleaseSRWLockExclusive(&g.lock); }
	};

	DXGI_FORMAT typed(DXGI_FORMAT f)
	{
		switch (f)
		{
		case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
		case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
		default: return f;
		}
	}

	// Called with the lock held
	bool ensure_info(ID3D11Device *dev)
	{
		if (g.info != nullptr)
			return true;
		D3D11_TEXTURE2D_DESC d = {};
		d.Width = 2;
		d.Height = 1;
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		const float zero[8] = {};
		const D3D11_SUBRESOURCE_DATA init = { zero, sizeof(zero), 0 };
		if (FAILED(dev->CreateTexture2D(&d, &init, &g.info)) || FAILED(dev->CreateShaderResourceView(g.info, nullptr, &g.info_view)))
		{
			safe_release(g.info);
			FORGE_LOG("motion vectors: info texture not created");
			return false;
		}
		return true;
	}
}

void forge::motion::publish(ID3D11DeviceContext *ctx, ID3D11Resource *vectors, unsigned int x, unsigned int y, unsigned int w, unsigned int h,
	float scale_x, float scale_y)
{
	if (ctx == nullptr || vectors == nullptr || w == 0 || h == 0)
		return;
	guard l;
	g.ctx = ctx;
	g.published = true;

	D3D11_TEXTURE2D_DESC d = {};
	ID3D11Texture2D *tex = nullptr;
	if (FAILED(vectors->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&tex))) || tex == nullptr)
		return;
	tex->GetDesc(&d);
	tex->Release();

	if (vectors != g.source)
	{
		ID3D11Device *dev = nullptr;
		ctx->GetDevice(&dev);
		if (dev == nullptr)
			return;
		D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
		vd.Format = typed(d.Format);
		vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		vd.Texture2D.MipLevels = 1;
		ID3D11ShaderResourceView *view = nullptr;
		const bool ok = ensure_info(dev) && SUCCEEDED(dev->CreateShaderResourceView(vectors, &vd, &view));
		dev->Release();
		if (!ok)
		{
			FORGE_LOG("motion vectors: no view of the game's vector texture (%ux%u format %d)", d.Width, d.Height, static_cast<int>(d.Format));
			return;
		}
		if (g.vectors != nullptr)
			g.retired.push_back(g.vectors);
		g.vectors = view;
		g.source = vectors;
		g.generation++;
		FORGE_LOG("motion vectors: game texture %p (%ux%u format %d) bound as %s, rectangle %u,%u %ux%u, scale %.3f %.3f",
			static_cast<void *>(vectors), d.Width, d.Height, static_cast<int>(d.Format), semantic_vectors, x, y, w, h, scale_x, scale_y);
	}

	const float values[8] = {
		scale_x / static_cast<float>(w), scale_y / static_cast<float>(h), 1.0f, 0.0f,
		static_cast<float>(x) / static_cast<float>(d.Width), static_cast<float>(y) / static_cast<float>(d.Height),
		static_cast<float>(w) / static_cast<float>(d.Width), static_cast<float>(h) / static_cast<float>(d.Height),
	};
	if (!g.valid || std::memcmp(values, g.values, sizeof(values)) != 0)
	{
		std::memcpy(g.values, values, sizeof(values));
		ctx->UpdateSubresource(g.info, 0, nullptr, g.values, sizeof(g.values), 0);
		g.valid = true;
	}
}

void forge::motion::end_frame()
{
	guard l;
	if (!g.published && g.valid && g.info != nullptr && g.ctx != nullptr)
	{
		g.values[2] = 0.0f;
		g.ctx->UpdateSubresource(g.info, 0, nullptr, g.values, sizeof(g.values), 0);
		g.valid = false;
	}
	g.published = false;
}

forge::motion::views forge::motion::get()
{
	guard l;
	views v;
	v.vectors = reinterpret_cast<uint64_t>(g.vectors);
	v.info = reinterpret_cast<uint64_t>(g.info_view);
	v.generation = g.generation;
	return v;
}

void forge::motion::bound(uint64_t generation)
{
	guard l;
	if (generation != g.generation)
		return;
	for (ID3D11ShaderResourceView *v : g.retired)
		v->Release();
	g.retired.clear();
}

void forge::motion::release()
{
	guard l;
	for (ID3D11ShaderResourceView *v : g.retired)
		v->Release();
	g.retired.clear();
	safe_release(g.vectors);
	safe_release(g.info_view);
	safe_release(g.info);
	g.ctx = nullptr;
	g.source = nullptr;
	g.valid = false;
	g.published = false;
	g.generation++;
}
