// testing (psreplace=HASH dbg_v0.hlsl): the game's per-vertex lighting alone
float4 main(float4 v0 : COLOR0) : COLOR
{
	return float4(v0.rgb, 1);
}
