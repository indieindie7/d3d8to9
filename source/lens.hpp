// U2Lens: blood on the camera's "lens" (Advent Rising's AdventMod), lens=1.
//
// When blood is spilt right by the camera a few drops land on the lens: small domes of liquid that
// bend the picture behind them (each samples the finished frame at an offset given by its slope, so
// a drop shows a small flipped, magnified patch of what is around it), tinted red (or the Seekers'
// purple) more at their thin edges than in their middle, with a dark rim and a small highlight. Some
// land as flat smears instead. They slide down slowly in fits and starts (stick-slip), leaving a
// thinning trail, and fade out over 3-6 s. A handful at a time, kept off the middle of the screen
// unless the blood came from there: it reads as "that was close", not a red screen.
//
// Drawn at the end of the post chain (RunPost, after the final pass): the frame is copied again and
// the drops drawn over it, blended, before the HUD. So it needs post=1.
//
// The mod (ModPlayerBlood) sends through the U2BloodCommand export:
//   lens strength kind            drops now (strength 0..1, kind 1 red 2 purple), near the edges
//   lensat x y z strength kind    blood at a world spot: drops if it is within reach of the camera
//                                 (lensparams' second number), stronger the nearer; they cluster
//                                 on the side of the screen it came from
//   lensclear                     all off (a new level)
// Each returns 1 when lens=1 (the mod then leaves out its own HUD splats), 0 when it is off.
// U2Shaders.ini: lens=1 (default off); lensparams= strength reach life size (a multiplier on the
// mod's strength, world units, seconds, a multiplier on drop size; 1 120 4.5 1); lensfx= refraction
// tint highlight opacity (1 1 1 1). tools/lens_sim.py in AdventMod's tools mirrors the shader.
#pragma once
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <windows.h>
#include "crash.hpp"

namespace U2Lens
{
	const int MaxDrops = 6;            // (the shader's constant registers are nearly full at six)
	const int MaxPending = 8;

	void (*Log)(const char *) = nullptr;
	static void Say(const char *Fmt, ...)
	{
		if (!Log) return;
		char Buf[256];
		va_list A; va_start(A, Fmt); vsnprintf(Buf, sizeof(Buf), Fmt, A); va_end(A);
		Log(Buf);
	}

	// a drop: centre (fractions of the screen: x of its width, y of its height), radius (of the
	// height), age and life (s), how fast it slides (heights/s), its trail (heights), a smear or a
	// drop, kind, strength (0..1), a phase for its stick-slip
	struct Drop { bool Used; float X, Y, R, Age, Life, Slide, Trail, Str, Phase; bool Flat; int Kind; };
	static Drop D_[MaxDrops] = {};
	struct Pending { float P[3], Str; int Kind; };
	static Pending Pend[MaxPending] = {};
	static int PendN = 0;
	static bool On = false;
	static float Params[4] = { 1.0f, 120.0f, 4.5f, 1.0f };   // lensparams= strength reach life size
	static float Fx[4] = { 1.0f, 1.0f, 1.0f, 1.0f };          // lensfx= refraction tint highlight opacity
	static int Live = 0;
	static IDirect3DPixelShader9 *PS = nullptr;
	static bool Broken = false;
	// the scene's view and projection, from its last perspective draw (for lensat)
	static D3DMATRIX View = {}, Proj = {};
	static bool HaveView = false;
	static LARGE_INTEGER LastQpc = {};
	static unsigned Rng = 0x2545F491u;
	static DWORD Splashes = 0, Frames = 0;

	inline float Rand() { Rng = Rng * 1664525u + 1013904223u; return (Rng >> 8) / 16777216.0f; }

	inline bool Handles(const char *Word)
	{
		return !_stricmp(Word, "lens") || !_stricmp(Word, "lensat") || !_stricmp(Word, "lensclear");
	}

	inline void CountLive()
	{
		Live = 0;
		for (const Drop &D : D_)
			Live += D.Used ? 1 : 0;
	}

	// drops for blood of strength Str (0..1); At: around that screen spot (fractions), else the edges
	inline void Splash(float Str, int Kind, bool At, float Sx, float Sy)
	{
		Str *= Params[0];
		if (Str <= 0.02f)
			return;
		if (Str > 1.2f) Str = 1.2f;
		const int N = 1 + (int)(Str * 3.0f + Rand() * 1.5f);
		for (int k = 0; k < N && k < 5; k++)
		{
			// a free slot, or the oldest
			int Pick = -1;
			float Oldest = -1;
			for (int i = 0; i < MaxDrops; i++)
			{
				if (!D_[i].Used) { Pick = i; break; }
				const float Spent = D_[i].Age / (D_[i].Life > 0.1f ? D_[i].Life : 0.1f);
				if (Spent > Oldest) { Oldest = Spent; Pick = i; }
			}
			Drop &D = D_[Pick];
			D.Used = true;
			if (At)
			{
				D.X = Sx + (Rand() - 0.5f) * 0.3f;
				D.Y = Sy + (Rand() - 0.5f) * 0.3f;
			}
			else
			{
				// toward the edges: the middle of the screen stays readable
				D.X = 0.5f + (0.2f + 0.26f * Rand()) * (Rand() < 0.5f ? -1.0f : 1.0f);
				D.Y = 0.12f + 0.7f * Rand();
			}
			D.X = D.X < 0.03f ? 0.03f : D.X > 0.97f ? 0.97f : D.X;
			D.Y = D.Y < 0.03f ? 0.03f : D.Y > 0.9f ? 0.9f : D.Y;
			D.Flat = Rand() < 0.3f;
			D.R = (0.018f + 0.032f * Rand()) * (0.65f + 0.5f * Str) * (D.Flat ? 1.5f : 1.0f) * Params[3];
			D.Age = 0;
			D.Life = Params[2] * (0.7f + 0.6f * Rand());
			D.Slide = D.Flat ? 0.004f : (0.008f + 0.02f * Rand()) * (D.R / 0.035f);
			D.Trail = 0;
			D.Str = Str > 1 ? 1 : Str;
			D.Phase = Rand() * 6.283f;
			D.Kind = Kind == 2 ? 2 : 1;
		}
		Splashes++;
		CountLive();
	}

	inline int Command(const char *Cmd)
	{
		char Word[16] = "";
		float V[5] = {};
		int Kind = 1;
		sscanf_s(Cmd, "%15s", Word, (unsigned)sizeof(Word));
		if (!_stricmp(Word, "lensclear"))
		{
			for (Drop &D : D_) D.Used = false;
			PendN = 0;
			Live = 0;
			return On ? 1 : 0;
		}
		if (!On)
			return 0;
		if (!_stricmp(Word, "lens"))
		{
			if (sscanf_s(Cmd, "%15s %f %d", Word, (unsigned)sizeof(Word), &V[0], &Kind) < 2)
				return 0;
			Splash(V[0], Kind, false, 0, 0);
			return 1;
		}
		if (sscanf_s(Cmd, "%15s %f %f %f %f %d", Word, (unsigned)sizeof(Word), &V[0], &V[1], &V[2], &V[3], &Kind) < 5)
			return 0;
		if (PendN < MaxPending)
		{
			Pending &P = Pend[PendN++];
			P.P[0] = V[0]; P.P[1] = V[1]; P.P[2] = V[2]; P.Str = V[3]; P.Kind = Kind;
		}
		return 1;
	}

	// a perspective world draw (U2Shaders::PostCheck): its view, while a lensat waits
	inline bool WantsView() { return On && PendN > 0; }
	inline void SeeView(IDirect3DDevice9 *Dev, const D3DMATRIX &P)
	{
		Dev->GetTransform(D3DTS_VIEW, &View);
		Proj = P;
		HaveView = true;
	}

	// the waiting world spots against the camera: drops for the near ones
	inline void Resolve()
	{
		if (PendN == 0)
			return;
		if (!HaveView)
		{
			if (Frames > 2) PendN = 0;           // no view to judge them by: dropped
			return;
		}
		const D3DMATRIX &V = View;
		for (int i = 0; i < PendN; i++)
		{
			const Pending &P = Pend[i];
			// camera space: row vector times the view
			const float Cx = P.P[0] * V._11 + P.P[1] * V._21 + P.P[2] * V._31 + V._41;
			const float Cy = P.P[0] * V._12 + P.P[1] * V._22 + P.P[2] * V._32 + V._42;
			const float Cz = P.P[0] * V._13 + P.P[1] * V._23 + P.P[2] * V._33 + V._43;
			const float Dist = sqrtf(Cx * Cx + Cy * Cy + Cz * Cz);
			const float Reach = Params[1] > 1 ? Params[1] : 1;
			if (Dist > Reach || Cz < -Reach * 0.25f)
				continue;                           // too far, or well behind the camera
			const float Near = 1 - Dist / Reach;
			bool At = false;
			float Sx = 0.5f, Sy = 0.5f;
			if (Cz > 1 && Proj._11 != 0 && Proj._22 != 0)
			{
				// where it is on screen (pulled in from the edges)
				Sx = 0.5f + 0.5f * Cx * Proj._11 / Cz;
				Sy = 0.5f - 0.5f * Cy * Proj._22 / Cz;
				Sx = Sx < 0.08f ? 0.08f : Sx > 0.92f ? 0.92f : Sx;
				Sy = Sy < 0.08f ? 0.08f : Sy > 0.85f ? 0.85f : Sy;
				At = true;
			}
			Splash(P.Str * (0.35f + 0.65f * Near), P.Kind, At, Sx, Sy);
		}
		PendN = 0;
	}

	// the frame's end: the drops age, slide and fade
	inline void NewFrame()
	{
		U2Crash::Where("lens blood");
		LARGE_INTEGER Now, Fq;
		QueryPerformanceCounter(&Now);
		QueryPerformanceFrequency(&Fq);
		float Dt = LastQpc.QuadPart ? (float)((Now.QuadPart - LastQpc.QuadPart) / (double)Fq.QuadPart) : 0.0f;
		LastQpc = Now;
		if (Dt > 0.1f) Dt = 0.1f;                 // (a hitch or a pause: no jump)
		Frames++;
		HaveView = false;
		if (Live == 0)
			return;
		for (Drop &D : D_)
		{
			if (!D.Used)
				continue;
			D.Age += Dt;
			if (D.Age >= D.Life || D.Y > 1.1f)
			{
				D.Used = false;
				continue;
			}
			// stick-slip: it holds, then runs a little; slower as it thins out
			const float Go = sinf(D.Age * 2.3f + D.Phase) * 0.5f + sinf(D.Age * 5.1f + D.Phase * 1.7f) * 0.5f;
			const float V = D.Slide * (Go > 0.2f ? (Go - 0.2f) * 2.5f : 0.0f) * (1 - 0.6f * D.Age / D.Life);
			D.Y += V * Dt;
			D.Trail += V * Dt;
			if (D.Trail > D.R * 5) D.Trail = D.R * 5;
		}
		CountLive();
		static DWORD Told = 0;
		const DWORD T = GetTickCount();
		if (Splashes > 0 && T - Told > 20000)
		{
			Told = T;
			Say("lens: %u splashes since the last note, %d drops on the lens", (unsigned)Splashes, Live);
			Splashes = 0;
		}
	}

	inline bool Active() { return On && !Broken && (Live > 0 || PendN > 0); }

	// c0 (1/W, 1/H, W/H, 0), c1 lensfx, c2-c13 two registers a drop: (x, y, r, alpha) (trail in radii, purple, flat, seed)
	inline void Fill(float (*C)[4], unsigned W, unsigned H)
	{
		C[0][0] = 1.0f / W; C[0][1] = 1.0f / H; C[0][2] = (float)W / (H ? H : 1); C[0][3] = 0;
		memcpy(C[1], Fx, sizeof(Fx));
		for (int i = 0; i < MaxDrops; i++)
		{
			float *A = C[2 + 2 * i], *B = C[3 + 2 * i];
			const Drop &D = D_[i];
			if (!D.Used)
			{
				// off the screen, with a size and a trail to divide by
				A[0] = -10; A[1] = -10; A[2] = 0.01f; A[3] = 0;
				B[0] = 1;
				continue;
			}
			// pops in over 60 ms, holds, fades over its last 40%
			const float In = D.Age < 0.06f ? D.Age / 0.06f : 1.0f;
			const float Left = (D.Life - D.Age) / (D.Life * 0.4f);
			const float Out = Left < 1 ? (Left > 0 ? Left : 0) : 1.0f;
			A[0] = D.X; A[1] = D.Y; A[2] = D.R; A[3] = In * Out * (0.55f + 0.45f * D.Str);
			B[0] = (D.Trail > D.R * 0.01f ? D.Trail : D.R * 0.01f) / D.R; B[1] = D.Kind == 2 ? 1.0f : 0.0f; B[2] = D.Flat ? 1.0f : 0.0f; B[3] = D.Phase;
		}
	}

	// the lens shader (tools/lens_sim.py mirrors it): for each pixel the drop with the most liquid
	// over it (a dome, or the thinning trail above it; smears are wider and flatter); its slope bends
	// the sampling of the frame copy; tint more at the thin edge, a dark rim, a highlight up-left
	static const char Source_[] =
		"sampler2D Scene : register(s0);\n"
		"float4 C[14] : register(c0);\n"
		"float4 main(float2 UV : TEXCOORD0) : COLOR\n"
		"{\n"
		"	float2 P = float2(UV.x * C[0].z, UV.y);\n"
		"	float Cov = 0, H = 0, R = 0.01;\n"
		"	float2 G = 0, K = 0;\n"
		"	[unroll] for (int i = 0; i < 6; i++)\n"
		"	{\n"
		"		float4 A = C[2 + 2 * i], B = C[3 + 2 * i];\n"
		"		float2 d = (P - float2(A.x * C[0].z, A.y)) / A.z;\n"
		"		d.x *= 1 - B.z * 0.45;\n"
		"		float hh = 1 - dot(d, d);\n"
		"		float up = -d.y / B.x;\n"
		"		float tw = 0.42 - 0.42 * saturate(up);\n"
		"		float tr = (0.5 - 0.5 * d.x * d.x / (tw * tw + 0.0001)) * (up > 0 && up < 1);\n"
		"		float h = max(hh, tr) * (1 - B.z * 0.5);\n"
		"		float c = saturate(h * 8) * A.w;\n"
		"		if (c > Cov)\n"
		"		{\n"
		"			Cov = c;\n"
		"			H = h;\n"
		"			G = hh >= tr ? d : float2(d.x / (tw + 0.01), 0) * 0.3;\n"
		"			K = B.yz;\n"
		"			R = A.z;\n"
		"		}\n"
		"	}\n"
		"	clip(Cov - 0.003);\n"
		"	float2 Off = -G * R * C[1].x * (1 - K.y * 0.7);\n"
		"	float3 Col = tex2D(Scene, UV + float2(Off.x / C[0].z, Off.y)).rgb;\n"
		"	float3 Tint = lerp(float3(0.95, 0.12, 0.08), float3(0.62, 0.22, 0.85), K.x);\n"
		"	float Edge = saturate(1 - H * 2.2);\n"
		"	Col = lerp(Col, Col * Tint * 1.6 + Tint * 0.04, saturate(0.3 + 0.6 * Edge + K.y * 0.25) * C[1].y);\n"
		"	Col *= 1 - 0.4 * Edge * Edge * C[1].y;\n"
		"	Col += saturate(dot(G, float2(-0.55, -0.75)) * 2 - 1) * (1 - Edge) * (1 - K.y) * 0.35 * C[1].z;\n"
		"	return float4(Col, saturate(Cov * C[1].w));\n"
		"}\n";

	inline bool Compile(IDirect3DDevice9 *Dev)
	{
		if (PS != nullptr)
			return true;
		if (Broken)
			return false;
		static const char *Profiles[2] = { "ps_2_a", "ps_2_b" };
		for (int p = 0; p < 2 && PS == nullptr; p++)
		{
			ID3DBlob *Code = nullptr, *Err = nullptr;
			const HRESULT hr = D3DCompile(Source_, sizeof(Source_) - 1, "lens", nullptr, nullptr, "main", Profiles[p], 0, 0, &Code, &Err);
			if (SUCCEEDED(hr) && Code != nullptr && SUCCEEDED(Dev->CreatePixelShader((const DWORD *)Code->GetBufferPointer(), &PS)))
				Say("lens: shader ready (%s)", Profiles[p]);
			else
				Say("lens: %s: %s", Profiles[p], Err ? (const char *)Err->GetBufferPointer() : "no shader");
			if (Code) Code->Release();
			if (Err) Err->Release();
		}
		if (PS == nullptr)
			Broken = true;
		return PS != nullptr;
	}

	// RunPost, after the final pass, with the post chain's state (stage 0 clamped and linear, the
	// quad's stream and FVF, no depth): Copy() has put the finished frame in Scene; the caller
	// draws the fullscreen quad after this
	static DWORD OldBlend[3] = {};
	static float OldConst[14][4];
	inline bool Begin(IDirect3DDevice9 *Dev, IDirect3DTexture9 *Scene, unsigned W, unsigned H)
	{
		if (!Compile(Dev) || Scene == nullptr)
			return false;
		float C[14][4] = {};
		Fill(C, W, H);
		Dev->GetPixelShaderConstantF(0, OldConst[0], 14);
		Dev->GetRenderState(D3DRS_SRCBLEND, &OldBlend[0]);
		Dev->GetRenderState(D3DRS_DESTBLEND, &OldBlend[1]);
		Dev->GetRenderState(D3DRS_BLENDOP, &OldBlend[2]);
		Dev->SetTexture(0, Scene);
		Dev->SetTexture(1, nullptr);
		Dev->SetPixelShader(PS);
		Dev->SetPixelShaderConstantF(0, C[0], 14);
		Dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		Dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		Dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		Dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
		return true;
	}

	inline void End(IDirect3DDevice9 *Dev)
	{
		Dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);   // (RunPost puts its own value back)
		Dev->SetRenderState(D3DRS_SRCBLEND, OldBlend[0]);
		Dev->SetRenderState(D3DRS_DESTBLEND, OldBlend[1]);
		Dev->SetRenderState(D3DRS_BLENDOP, OldBlend[2]);
		Dev->SetPixelShaderConstantF(0, OldConst[0], 14);
	}

	inline void Release()
	{
		if (PS) { PS->Release(); PS = nullptr; }
	}
}
