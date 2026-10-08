// Unreal II terrain layers without the visible tile grid: hex-tiling of the layer texture.
//
// Use with layer=<hash of the layer texture> terrain_hex.hlsl (one rule per terrain layer texture).
// The rule binds only this shader: Unreal II's own fixed-function setup stays, so TEXCOORD0 arrives
// with the layer's tiling already applied (texture matrix), TEXCOORD1 with the alpha map's, the
// samplers are the game's, and the pass keeps its alpha blending over the layers below.
//
// The fork reads the draw's stage setup and passes it in c1 (U2Shaders.log: "layer <hash>: st0 ...");
// this shader redoes those stages with the stage-0 read replaced by HexTile. The terrain's 2-stage
// passes (layer x lighting, then x alpha map) and its single-stage far passes both work.
//
// HexHash / HexGrid / HexTile: from AdventMod/U2Shaders/terrain_src.hlsl (Mikkelsen 2022,
// "Practical Real-Time Hex-Tiling"), copied unchanged. Offsets depend only on the hex cell in uv
// space, so every layer that shares a tiling shifts the same way and blends without seams.

sampler2D Layer : register(s0);   // the layer texture
sampler2D Mask  : register(s1);   // the layer's alpha map (when stage 1 is used)
float4    Info  : register(c0);   // time, 1, 1/width, 1/height
float4    Setup : register(c1);   // the game's stage setup, read by the fork per draw (see LayerBegin):
                                  // x stage 0 colour 0 tex, 1 tex x diffuse, 2 tex x diffuse x 2
                                  // y stage 1 colour 0 unchanged, 1 x mask, 2 x mask x 2
                                  // z alpha 0 stage 0's, 1 x mask alpha, 2 mask alpha only;  w stage 1 used
float4    Setup0 : register(c2);  // x stage 0 alpha: 0 texture, 1 texture x diffuse, 2 diffuse, 3 texture x diffuse x 2

#define HEX 1                      // 0: plain read (checks the replication against the stock look)
#define CELL 1.5                   // texture repeats per hex
// far repeats (seen from 50-200 m): the same texture read again ~9x larger, hex-tiled too, modulates the
// near one's brightness against the texture's mean (its smallest mip), plus a slow value-noise
// brightness drift, so big slopes stop showing the same blotch pattern every few metres
#define MACRO 0.11                 // the macro read's scale (uv x MACRO)
#define MACRO_AMT 0.55             // how much the macro read modulates (0 off)
#define DRIFT_SCALE 0.035          // value-noise scale in uv
#define DRIFT_AMT 0.16             // +- brightness drift

float2 HexHash(float2 p)
{
	return frac(sin(float2(dot(p, float2(127.1, 311.7)), dot(p, float2(269.5, 183.3)))) * 43758.5453);
}

void HexGrid(float2 st, out float3 w, out float2 v1, out float2 v2, out float2 v3)
{
	st *= 3.4641016;
	float2 skew = float2(st.x - 0.57735027 * st.y, 1.15470054 * st.y);
	float2 b = floor(skew);
	float3 t = float3(frac(skew), 0);
	t.z = 1 - t.x - t.y;
	float s = step(0, -t.z), s2 = 2 * s - 1;
	w = float3(-t.z * s2, s - t.y * s2, s - t.x * s2);
	v1 = b + float2(s, s);
	v2 = b + float2(s, 1 - s);
	v3 = b + float2(1 - s, s);
}

float4 HexTile(sampler2D s, float2 uv, float cell)
{
	float2 dx = ddx(uv), dy = ddy(uv);
	float3 w;
	float2 v1, v2, v3;
	HexGrid(uv / cell, w, v1, v2, v3);
	float4 a = tex2Dgrad(s, uv + HexHash(v1), dx, dy);
	float4 b = tex2Dgrad(s, uv + HexHash(v2), dx, dy);
	float4 c = tex2Dgrad(s, uv + HexHash(v3), dx, dy);
	float3 lum = float3(dot(a.rgb, float3(0.3, 0.59, 0.11)), dot(b.rgb, float3(0.3, 0.59, 0.11)), dot(c.rgb, float3(0.3, 0.59, 0.11)));
	float3 k = w * w * w * w * w * w * (0.4 + lum);
	k /= max(k.x + k.y + k.z, 0.0001);
	return a * k.x + b * k.y + c * k.z;
}

float VNoise(float2 p)
{
	float2 i = floor(p), f = frac(p);
	f = f * f * (3 - 2 * f);
	float a = HexHash(i).x, b = HexHash(i + float2(1, 0)).x, c = HexHash(i + float2(0, 1)).x, d = HexHash(i + float2(1, 1)).x;
	return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float4 main(float2 uv : TEXCOORD0, float2 uvm : TEXCOORD1, float4 diffuse : COLOR0) : COLOR
{
#if HEX
	// up close the texture is magnified (a texel covers several pixels) and no repeat can show: the
	// game's own read there (hex blending of magnified texels only blurs and grains; Q30, 2026-10-08).
	// Hex tiling fades in as texels shrink below ~1 per pixel.
	float fp = max(length(ddx(uv)), length(ddy(uv))) / max(Info.z, 0.00001);   // texels per pixel
	float hexw = saturate((fp - 0.35) / 0.65);
	float4 t = tex2D(Layer, uv);
	if (hexw > 0.001)
		t = lerp(t, HexTile(Layer, uv, CELL), hexw);
	if (MACRO_AMT > 0 && hexw > 0.001)
	{
		const float3 L = float3(0.3, 0.59, 0.11);
		float mean = max(dot(tex2Dbias(Layer, float4(uv, 0, 12)).rgb, L), 0.02);
		float far = dot(HexTile(Layer, uv * MACRO + 0.37, CELL).rgb, L);
		t.rgb *= lerp(1, clamp(far / mean, 0.55, 1.6), MACRO_AMT * hexw);
		t.rgb *= 1 + DRIFT_AMT * hexw * (2 * VNoise(uv * DRIFT_SCALE) - 1);
	}
#else
	float4 t = tex2D(Layer, uv);
#endif
	float3 c = Setup.x > 0.5 ? t.rgb * diffuse.rgb * Setup.x : t.rgb;
	float a = Setup0.x < 0.5 ? t.a : (Setup0.x < 1.5 ? t.a * diffuse.a : (Setup0.x < 2.5 ? diffuse.a : saturate(t.a * diffuse.a * 2)));
	if (Setup.w > 0.5)
	{
		float4 m = tex2D(Mask, uvm);
		if (Setup.y > 0.5)
			c = saturate(c) * m.rgb * Setup.y;
		a = Setup.z > 1.5 ? m.a : (Setup.z > 0.5 ? a * m.a : a);
	}
	return float4(saturate(c), a);
}
