// gi.hlsl - screen-space global illumination by radiance cascades (gi=1), compiled by the
// wrapper as ps_3_0/vs_3_0 with GI_PASS set:
//   1 GBufPS     depth -> view depth + normal, half size
//   2 CascadePS  one cascade: for each probe and direction, what the screen shows along
//                that direction over the cascade's stretch of pixels, merged with the next
//                (farther, finer in angle) cascade
//   3 ResolvePS  the nearest cascade gathered per pixel: bounce light added, corners darkened
//
// Radiance cascades (Sannikov, Path of Exile 2): light from near needs many probes but few
// directions, light from far few probes but many directions. Cascade c has probes every
// 2^(c+1) of its texels and 4^(c+1) directions, and covers a stretch 4x as long as the
// one before. Each cascade is one texture (the frame's size over gires): k x k tiles
// (k = 2^(c+1)), one per direction, each a small picture of the screen's probes, so the
// probes of a direction interpolate with plain bilinear filtering.
//
// What one direction stores (here: a horizon version, as there is only the screen to trace):
// rgb = light arriving from it, summed over bands of elevation over the surface, each band
// lit by the nearest thing that rises into it; a = the highest elevation reached (its sine).
// Merging adds the farther cascade's light for the part of the sky still open above that.

float4 Px : register(c0);       // the target's 1/w, 1/h, w, h
float4 Proj : register(c1);     // the scene projection's _11, _22, _33, _43
float4 Casc : register(c2);     // tiles per axis k, stretch start (pixels), stretch length, 1 = merge the next cascade
                                // (the resolve: 0, 0, the cascade texture's w, h)
float4 Scr : register(c3);      // the frame's 1/w, 1/h, w, h
float4 Fx : register(c4);       // bounce strength, corner darkening, reach (world units), debug view

sampler S0 : register(s0);
sampler S1 : register(s1);
sampler S2 : register(s2);

void GiVS(float4 p : POSITION, float2 uv : TEXCOORD0, out float4 op : POSITION, out float2 ouv : TEXCOORD0)
{
	op = float4(p.x - Px.x, p.y + Px.y, p.z, 1);
	ouv = uv;
}

float3 ViewPos(float2 uv, float z)
{
	return float3((2 * uv.x - 1) * z / Proj.x, (1 - 2 * uv.y) * z / Proj.y, z);
}

#if GI_PASS == 1
// S0 = the scene's depth buffer
float ViewZ(float2 uv)
{
	float d = tex2Dlod(S0, float4(uv, 0, 0)).r;
	return d >= 0.999999 ? 0 : Proj.w / (d - Proj.z);
}

float4 GBufPS(float2 uv : TEXCOORD0) : COLOR
{
	float z = ViewZ(uv);
	if (z <= 0)
		return float4(0, 0, 0, 0);                  // sky
	float3 p = ViewPos(uv, z);
	// the normal from the nearer neighbour on each axis (so edges don't bend it)
	float2 dx = float2(Scr.x, 0), dy = float2(0, Scr.y);
	float zl = ViewZ(uv - dx), zr = ViewZ(uv + dx), zu = ViewZ(uv - dy), zd = ViewZ(uv + dy);
	// both neighbours where the surface carries on (steadier: far depth comes in coarse steps)
	float3 pl = ViewPos(uv - dx, zl), pr = ViewPos(uv + dx, zr), pu = ViewPos(uv - dy, zu), pd = ViewPos(uv + dy, zd);
	float el = abs(zl - z), er = abs(zr - z), eu = abs(zu - z), ed = abs(zd - z), same = 0.01 * z;
	float3 ax = (zl > 0 && zr > 0 && el < same && er < same) ? (pr - pl) * 0.5 : (el < er ? p - pl : pr - p);
	float3 ay = (zu > 0 && zd > 0 && eu < same && ed < same) ? (pd - pu) * 0.5 : (eu < ed ? p - pu : pd - p);
	float3 n = normalize(cross(ay, ax));
	if (n.z > 0)
		n = -n;                                     // towards the camera
	return float4(z, n.x, n.y, 1);
}
#endif

float3 Normal(float4 g)
{
	return float3(g.y, g.z, -sqrt(saturate(1 - g.y * g.y - g.z * g.z)));
}

#if GI_PASS == 2
// S0 = depth + normal, S1 = the frame, S2 = the next cascade
float4 CascadePS(float2 uv : TEXCOORD0) : COLOR
{
	float k = Casc.x, n = k * k;
	float2 tile = floor(uv * k);
	float dirIndex = tile.y * k + tile.x;
	// the probe's place on the screen, kept half a texel inside its tile
	float2 tileSize = Px.zw / k;
	float2 puv = clamp(frac(uv * k), 0.5 / tileSize, 1 - 0.5 / tileSize);
	float4 g = tex2Dlod(S0, float4(puv, 0, 0));
	if (g.a < 0.5)
		return float4(0, 0, 0, 0);
	float3 p0 = ViewPos(puv, g.x), n0 = Normal(g);
	float angle = 6.2831853 * (dirIndex + 0.5) / n;
	float2 dir = float2(cos(angle), sin(angle)) * Scr.xy;

	float h = 0;
	float3 light = 0;
	for (int i = 0; i < 8; i++)
	{
		float t = Casc.y + Casc.z * (i + 0.5) / 8;
		float2 suv = puv + dir * t;
		if (suv.x < 0 || suv.x > 1 || suv.y < 0 || suv.y > 1)
			break;
		float4 gs = tex2Dlod(S0, float4(suv, 0, 0));
		if (gs.a < 0.5)
			continue;
		float3 v = ViewPos(suv, gs.x) - p0;
		float dist = length(v);
		// how high it stands over the surface; things beyond the reach fade out (on a
		// screen, something far in front of the surface would else pass for a wall next to it)
		// (a little under the horizon doesn't count: sloped surfaces would else shade
		// themselves in stripes, from the depth buffer's steps)
		float s = saturate((dot(v, n0) / max(dist, 0.001) - 0.1) / 0.9);
		float near = saturate(1 - dist * dist / (Fx.z * Fx.z));
		s = lerp(h, s, near);
		if (s > h)
		{
			// the frame is gamma-space: light adds up in linear
			light += pow(max(tex2Dlod(S1, float4(suv, 0, 0)).rgb, 0), 2.2) * (s * s - h * h);
			h = s;
		}
	}
	if (Casc.w > 0.5)
	{
		// the four finer directions of the next cascade that this one spans
		float ku = k * 2;
		float2 tileSizeU = Px.zw / ku;
		float2 puvU = clamp(puv, 0.5 / tileSizeU, 1 - 0.5 / tileSizeU);
		float4 up = 0;
		for (int j = 0; j < 4; j++)
		{
			float iu = dirIndex * 4 + j;
			float ty = floor((iu + 0.5) / ku);
			float tx = iu - ty * ku;
			up += tex2Dlod(S2, float4((float2(tx, ty) + puvU) / ku, 0, 0));
		}
		up *= 0.25;
		if (up.a > h)
		{
			light += up.rgb * (up.a * up.a - h * h) / max(up.a * up.a, 0.0001);
			h = up.a;
		}
	}
	return float4(light, h);
}
#endif

#if GI_PASS == 3
// S0 = depth + normal, S1 = the frame, S2 = the nearest cascade (2 x 2 directions)
float4 ResolvePS(float2 uv : TEXCOORD0) : COLOR
{
	float4 c = tex2D(S1, uv);
	float4 g = tex2Dlod(S0, float4(uv, 0, 0));
	if (g.a < 0.5)
		return c;
	float2 tileSize = Casc.zw * 0.5;                // the cascade texture's size, 2 tiles per axis
	float2 puv = clamp(uv, 0.5 / tileSize, 1 - 0.5 / tileSize);
	float4 sum = 0;
	float open = 0;
	for (int j = 0; j < 4; j++)
	{
		float2 t = float2(j % 2, j / 2);
		float4 r = tex2Dlod(S2, float4((t + puv) * 0.5, 0, 0));
		sum += r;
		open += r.a * r.a;
	}
	float3 light = sum.rgb * 0.25;
	float shade = 1 - Fx.y * open * 0.25;
	// the surface's own colour, guessed from the lit frame: its hue, not its brightness
	float3 hue = c.rgb / (max(c.r, max(c.g, c.b)) + 0.08);
	if (Fx.w > 3.5)
		return float4(frac(g.x / 500), 0, 0, 1);    // 4: depth, in bands of 500 units
	if (Fx.w > 2.5)
		return float4(Normal(g) * 0.5 + 0.5, 1);    // 3: normals
	if (Fx.w > 1.5)
		return float4(shade, shade, shade, 1);      // 2: the corner darkening alone
	if (Fx.w > 0.5)
		return float4(pow(max(light, 0), 1 / 2.2), 1);      // 1: the gathered light alone
	float3 lit = pow(max(c.rgb, 0), 2.2) * shade + pow(max(hue, 0), 2.2) * light * Fx.x;
	return float4(pow(max(lit, 0), 1 / 2.2), c.a);
}
#endif
