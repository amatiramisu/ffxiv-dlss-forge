// DLSS NR composition kernels, compiled ahead of time by build_shaders.sh with Microsoft's
// d3dcompiler_47 into nr_shaders.h. Wine's built-in compiler drops texture loads in the resolve.
//
// The composition: two-branch luminance ratio, OkLab hue correction, AP1 gamut compression, highlight
// guard, skin/environment split, compare and debug views, and the sRGB decode's ratio transfer.
cbuffer params : register(b0)
{
	uint4 g_rect;      // image base x, base y, width, height
	uint4 g_guides;    // depth base x, base y, motion base x, base y
	uint4 g_sizes;     // source depth w, depth h, work w, work h
	float4 g_strength; // detail, colour, guard, white point
	uint4 g_modes;     // passthrough, debug view, apply model, compare
	float4 g_compare;  // split, swap, regions, show mask
	float4 g_regions;  // skin detail, skin colour, environment detail, environment colour
	float4 g_mask_env;  // developer mask: environment structure, tone; show groups, show automask estimate
	float4 g_mask_near; // near structure, tone, on, log2 of the band's end
	float4 g_mask_far;  // far structure, tone, on, log2 of the band's start
	float4 g_mask_depth; // feather (log2), depth inverted, 0, 0
	uint4 g_motion_size; // source motion w, motion h, 0, 0
	float4 g_jitter; // raster jitter UV, correction from jittered to unjittered motion UV
	float4 g_motion_scale; // raw motion to UV scale x, y, 0, 0
	float4 g_codec;        // sRGB decode: brightness bound (stops), colour bound (stops), divisor rise, divisor fall (stops this frame)
	uint4 g_codec_state;   // sRGB decode: snap the divisor to this frame's measurement, 0, 0, 0
};

// Each kernel is compiled on its own with only its resources declared, numbered from 0. Wine's
// d3dcompiler_47 misreads registers of resources declared for other entry points in the same source.
#if defined(METER)
Texture2D<float4> g_image : register(t0);
RWTexture2D<float> o_state : register(u0);
#elif defined(PACK_COLOR)
Texture2D<float4> g_image : register(t0);
Texture2D<float> g_state : register(t1);
RWTexture2D<float4> o_keep : register(u0);
RWTexture2D<float4> o_proxy : register(u1);
#elif defined(PACK_GUIDES)
Texture2D<float> g_depth : register(t0);
Texture2D<float2> g_motion : register(t1);
RWTexture2D<float> o_depth : register(u0);
RWTexture2D<float2> o_motion : register(u1);
#elif defined(PACK_WORK)
Texture2D<float4> g_keep : register(t0);
Texture2D<float> g_state : register(t1);
RWTexture2D<float4> o_proxy : register(u0);
#elif defined(PACK_MASK)
Texture2D<float> g_depth : register(t0);
RWTexture2D<float4> o_mask : register(u0);
RWTexture2D<float> o_groups : register(u1);
#elif defined(TRANSFER)
SamplerState g_linear : register(s0);
Texture2D<float4> g_src : register(t0);
Texture2D<float4> g_model : register(t1);
Texture2D<float4> g_proxy : register(t2);
RWTexture2D<float4> o_work : register(u0);
#elif defined(RESOLVE)
SamplerState g_linear : register(s0);
Texture2D<float4> g_keep : register(t0);
Texture2D<float4> g_model : register(t1);
Texture2D<float4> g_proxy : register(t2);
Texture2D<float> g_groups : register(t3);
Texture2D<float4> g_probe : register(t4);
Texture2D<float4> g_src : register(t5);
RWTexture2D<float4> o_image : register(u0);
#endif

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

// g_modes.x: 0 linear HDR (IsHDR), 1 display-referred frame edited directly, 2 display-referred frame through the sRGB decode
bool passthrough() { return g_modes.x == 1; }
bool codec() { return g_modes.x == 2; }
// The model's input and output are shown as stored (debug views) in both display-referred modes
bool stored_domain() { return g_modes.x != 0; }
float white_point() { return max(g_strength.w, 1e-4); }

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

// isfinite() spelled out: Wine's d3dcompiler_47 has no such intrinsic, and an ordered compare is false for NaN
bool Finite(float v) { return abs(v) <= 3.402823e38; }
bool Finite3(float3 v) { return Finite(v.x) && Finite(v.y) && Finite(v.z); }
float SanitizeFinite(float v, float fallback) { return Finite(v) ? v : fallback; }
float3 SanitizeFinite3(float3 v, float3 fallback)
{
	return float3(SanitizeFinite(v.x, fallback.x), SanitizeFinite(v.y, fallback.y), SanitizeFinite(v.z, fallback.z));
}
float SafeDivide(float n, float d, float fallback) { return abs(d) > 1e-8 ? n / d : fallback; }

float3 LMSToBT709(float3 c)
{
	const float3x3 m = { 5.62059812, -4.57145756, 0.15577924,
	                     -1.15555585, 2.25800438, -0.15415806,
	                     0.03059913, -0.19018011, 1.06820532 };
	return mul(m, c);
}

float3 BT709ToLMS(float3 c)
{
	const float3x3 m = { 0.30569589, 0.62271286, 0.04528636,
	                     0.15776262, 0.76968599, 0.08807030,
	                     0.01933082, 0.11919478, 0.95053215 };
	return mul(m, c);
}

float3 D65NeutralBT709(float3 state, float luminance)
{
	float3 d65 = LMSToBT709(max(state, 1e-8));
	float d65Y = max(dot(d65, kLuma), 1e-8);
	return d65 * (luminance / d65Y);
}

float GamutCompressionScale(float3 c, float3 state)
{
	c = SanitizeFinite3(c, float3(0.0, 0.0, 0.0));
	const float y = dot(c, kLuma);
	if (!(y > 1e-8))
		return 1.0;
	const float3 n = D65NeutralBT709(state, y);
	float s = 1.0;
	if (c.r < 0.0 && n.r > c.r) s = min(s, SafeDivide(n.r, n.r - c.r, 1.0));
	if (c.g < 0.0 && n.g > c.g) s = min(s, SafeDivide(n.g, n.g - c.g, 1.0));
	if (c.b < 0.0 && n.b > c.b) s = min(s, SafeDivide(n.b, n.b - c.b, 1.0));
	return saturate(SanitizeFinite(s, 1.0));
}

// Pulls an out-of-gamut colour toward the neutral axis by one factor, so hue survives; exact identity otherwise
float3 ClampAp1(float3 c)
{
	const float3 state = BT709ToLMS(float3(0.18, 0.18, 0.18));
	const float s = GamutCompressionScale(c, state);
	if (s >= 1.0)
		return c;
	const float y = dot(c, kLuma);
	const float3 n = D65NeutralBT709(state, y);
	return SanitizeFinite3(n + (c - n) * s, max(n, 0.0));
}

float3 CbrtSigned(float3 v) { return sign(v) * pow(abs(v), 1.0 / 3.0); }

float3 ToOkLab(float3 c)
{
	const float3x3 rgb_to_lms = { 0.4122214708, 0.5363325363, 0.0514459929,
	                              0.2119034982, 0.6806995451, 0.1073969566,
	                              0.0883024619, 0.2817188376, 0.6299787005 };
	const float3x3 lms_to_lab = { 0.2104542553, 0.7936177850, -0.0040720468,
	                              1.9779984951, -2.4285922050, 0.4505937099,
	                              0.0259040371, 0.7827717662, -0.8086757660 };
	return mul(lms_to_lab, CbrtSigned(mul(rgb_to_lms, c)));
}

float3 FromOkLab(float3 lab)
{
	const float3x3 lab_to_lms = { 1.0, 0.3963377774, 0.2158037573,
	                              1.0, -0.1055613458, -0.0638541728,
	                              1.0, -0.0894841775, -1.2914855480 };
	const float3x3 lms_to_rgb = { 4.0767416621, -3.3077115913, 0.2309699292,
	                              -1.2684380046, 2.6097574011, -0.3413193965,
	                              -0.0041960863, -0.7034186147, 1.7076147010 };
	float3 lms = mul(lab_to_lms, lab);
	return mul(lms_to_rgb, lms * lms * lms);
}

// Hue and chroma direction from `correct`, chroma magnitude from `incorrect`
float3 HueOkLab(float3 incorrect, float3 correct)
{
	float3 a = ToOkLab(incorrect);
	const float3 b = ToOkLab(correct);
	const float ca = length(a.yz);
	const float cb = length(b.yz);
	const float2 dir = cb > 1e-5 ? b.yz / cb : float2(0.0, 0.0);
	a.yz = dir * ca;
	return ClampAp1(FromOkLab(a));
}

// Highlight roll-off for HDR input, reproduced exactly by the resolve
float3 SoftKnee(float3 d)
{
	if (passthrough())
		return d;
	float l = dot(d, kLuma);
	if (l > 0.75)
	{
		float rolled = 0.75 + 0.25 * (1.0 - exp(-(l - 0.75) / 0.25));
		d *= rolled / l;
	}
	float peak = max(d.r, max(d.g, d.b));
	if (peak > 1.0)
		d /= peak;
	return d;
}

float3 Encode(float3 frame)
{
	if (passthrough())
		return frame;
	return LinearToSrgb(SoftKnee(frame / white_point()));
}

// The sRGB decode for float frames without IsHDR: the frame is read as linear light and divided by the
// metered divisor; its peak channel gains a small toe (at most 0.014) and rolls off above 0.75, and the
// result is sRGB-encoded. FFXIV's frame is already gamma-encoded, so the model sees a brighter, flatter
// picture than the game shows.
float3 CodecEncode(float3 frame, float divisor)
{
	float3 d = frame / max(divisor, 1e-4);
	float peak = max(d.r, max(d.g, d.b));
	const float toed = peak + 0.014 * peak / (peak + 0.001);
	d *= toed / max(peak, 1e-6);
	peak = toed;
	if (peak > 0.75)
		d *= (0.75 + 0.25 * (1.0 - exp2(-(peak - 0.75) * 8.32547569))) / max(peak, 1e-6);
	return LinearToSrgb(min(d, 1.0));
}

// Ratio transfer of one pass: per-channel gains between the decoded model output and input, in stops. The
// brightness gain is their luma-weighted mean, bounded to g_codec.x stops; each channel's colour gain around
// it is bounded to g_codec.y stops and scaled by `colour`. The gain fades out where the model's input is near
// black and multiplies `src`, the image the pass enhances; `detail` scales the result.
float3 CodecTransfer(float3 src, float3 proxyEncoded, float3 modelEncoded, float detail, float colour)
{
	const float kFloor = 1.0 / 1024.0;
	const float3 P = SrgbToLinear(proxyEncoded);
	const float3 M = SrgbToLinear(modelEncoded);
	const float3 gains = log2(max(M, kFloor) / max(P, kFloor));
	const float lumaGain = dot(gains, kLuma);
	const float3 chroma = clamp(gains - lumaGain, -g_codec.y, g_codec.y) * colour;
	const float near = smoothstep(0.0, 1.0, saturate((max(P.r, max(P.g, P.b)) - kFloor) * 68.2666702));
	const float3 gain = exp2(near * (clamp(lumaGain, -g_codec.x, g_codec.x) + chroma));
	return src + detail * (src * gain - src);
}

#if defined(PACK_COLOR) || defined(PACK_WORK)
float3 EncodeInput(float3 frame)
{
	return codec() ? CodecEncode(frame, g_state.Load(int3(0, 0, 0))) : Encode(frame);
}
#endif

// Keeps the residual inside the unit cube while preserving its direction
float3 CubeScaleResidual(float3 P, float3 T)
{
	if (passthrough())
		return T;
	float3 d = T - P;
	float alpha = 1.0;
	[unroll] for (int c = 0; c < 3; ++c)
	{
		if (d[c] > 1e-6)
			alpha = min(alpha, (1.0 - P[c]) / d[c]);
		else if (d[c] < -1e-6)
			alpha = min(alpha, (0.0 - P[c]) / d[c]);
	}
	return P + saturate(alpha) * d;
}

// Colour test for skin tones: brightness plus two colour differences inside the range skin usually occupies.
// Wood, sand, brass and warm cloth pass it as well.
float SkinColourWeight(float3 rgb)
{
	rgb = saturate(rgb);
	float y = dot(rgb, float3(0.299, 0.587, 0.114));
	float cb = (rgb.b - y) * 0.564 + 0.5;
	float cr = (rgb.r - y) * 0.713 + 0.5;
	float2 dist = (float2(cb, cr) - float2(0.405, 0.600)) / float2(0.090, 0.110);
	float chroma = max(rgb.r, max(rgb.g, rgb.b)) - min(rgb.r, min(rgb.g, rgb.b));
	return (1.0 - smoothstep(0.55, 1.35, length(dist))) * smoothstep(0.02, 0.10, chroma);
}

// The frame as the model and the composition see it: finite and non-negative
float3 Usable(float3 v)
{
	return max(SanitizeFinite3(v, float3(0.0, 0.0, 0.0)), 0.0);
}


#if defined(METER)
// The codec's divisor, metered from a log2 histogram of the peak channel over a 64 x 36 grid.
// When enough of the frame sits above 0.75, the divisor places the median at 0.3; it never drops below 1.
// The applied divisor holds inside a dead band, rises and falls at limited rates and snaps on a reset.
// o_state: x 0 the applied divisor, x 1 whether it is moving.
groupshared uint s_hist[256];
groupshared uint s_total;
groupshared uint s_bright;

[numthreads(64, 1, 1)]
void meter(uint3 tid : SV_GroupThreadID)
{
	const uint t = tid.x;
	for (uint b = t; b < 256; b += 64)
		s_hist[b] = 0;
	if (t == 0)
	{
		s_total = 0;
		s_bright = 0;
	}
	GroupMemoryBarrierWithGroupSync();
	const uint W = max(g_rect.z, 1u), H = max(g_rect.w, 1u);
	for (uint i = t; i < 2304; i += 64)
	{
		const uint x = min(((2 * (i & 63) + 1) * W) >> 7, W - 1);
		const uint y = min(((2 * (i >> 6) + 1) * H) / 72, H - 1);
		const float3 c = g_image.Load(int3(int2(g_rect.xy + uint2(x, y)), 0)).rgb;
		const float m = max(c.r, max(c.g, c.b));
		if (Finite(m) && m > 0.0)
		{
			const float l = min(log2(max(m, 5.96046448e-08)), 24.0);
			const uint bin = min((uint) floor((l + 24.0) * 5.33333349), 255u);
			InterlockedAdd(s_hist[bin], 1u);
			InterlockedAdd(s_total, 1u);
			if (m > 0.75)
				InterlockedAdd(s_bright, 1u);
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (t != 0)
		return;

	float candidate = 1.0;
	if (s_total > 0)
	{
		const float bright = (float) s_bright / (float) s_total;
		const float weight = smoothstep(0.0, 1.0, saturate((log2(max(bright, 1e-6)) + 6.0) * 0.5));
		if (weight > 0.0)
		{
			const uint target = (s_total - 1) >> 1;
			uint cum = 0, bin = 0, before = 0;
			for (uint k = 0; k < 256; ++k)
			{
				const uint n = s_hist[k];
				if (cum + n > target)
				{
					bin = k;
					before = cum;
					break;
				}
				cum += n;
			}
			const float pos = (float) bin + ((float) (target - before) + 0.5) / (float) max(s_hist[bin], 1u);
			candidate = exp2(max(pos * 0.1875 - 22.2630348, 0.0) * weight);
		}
	}

	const float previous = o_state[uint2(0, 0)];
	float moving = o_state[uint2(1, 0)];
	float applied = candidate;
	if (g_codec_state.x == 0 && Finite(previous) && previous > 0.0)
	{
		const float step = log2(candidate / previous);
		if (abs(step) > (moving > 0.5 ? 0.02 : 0.15))
		{
			applied = previous * exp2(clamp(step, -g_codec.w, g_codec.z));
			moving = 1.0;
		}
		else
		{
			applied = previous;
			moving = 0.0;
		}
	}
	else
		moving = 0.0;
	o_state[uint2(0, 0)] = applied;
	o_state[uint2(1, 0)] = moving;
}
#endif

#if defined(PACK_COLOR)
// The DLSS output region: the game's exact values for the composition, and the model's input when it runs at full size
[numthreads(8, 8, 1)]
void pack_color(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= g_rect.z || id.y >= g_rect.w)
		return;
	float4 s = g_image.Load(int3(g_rect.xy + id.xy, 0));
	o_keep[id.xy] = s;
	o_proxy[id.xy] = float4(EncodeInput(Usable(s.rgb)), s.a);
}
#endif

#if defined(PACK_GUIDES)
// Color is already reconstructed by SR. Locate its corresponding raster samples using
// this frame's jitter; keep depth/motion as point samples to avoid blending across surfaces.
// Both prepared guides use the model grid and unjittered UV-space motion.
[numthreads(8, 8, 1)]
void pack_guides(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= g_sizes.z || id.y >= g_sizes.w)
		return;
	const float2 uv = (float2(id.xy) + 0.5) / float2(g_sizes.zw) + g_jitter.xy;
	const int2 depthAt = clamp(int2(floor(uv * float2(g_sizes.xy))), int2(0, 0), int2(g_sizes.xy) - 1);
	const int2 motionAt = clamp(int2(floor(uv * float2(g_motion_size.xy))), int2(0, 0), int2(g_motion_size.xy) - 1);
	const float depth = g_depth.Load(int3(int2(g_guides.xy) + depthAt, 0));
	const float2 motion = g_motion.Load(int3(int2(g_guides.zw) + motionAt, 0)) * g_motion_scale.xy + g_jitter.zw;
	o_depth[id.xy] = SanitizeFinite(depth, g_mask_depth.y != 0.0 ? 0.0 : 1.0);
	o_motion[id.xy] = Finite(motion.x) && Finite(motion.y) ? motion : float2(0.0, 0.0);
}
#endif

#if defined(PACK_WORK)
// The model's input at a working size other than the frame's: an exact area average of the frame
[numthreads(8, 8, 1)]
void pack_work(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= g_sizes.z || id.y >= g_sizes.w)
		return;
	const float sw = (float) g_rect.z, sh = (float) g_rect.w;
	const float x0 = ((float) id.x * sw) / (float) g_sizes.z, x1 = ((float) (id.x + 1) * sw) / (float) g_sizes.z;
	const float y0 = ((float) id.y * sh) / (float) g_sizes.w, y1 = ((float) (id.y + 1) * sh) / (float) g_sizes.w;
	const float area = (x1 - x0) * (y1 - y0);
	const int i0 = (int) floor(x0), i1 = (int) ceil(x1) - 1;
	const int j0 = (int) floor(y0), j1 = (int) ceil(y1) - 1;
	float3 acc = 0.0;
	for (int j = j0; j <= j1; ++j)
	{
		const int jj = clamp(j, 0, (int) g_rect.w - 1);
		const float wy = max(min(y1, (float) j + 1.0) - max(y0, (float) j), 0.0);
		for (int i = i0; i <= i1; ++i)
		{
			const int ii = clamp(i, 0, (int) g_rect.z - 1);
			const float wx = max(min(x1, (float) i + 1.0) - max(x0, (float) i), 0.0);
			acc += Usable(g_keep.Load(int3(ii, jj, 0)).rgb) * (wx * wy);
		}
	}
	o_proxy[id.xy] = float4(EncodeInput(acc / area), 1.0);
}
#endif

#if defined(PACK_MASK)
// The developer mask at the model's working size: per pixel the structure and tone values of the group that
// selects it. Groups layer from the environment up: far band, then near band, with the values interpolated
// across a band's soft edge. o_groups keeps which group won, for the overlay.
[numthreads(8, 8, 1)]
void pack_mask(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= g_sizes.z || id.y >= g_sizes.w)
		return;
	// The prepared depth already shares the model grid and its unjittered color footprint.
	const float d = SanitizeFinite(g_depth.Load(int3(id.xy, 0)), 0.0);
	// Distance in multiples of the near plane: a reversed-Z buffer stores near / z, the usual one 1 - near / z
	const float dist = g_mask_depth.y != 0.0 ? 1.0 / max(d, 1e-6) : 1.0 / max(1.0 - d, 1e-6);
	const float ld = log2(max(dist, 1.0));
	const float feather = max(g_mask_depth.x, 1e-3);
	const float wNear = g_mask_near.z != 0.0 ? 1.0 - smoothstep(g_mask_near.w - feather, g_mask_near.w + feather, ld) : 0.0;
	const float wFar = g_mask_far.z != 0.0 ? smoothstep(g_mask_far.w - feather, g_mask_far.w + feather, ld) : 0.0;

	float2 v = g_mask_env.xy;
	float group = 0.0, best = 1.0 - max(wNear, wFar);
	v = lerp(v, g_mask_far.xy, wFar);
	if (wFar > best) { best = wFar; group = 2.0; }
	v = lerp(v, g_mask_near.xy, wNear);
	if (wNear > best) { best = wNear; group = 1.0; }
	// The snippet reads weight, tone, structure from red, green, blue (local strength = weight * value)
	o_mask[id.xy] = float4(1.0, saturate(v.y), saturate(v.x), 1.0);
	o_groups[id.xy] = group * 0.25;
}
#endif


#if defined(TRANSFER)
// Per-pass chaining: one pass's transfer applied to the image it enhanced, kept for the next pass's input
[numthreads(8, 8, 1)]
void transfer(uint3 id : SV_DispatchThreadID)
{
	if (id.x >= g_rect.z || id.y >= g_rect.w)
		return;
	const float2 uv = (float2(id.xy) + 0.5) / float2(g_rect.zw);
	const float4 s = g_src.Load(int3(id.xy, 0));
	const float3 m = g_model.SampleLevel(g_linear, uv, 0).rgb;
	const float3 p = g_proxy.SampleLevel(g_linear, uv, 0).rgb;
	o_work[id.xy] = float4(Finite3(m) ? CodecTransfer(Usable(s.rgb), p, m, g_strength.x, g_strength.y) : s.rgb, s.a);
}
#endif

#if defined(RESOLVE)
// Overlay colour of a developer-mask group: 1 near (blue), 2 far (violet)
float3 GroupTint(float id)
{
	if (id > 1.5) return float3(0.65, 0.25, 1.0);
	if (id > 0.5) return float3(0.2, 0.55, 1.0);
	return float3(0.0, 0.0, 0.0);
}

[numthreads(8, 8, 1)]
void resolve(uint3 id : SV_DispatchThreadID)
{
	const uint W = g_rect.z, H = g_rect.w;
	if (id.x >= W || id.y >= H)
		return;
	const float2 uv = (float2(id.xy) + 0.5) / float2(W, H);
	const uint compare = g_modes.w;
	const bool swap = g_compare.y != 0.0;

	float2 cmpUv = uv;
	bool showOriginal = false, onDivider = false, outsideFrame = false;
	if (compare == 1)
	{
		showOriginal = (uv.x < 0.5) != swap;
		float2 half2 = float2(uv.x < 0.5 ? uv.x * 2.0 : (uv.x - 0.5) * 2.0, uv.y) - 0.5;
		cmpUv = float2(0.5 + half2.x, 0.5 + half2.y * 2.0);
		outsideFrame = cmpUv.x < 0.0 || cmpUv.x > 1.0 || cmpUv.y < 0.0 || cmpUv.y > 1.0;
		onDivider = abs(uv.x - 0.5) < (1.0 / max(W, 1u));
	}
	else if (compare == 2)
	{
		showOriginal = (uv.x < g_compare.x) != swap;
		onDivider = abs(uv.x - g_compare.x) < (1.0 / max(W, 1u));
	}

	const float4 proxySample = g_proxy.SampleLevel(g_linear, cmpUv, 0);
	const float4 modelSample = g_model.SampleLevel(g_linear, cmpUv, 0);
	// Load only: Wine's compiler returns zeros for a texture that one shader both loads and samples
	const int2 keepAt = compare == 1 ? clamp(int2(cmpUv * float2(W, H)), int2(0, 0), int2(W - 1, H - 1)) : int2(id.xy);
	const float4 originalSample = g_keep.Load(int3(keepAt, 0)); // the game's values, written back where nothing changes
	const float3 kept = Usable(originalSample.rgb);
	float3 proxy = stored_domain() ? proxySample.rgb : SrgbToLinear(proxySample.rgb);
	float3 model = stored_domain() ? modelSample.rgb : SrgbToLinear(modelSample.rgb);
	const bool modelFinite = Finite3(modelSample.rgb) && all(abs(modelSample.rgb) < 65504.0);

	const float normScale = stored_domain() ? 1.0 : white_point();
	const float3 original = kept / normScale;
	const float originalLuma = dot(original, kLuma);
	float proxyLuma = dot(proxy, kLuma);

	if (g_modes.z == 0 || !modelFinite)
	{
		o_image[g_rect.xy + id.xy] = originalSample;
		return;
	}
	if (g_modes.y == 1)
	{
		o_image[g_rect.xy + id.xy] = float4(proxy * normScale, originalSample.a);
		return;
	}
	if (g_modes.y == 2)
	{
		o_image[g_rect.xy + id.xy] = float4(model * normScale, originalSample.a);
		return;
	}

	const float3 edit = model - proxy;
	if (g_modes.y == 3)
	{
		const float3 shown = saturate(0.5 + edit * 20.0);
		o_image[g_rect.xy + id.xy] = float4(stored_domain() ? shown : SrgbToLinear(shown) * normScale, originalSample.a);
		return;
	}

	const float detail = g_strength.x;
	const float colour = g_strength.y;
	float3 result;
	if (codec())
	{
		// The last pass's transfer onto the image it enhanced: the game's frame, or with per-pass chaining
		// the result of the passes before it
		const float3 src = Usable(g_src.Load(int3(keepAt, 0)).rgb);
		result = CodecTransfer(src, proxySample.rgb, modelSample.rgb, detail, colour);
	}
	else
	{
	// A model that worked below the frame's size brings back only its change (matched residual)
	if (g_sizes.z != W || g_sizes.w != H)
	{
		const float3 fullProxy = passthrough() ? saturate(original) : saturate(SoftKnee(original));
		proxy = fullProxy;
		proxyLuma = dot(proxy, kLuma);
		model = CubeScaleResidual(fullProxy, fullProxy + edit);
	}

	const float modelLuma = dot(model, kLuma);
	float3 upgraded;
	if (passthrough())
	{
		// SDR: transfer the finite model edit, including valid black output. Using the
		// exact input as reference also preserves identity at reduced working sizes.
		upgraded = lerp(original, original + (model - proxy), saturate(detail));
	}
	else if (modelLuma <= 1e-5)
	{
		upgraded = original;
	}
	else
	{
		float ratio;
		if (originalLuma < proxyLuma)
			ratio = originalLuma / max(proxyLuma, 1e-6);
		else
			ratio = (modelLuma + max(0.0, originalLuma - proxyLuma)) / modelLuma;
		upgraded = lerp(original, HueOkLab(model * ratio, model), saturate(detail));
	}

	const float upgradedLuma = dot(upgraded, kLuma);
	const float kRatioFloor = 1.0 / 512.0;
	const float lumaRatio = (upgradedLuma + kRatioFloor) / (originalLuma + kRatioFloor);
	const float amplified = pow(max(lumaRatio, 1e-6), 1.0 + max(detail - 1.0, 0.0));
	const float guard = max(g_strength.z, 1.0);
	const float boundedRatio = clamp(amplified, 1.0 / guard, guard);
	upgraded *= boundedRatio / max(lumaRatio, 1e-6);

	result = lerp(original * boundedRatio, upgraded, min(colour, 1.0));
	if (colour > 1.0)
		result = ClampAp1(FromOkLab(float3(1.0, colour, colour) * ToOkLab(max(result, 0.0))));
	result *= normScale;
	}

	if (g_compare.z != 0.0)
	{
		const float3 displayRgb = passthrough() ? original : LinearToSrgb(saturate(original));
		const float mask = SkinColourWeight(displayRgb);
		const float d = lerp(g_regions.z, g_regions.x, mask);
		const float c = lerp(g_regions.w, g_regions.y, mask);
		const float baseY = dot(kept, kLuma);
		const float editedY = dot(max(result, 0.0), kLuma);
		const float wantedY = lerp(baseY, editedY, d);
		const float3 baseChroma = kept / max(baseY, 1e-6);
		const float3 editedChroma = result / max(editedY, 1e-6);
		if (d == 0.0 && c == 0.0)
			result = kept;
		else if (d != 1.0 || c != 1.0)
			result = ClampAp1(lerp(baseChroma, editedChroma, c) * wantedY);
		if (g_compare.w != 0.0)
			result = mask.xxx * normScale;
	}

	// The composition's change, applied to the game's own values: where it changes nothing, the frame
	// keeps exactly what the game wrote, negative and non-finite values included
	float3 shown = originalSample.rgb + (max(result, 0.0) - kept);
	// Overlays: the developer mask's groups, and the estimate of the model's own character mask (where the
	// second pass, run with the other base character value, differs from the first)
	if (g_mask_env.z != 0.0)
	{
		const float group = g_groups.SampleLevel(g_linear, cmpUv, 0) * 4.0;
		const float3 tint = GroupTint(group);
		if (any(tint > 0.0))
			shown = lerp(shown, tint * normScale, 0.45);
	}
	if (g_mask_env.w != 0.0)
	{
		const float3 other = g_probe.SampleLevel(g_linear, cmpUv, 0).rgb;
		const float estimate = saturate(dot(abs(other - modelSample.rgb), kLuma) * 12.0);
		shown = lerp(shown, float3(1.0, 0.1, 0.1) * normScale, estimate * 0.7);
	}
	if (showOriginal)
		shown = originalSample.rgb;
	if (outsideFrame)
		shown = float3(0.0, 0.0, 0.0);
	if (onDivider)
		shown = float3(normScale, normScale, normScale);
	o_image[g_rect.xy + id.xy] = float4(shown, originalSample.a);
}
#endif
