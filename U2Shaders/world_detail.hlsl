// World-space detail on flat-palette meshes (Avalon Q81, 2026-10-09): the generated buildings take their
// colour from a palette strip (each face samples one swatch), so they read as flat plastic. This lays a
// low-res CC0 detail texture (ambientCG, grey, mean 0.5: tools/make_detail_dds.py) over them in WORLD
// space, triplanar (no mesh UVs needed), hex-tiled against visible repeats, and multiplies it into the
// palette colour: every building keeps its colour and gains surface.
//
// Use with surface=<palette hash> world_detail.hlsl detail_concrete034.dds
// The fork binds the detail on s3 and passes camera -> world in c4..c7 (see SurfaceBegin). The shader
// redoes the texture stages as world_parallax.hlsl does: palette x vertex colour x stage 1 (c2).
// Needs ddx/ddy: ps_2_a.

sampler2D Tex    : register(s0);   // the palette strip
sampler2D Second : register(s1);   // stage 1's texture, when the draw has one
sampler2D Detail : register(s3);   // the grey detail, mean 0.5
float4    Info    : register(c0);  // time, 1, 1/width, 1/height
float4    Combine : register(c2);  // x: vertex colour factor, y: stage 1 factor (0 = not used)
float4    W0 : register(c4);       // camera -> world, row vectors: world = p.x W0 + p.y W1 + p.z W2 + W3
float4    W1 : register(c5);
float4    W2 : register(c6);
float4    W3 : register(c7);

#define REPEAT 192.0               // world units per detail repeat (~4 m: a shack face shows 1-2 repeats)
#define STRENGTH 1.0               // 0 = the stock look, 1 = the full detail contrast
#define MACRO 0.125                // a second, 8x larger read (~30 m) breaks up repeats from far away
#define MACRO_AMT 0.35
#define DEBUG 0                    // 1: draw only the detail in magenta (which surfaces take the rule)
#define LIFT 0.10                  // part of the detail ADDED (grime and wear show on dark paint too)
#define NEAR_FADE 300.0            // closer than this the detail softens (a 256 px texture blurs up close)

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

// one hex-tiled grey read (Mikkelsen 2022, as terrain_hex.hlsl), gradients from the caller
float HexGrey(float2 uv, float2 dx, float2 dy)
{
	float3 w;
	float2 v1, v2, v3;
	HexGrid(uv, w, v1, v2, v3);
	float a = tex2Dgrad(Detail, uv + HexHash(v1), dx, dy).r;
	float b = tex2Dgrad(Detail, uv + HexHash(v2), dx, dy).r;
	float c = tex2Dgrad(Detail, uv + HexHash(v3), dx, dy).r;
	float3 k = w * w * w * w * w * w * (0.4 + float3(a, b, c));
	k /= max(k.x + k.y + k.z, 0.0001);
	return a * k.x + b * k.y + c * k.z;
}

float Triplanar(float3 wp, float3 n, float scale)
{
	float3 p = wp / scale;
	float3 dpx = ddx(p), dpy = ddy(p);
	float3 bw = pow(abs(n), 4);
	bw /= bw.x + bw.y + bw.z;
	float d = 0;
	d += bw.x * HexGrey(p.yz, dpx.yz, dpy.yz);
	d += bw.y * HexGrey(p.xz, dpx.xz, dpy.xz);
	d += bw.z * HexGrey(p.xy, dpx.xy, dpy.xy);
	return d;
}

// the macro read: plain triplanar (no hex), far too large to show a repeat
float TriplanarPlain(float3 wp, float3 n, float scale)
{
	float3 p = wp / scale;
	float3 bw = pow(abs(n), 4);
	bw /= bw.x + bw.y + bw.z;
	return bw.x * tex2D(Detail, p.yz).r + bw.y * tex2D(Detail, p.xz).r + bw.z * tex2D(Detail, p.xy).r;
}

float4 main(float2 uv : TEXCOORD0, float2 t1 : TEXCOORD1, float3 pos : TEXCOORD2,
            float4 diffuse : COLOR0) : COLOR
{
	float3 wp = pos.x * W0.xyz + pos.y * W1.xyz + pos.z * W2.xyz + W3.xyz;
	float3 n = normalize(cross(ddx(wp), ddy(wp)));

	float d = Triplanar(wp, n, REPEAT);
	float m = TriplanarPlain(wp + 517.0, n, REPEAT / MACRO);
	float near = saturate(length(pos) / NEAR_FADE);
	float k = STRENGTH * (0.6 + 0.4 * near);
	float detail = lerp(1, 2 * d, k) * lerp(1, 2 * m, MACRO_AMT);

	if (DEBUG == 1) return float4(detail, 0, detail, 1);
	if (DEBUG == 2) return float4(frac(wp / 256.0), 1);
	if (DEBUG == 3) return float4(d, d, d, 1);
	float4 t = tex2D(Tex, uv);
	float3 c = t.rgb * detail + (detail - 1) * LIFT;
	float a = t.a;
	if (Combine.x > 0)
	{
		c = saturate(c * diffuse.rgb * Combine.x);
		a *= diffuse.a;
	}
	if (Combine.y > 0)
		c = saturate(c * tex2D(Second, t1).rgb * Combine.y);
	return float4(c, a);
}
