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

float4 main(float2 uv : TEXCOORD0, float2 uvm : TEXCOORD1, float4 diffuse : COLOR0) : COLOR
{
#if HEX
	float4 t = HexTile(Layer, uv, CELL);
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
