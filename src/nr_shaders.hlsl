// NGX input packing, direct output and inspection views.
cbuffer params : register(b0)
{
	uint4 g_rect;         // target base x, base y, width, height
	uint4 g_guides;       // depth base x, base y, motion base x, base y
	uint4 g_sizes;        // depth width, height, model width, height
	uint4 g_motion_size;  // motion width, height, motion present, depth present
	float4 g_encoding;   // HDR white, motion-to-UV x, y, 0
	float4 g_motion_map; // guide sample offset UV, motion jitter correction UV
	uint4 g_modes;        // 0 HDR or 1 stored SDR, debug view, apply, compare
	float4 g_compare;     // split, swap, 0, 0
	float4 g_mask_env;    // environment structure, tone, show groups, show estimate
	float4 g_mask_near;   // near structure, tone, enabled, log2 end distance
	float4 g_mask_far;    // far structure, tone, enabled, log2 start distance
	float4 g_mask_depth;  // feather, inverted depth, 0, 0
};

#if defined(PACK_COLOR)
Texture2D<float4> g_image : register(t0);
RWTexture2D<float4> o_proxy : register(u0);
#elif defined(PACK_GUIDES)
Texture2D<float> g_depth : register(t0);
Texture2D<float2> g_motion : register(t1);
RWTexture2D<float> o_depth : register(u0);
RWTexture2D<float2> o_motion : register(u1);
#elif defined(PACK_WORK)
Texture2D<float4> g_keep : register(t0);
RWTexture2D<float4> o_proxy : register(u0);
#elif defined(PACK_MASK)
Texture2D<float> g_depth : register(t0);
RWTexture2D<float4> o_mask : register(u0);
RWTexture2D<float> o_groups : register(u1);
#elif defined(STATS)
Texture2D<float4> g_a : register(t0);
Texture2D<float4> g_b : register(t1);
RWTexture2D<float> o_stats : register(u0);
cbuffer stats_params : register(b1)
{
	uint4 g_stats_a;
	uint4 g_stats_b;
	uint4 g_stats_mode;   // row, sRGB luminance, motion, 0
	float4 g_stats_scale;
};
#elif defined(RESOLVE)
Texture2D<float4> g_keep : register(t0);
Texture2D<float4> g_model : register(t1);
Texture2D<float4> g_proxy : register(t2);
Texture2D<float> g_groups : register(t3);
Texture2D<float4> g_probe : register(t4);
#endif

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
bool Finite(float v) { return abs(v) <= 3.402823e38; }
bool Finite3(float3 v) { return Finite(v.x) && Finite(v.y) && Finite(v.z); }
float3 FiniteColor(float3 v)
{
	return float3(Finite(v.x) ? v.x : 0.0, Finite(v.y) ? v.y : 0.0, Finite(v.z) ? v.z : 0.0);
}
float3 Usable(float3 v) { return max(FiniteColor(v), 0.0); }
float3 LinearToSrgb(float3 v)
{
	v = saturate(v);
	return lerp(v * 12.92, 1.055 * pow(max(v, 1e-8), 1.0 / 2.4) - 0.055, step(0.0031308, v));
}
float3 SrgbToLinear(float3 v)
{
	v = saturate(v);
	return lerp(v / 12.92, pow((v + 0.055) / 1.055, 2.4), step(0.04045, v));
}
float3 Encode(float3 v)
{
	return g_modes.x != 0 ? v : LinearToSrgb(v / g_encoding.x);
}
float3 Decode(float3 v)
{
	return g_modes.x != 0 ? v : SrgbToLinear(v) * g_encoding.x;
}

#if defined(PACK_COLOR)
[numthreads(8, 8, 1)]
void pack_color(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= g_rect.zw)) return;
	float4 c = g_image.Load(int3(id.xy, 0));
	o_proxy[id.xy] = float4(Encode(FiniteColor(c.rgb)), c.a);
}
#endif

#if defined(PACK_GUIDES)
[numthreads(8, 8, 1)]
void pack_guides(uint3 id : SV_DispatchThreadID)
{
	if (g_motion_size.w != 0 && all(id.xy < g_sizes.xy))
		o_depth[id.xy] = g_depth.Load(int3(g_guides.xy + id.xy, 0));
	if (g_motion_size.z != 0 && all(id.xy < g_sizes.zw))
	{
		float2 uv = (float2(id.xy) + 0.5) / float2(g_sizes.zw) + g_motion_map.xy;
		int2 at = clamp(int2(floor(uv * float2(g_motion_size.xy))), int2(0, 0), int2(g_motion_size.xy) - 1);
		float2 mv = g_motion.Load(int3(int2(g_guides.zw) + at, 0)) * g_encoding.yz + g_motion_map.zw;
		o_motion[id.xy] = Finite(mv.x) && Finite(mv.y) ? mv : float2(0.0, 0.0);
	}
}
#endif

#if defined(PACK_WORK)
// Area sampling preserves the average of each input footprint.
[numthreads(8, 8, 1)]
void pack_work(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= g_sizes.zw)) return;
	float2 lo = float2(id.xy) * float2(g_rect.zw) / float2(g_sizes.zw);
	float2 hi = float2(id.xy + 1) * float2(g_rect.zw) / float2(g_sizes.zw);
	float3 acc = 0.0;
	for (int y = (int)floor(lo.y); y < (int)ceil(hi.y); ++y)
		for (int x = (int)floor(lo.x); x < (int)ceil(hi.x); ++x)
		{
			float2 weight = max(min(hi, float2(x + 1, y + 1)) - max(lo, float2(x, y)), 0.0);
			int2 at = clamp(int2(x, y), int2(0, 0), int2(g_rect.zw) - 1);
			acc += FiniteColor(g_keep.Load(int3(at, 0)).rgb) * weight.x * weight.y;
		}
	o_proxy[id.xy] = float4(Encode(acc / ((hi.x - lo.x) * (hi.y - lo.y))), 1.0);
}
#endif

#if defined(PACK_MASK)
[numthreads(8, 8, 1)]
void pack_mask(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= g_sizes.zw)) return;
	float nearWeight = 0.0, farWeight = 0.0;
	if (g_motion_size.w != 0)
	{
		float2 uv = (float2(id.xy) + 0.5) / float2(g_sizes.zw) + g_motion_map.xy;
		int2 at = clamp(int2(floor(uv * float2(g_sizes.xy))), int2(0, 0), int2(g_sizes.xy) - 1);
		float d = g_depth.Load(int3(at, 0));
		d = Finite(d) ? d : 0.0;
		float dist = g_mask_depth.y != 0.0 ? 1.0 / max(d, 1e-6) : 1.0 / max(1.0 - d, 1e-6);
		float ld = log2(max(dist, 1.0));
		float feather = max(g_mask_depth.x, 1e-3);
		nearWeight = g_mask_near.z != 0.0 ? 1.0 - smoothstep(g_mask_near.w - feather, g_mask_near.w + feather, ld) : 0.0;
		farWeight = g_mask_far.z != 0.0 ? smoothstep(g_mask_far.w - feather, g_mask_far.w + feather, ld) : 0.0;
	}
	float2 v = lerp(lerp(g_mask_env.xy, g_mask_far.xy, farWeight), g_mask_near.xy, nearWeight);
	float group = 0.0, best = 1.0 - max(nearWeight, farWeight);
	if (farWeight > best) { best = farWeight; group = 2.0; }
	if (nearWeight > best) group = 1.0;
	o_mask[id.xy] = float4(1.0, saturate(v.y), saturate(v.x), 1.0);
	if (g_mask_env.z != 0.0) o_groups[id.xy] = group * 0.25;
}
#endif

#if defined(OUTPUT_VS)
float4 output_vs(uint id : SV_VertexID) : SV_Position
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
#endif

#if defined(RESOLVE)
float4 ReadScaled(Texture2D<float4> image, int2 pixel)
{
	float4 value = image.Load(int3(pixel, 0));
	if (any(g_rect.zw != g_sizes.zw))
	{
		float2 p = (float2(pixel) + 0.5) * float2(g_sizes.zw) / float2(g_rect.zw) - 0.5;
		int2 lo = int2(floor(p)), top = int2(g_sizes.zw) - 1;
		float2 f = frac(p);
		float4 a = image.Load(int3(clamp(lo, int2(0, 0), top), 0));
		float4 b = image.Load(int3(clamp(lo + int2(1, 0), int2(0, 0), top), 0));
		float4 c = image.Load(int3(clamp(lo + int2(0, 1), int2(0, 0), top), 0));
		float4 d = image.Load(int3(clamp(lo + 1, int2(0, 0), top), 0));
		value = lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
	}
	return value;
}

float4 resolve(float4 position : SV_Position) : SV_Target
{
	int2 pixel = int2(position.xy);
	float2 uv = position.xy / float2(g_rect.zw);
	bool originalSide = false, divider = false, outside = false;
	if (g_modes.w == 1)
	{
		originalSide = (uv.x < 0.5) != (g_compare.y != 0.0);
		float2 p = float2(frac(uv.x * 2.0), (uv.y - 0.5) * 2.0 + 0.5);
		outside = any(p < 0.0) || any(p > 1.0);
		pixel = clamp(int2(p * float2(g_rect.zw)), int2(0, 0), int2(g_rect.zw) - 1);
		divider = abs(uv.x - 0.5) < 1.0 / g_rect.z;
	}
	else if (g_modes.w == 2)
	{
		originalSide = (uv.x < g_compare.x) != (g_compare.y != 0.0);
		divider = abs(uv.x - g_compare.x) < 1.0 / g_rect.z;
	}
	float4 original = g_keep.Load(int3(pixel, 0));
	if (g_modes.z == 0) return original;
	float3 model = ReadScaled(g_model, pixel).rgb;
	float3 shown = Finite3(model) ? Decode(model) : original.rgb;
	if (g_modes.y == 1) shown = Decode(ReadScaled(g_proxy, pixel).rgb);
	if (g_modes.y == 3) shown = saturate(0.5 + (model - ReadScaled(g_proxy, pixel).rgb) * 20.0);
	if (g_mask_env.z != 0.0)
	{
		int2 at = min(int2((float2(pixel) + 0.5) * float2(g_sizes.zw) / float2(g_rect.zw)), int2(g_sizes.zw) - 1);
		float group = g_groups.Load(int3(at, 0)) * 4.0;
		if (group > 0.5) shown = lerp(shown, group > 1.5 ? float3(0.65, 0.25, 1.0) : float3(0.2, 0.55, 1.0), 0.45);
	}
	if (g_mask_env.w != 0.0)
	{
		float weight = saturate(dot(abs(ReadScaled(g_probe, pixel).rgb - model), kLuma) * 12.0);
		shown = lerp(shown, float3(1.0, 0.1, 0.1), weight * 0.7);
	}
	if (originalSide) shown = original.rgb;
	if (outside) shown = 0.0;
	if (divider) shown = 1.0;
	return float4(shown, original.a);
}
#endif

#if defined(STATS)
groupshared float4 s_acc[64];
groupshared float s_top[64];
[numthreads(64, 1, 1)]
void stats(uint3 tid : SV_GroupThreadID)
{
	uint t = tid.x;
	float4 acc = 0.0;
	float top = 0.0;
	for (uint i = t; i < 2304; i += 64)
	{
		float2 uv = (float2((float)(i & 63), (float)(i >> 6)) + 0.5) / float2(64.0, 36.0);
		int2 pa = int2(g_stats_a.xy) + min(int2(uv * float2(g_stats_a.zw)), int2(g_stats_a.zw) - 1);
		float4 a = g_a.Load(int3(pa, 0));
		if (g_stats_mode.z != 0)
		{
			float2 mv = float2(Finite(a.x) ? a.x : 0.0, Finite(a.y) ? a.y : 0.0) * g_stats_scale.xy;
			float len = length(mv);
			acc.x += len;
			top = max(top, len);
		}
		else
		{
			int2 pb = int2(g_stats_b.xy) + min(int2(uv * float2(g_stats_b.zw)), int2(g_stats_b.zw) - 1);
			float3 ca = Usable(a.rgb), cb = Usable(g_b.Load(int3(pb, 0)).rgb);
			if (g_stats_mode.y != 0) { ca = SrgbToLinear(ca); cb = SrgbToLinear(cb); }
			float ya = dot(ca, kLuma), yb = dot(cb, kLuma);
			float d = log2((yb + 1e-4) / (ya + 1e-4));
			acc += float4(ya, yb, d, abs(d));
		}
	}
	s_acc[t] = acc;
	s_top[t] = top;
	GroupMemoryBarrierWithGroupSync();
	if (t != 0) return;
	float4 total = 0.0;
	float peak = 0.0;
	for (uint k = 0; k < 64; ++k) { total += s_acc[k]; peak = max(peak, s_top[k]); }
	float4 mean = total / 2304.0;
	uint row = g_stats_mode.x;
	o_stats[uint2(0, row)] = mean.x;
	o_stats[uint2(1, row)] = mean.y;
	o_stats[uint2(2, row)] = mean.z;
	o_stats[uint2(3, row)] = mean.w;
	o_stats[uint2(4, row)] = peak;
}
#endif
