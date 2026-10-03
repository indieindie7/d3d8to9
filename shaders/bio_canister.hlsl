// UT Bio Rifle canister (first-person gun): the window the game draws as a plain environment-
// mapped surface (a cube map alone on stage 0, no texture of its own) becomes a glass tube of
// sloshing green goo: the frame behind bent through it, bubbles rising, glitter suspended in it,
// and the glass's own reflection of the surroundings on top.
// Rule: glass=c0be0001 bio_canister.hlsl (c0be0001 = "a cube map alone"; in Unreal II that is
// this canister; check stagelog=1 before reusing it in another game)
//
// noise(): value noise by Inigo Quilez, MIT License
//   Copyright (c) 2013 Inigo Quilez - https://www.shadertoy.com/view/lsf3WH

samplerCUBE Env  : register(s0);   // the game's environment cube map
sampler2D   Scene : register(s1);   // the frame so far
float4      Info  : register(c0);   // time, normals valid, 1/width, 1/height
float4x4    Proj  : register(c4);

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

float4 main(float3 normal : TEXCOORD1, float3 pos : TEXCOORD2) : COLOR
{
	float t = Info.x;
	float3 n = normalize(normal);
	float3 v = normalize(-pos);
	n = dot(n, v) < 0 ? -n : n;
	float facing = saturate(dot(n, v));

	// the gun moves with the camera, so camera-space position is a stable map on the tube
	float2 q = pos.xy * 0.9;
	float slosh = noise(q * 1.5 + float2(t * 0.4, -t * 0.9)) + 0.5 * noise(q * 3.1 + float2(-t * 0.7, -t * 1.6));
	float bubbles = smoothstep(0.80, 1.0, noise(q * 6.0 + float2(0, -t * 2.5)));

	float4 clip = mul(Proj, float4(pos, 1.0));
	float2 screen = clip.xy / clip.w * float2(0.5, -0.5) + 0.5 + Info.zw * 0.5;
	float2 bend = n.xy * float2(1.0, -1.0) * 0.04 + (slosh - 0.75) * 0.02;
	float3 behind = tex2D(Scene, screen + bend).rgb;

	float3 goo = float3(0.25, 0.95, 0.15);
	float3 colour = behind * float3(0.35, 0.95, 0.30) * 0.6 + goo * (0.22 + 0.25 * slosh);
	colour += bubbles * float3(0.7, 1.0, 0.6) * 0.45;

	// glitter, as in the blobs
	float2 g = q * 18.0 + float2(t * 0.2, -t * 0.5);
	float2 cell = floor(g);
	float pick = hash(cell);
	float2 at = float2(hash(cell + 3.1), hash(cell + 7.7)) - 0.5;
	float flake = smoothstep(0.12, 0.0, length(frac(g) - 0.5 - at * 0.6)) * step(0.6, pick);
	float twinkle = pow(saturate(sin(t * 3.0 + pick * 40.0)), 8.0);
	colour += flake * twinkle * float3(0.85, 1.0, 0.8) * 1.2;

	// the glass: the surroundings reflected, strongest at grazing angles (Fresnel)
	float3 refl = texCUBE(Env, reflect(-v, n)).rgb;
	float fres = 0.08 + 0.6 * pow(1.0 - facing, 4.0);
	colour = lerp(colour, refl, fres);
	return float4(colour, 1.0);
}
