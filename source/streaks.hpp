// U2Streaks: blood running down characters (Advent Rising's AdventMod), streaks=1.
//
// Wall blood is a texture the layer paints (runs.hpp); a character's skin can't take that (its UV
// "down" isn't the body's "down", and a projector on a skinned mesh comes out as one flat colour).
// So the streaks are worked out per pixel in world space: after a character draw the device draws
// the same geometry again (the game's own vertex processing, fixed function) with a pixel shader
// that knows where the bleeding points are. From each one 2-3 thin rivulets run straight down in
// world space (gravity, whatever the pose), wobbling a little, slowing as they lengthen, with a
// bead at the front; the wound itself has a small splat. Only pixels near the source's vertical
// line and near the body surface there take it (so the back of the body and neighbours stay clean).
// The colour is multiplied by the lit colour of the vertex lighting (it darkens under the game's
// light, it doesn't glow), a small wet highlight is added while fresh, and fog fades it like the
// rest. It dries: the colour goes dark brown/plum and the highlight goes.
//
// The mod (ModGore) sends the sources every tick through the U2BloodCommand export:
//   streak K x y z nx ny kind age str seed   slot K (0-7): world position, outward direction of the
//                                            body there (nx, ny: horizontal), kind 1 red 2 purple,
//                                            age in seconds, strength (~0.3-1.5), a random seed
//   streaks n                                slots n.. are unused (n = 0: none)
//   streakclear                              all off
// U2Shaders.ini: streaks=1 (default off); streakparams= width length speed drysecs (world units,
// seconds; defaults 2.5 40 6 50; width is a rivulet's typical full width); streakfx= gloss opacity gain reach (1 0.92 2 25: highlight
// strength, how opaque fresh blood is, the colour gain over the lit colour (Advent draws skins at
// double brightness), how far across from a source a streak may reach).
//
// The shader takes two sources a pass; a draw gets a pass per two sources near it (by the draw's
// world matrix when it has one, else all near the camera), at most four passes.
//
// hands=1: blood on the player's own hands, forearms and held weapon, with the same machinery: one
// more pass (its own shader) over character draws near the player's hands. The mod (ModPlayerBlood)
// sends up to six capsules every tick (hand, forearm, weapon, blade: a segment and a radius) and how
// bloody they are. Pixels inside a capsule take blood in a smudged pattern worked out in the capsule's
// own frame (along the bone, its side axis, around), so the pattern rides with the hand instead of
// swimming through world space; the more blood, the more of the pattern fills in. Weights at the two
// ends let the blood thin out up the forearm or along the barrel. It dries like the streaks.
//   hand K ax ay az bx by bz r sx sy sz wa wb   capsule K (0-4): ends A and B (world), radius, a side
//                                               axis (any length; hundredths are fine), the weights
//                                               at A and B in percent
//   hands amount kind wet seed n                amount and wet in percent (0-100), kind 1 red 2 purple,
//                                               a pattern seed, capsules n.. unused ("hands 0": off)
//   handclear                                   all off
// Capsules the mod stops sending go after 2 s. U2Shaders.ini: hands=1 (default off); handsparams=
// radius pattern opacity gain (multiplier on the capsules' radius, the pattern's scale in radians
// per world unit (0.8: blotches a few units across), opacity, colour gain over the lit colour;
// 1 0.8 0.9 2). Five capsules at most (the shader's constant registers are full).
#pragma once
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <windows.h>
#include "crash.hpp"

namespace U2Streaks
{
	const int MaxSources = 8;
	const int PerPass = 2;
	const float ViewReach = 3000.0f;       // sources farther from the camera than this aren't drawn
	const float DrawReach = 220.0f;        // ... or farther than this from a draw's origin

	void (*Log)(const char *) = nullptr;
	static void Say(const char *Fmt, ...)
	{
		if (!Log) return;
		char Buf[256];
		va_list A; va_start(A, Fmt); vsnprintf(Buf, sizeof(Buf), Fmt, A); va_end(A);
		Log(Buf);
	}

	struct Source { bool Used; float X, Y, Z, Nx, Ny, Age, Str; unsigned Seed; int Kind; DWORD Got; };
	static Source Src[MaxSources] = {};
	static bool On = false;
	static float Params[4] = { 2.5f, 40.0f, 6.0f, 50.0f };   // streakparams= width length speed drysecs
	static float Fx[4] = { 1.0f, 0.92f, 2.0f, 25.0f };       // streakfx= gloss opacity gain reach
	static int Live = 0;                                      // slots in use
	static IDirect3DPixelShader9 *PS = nullptr;
	static bool Broken = false;
	// per frame: the sources near the camera, nearest first (picked at the frame's first character draw)
	static bool NeedPick = true;
	static int Near[MaxSources];
	static int NearCount = 0;
	// per draw: the sources for this draw's passes
	static int DrawList[MaxSources];
	static int DrawCount = 0;
	static DWORD Draws = 0, Passes = 0;   // counted for the log
	static bool ToldFirst = false;
	// state put back after a pass
	static IDirect3DPixelShader9 *OldPS = nullptr;
	static float OldConst[24][4];
	static UINT OldConstN = 17;
	static DWORD OldRS[7], OldTCI[2], OldTTF[2];
	static D3DMATRIX OldTexMat[2];

	// ---- hands=1 ----
	const int MaxCaps = 5;            // (the shader's constant registers are full at five)
	const float HandReach = 160.0f;        // a placed draw farther than this from the hands takes no hands pass
	struct Capsule { bool Used; float A[3], B[3], R, S[3], WA, WB; };
	static Capsule Cap[MaxCaps] = {};
	static bool HandsOn = false;
	static float HandsParams[4] = { 1.0f, 0.8f, 0.9f, 2.0f };     // handsparams= radius pattern opacity gain
	static float HandAmount = 0, HandWet = 0;
	static int HandKind = 1, HandCaps = 0;
	static unsigned HandSeed = 0;
	static DWORD HandGot = 0;
	static IDirect3DPixelShader9 *HandPS = nullptr;
	static bool HandBroken = false, HandDraw = false, ToldHands = false;
	static int StreakPasses = 0;
	static DWORD HandDraws = 0;

	inline bool HandsLive()
	{
		return HandsOn && !HandBroken && HandAmount > 0.005f && HandCaps > 0 && GetTickCount() - HandGot < 2000;
	}
	// anything for the per-draw hook to do
	inline bool Any() { return (On && Live > 0) || HandsLive(); }

	inline bool Handles(const char *Word)
	{
		return !_stricmp(Word, "streak") || !_stricmp(Word, "streaks") || !_stricmp(Word, "streakclear")
			|| !_stricmp(Word, "hand") || !_stricmp(Word, "hands") || !_stricmp(Word, "handclear");
	}

	inline void CountCaps()
	{
		HandCaps = 0;
		for (const Capsule &C : Cap)
			HandCaps += C.Used ? 1 : 0;
	}

	inline int HandCommand(const char *Cmd)
	{
		char Word[16] = "";
		sscanf_s(Cmd, "%15s", Word, (unsigned)sizeof(Word));
		if (!_stricmp(Word, "handclear"))
		{
			for (Capsule &C : Cap) C.Used = false;
			HandCaps = 0;
			HandAmount = 0;
			return HandsOn ? 1 : 0;
		}
		if (!_stricmp(Word, "hands"))
		{
			float Amount = 0, Wet = 0, Seed = 0;
			int Kind = 1, N = MaxCaps;
			const int Got = sscanf_s(Cmd, "%15s %f %d %f %f %d", Word, (unsigned)sizeof(Word), &Amount, &Kind, &Wet, &Seed, &N);
			if (Got < 2)
				return 0;
			if (!ToldHands && Amount > 0)
			{
				ToldHands = true;
				Say("hands: first blood on the player's hands (amount %.0f%%, %s, %d capsules)%s", Amount, Kind == 2 ? "purple" : "red", N, HandsOn ? "" : " - hands=0 in U2Shaders.ini, not drawn");
			}
			HandAmount = (Amount < 0 ? 0 : Amount > 100 ? 100 : Amount) / 100.0f;
			HandWet = (Wet < 0 ? 0 : Wet > 100 ? 100 : Wet) / 100.0f;
			HandKind = Kind == 2 ? 2 : 1;
			HandSeed = (unsigned)Seed;
			for (int i = (N < 0 ? 0 : N); i < MaxCaps; i++)
				Cap[i].Used = false;
			CountCaps();
			HandGot = GetTickCount();
			return HandsOn ? 1 : 0;
		}
		int K = -1;
		float V[12] = {};
		const int Got = sscanf_s(Cmd, "%15s %d %f %f %f %f %f %f %f %f %f %f %f %f", Word, (unsigned)sizeof(Word), &K,
			&V[0], &V[1], &V[2], &V[3], &V[4], &V[5], &V[6], &V[7], &V[8], &V[9], &V[10], &V[11]);
		if (Got < 9 || K < 0 || K >= MaxCaps)
			return 0;
		Capsule &C = Cap[K];
		for (int c = 0; c < 3; c++) { C.A[c] = V[c]; C.B[c] = V[3 + c]; }
		C.R = V[6] > 0.5f ? V[6] : 0.5f;
		// the side axis: any length, but not along the capsule (the shader squares it up)
		float S[3] = { V[7], V[8], V[9] };
		if (Got < 12 || S[0] * S[0] + S[1] * S[1] + S[2] * S[2] < 1e-6f) { S[0] = 0; S[1] = 0; S[2] = 1; }
		const float L = sqrtf(S[0] * S[0] + S[1] * S[1] + S[2] * S[2]);
		for (int c = 0; c < 3; c++) C.S[c] = S[c] / L;
		C.WA = Got >= 13 ? V[10] / 100.0f : 1.0f;
		C.WB = Got >= 14 ? V[11] / 100.0f : C.WA;
		C.Used = true;
		CountCaps();
		HandGot = GetTickCount();
		return HandsOn ? 1 : 0;
	}

	inline int Command(const char *Cmd)
	{
		{
			char First[16] = "";
			sscanf_s(Cmd, "%15s", First, (unsigned)sizeof(First));
			if (!_strnicmp(First, "hand", 4))
				return HandCommand(Cmd);
		}
		char Word[16] = "";
		int K = -1, Kind = 1;
		float X = 0, Y = 0, Z = 0, Nx = 1, Ny = 0, Age = 0, Str = 1, Seed = 0;
		const int Got = sscanf_s(Cmd, "%15s %d %f %f %f %f %f %d %f %f %f", Word, (unsigned)sizeof(Word), &K, &X, &Y, &Z, &Nx, &Ny, &Kind, &Age, &Str, &Seed);
		if (!_stricmp(Word, "streakclear"))
		{
			for (Source &S : Src) S.Used = false;
			Live = 0;
			return 1;
		}
		if (!_stricmp(Word, "streaks"))
		{
			if (Got < 2 || K < 0) return 0;
			for (int i = K; i < MaxSources; i++) Src[i].Used = false;
			Live = 0;
			for (Source &S : Src) Live += S.Used ? 1 : 0;
			return 1;
		}
		if (Got < 11 || K < 0 || K >= MaxSources)
			return 0;
		Source &S = Src[K];
		if (!S.Used)
			Say("streaks: slot %d a %s source at %.0f %.0f %.0f, age %.1f s, strength %.2f", K, Kind == 2 ? "purple" : "red", X, Y, Z, Age, Str);
		const float L = sqrtf(Nx * Nx + Ny * Ny);
		S.Used = true;
		S.X = X; S.Y = Y; S.Z = Z;
		S.Nx = L > 1e-3f ? Nx / L : 1.0f; S.Ny = L > 1e-3f ? Ny / L : 0.0f;
		S.Kind = Kind; S.Age = Age; S.Str = Str; S.Seed = (unsigned)Seed; S.Got = GetTickCount();
		Live = 0;
		for (Source &T : Src) Live += T.Used ? 1 : 0;
		return 1;
	}

	// the frame's end: next frame picks again; sources the mod stopped sending go after a minute
	// (a new level's ModGore sends "streaks 0" first)
	inline void NewFrame()
	{
		U2Crash::Where("blood streaks");
		NeedPick = true;
		if (HandsOn)
		{
			static DWORD HandTold = 0;
			const DWORD T = GetTickCount();
			if (T - HandTold > 20000 && HandDraws > 0)
			{
				HandTold = T;
				Say("hands: %d capsules, amount %.2f wet %.2f; %u character draws took the hands pass since the last note", HandCaps, HandAmount, HandWet, (unsigned)HandDraws);
				HandDraws = 0;
			}
		}
		if (Live == 0)
			return;
		const DWORD Now = GetTickCount();
		Live = 0;
		for (Source &S : Src)
		{
			if (S.Used && Now - S.Got > 60000)
				S.Used = false;
			Live += S.Used ? 1 : 0;
		}
		static DWORD LastTold = 0;
		if (Now - LastTold > 20000 && Draws > 0)
		{
			LastTold = Now;
			Say("streaks: %d sources, %d near the camera; %u character draws took %u passes since the last note", Live, NearCount, (unsigned)Draws, (unsigned)Passes);
			Draws = Passes = 0;
		}
	}

	inline bool Compile(IDirect3DDevice9 *Dev);

	// a character draw (lit, solid, depth-tested, textured, an FVF with normals: Advent's characters
	// and their weapons; its level meshes come through vertex declarations), with sources near it?
	// Fills DrawList. Offscreen draws (shadow maps) are the caller's to refuse.
	inline bool CompileHands(IDirect3DDevice9 *Dev);

	inline bool Wants(IDirect3DDevice9 *Dev, bool FixedFunction, bool Textured)
	{
		StreakPasses = 0;
		HandDraw = false;
		const bool Streaking = On && Live > 0 && !Broken, Handing = HandsLive();
		if ((!Streaking && !Handing) || !FixedFunction || !Textured)
			return false;
		DWORD Lit = 0, Blend = 0, Z = 0, Test = 0, Fvf = 0;
		Dev->GetRenderState(D3DRS_LIGHTING, &Lit);
		Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &Blend);
		Dev->GetRenderState(D3DRS_ZENABLE, &Z);
		Dev->GetRenderState(D3DRS_ALPHATESTENABLE, &Test);
		if (!Lit || Blend || !Z || Test)
			return false;
		Dev->GetFVF(&Fvf);
		if (Fvf == 0 || !(Fvf & D3DFVF_NORMAL))
			return false;
		D3DMATRIX V, Wm;
		Dev->GetTransform(D3DTS_VIEW, &V);
		// the draw's origin: a mesh drawn in its own space has its actor's place in the world matrix
		Dev->GetTransform(D3DTS_WORLD, &Wm);
		const bool Placed = fabsf(Wm._41) + fabsf(Wm._42) + fabsf(Wm._43) > 0.01f;
		if (Handing)
		{
			// the hands pass: a draw near the capsules (the player, the gun, the blade; a body the
			// fist is in), or any draw that isn't placed (the shader keeps to the capsules)
			HandDraw = !Placed;
			for (int i = 0; i < MaxCaps && !HandDraw; i++)
			{
				const Capsule &C = Cap[i];
				if (!C.Used)
					continue;
				const float dx = C.A[0] - Wm._41, dy = C.A[1] - Wm._42, dz = C.A[2] - Wm._43;
				HandDraw = dx * dx + dy * dy + dz * dz < HandReach * HandReach;
			}
			if (HandDraw && HandPS == nullptr && !CompileHands(Dev))
				HandDraw = false;
			if (HandDraw)
				HandDraws++;
		}
		if (Streaking)
		{
			if (NeedPick)
			{
				// the camera: the view's inverse translation
				NeedPick = false;
				NearCount = 0;
				const float Cx = -(V._41 * V._11 + V._42 * V._12 + V._43 * V._13);
				const float Cy = -(V._41 * V._21 + V._42 * V._22 + V._43 * V._23);
				const float Cz = -(V._41 * V._31 + V._42 * V._32 + V._43 * V._33);
				float D[MaxSources];
				for (int i = 0; i < MaxSources; i++)
				{
					const Source &S = Src[i];
					if (!S.Used || S.Str <= 0)
						continue;
					const float d = sqrtf((S.X - Cx) * (S.X - Cx) + (S.Y - Cy) * (S.Y - Cy) + (S.Z - Cz) * (S.Z - Cz));
					if (d > ViewReach)
						continue;
					int k = NearCount++;
					while (k > 0 && D[k - 1] > d) { D[k] = D[k - 1]; Near[k] = Near[k - 1]; k--; }
					D[k] = d; Near[k] = i;
				}
			}
			DrawCount = 0;
			for (int k = 0; k < NearCount; k++)
			{
				const Source &S = Src[Near[k]];
				if (Placed)
				{
					const float dx = S.X - Wm._41, dy = S.Y - Wm._42, dz = S.Z - Wm._43;
					if (dx * dx + dy * dy + dz * dz > DrawReach * DrawReach)
						continue;
				}
				DrawList[DrawCount++] = Near[k];
			}
			if (DrawCount > 4 * PerPass)
				DrawCount = 4 * PerPass;
			if (DrawCount > 0 && !ToldFirst)
			{
				ToldFirst = true;
				Say("streaks: first character draw with sources (fvf %x, world origin %.0f %.0f %.0f%s, %d of %d sources)",
					(unsigned)Fvf, Wm._41, Wm._42, Wm._43, Placed ? "" : ": not placed, every near source taken", DrawCount, NearCount);
			}
			if (DrawCount > 0 && (PS != nullptr || Compile(Dev)))
			{
				StreakPasses = (DrawCount + PerPass - 1) / PerPass;
				Draws++;
			}
		}
		return StreakPasses > 0 || HandDraw;
	}

	inline float Rand(unsigned &R) { R = R * 1664525u + 1013904223u; return (R >> 8) / 16777216.0f; }
	inline float Smooth(float a, float b, float x) { float t = (x - a) / (b - a); t = t < 0 ? 0 : t > 1 ? 1 : t; return t * t * (3 - 2 * t); }

	// one source's five registers (see the shader); the rivulets come from its seed
	inline void Fill(const Source &S, float *C)
	{
		const float W = Params[0], Len = Params[1], Speed = Params[2], DrySecs = Params[3] > 1 ? Params[3] : 1;
		const DWORD Since = GetTickCount() - S.Got;
		const float Age = S.Age + (Since < 300 ? Since / 1000.0f : 0.3f);
		const float Str = S.Str < 0 ? 0 : S.Str > 1.5f ? 1.5f : S.Str, Str1 = Str > 1 ? 1 : Str;
		const float Wet = 1 - Smooth(DrySecs * 0.2f, DrySecs, Age);
		// fresh and dried colours: red to dark brown, purple (the aliens) to dark plum
		static const float Fresh[2][3] = { { 0.50f, 0.03f, 0.03f }, { 0.32f, 0.06f, 0.42f } };
		static const float Dried[2][3] = { { 0.17f, 0.045f, 0.03f }, { 0.12f, 0.035f, 0.14f } };
		const int K = S.Kind == 2 ? 1 : 0;
		float Col[3];
		for (int c = 0; c < 3; c++)
			Col[c] = Dried[K][c] + (Fresh[K][c] - Dried[K][c]) * Wet;
		C[0] = S.X; C[1] = S.Y; C[2] = S.Z;
		C[3] = W * 0.9f * (0.6f + 0.4f * Str1) * (Age * 4 + 0.3f < 1 ? Age * 4 + 0.3f : 1);   // the splat grows in a moment
		C[4] = S.Nx; C[5] = S.Ny; C[6] = Str * 2 < 1 ? Str * 2 : 1; C[7] = Wet;   // (the mod fades the strength out at the end)
		unsigned R = S.Seed * 2654435761u + 12345u;
		const int Count = Rand(R) < 0.5f ? 2 : 3;
		for (int j = 0; j < 3; j++)
		{
			float *Rv = C + 8 + 4 * j;
			const float Off = j == 0 ? (Rand(R) - 0.5f) * W * 0.6f : (Rand(R) - 0.5f) * W * 3.6f;
			float Width = W * 0.5f * (0.45f + 0.55f * Rand(R)) * (0.7f + 0.3f * Str1) * (j == 0 ? 1.3f : 1.0f);   // (half-width)
			const float Delay = j * 0.35f + Rand(R) * 0.4f;
			const float V = Speed * (0.55f + 0.6f * Rand(R)) * (0.6f + 0.4f * Str);
			const float Max = Len * (0.45f + 0.55f * Rand(R)) * (0.5f + 0.5f * Str);
			const float A = Age > Delay ? Age - Delay : 0;
			// it slows as it runs out: length = Max (1 - e^(-V t / Max)), V at the start
			const float L = Max > 0.1f ? Max * (1 - expf(-V * A / Max)) : 0;
			if (j >= Count || A <= 0)
				Width = 0;
			Rv[0] = Off; Rv[1] = Width; Rv[2] = L; Rv[3] = Col[j];
		}
	}

	inline void FillHands(float (*C)[4]);

	// before pass Pass of the draw Wants took: false = no more passes
	inline bool Begin(IDirect3DDevice9 *Dev, int Pass)
	{
		// the streak passes first, then the hands pass
		const bool Hands = Pass == StreakPasses && HandDraw && HandPS != nullptr;
		if (!Hands && (Pass >= StreakPasses || Pass * PerPass >= DrawCount || PS == nullptr))
			return false;
		OldConstN = Hands ? 23 : 17;
		static const D3DRENDERSTATETYPE RS[7] = { D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_ZWRITEENABLE, D3DRS_FOGENABLE, D3DRS_SEPARATEALPHABLENDENABLE };
		for (int i = 0; i < 7; i++)
			Dev->GetRenderState(RS[i], &OldRS[i]);
		Dev->GetPixelShader(&OldPS);
		Dev->GetPixelShaderConstantF(0, OldConst[0], OldConstN);

		float C[24][4] = {};
		D3DMATRIX V;
		Dev->GetTransform(D3DTS_VIEW, &V);
		// camera -> world: the transpose of the view's rotation, and the camera's place
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				C[r][c] = V.m[c][r];
		for (int c = 0; c < 3; c++)
			C[3][c] = -(V.m[3][0] * C[0][c] + V.m[3][1] * C[1][c] + V.m[3][2] * C[2][c]);
		DWORD FogOn = 0, Table = 0, Vert = 0, Range = 0, FS = 0, FE = 0, FD = 0;
		Dev->GetRenderState(D3DRS_FOGENABLE, &FogOn);
		Dev->GetRenderState(D3DRS_FOGTABLEMODE, &Table);
		Dev->GetRenderState(D3DRS_FOGVERTEXMODE, &Vert);
		Dev->GetRenderState(D3DRS_RANGEFOGENABLE, &Range);
		Dev->GetRenderState(D3DRS_FOGSTART, &FS);
		Dev->GetRenderState(D3DRS_FOGEND, &FE);
		Dev->GetRenderState(D3DRS_FOGDENSITY, &FD);
		const DWORD Mode = Table != D3DFOG_NONE ? Table : Vert;
		C[0][3] = Fx[0];
		C[1][3] = Fx[1];
		C[2][3] = Range && Table == D3DFOG_NONE ? 1.0f : 0.0f;
		C[3][3] = Fx[2];
		C[4][0] = Fx[3] > 1 ? Fx[3] : 1; C[4][1] = 9; C[4][2] = 14;   // reach across; out of the body and into it from the source
		C[5][0] = FogOn ? (float)(Mode == D3DFOG_LINEAR ? 3 : Mode == D3DFOG_EXP2 ? 2 : Mode == D3DFOG_EXP ? 1 : 0) : 0.0f;
		memcpy(&C[5][1], &FS, 4); memcpy(&C[5][2], &FE, 4); memcpy(&C[5][3], &FD, 4);
		for (int c = 0; c < 3; c++)
			C[6][c] = V.m[2][c];                 // Unreal's up (world Z) in camera space
		if (Hands)
			FillHands(C);
		else
			for (int k = 0; k < PerPass; k++)
			{
				const int n = Pass * PerPass + k;
				if (n < DrawCount)
					Fill(Src[DrawList[n]], C[7 + 5 * k]);   // (an unused one stays all zero: no splat, no rivulets)
			}
		Dev->SetPixelShaderConstantF(0, C[0], OldConstN);

		// camera-space position and normal from stages 0 and 1 (the shader samples no texture)
		static const D3DMATRIX Identity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
		for (DWORD s = 0; s < 2; s++)
		{
			Dev->GetTextureStageState(s, D3DTSS_TEXCOORDINDEX, &OldTCI[s]);
			Dev->GetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, &OldTTF[s]);
			Dev->GetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), &OldTexMat[s]);
			Dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, (s == 0 ? D3DTSS_TCI_CAMERASPACEPOSITION : D3DTSS_TCI_CAMERASPACENORMAL) | s);
			Dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
			Dev->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), &Identity);
		}
		// premultiplied "over": the shader's colour is added, its alpha (1 - coverage) keeps what is there
		Dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		Dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
		Dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCALPHA);
		Dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
		Dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
		Dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		Dev->SetRenderState(D3DRS_FOGENABLE, FALSE);    // the shader fogs it
		Dev->SetPixelShader(Hands ? HandPS : PS);
		Passes++;
		return true;
	}

	inline void End(IDirect3DDevice9 *Dev)
	{
		static const D3DRENDERSTATETYPE RS[7] = { D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_ZWRITEENABLE, D3DRS_FOGENABLE, D3DRS_SEPARATEALPHABLENDENABLE };
		for (int i = 0; i < 7; i++)
			Dev->SetRenderState(RS[i], OldRS[i]);
		Dev->SetPixelShader(OldPS);
		if (OldPS) { OldPS->Release(); OldPS = nullptr; }
		Dev->SetPixelShaderConstantF(0, OldConst[0], OldConstN);
		for (DWORD s = 0; s < 2; s++)
		{
			Dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, OldTCI[s]);
			Dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, OldTTF[s]);
			Dev->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), &OldTexMat[s]);
		}
	}

	// the streak shader (tools/streaks_sim.py in AdventMod mirrors its maths)
	static const char Source_[] =
		"float4 C[17] : register(c0);\n"
		"float Wave(float x) { float z = abs(frac(x) - 0.5) * 2; return z * z * (3 - 2 * z) - 0.5; }\n"
		"float4 main(float4 Lit : COLOR0, float3 P : TEXCOORD0, float3 Nc : TEXCOORD1) : COLOR\n"
		"{\n"
		"	float3 W = P.x * C[0].xyz + P.y * C[1].xyz + P.z * C[2].xyz + C[3].xyz;\n"
		"	float Cov = 0, Wet = 0, Sum = 0;\n"
		"	float3 Col = 0;\n"
		"	[unroll] for (int i = 0; i < 2; i++)\n"
		"	{\n"
		"		float4 S = C[7 + 5 * i], A = C[8 + 5 * i];\n"
		"		float3 D = W - S.xyz;\n"
		"		float H = -D.z;\n"
		"		float U = dot(D.xy, float2(-A.y, A.x));\n"
		"		float V = dot(D.xy, A.xy);\n"
		"		float Gate = saturate((C[4].x - abs(U)) * 0.25) * saturate((V + C[4].z) * 0.33) * saturate((C[4].y - V) * 0.33);\n"
		"		float c = saturate((S.w - length(float2(U, H * 0.8))) * 1.6);\n"
		"		float t = saturate(H * 0.1);\n"
		"		[unroll] for (int j = 0; j < 3; j++)\n"
		"		{\n"
		"			float4 R = C[9 + 5 * i + j];\n"
		"			float Ph = R.x * 0.37 + R.y * 1.31;\n"
		"			float X = U - R.x * t - (Wave(H * 0.045 + Ph) * 1.2 + Wave(H * 0.13 + Ph * 1.7) * 0.5) * R.y * t;\n"
		"			float Wd = R.y * (1 - 0.35 * saturate(H / max(R.z, 1)));\n"
		"			float Line = saturate((Wd - abs(X)) * 1.8) * saturate(H * 2 + 1) * saturate((R.z - H) * 2);\n"
		"			float Bead = saturate((R.y * 1.25 - length(float2(X, (H - R.z) * 0.85))) * 1.8) * saturate(R.z - 0.5);\n"
		"			c = max(c, max(Line, Bead));\n"
		"		}\n"
		"		c *= Gate * A.z;\n"
		"		Cov = max(Cov, c);\n"
		"		Col += c * float3(C[9 + 5 * i].w, C[10 + 5 * i].w, C[11 + 5 * i].w);\n"
		"		Wet += c * A.w;\n"
		"		Sum += c;\n"
		"	}\n"
		"	clip(Cov - 0.004);\n"
		"	Col /= max(Sum, 0.001);\n"
		"	Wet /= max(Sum, 0.001);\n"
		"	float3 N = normalize(Nc);\n"
		"	float3 E = normalize(-P);\n"
		"	float3 Hh = normalize(E + C[6].xyz);\n"
		"	float L = dot(Lit.rgb, float3(0.3, 0.5, 0.2));\n"
		"	float Spec = (pow(saturate(dot(N, Hh)), 40) * 0.8 + pow(1 - saturate(dot(N, E)), 4) * 0.25) * Wet * C[0].w * (L + 0.15);\n"
		"	float3 Rgb = Col * Lit.rgb * C[3].w + Spec;\n"
		"	float Dist = C[2].w > 0.5 ? length(P) : P.z;\n"
		"	float F = 1;\n"
		"	if (C[5].x > 2.5) F = saturate((C[5].z - Dist) / max(C[5].z - C[5].y, 1));\n"
		"	else if (C[5].x > 1.5) F = exp(-pow(C[5].w * Dist, 2));\n"
		"	else if (C[5].x > 0.5) F = exp(-C[5].w * Dist);\n"
		"	float K = Cov * F * C[1].w;\n"
		"	return float4(Rgb * K, 1 - K);\n"
		"}\n";

	inline bool Compile(IDirect3DDevice9 *Dev)
	{
		if (PS != nullptr)
			return true;
		static const char *Profiles[2] = { "ps_2_a", "ps_2_b" };
		for (int p = 0; p < 2 && PS == nullptr; p++)
		{
			ID3DBlob *Code = nullptr, *Err = nullptr;
			const HRESULT hr = D3DCompile(Source_, sizeof(Source_) - 1, "streaks", nullptr, nullptr, "main", Profiles[p], 0, 0, &Code, &Err);
			if (SUCCEEDED(hr) && Code != nullptr && SUCCEEDED(Dev->CreatePixelShader((const DWORD *)Code->GetBufferPointer(), &PS)))
				Say("streaks: shader ready (%s)", Profiles[p]);
			else
				Say("streaks: %s: %s", Profiles[p], Err ? (const char *)Err->GetBufferPointer() : "no shader");
			if (Code) Code->Release();
			if (Err) Err->Release();
		}
		if (PS == nullptr)
			Broken = true;
		return PS != nullptr;
	}

	// ---- hands=1: the hands pass's registers and shader ----
	// c0-c3 camera -> world (w: gloss, opacity, -, gain), c4 amount wet pattern phase, c5 shape
	// numbers (edge sharpness, stretch along the bone, pattern amplitude, fill range), c6 up (camera
	// space; w: the fill's threshold at no blood), c7 colour and radius multiplier, c8-c22 five
	// capsules of three registers: (A, radius) (B, weight at B) (side axis, weight at A). No fog:
	// the player's hands are near the camera. (The shader's 32 constant registers are all in use:
	// 23 here and 9 of its own literals.)
	inline void FillHands(float (*C)[4])
	{
		static const float Fresh[2][3] = { { 0.50f, 0.03f, 0.03f }, { 0.32f, 0.06f, 0.42f } };
		static const float Dried[2][3] = { { 0.17f, 0.045f, 0.03f }, { 0.12f, 0.035f, 0.14f } };
		const int K = HandKind == 2 ? 1 : 0;
		C[1][3] = HandsParams[2];
		C[3][3] = HandsParams[3];
		C[4][0] = HandAmount; C[4][1] = HandWet; C[4][2] = HandsParams[1] > 0.001f ? HandsParams[1] : 0.001f;
		C[4][3] = (float)(HandSeed % 1000u) * 0.37f;
		for (int c = 0; c < 3; c++)
			C[7][c] = Dried[K][c] + (Fresh[K][c] - Dried[K][c]) * HandWet;
		C[7][3] = HandsParams[0] > 0.05f ? HandsParams[0] : 0.05f;
		C[5][0] = 3.3f; C[5][1] = 0.45f; C[5][2] = 1.0f / 6.0f; C[5][3] = 0.62f;
		C[6][3] = 0.78f;
		for (int i = 0; i < MaxCaps; i++)
		{
			float *R = C[8 + 3 * i];
			const Capsule &P = Cap[i];
			if (!P.Used)
				continue;                            // (all zero: radius 0, the shader skips it)
			R[0] = P.A[0]; R[1] = P.A[1]; R[2] = P.A[2]; R[3] = P.R;
			R[4] = P.B[0]; R[5] = P.B[1]; R[6] = P.B[2]; R[7] = P.WB;
			R[8] = P.S[0]; R[9] = P.S[1]; R[10] = P.S[2]; R[11] = P.WA;
		}
	}

	// the hands shader: the capsule the pixel is deepest in gives the frame the pattern is read in
	// (the capsule's radius doubles as its pattern's phase); the pattern is three warped crossing
	// waves at two scales, stretched along the bone (smudges), and the amount (times the capsule's
	// weight there) sets how much of it fills in
	static const char HandSource_[] =
		"float4 C[23] : register(c0);\n"
		"float Wavy(float3 p)\n"
		"{\n"
		"	return sin(p.x + sin(p.y * 1.7) * 1.5) + sin(p.y + sin(p.z * 1.7) * 1.5) + sin(p.z + sin(p.x * 1.7) * 1.5);\n"
		"}\n"
		"float4 main(float4 Lit : COLOR0, float3 P : TEXCOORD0, float3 Nc : TEXCOORD1) : COLOR\n"
		"{\n"
		"	float3 W = P.x * C[0].xyz + P.y * C[1].xyz + P.z * C[2].xyz + C[3].xyz;\n"
		"	float Best = 0, Wt = 0;\n"
		"	float3 Q = 0;\n"
		"	[unroll] for (int i = 0; i < 5; i++)\n"
		"	{\n"
		"		float4 A = C[8 + 3 * i], B = C[9 + 3 * i], S = C[10 + 3 * i];\n"
		"		float3 Ax = B.xyz - A.xyz;\n"
		"		float Ln = length(Ax) + 0.01;\n"
		"		float3 U = Ax / Ln;\n"
		"		float3 D = W - A.xyz;\n"
		"		float t = dot(D, U);\n"
		"		float k = saturate(t / Ln);\n"
		"		float R = A.w * C[7].w;\n"
		"		float Inside = saturate((R - length(D - U * (k * Ln))) * C[5].x / R);\n"
		"		float3 Sd = normalize(S.xyz - U * dot(S.xyz, U) + 0.001);\n"
		"		float3 q = float3(t * C[5].y, dot(D, Sd), dot(D, cross(U, Sd))) * C[4].z + (C[4].w + A.w);\n"
		"		if (Inside > Best)\n"
		"		{\n"
		"			Best = Inside;\n"
		"			Q = q;\n"
		"			Wt = lerp(S.w, B.w, k);\n"
		"		}\n"
		"	}\n"
		"	clip(Best - 0.004);\n"
		"	float n = (Wavy(Q) * 0.65 + Wavy(Q.yzx * 2.7) * 0.35) * C[5].z + 0.5;\n"
		"	float Th = C[6].w - C[5].w * C[4].x * Wt;\n"
		"	float Cov = Best * smoothstep(Th - 0.06, Th + 0.06, n);\n"
		"	clip(Cov - 0.004);\n"
		"	float3 N = normalize(Nc);\n"
		"	float3 Hh = normalize(normalize(-P) + C[6].xyz);\n"
		"	float Spec = pow(saturate(dot(N, Hh)), 32) * C[4].y * C[0].w * (Lit.g + 0.15);\n"
		"	float3 Rgb = C[7].rgb * (0.8 + 0.4 * saturate((n - Th) * 4)) * Lit.rgb * C[3].w + Spec;\n"
		"	float K = Cov * C[1].w * (0.75 + 0.25 * C[4].y);\n"
		"	return float4(Rgb * K, 1 - K);\n"
		"}\n";

	inline bool CompileHands(IDirect3DDevice9 *Dev)
	{
		if (HandPS != nullptr)
			return true;
		if (HandBroken)
			return false;
		static const char *Profiles[2] = { "ps_2_a", "ps_2_b" };
		for (int p = 0; p < 2 && HandPS == nullptr; p++)
		{
			ID3DBlob *Code = nullptr, *Err = nullptr;
			const HRESULT hr = D3DCompile(HandSource_, sizeof(HandSource_) - 1, "hands", nullptr, nullptr, "main", Profiles[p], 0, 0, &Code, &Err);
			if (SUCCEEDED(hr) && Code != nullptr && SUCCEEDED(Dev->CreatePixelShader((const DWORD *)Code->GetBufferPointer(), &HandPS)))
				Say("hands: shader ready (%s)", Profiles[p]);
			else
				Say("hands: %s: %s", Profiles[p], Err ? (const char *)Err->GetBufferPointer() : "no shader");
			if (Code) Code->Release();
			if (Err) Err->Release();
		}
		if (HandPS == nullptr)
			HandBroken = true;
		return HandPS != nullptr;
	}

	inline void Release()
	{
		if (PS) { PS->Release(); PS = nullptr; }
		if (HandPS) { HandPS->Release(); HandPS = nullptr; }
	}
}
