// UT Bio Rifle (first-person gun): the goo window becomes see-through liquid; the rest of the
// gun is drawn as the game would (its texture times the vertex lighting). The whole gun shares
// one texture, so the goo is told apart by colour: the bright orange-yellow of the window.
// Rule: glass=<BioAtlas hash> bio_gun.hlsl
//
// noise(): value noise by Inigo Quilez, MIT License
//   Copyright (c) 2013 Inigo Quilez - https://www.shadertoy.com/view/lsf3WH

sampler2D Tex   : register(s0);
sampler2D Scene : register(s1);
float4    Info  : register(c0);   // time, normals valid, 1/width, 1/height
float4x4  Proj  : register(c4);

float hash(float2 p)
{
	p = 50.0 * frac(p * 0.3183099 + float2(0.71, 0.113));
	return frac(p.x * p.y * (p.x + p.y));
}

float noise(float2 p)
{
	float2 i = floor(p);
	float2 f = frac(p);
	float2 u = f * f * (3.0 - 2.0 * f);
	return lerp(lerp(hash(i), hash(i + float2(1, 0)), u.x),
	            lerp(hash(i + float2(0, 1)), hash(i + float2(1, 1)), u.x), u.y);
}

float4 main(float2 uv : TEXCOORD0, float3 normal : TEXCOORD1, float3 pos : TEXCOORD2, float4 diffuse : COLOR0) : COLOR
{
	float4 tex = tex2D(Tex, uv);
	float3 lit = tex.rgb * diffuse.rgb * 2.0;          // the stage's MODULATE2X

	// goo: the window's bright orange-yellow (red high, blue low)
	float sat = tex.r - tex.b;
	float goo = smoothstep(0.35, 0.55, sat) * smoothstep(0.45, 0.65, tex.r);
	if (goo <= 0.0)
		return float4(lit, tex.a);

	float t = Info.x;
	float3 n = normalize(normal);
	float3 v = normalize(-pos);
	float facing = saturate(dot(n, v));

	// slow sloshing: noise rising through the window
	float2 q = uv * 40.0;
	float w = noise(q + float2(0, -t * 1.2)) + 0.5 * noise(q * 2.1 + float2(t * 0.7, -t * 2.0));
	float bubbles = smoothstep(0.85, 1.05, noise(q * 3.0 + float2(0, -t * 3.0)));

	float4 clip = mul(Proj, float4(pos, 1.0));
	float2 screen = clip.xy / clip.w * float2(0.5, -0.5) + 0.5 + Info.zw * 0.5;
	float2 bend = n.xy * float2(1.0, -1.0) * 0.03 + (w - 0.75) * 0.02;
	float3 behind = tex2D(Scene, screen + bend).rgb;

	// the liquid keeps the window's own colour; what's behind shows through it, bent
	float3 own = tex.rgb;
	float3 liquid = behind * own * 0.9 + own * (0.35 + 0.35 * w);
	liquid += bubbles * (own * 0.5 + 0.5) * 0.6;
	liquid += (own * 0.5 + 0.5) * pow(1.0 - facing, 3.0) * 0.4;
	return float4(lerp(lit, liquid, goo), tex.a);
}
