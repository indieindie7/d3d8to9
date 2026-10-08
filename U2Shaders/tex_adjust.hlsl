// tex_adjust.hlsl - the texture editor's adjust bake (source/texedit.hpp, GM panel "Texture").
//
// Not a per-draw rule: texedit.hpp draws the game's texture through this shader once per mip level
// into a render-target texture of the same size, and that copy is swapped in for every draw that
// uses the texture's hash (like replace=). So it works under any stage setup and costs nothing per
// frame; a slider change re-bakes it (one quad per level).
//
// Each level is drawn 1:1 from the source's own mip level (sampler: point, MAXMIPLEVEL = the level),
// so the artist's mips are kept. Sharpening uses that level's texel size; addressing is WRAP, so
// tiling textures stay seamless.
//
// Order: sharpness (unsharp mask, negative = soften) -> brightness -> contrast (about mid-grey) ->
// gamma (levels) -> curves (shadows / mids / highlights, black and white points kept) ->
// saturation and hue (one 3x3 matrix, made on the CPU). Alpha is passed through unchanged.

sampler2D Src  : register(s0);
float4    Texel : register(c0);   // 1/width, 1/height of this level, sharpness, 0
float4    Tone  : register(c1);   // brightness (added), contrast, 1/gamma, 0
float4    Row0  : register(c2);   // saturation x hue rotation, rows (xyz)
float4    Row1  : register(c3);
float4    Row2  : register(c4);
float4    Curve : register(c5);   // shadows, mids, highlights (already x0.25), 0

float4 main(float2 uv : TEXCOORD0) : COLOR
{
	float4 c = tex2D(Src, uv);
	float3 n = tex2D(Src, uv + float2(Texel.x, 0)).rgb + tex2D(Src, uv - float2(Texel.x, 0)).rgb
	         + tex2D(Src, uv + float2(0, Texel.y)).rgb + tex2D(Src, uv - float2(0, Texel.y)).rgb;
	float3 rgb = saturate(c.rgb + Texel.z * (c.rgb - n * 0.25));

	rgb = (rgb + Tone.x - 0.5) * Tone.y + 0.5;
	rgb = pow(saturate(rgb), Tone.z);

	// curves: three bumps that are 0 at black and white (peaks at 1/3, 1/2, 2/3)
	float3 ix = 1 - rgb;
	rgb += Curve.x * (6.75 * ix * ix * rgb) + Curve.y * (4 * rgb * ix) + Curve.z * (6.75 * rgb * rgb * ix);
	rgb = saturate(rgb);

	rgb = float3(dot(Row0.xyz, rgb), dot(Row1.xyz, rgb), dot(Row2.xyz, rgb));
	return float4(saturate(rgb), c.a);
}
