cbuffer Params : register(b0)
{
	uint4 input_size;
	uint4 output_size;
	uint4 guide_size;
	float4 scale;
	float4 jitter;
	float4 render_size;
	uint4 flags;
	uint4 images;
	uint4 depth_origin;
};
Texture2D<float4> current_input : register(t0);
Texture2D<float4> previous_input : register(t1);
Texture2D<float4> current_output : register(t2);
Texture2D<float4> previous_output : register(t3);
Texture2D<float2> motion : register(t4);
Texture2D<float> depth : register(t5);
Texture2D<float4> final_output : register(t6);
Texture2D<float> exposure : register(t7);
SamplerState linear_clamp : register(s0);
RWTexture2D<float4> statistics : register(u0);
RWTexture2D<float4> samples : register(u1);

static const uint COLS = 28;
static const uint TILE_W = 24, TILE_H = 12;
static const float BIG = 3.402823e38;
static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);
groupshared float4 reduce_values[64 * COLS];

bool Usable(float v) { return abs(v) < 1e10; }
bool Usable3(float3 v) { return all(abs(v) < 1e10); }
float2 PixelUV(float2 uv, uint2 size)
{
	return (min(floor(uv * float2(size)), float2(size) - 1.0) + 0.5) / float2(max(size, uint2(1, 1)));
}
float2 ReadMotion(float2 uv)
{
	int2 at = clamp(int2(floor(uv * float2(guide_size.xy))), int2(0, 0), int2(guide_size.xy) - 1);
	return motion.Load(int3(at, 0));
}
float2 Hypothesis(uint h, float2 raw)
{
	float2 uv = raw * scale.xy;
	if (h == 0) return 0.0;
	if (h == 2) return uv + jitter.zw;
	if (h == 3) return uv - jitter.zw;
	if (h == 4) return -uv;
	if (h == 5) return uv * 0.5;
	if (h == 6) return uv * 2.0;
	if (h == 7) return raw * scale.zw;
	return uv;
}
float4 Error(float3 current, float2 prev_uv, Texture2D<float4> previous)
{
	if (any(prev_uv < 0.0) || any(prev_uv >= 1.0)) return float4(0, 0, 0, 1);
	float3 old = previous.SampleLevel(linear_clamp, prev_uv, 0).rgb;
	if (!Usable3(old)) return float4(0, 0, 0, 1);
	float3 d = old - current;
	return float4(dot(d, d) / 3.0, dot(abs(d), 1.0.xxx) / 3.0, 1, 0);
}

[numthreads(64, 1, 1)]
void probe(uint3 thread : SV_GroupThreadID, uint3 group : SV_GroupID)
{
	uint region = group.x, t = thread.x;
	uint2 tile = uint2(region % 3, region / 3);
	float4 a[COLS];
	[unroll] for (uint init = 0; init < COLS; ++init) a[init] = 0.0;
	a[3] = float4(BIG, BIG, -BIG, -BIG);
	a[6].yz = float2(BIG, -BIG);
	a[26] = float4(BIG, -BIG, BIG, -BIG);
	a[27].xy = float2(BIG, -BIG);
	for (uint index = t; index < TILE_W * TILE_H; index += 64)
	{
		uint2 p = tile * uint2(TILE_W, TILE_H) + uint2(index % TILE_W, index / TILE_W);
		float2 uv = (float2(p) + 0.5) / float2(TILE_W * 3, TILE_H * 3);
		float2 iu = PixelUV(uv, input_size.xy), ou = PixelUV(uv, output_size.xy);
		float4 ci = current_input.Load(int3(int2(iu * float2(input_size.xy)), 0));
		float4 co = current_output.Load(int3(int2(ou * float2(output_size.xy)), 0));
		float4 cf = final_output.Load(int3(int2(ou * float2(output_size.xy)), 0));
		float2 mi = ReadMotion(iu), mo = ReadMotion(ou + jitter.xy);
		int2 dp = clamp(int2(iu * float2(guide_size.zw)), int2(0, 0), int2(guide_size.zw) - 1);
		float z = depth.Load(int3(dp + int2(depth_origin.xy), 0));
		bool vi = images.x != 0 && Usable3(ci.rgb);
		bool vo = images.y != 0 && Usable3(co.rgb);
		bool vf = images.z != 0 && Usable3(cf.rgb);
		bool vm = flags.z != 0 && all(abs(mi) < 1e10);
		bool vmo = flags.z != 0 && all(abs(mo) < 1e10);
		a[0].w += 1.0;
		if (vm)
		{
			float2 pixels = mi * scale.xy * render_size.xy;
			a[0].x += 1.0;
			a[0].y += all(mi == 0.0) ? 1.0 : 0.0;
			a[0].z += all(abs(pixels) < 0.001) ? 1.0 : 0.0;
			a[1] += float4(mi, mi * mi);
			a[2] += float4(pixels, pixels * pixels);
			a[3].xy = min(a[3].xy, mi);
			a[3].zw = max(a[3].zw, mi);
			a[4].x = max(a[4].x, length(pixels));
			a[27].z += any(abs(mi * scale.xy) > 1.0) ? 1.0 : 0.0;
		}
		a[4].yzw += float3(images.x != 0 && !vi, images.y != 0 && !vo, images.z != 0 && !vf);
		a[7] += float4(vi, vo, vf, vmo);
		if (vi)
		{
			float l = dot(ci.rgb, LUMA);
			a[5].x += l;
			a[24].z += l * l;
			a[26].x = min(a[26].x, min(ci.r, min(ci.g, ci.b)));
			a[26].y = max(a[26].y, max(ci.r, max(ci.g, ci.b)));
		}
		if (vo)
		{
			float l = dot(co.rgb, LUMA);
			a[5].y += l;
			a[24].w += l * l;
			a[26].z = min(a[26].z, min(co.r, min(co.g, co.b)));
			a[26].w = max(a[26].w, max(co.r, max(co.g, co.b)));
		}
		if (vf)
		{
			float l = dot(cf.rgb, LUMA);
			a[24].xy += float2(l, l * l);
			a[27].x = min(a[27].x, min(cf.r, min(cf.g, cf.b)));
			a[27].y = max(a[27].y, max(cf.r, max(cf.g, cf.b)));
		}
		if (vo && vf)
		{
			float l0 = dot(co.rgb, LUMA), l1 = dot(cf.rgb, LUMA);
			a[25] += float4(dot(abs(cf.rgb - co.rgb), 1.0.xxx) / 3.0, l1 - l0,
				log2((max(l1, 0.0) + 1e-4) / (max(l0, 0.0) + 1e-4)), 1.0);
		}
		float e = exposure.Load(int3(0, 0, 0));
		if (render_size.z != 0.0 && Usable(e)) a[5].zw += float2(e, 1.0);
		if (flags.w != 0 && Usable(z))
		{
			a[6].x += z;
			a[6].y = min(a[6].y, z);
			a[6].z = max(a[6].z, z);
			a[6].w += 1.0;
			a[27].w += z == 0.0 ? 1.0 : 0.0;
		}
		[unroll] for (uint h = 0; h < 8; ++h)
		{
			if (flags.x != 0 && vi && (h == 0 || vm))
				a[8 + h] += Error(ci.rgb, iu + Hypothesis(h, mi), previous_input);
			if (flags.y != 0 && vo && (h == 0 || vmo))
				a[16 + h] += Error(co.rgb, ou + Hypothesis(h, mo), previous_output);
		}
		if (images.w != 0)
		{
			samples[p] = float4(mi, mo);
			samples[p + uint2(0, 36)] = ci;
			samples[p + uint2(0, 72)] = co;
			samples[p + uint2(0, 108)] = cf;
			samples[p + uint2(0, 144)] = float4(z, iu, (vi ? 1 : 0) + (vo ? 2 : 0) + (vf ? 4 : 0) + (vm ? 8 : 0) + (vmo ? 16 : 0));
		}
	}
	[unroll] for (uint store = 0; store < COLS; ++store) reduce_values[store * 64 + t] = a[store];
	GroupMemoryBarrierWithGroupSync();
	if (t != 0) return;
	for (uint c = 0; c < COLS; ++c)
	{
		float4 v = reduce_values[c * 64];
		for (uint k = 1; k < 64; ++k)
		{
			float4 b = reduce_values[c * 64 + k];
			if (c == 3) v = float4(min(v.xy, b.xy), max(v.zw, b.zw));
			else if (c == 4) v = float4(max(v.x, b.x), v.yzw + b.yzw);
			else if (c == 6) v = float4(v.x + b.x, min(v.y, b.y), max(v.z, b.z), v.w + b.w);
			else if (c == 26) v = float4(min(v.x, b.x), max(v.y, b.y), min(v.z, b.z), max(v.w, b.w));
			else if (c == 27) v = float4(min(v.x, b.x), max(v.y, b.y), v.zw + b.zw);
			else v += b;
		}
		statistics[uint2(c, region)] = v;
	}
}
