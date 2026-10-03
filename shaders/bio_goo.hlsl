// Bio Rifle goo blobs (UT Bio Rifle port): see-through green liquid, Half-Life 2 style.
// The frame behind is looked up a little sideways along the blob's normal (refraction),
// tinted toxic green, with a wobble that crawls over the surface, a bright rim where the
// liquid turns away from the eye and a sharp highlight. Rule: glass=<BioGreen hash> bio_goo.hlsl
//
// noise(): value noise by Inigo Quilez, MIT License
//   Copyright (c) 2013 Inigo Quilez - https://www.shadertoy.com/view/lsf3WH

sampler2D Tex   : register(s0);   // the goo's own texture
sampler2D Scene : register(s1);   // the frame so far
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

float4 main(float2 uv : TEXCOORD0, float3 normal : TEXCOORD1, float3 pos : TEXCOORD2) : COLOR
{
	float t = Info.x;
	// the blob mesh is low-poly: bend the normal with noise so the facets read as a wobbling skin
	float2 nq = uv * 5.0 + float2(t * 0.5, -t * 0.3);
	float3 n = normalize(normal + 0.45 * float3(noise(nq) - 0.5, noise(nq + 7.3) - 0.5, 0.0));
	float3 v = normalize(-pos);
	float facing = saturate(dot(n, v));
	float rim = pow(1.0 - facing, 3.0);

	// the surface wobbles: two layers of noise drifting against each other
	float2 q = uv * 6.0;
	float w = noise(q + float2(t * 0.6, t * 0.4)) + 0.5 * noise(q * 2.3 - float2(t * 0.9, 0));
	float2 wobble = (w - 0.75) * 0.025;

	float4 clip = mul(Proj, float4(pos, 1.0));
	float2 screen = clip.xy / clip.w * float2(0.5, -0.5) + 0.5 + Info.zw * 0.5;
	float2 bend = n.xy * float2(1.0, -1.0) * 0.045 + wobble;
	float3 behind = tex2D(Scene, screen + bend).rgb;

	float3 goo   = tex2D(Tex, uv).rgb;                 // keeps the texture's own mottling
	float3 tint  = float3(0.35, 1.0, 0.25);
	float3 deep  = float3(0.05, 0.35, 0.02);
	float thick = 0.35 + 0.45 * facing;                // looking straight in: more liquid
	float3 colour = behind * tint * (1.0 - thick * 0.6) + lerp(deep, goo * tint, 0.6) * thick * 0.7;

	// rim light and a sharp highlight from up and to the left of the eye
	float3 l = normalize(float3(-0.4, 0.6, -0.7));
	float spec = pow(saturate(dot(reflect(-l, n), v)), 40.0);
	colour += float3(0.55, 1.0, 0.45) * rim * 0.7 + spec * 0.9;
	return float4(colour, 1.0);
}
