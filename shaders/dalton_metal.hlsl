// Dalton's armour as separate materials: polished plates, brushed gunmetal and rough cloth.
// The material comes from the texture's alpha, which the replace= rules swap for a baked
// material map (U2GraphicsPack/dalton/material_map.py; the game's own alpha is a noisy gloss mask):
//   1.0 polished plate -> chrome with gold rims (UT2004 champion look; ALL_GOLD: gold plates)
//   0.66 gunmetal      -> dark brushed steel
//   0.33 cloth         -> the game's diffuse, flatter, with a faint weave
//   0.0 skin           -> the game's own look
// Rules: replace=9bc7ce56 PlayerTorso_Material.dds, replace=272668a7 PlayerLimbs_Material.dds,
//        glass=9bc7ce56 dalton_metal.hlsl, glass=272668a7 dalton_metal.hlsl
//
// noise(): value noise by Inigo Quilez, MIT License
//   Copyright (c) 2013 Inigo Quilez - https://www.shadertoy.com/view/lsf3WH

sampler2D Tex   : register(s0);
sampler2D Scene : register(s1);
float4    Info  : register(c0);   // time, normals valid, 1/width, 1/height
float4x4  Proj  : register(c4);

static const float Texel = 1.0 / 512.0;

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

// a studio-style chrome environment in camera space (z into the screen): bright sky above, a
// big soft light behind the camera (plates facing the viewer catch it), a dark horizon band to
// the sides, dark ground below; plus a little of the frame itself for local colour
float3 chromeEnv(float3 r, float2 screen, float3 n)
{
	float y = r.y;
	float3 sky = lerp(float3(0.50, 0.56, 0.66), float3(1.10, 1.12, 1.16), saturate(y * 1.6));
	float3 ground = lerp(float3(0.18, 0.16, 0.14), float3(0.05, 0.05, 0.05), saturate(-y * 2.5));
	float3 env = y > 0.0 ? sky : ground;
	env = lerp(float3(0.03, 0.03, 0.04), env, smoothstep(0.0, 0.10, abs(y)));   // horizon line
	float front = smoothstep(0.55, 0.95, -r.z) * smoothstep(-0.45, -0.1, y);   // softbox behind the camera
	env = lerp(env, float3(1.15, 1.15, 1.18), front * 0.85);
	float3 local = tex2D(Scene, screen + n.xy * float2(0.06, -0.06)).rgb;
	return lerp(env, local, 0.15);
}

float plate(float2 uv)
{
	return smoothstep(0.8, 0.95, tex2D(Tex, uv).a);
}

float4 main(float2 uv : TEXCOORD0, float3 normal : TEXCOORD1, float3 pos : TEXCOORD2, float4 diffuse : COLOR0) : COLOR
{
	float4 tex = tex2D(Tex, uv);
	float3 light = diffuse.rgb * 2.0;                 // the stage's MODULATE2X
	float3 lit = tex.rgb * light;

	float a = tex.a;
	float isPlate = smoothstep(0.8, 0.95, a);
	float isGun = smoothstep(0.45, 0.6, a) * (1.0 - isPlate);
	float isCloth = smoothstep(0.15, 0.25, a) * (1.0 - isPlate - isGun);
	if (isPlate + isGun <= 0.0)
	{
		// cloth: flatter and a little duller, a faint weave so it reads as fabric next to the metal
		float weave = 0.93 + 0.07 * noise(uv * 900.0);
		float grey = dot(lit, float3(0.3, 0.5, 0.2));
		float3 cloth = lerp(lit, grey.xxx, 0.15) * weave * 0.95;
		return float4(lerp(lit, cloth, isCloth), 1.0);
	}

	// rims: plate texels within ~6 texels of the plate's edge
	float edge = 1.0;
	const float R = 6.0 * Texel;
	edge = min(edge, plate(uv + float2( R, 0)));
	edge = min(edge, plate(uv + float2(-R, 0)));
	edge = min(edge, plate(uv + float2(0,  R)));
	edge = min(edge, plate(uv + float2(0, -R)));
	edge = min(edge, plate(uv + float2( R,  R) * 0.7));
	edge = min(edge, plate(uv + float2(-R,  R) * 0.7));
	edge = min(edge, plate(uv + float2( R, -R) * 0.7));
	edge = min(edge, plate(uv + float2(-R, -R) * 0.7));
	float rim = saturate(isPlate - edge);

	float3 n = normalize(normal);
	float3 v = normalize(-pos);
	n = dot(n, v) < 0 ? -n : n;
	float facing = saturate(dot(n, v));
	float3 r = reflect(-v, n);

	float4 clip = mul(Proj, float4(pos, 1.0));
	float2 screen = clip.xy / clip.w * float2(0.5, -0.5) + 0.5 + Info.zw * 0.5;
	float3 env = chromeEnv(r, screen, n);

	// the texture's painted detail (scratches, panel lines, bevels) kept as brightness
	float detail = dot(tex.rgb, float3(0.3, 0.5, 0.2));
	detail = 0.6 + 1.6 * detail;

	// the scene's light still matters: a dark corner shouldn't glow
	float lum = saturate(dot(light, float3(0.3, 0.5, 0.2)));
	float shade = 0.35 + 0.75 * lum;

	float3 chrome = env * float3(0.95, 0.98, 1.05) * detail * 1.1;
	float3 gold = (env * 0.8 + 0.2) * float3(1.0, 0.72, 0.26) * detail * 1.2;
#ifdef ALL_GOLD
	float3 plateCol = lerp(gold, gold * 0.6 + chrome * 0.25, rim);   // gold plates, darker burnished rims
#else
	float3 plateCol = lerp(chrome, gold, rim);                        // chrome plates, gold rims
#endif
	float3 gunCol = (env * 0.35 + 0.05) * float3(0.85, 0.88, 0.95) * detail;

	// highlights: a key light up and to the left behind the camera (camera space, z into the
	// screen) and a fill from the right; tight on the plates, broad on the brushed steel
	float3 key = normalize(float3(-0.4, 0.7, -0.6));
	float3 fill = normalize(float3(0.6, 0.3, -0.7));
	float specPlate = pow(saturate(dot(r, key)), 30.0) + 0.35 * pow(saturate(dot(r, fill)), 12.0);
	float specGun = 0.5 * pow(saturate(dot(r, key)), 8.0);
	float fres = 0.7 + 0.3 * pow(1.0 - facing, 3.0);
	plateCol = plateCol * shade * fres + specPlate * lum * lerp(float3(1, 1, 1), float3(1.0, 0.85, 0.5), rim) * 1.8;
	gunCol = gunCol * shade + specGun * lum * float3(0.8, 0.85, 0.9);

	float3 col = lit;
	col = lerp(col, gunCol, isGun);
	col = lerp(col, plateCol, isPlate);
	return float4(col, 1.0);
}
