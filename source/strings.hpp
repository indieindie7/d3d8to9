// U2Strings: goo strings for Advent Rising's gore (AdventMod), strings=1.
//
// Sticky strands of blood (or the Seekers' purple) stretched between two body parts: a severed
// limb and its stump, a gib and the piece it came apart from. The mod (ModGore) owns the pairs
// and decides when one snaps; the layer only draws them, as camera-facing triangle strips in
// world space, once a frame after the world and before the post chain and the HUD (at the
// frame's first HUD draw, see U2Shaders::StringsCheck), with the game's own view, projection,
// viewport, fog and depth buffer (depth tested, not written: walls and bodies hide them).
//
// A string is a curve between its two ends: the chord, plus a sag under gravity (perpendicular
// to the chord) that grows with slack (the parabola's sag for a strand longer than its chord),
// carried by the middle as a damped spring so it wobbles when the ends move fast. It thins in
// the middle as it stretches (volume roughly kept: radius ~ 1/sqrt(stretch), with a neck), has a
// blob where it holds on at each end, and the thin middle lets light through.
// Snapped: each end keeps a half that whips back, falls to hang, shortens over ~1 s to a short
// stub and drips (a bead swells at the tip, lets go and falls), fading out after ~3 s.
// The look: a wet cylinder by the strip's across coordinate (dark body, a bright specular
// stripe, a wet rim), multiplied by a rough scene light level (stringfx=), fogged by the game's
// fog states (fixed-function fog after the pixel shader).
//
// The mod sends through the U2BloodCommand export, every tick, for each live pair in a stable slot:
//   string K ax ay az bx by bz kind thick rest age snap seed
//        slot K (0-11): the two ends (world), kind 1 red 2 purple, radius (world units), rest
//        length (the strand's own length), age in seconds, seconds since it snapped (< 0: whole),
//        a random id (a new id in a slot starts it over)
//   stringoff K                 slot K is done
//   stringclear                 all off (a new level)
// U2Shaders.ini: strings=1 (default off); stringparams= thickness sag stretch life (multipliers on
// radius, sag and middle thinning; the snapped halves' shortening time in seconds; 1 1 1 1);
// stringfx= light gloss opacity rim (scene light level, highlight, opacity, wet rim; 0.55 1 0.9 0.5).
// tools/strings_sim.py in AdventMod's tools mirrors the shape maths.
#pragma once
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <windows.h>
#include "crash.hpp"

namespace U2Strings
{
	const int MaxStrings = 12;
	const int Segs = 16;                  // segments along a string (and a snapped half)
	const int BeadSegs = 6;               // segments of a drop
	const float Gravity = 950.0f;         // Unreal's (world units/s^2), for falling drops
	const float ViewReach = 4000.0f;      // strings farther from the camera aren't drawn

	void (*Log)(const char *) = nullptr;
	static void Say(const char *Fmt, ...)
	{
		if (!Log) return;
		char Buf[256];
		va_list A; va_start(A, Fmt); vsnprintf(Buf, sizeof(Buf), Fmt, A); va_end(A);
		Log(Buf);
	}

	struct Str
	{
		bool Used;
		float A[3], B[3];
		int Kind;
		float Thick, Rest, Age, Snap;
		unsigned Seed;
		DWORD Got;
		// the layer's own: the middle's spring (world place and velocity, last target), and the
		// halves at the snap (their length and the way each pointed)
		bool Init, Snapped;
		float M[3], V[3], T[3];
		float Half[2], Dir[2][3];
	};
	static Str S_[MaxStrings] = {};
	static bool On = false;
	static float Params[4] = { 1.0f, 1.0f, 1.0f, 1.0f };       // stringparams= thickness sag stretch life
	static float Fx[4] = { 0.55f, 1.0f, 0.9f, 0.5f };          // stringfx= light gloss opacity rim
	static int Live = 0;
	static IDirect3DPixelShader9 *PS = nullptr;
	static bool Broken = false;
	// the frame: the scene's view as its last world draw had it, and whether it is drawn yet
	static bool Saw3D = false, Drawn = false, HaveView = false, HaveFog = false;
	static D3DMATRIX View = {}, Proj = {};
	static D3DVIEWPORT9 Vp = {};
	static const D3DRENDERSTATETYPE FogRS[8] = { D3DRS_FOGENABLE, D3DRS_FOGCOLOR, D3DRS_FOGTABLEMODE, D3DRS_FOGVERTEXMODE,
		D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY, D3DRS_RANGEFOGENABLE };
	static DWORD Fog[8] = {};
	static LARGE_INTEGER LastQpc = {};
	static DWORD Frames = 0, Strips = 0;
	static bool ToldFirst = false;

	struct Vtx { float x, y, z; DWORD c; float u0, v0, u1, v1; };
	const DWORD VtxFvf = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2;
	const int MaxVerts = MaxStrings * (2 * Segs * 6 + 4 * BeadSegs * 6);
	static Vtx Buf[MaxVerts];
	static int Count = 0;

	inline void CountLive()
	{
		Live = 0;
		for (const Str &S : S_)
			Live += S.Used ? 1 : 0;
	}

	inline bool Handles(const char *Word)
	{
		return !_stricmp(Word, "string") || !_stricmp(Word, "stringoff") || !_stricmp(Word, "stringclear");
	}

	inline int Command(const char *Cmd)
	{
		char Word[16] = "";
		int K = -1, Kind = 1;
		float P[6] = {}, Thick = 1, Rest = 20, Age = 0, Snap = -1, Seed = 0;
		const int Got = sscanf_s(Cmd, "%15s %d %f %f %f %f %f %f %d %f %f %f %f %f", Word, (unsigned)sizeof(Word), &K,
			&P[0], &P[1], &P[2], &P[3], &P[4], &P[5], &Kind, &Thick, &Rest, &Age, &Snap, &Seed);
		if (!_stricmp(Word, "stringclear"))
		{
			for (Str &S : S_) S.Used = false;
			Live = 0;
			return 1;
		}
		if (K < 0 || K >= MaxStrings)
			return 0;
		Str &S = S_[K];
		if (!_stricmp(Word, "stringoff"))
		{
			S.Used = false;
			CountLive();
			return 1;
		}
		if (Got < 14)
			return 0;
		const unsigned Id = (unsigned)Seed;
		if (!S.Used || S.Seed != Id || Age < S.Age - 0.5f)
		{
			S.Init = S.Snapped = false;       // a new string in the slot
			if (!S.Used || S.Seed != Id)
				Say("strings: slot %d a %s string, %.0f units apart, rest %.0f, radius %.2f", K, Kind == 2 ? "purple" : "red",
					sqrtf((P[3] - P[0]) * (P[3] - P[0]) + (P[4] - P[1]) * (P[4] - P[1]) + (P[5] - P[2]) * (P[5] - P[2])), Rest, Thick);
		}
		if (Snap < 0)
			S.Snapped = false;
		S.Used = true;
		for (int c = 0; c < 3; c++) { S.A[c] = P[c]; S.B[c] = P[3 + c]; }
		S.Kind = Kind;
		S.Thick = Thick > 0.05f ? Thick : 0.05f;
		S.Rest = Rest > 1 ? Rest : 1;
		S.Age = Age;
		S.Snap = Snap;
		S.Seed = Id;
		S.Got = GetTickCount();
		CountLive();
		return 1;
	}

	// a perspective world draw on the screen (the caller checks): the view the strings are drawn
	// with, and the fog of an opaque one (additive draws set their own fog colour)
	inline void Capture(IDirect3DDevice9 *Dev, bool Opaque)
	{
		Dev->GetTransform(D3DTS_VIEW, &View);
		Dev->GetTransform(D3DTS_PROJECTION, &Proj);
		Dev->GetViewport(&Vp);
		HaveView = Saw3D = true;
		if (Opaque)
		{
			for (int i = 0; i < 8; i++)
				Dev->GetRenderState(FogRS[i], &Fog[i]);
			HaveFog = true;
		}
	}

	// the frame's end; strings the mod stopped sending go after a minute
	inline void NewFrame()
	{
		Saw3D = Drawn = false;
		if (Live == 0)
			return;
		const DWORD Now = GetTickCount();
		for (Str &S : S_)
			if (S.Used && Now - S.Got > 60000)
				S.Used = false;
		CountLive();
		static DWORD LastTold = 0;
		if (Now - LastTold > 20000 && Frames > 0)
		{
			LastTold = Now;
			Say("strings: %d live; %u frames drew %u strips since the last note", Live, (unsigned)Frames, (unsigned)Strips);
			Frames = Strips = 0;
		}
	}

	inline float Smooth(float a, float b, float x) { float t = (x - a) / (b - a); t = t < 0 ? 0 : t > 1 ? 1 : t; return t * t * (3 - 2 * t); }
	inline float Clamp(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
	inline float Dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
	inline float Len(const float *a) { return sqrtf(Dot(a, a)); }
	inline void Cross(const float *a, const float *b, float *o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }

	// the camera's place, its right (world) and a world point's view depth
	static float Cam[3], Right[3];
	inline float Depth(const float *p) { return p[0] * View._13 + p[1] * View._23 + p[2] * View._33 + View._43; }

	// one camera-facing ribbon through n points: radius, how thin the goo is there (0-1, lets light
	// through) and an alpha factor per point
	inline void Ribbon(const float (*P)[3], const float *R, const float *Thin, const float *Alpha, int n, DWORD Col)
	{
		if (n < 2 || Count + (n - 1) * 6 > MaxVerts)
			return;
		Vtx L[Segs + 1], Rt[Segs + 1];
		if (n > Segs + 1)
			n = Segs + 1;
		const float PxK = Proj._22 * (Vp.Height > 0 ? Vp.Height : 1) * 0.5f;   // pixels per world unit at depth 1
		for (int i = 0; i < n; i++)
		{
			const int a = i > 0 ? i - 1 : 0, b = i < n - 1 ? i + 1 : n - 1;
			float T[3] = { P[b][0] - P[a][0], P[b][1] - P[a][1], P[b][2] - P[a][2] };
			float E[3] = { Cam[0] - P[i][0], Cam[1] - P[i][1], Cam[2] - P[i][2] };
			float Sd[3];
			Cross(T, E, Sd);
			float l = Len(Sd);
			if (l < 1e-4f * (Len(T) * Len(E) + 1e-6f))
			{
				Sd[0] = Right[0]; Sd[1] = Right[1]; Sd[2] = Right[2];   // looking straight along it
				l = 1;
			}
			// at least ~1.2 pixels each side (a 0.7 one came out dotted in game), fainter instead of thinner
			float r = R[i], Cover = 1;
			const float z = Depth(P[i]);
			const float Px = r * PxK / (z > 1 ? z : 1);
			if (Px < 1.2f)
			{
				Cover = Px / 1.2f;
				r = Px > 1e-6f ? r * 1.2f / Px : 0;
			}
			const float k = r / l;
			const float t = (float)i / (n - 1);
			Vtx &Lv = L[i], &Rv = Rt[i];
			Lv.x = P[i][0] - Sd[0] * k; Lv.y = P[i][1] - Sd[1] * k; Lv.z = P[i][2] - Sd[2] * k;
			Rv.x = P[i][0] + Sd[0] * k; Rv.y = P[i][1] + Sd[1] * k; Rv.z = P[i][2] + Sd[2] * k;
			Lv.c = Rv.c = Col;
			Lv.u0 = -1; Rv.u0 = 1;
			Lv.v0 = Rv.v0 = t;
			Lv.u1 = Rv.u1 = Thin[i];
			Lv.v1 = Rv.v1 = Cover * Alpha[i];
		}
		for (int i = 0; i < n - 1; i++)
		{
			Vtx *O = Buf + Count;
			O[0] = L[i]; O[1] = Rt[i]; O[2] = L[i + 1];
			O[3] = Rt[i]; O[4] = Rt[i + 1]; O[5] = L[i + 1];
			Count += 6;
		}
		Strips++;
	}

	// a drop: a little ribbon along Axis (unit) with a round profile, centred at C, Stretch long per radius
	inline void Drop(const float *C, const float *Axis, float r, float Stretch, float Alpha, DWORD Col)
	{
		float P[BeadSegs + 1][3], R[BeadSegs + 1], Th[BeadSegs + 1], Al[BeadSegs + 1];
		for (int k = 0; k <= BeadSegs; k++)
		{
			const float s = -1 + 2.0f * k / BeadSegs;
			for (int c = 0; c < 3; c++)
				P[k][c] = C[c] + Axis[c] * r * Stretch * s;
			R[k] = r * sqrtf(Clamp(1 - s * s, 0, 1));
			Th[k] = 0.1f;
			Al[k] = Alpha;
		}
		Ribbon(P, R, Th, Al, BeadSegs + 1, Col);
	}

	inline DWORD Colour(const Str &S)
	{
		// dark red, or the Seekers' purple; a little variation by the string's id
		const float v = 0.9f + 0.2f * ((S.Seed * 2654435761u >> 24) / 255.0f);
		const float r = (S.Kind == 2 ? 0.30f : 0.46f) * v, g = (S.Kind == 2 ? 0.05f : 0.03f) * v, b = (S.Kind == 2 ? 0.40f : 0.03f) * v;
		return D3DCOLOR_ARGB(255, (int)(Clamp(r, 0, 1) * 255), (int)(Clamp(g, 0, 1) * 255), (int)(Clamp(b, 0, 1) * 255));
	}

	// the string's shape this frame: the sag's target and the middle's spring, Dt seconds on
	inline void Spring(Str &S, float Dt, float &Chord, float *U, float *Mid)
	{
		float D[3] = { S.B[0] - S.A[0], S.B[1] - S.A[1], S.B[2] - S.A[2] };
		Chord = Len(D);
		for (int c = 0; c < 3; c++)
		{
			U[c] = Chord > 0.01f ? D[c] / Chord : (c == 0 ? 1.0f : 0.0f);
			Mid[c] = (S.A[c] + S.B[c]) * 0.5f;
		}
		// the strand is a little longer than its rest length: it sags at rest, sags less as it
		// stretches (the parabola: length ~ chord + 8 sag^2 / (3 chord); a deep loop: sag -> slack / 2)
		const float Strand = S.Rest * 1.08f;
		const float Slack = Strand - Chord > 0 ? Strand - Chord : 0;
		float Sag = sqrtf(3 * Chord * Slack / 8 + Slack * Slack / 4);
		if (Sag > Strand * 0.5f) Sag = Strand * 0.5f;
		if (Sag < 0.03f * Chord) Sag = 0.03f * Chord;          // even taut, goo hangs a little
		Sag *= Params[1];
		// gravity across the chord (a vertical string has nowhere to sag)
		const float G[3] = { U[2] * U[0], U[2] * U[1], -1 + U[2] * U[2] };
		float T[3];
		for (int c = 0; c < 3; c++)
			T[c] = Mid[c] + G[c] * Sag;
		if (!S.Init)
		{
			S.Init = true;
			for (int c = 0; c < 3; c++) { S.M[c] = T[c]; S.V[c] = 0; S.T[c] = T[c]; }
			return;
		}
		// a damped spring after the target (~2.2 Hz, light damping): the middle lags when the ends move
		const float K = 190.0f, Damp = 5.0f;
		const int Steps = Dt > 0.0125f ? (int)ceilf(Dt / 0.0125f) : 1;
		const float h = Dt / Steps;
		float Tv[3];
		for (int c = 0; c < 3; c++)
			Tv[c] = Dt > 1e-4f ? (T[c] - S.T[c]) / Dt : 0;
		for (int i = 0; i < Steps; i++)
		{
			const float f = (float)(i + 1) / Steps;
			for (int c = 0; c < 3; c++)
			{
				const float Tc = S.T[c] + (T[c] - S.T[c]) * f;
				S.V[c] += (K * (Tc - S.M[c]) - Damp * (S.V[c] - Tv[c])) * h;
				S.M[c] += S.V[c] * h;
			}
		}
		for (int c = 0; c < 3; c++)
			S.T[c] = T[c];
		// never farther off than half the chord (a fast yank)
		float Off[3] = { S.M[0] - T[0], S.M[1] - T[1], S.M[2] - T[2] };
		const float Lim = Chord * 0.5f + 4, Lo = Len(Off);
		if (Lo > Lim)
			for (int c = 0; c < 3; c++)
			{
				S.M[c] = T[c] + Off[c] * Lim / Lo;
				S.V[c] *= 0.5f;
			}
	}

	inline void Whole(const Str &S, float Chord, const float *Mid)
	{
		float P[Segs + 1][3], R[Segs + 1], Th[Segs + 1], Al[Segs + 1];
		const float Stretch = Chord / S.Rest;
		const float R0 = S.Thick * Params[0] * (Stretch > 1 ? 1 / sqrtf(Stretch) : 1.0f);
		const float Neck = Clamp((Stretch - 1) * 0.5f * Params[2], 0, 0.8f);
		const float Bow[3] = { S.M[0] - Mid[0], S.M[1] - Mid[1], S.M[2] - Mid[2] };
		const float Fade = Clamp(S.Age * 8, 0, 1);              // (it appears over a moment)
		for (int i = 0; i <= Segs; i++)
		{
			const float t = (float)i / Segs, w = 4 * t * (1 - t), sn = sinf(3.14159265f * t);
			for (int c = 0; c < 3; c++)
				P[i][c] = S.A[c] + (S.B[c] - S.A[c]) * t + Bow[c] * w;
			const float Blob = 1 + 0.8f * expf(-(t / 0.06f) * (t / 0.06f)) + 0.8f * expf(-((1 - t) / 0.06f) * ((1 - t) / 0.06f));
			R[i] = R0 * (1 - Neck * sn * sn) * Blob;
			Th[i] = Clamp(0.15f + Neck * sn * sn / 0.8f, 0, 1);
			Al[i] = Fade;
		}
		Ribbon(P, R, Th, Al, Segs + 1, Colour(S));
	}

	// snapped: the two halves hanging from the ends, shortening, dripping
	inline void Halves(Str &S, float Chord, const float *Mid)
	{
		const float R0 = S.Thick * Params[0];
		if (!S.Snapped)
		{
			S.Snapped = true;
			const float H = Clamp(Chord * 0.5f, 2, S.Rest * 0.9f);
			for (int h = 0; h < 2; h++)
			{
				const float *E = h ? S.B : S.A;
				float D[3] = { S.M[0] - E[0], S.M[1] - E[1], S.M[2] - E[2] };
				const float l = Len(D);
				for (int c = 0; c < 3; c++)
					S.Dir[h][c] = l > 0.01f ? D[c] / l : (c == 2 ? -1.0f : 0.0f);
				S.Half[h] = H;
			}
		}
		const DWORD Since = GetTickCount() - S.Got;
		const float s = S.Snap + (Since < 300 ? Since / 1000.0f : 0.3f);
		const float Life = Params[3] > 0.1f ? Params[3] : 0.1f;
		const float Fade = 1 - Smooth(2.5f * Life, 3.0f * Life, s);
		if (Fade <= 0)
			return;
		const float Stub = 2 + 1.5f * R0;
		const float Bend = Smooth(0, 0.4f * Life, s);
		const float Down[3] = { 0, 0, -1 };
		const DWORD Col = Colour(S);
		for (int h = 0; h < 2; h++)
		{
			const float *E = h ? S.B : S.A, *Dir = S.Dir[h];
			const float L = Stub + (S.Half[h] - Stub > 0 ? S.Half[h] - Stub : 0) * (1 - Smooth(0, Life, s));
			float Sw[3] = { -Dir[1], Dir[0], 0 };
			const float SwL = Len(Sw);
			if (SwL > 1e-3f) { Sw[0] /= SwL; Sw[1] /= SwL; } else { Sw[0] = 1; Sw[1] = 0; }
			const float Swing = sinf(s * 11 + h * 2.0f) * expf(-s * 2.5f) * 0.3f;
			float P[Segs + 1][3], R[Segs + 1], Th[Segs + 1], Al[Segs + 1];
			for (int i = 0; i <= Segs; i++)
			{
				const float t = (float)i / Segs;
				for (int c = 0; c < 3; c++)
					P[i][c] = E[c] + L * (Dir[c] * t * (1 - Bend) + Down[c] * (Bend * t + (1 - Bend) * 0.35f * t * t) + Sw[c] * Swing * t * t);
				R[i] = R0 * (1 - 0.55f * t) * (1 + 0.8f * expf(-(t / 0.08f) * (t / 0.08f)));
				Th[i] = 0.2f + 0.4f * t;
				Al[i] = Fade;
			}
			Ribbon(P, R, Th, Al, Segs + 1, Col);
			// the drip at the tip: a bead swells, lets go and falls
			const float *Tip = P[Segs];
			const float Rb = R0 * 0.9f;
			const float Start = 0.25f * Life, Period = 0.7f * Life;
			float Grow = 0.5f;
			if (s > Start)
			{
				const float Ph = (s - Start) / Period + h * 0.37f;
				const float f = Ph - floorf(Ph);
				Grow = f;
				const float Tf = f * Period, Fall = 0.5f * Gravity * Tf * Tf;
				if (Fall < 300)
				{
					const float Rd = Rb * 0.75f;
					const float C[3] = { Tip[0], Tip[1], Tip[2] - Rb - Fall - Rd };
					Drop(C, Down, Rd, 1.0f + Clamp(Gravity * Tf * 0.004f, 0, 1.5f), Fade, Col);
				}
			}
			const float Rt = Rb * (0.55f + 0.6f * Grow);
			const float C[3] = { Tip[0], Tip[1], Tip[2] - Rt * 0.7f };
			Drop(C, Down, Rt, 1.15f, Fade, Col);
		}
	}

	inline bool Compile(IDirect3DDevice9 *Dev);

	// the render states the pass sets, saved and put back
	static const D3DRENDERSTATETYPE PassRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,
		D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE,
		D3DRS_LIGHTING, D3DRS_SRGBWRITEENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE, D3DRS_FILLMODE,
		D3DRS_SHADEMODE, D3DRS_DEPTHBIAS, D3DRS_SLOPESCALEDEPTHBIAS, D3DRS_VERTEXBLEND, D3DRS_INDEXEDVERTEXBLENDENABLE, D3DRS_SPECULARENABLE,
		D3DRS_FOGENABLE, D3DRS_FOGCOLOR, D3DRS_FOGTABLEMODE, D3DRS_FOGVERTEXMODE, D3DRS_FOGSTART, D3DRS_FOGEND, D3DRS_FOGDENSITY,
		D3DRS_RANGEFOGENABLE };
	const int PassRSCount = sizeof(PassRS) / sizeof(PassRS[0]);

	// once a frame, after the world (the first HUD draw, or Present when the frame has none)
	inline void Draw(IDirect3DDevice9 *Dev)
	{
		if (Drawn || !On || !HaveView || Live == 0 || Broken)
			return;
		Drawn = true;
		U2Crash::Where("goo strings");
		if (PS == nullptr && !Compile(Dev))
			return;
		LARGE_INTEGER Now, Freq;
		QueryPerformanceCounter(&Now);
		QueryPerformanceFrequency(&Freq);
		float Dt = LastQpc.QuadPart ? (float)((double)(Now.QuadPart - LastQpc.QuadPart) / (double)Freq.QuadPart) : 0.0f;
		LastQpc = Now;
		Dt = Clamp(Dt, 0, 0.05f);
		// the camera: the view's inverse translation; its right: the view's first column
		const D3DMATRIX &V = View;
		Cam[0] = -(V._41 * V._11 + V._42 * V._12 + V._43 * V._13);
		Cam[1] = -(V._41 * V._21 + V._42 * V._22 + V._43 * V._23);
		Cam[2] = -(V._41 * V._31 + V._42 * V._32 + V._43 * V._33);
		Right[0] = V._11; Right[1] = V._21; Right[2] = V._31;
		Count = 0;
		for (Str &S : S_)
		{
			if (!S.Used)
				continue;
			float Chord, U[3], Mid[3];
			Spring(S, Dt, Chord, U, Mid);
			const float Dc[3] = { Mid[0] - Cam[0], Mid[1] - Cam[1], Mid[2] - Cam[2] };
			if (Len(Dc) > ViewReach + Chord)
				continue;
			if (S.Snap < 0)
				Whole(S, Chord, Mid);
			else
				Halves(S, Chord, Mid);
		}
		if (Count < 3)
			return;
		if (!ToldFirst)
		{
			ToldFirst = true;
			Say("strings: first frame drawn (%d triangles, viewport %ux%u, fog %s)", Count / 3, (unsigned)Vp.Width, (unsigned)Vp.Height,
				HaveFog && Fog[0] ? "on" : "off");
		}
		Frames++;

		// save
		DWORD OldRS[PassRSCount];
		for (int i = 0; i < PassRSCount; i++)
			Dev->GetRenderState(PassRS[i], &OldRS[i]);
		D3DMATRIX OldW, OldV, OldP;
		Dev->GetTransform(D3DTS_WORLD, &OldW);
		Dev->GetTransform(D3DTS_VIEW, &OldV);
		Dev->GetTransform(D3DTS_PROJECTION, &OldP);
		D3DVIEWPORT9 OldVp;
		Dev->GetViewport(&OldVp);
		IDirect3DVertexDeclaration9 *OldDecl = nullptr;
		DWORD OldFvf = 0;
		Dev->GetVertexDeclaration(&OldDecl);
		Dev->GetFVF(&OldFvf);
		IDirect3DVertexShader9 *OldVS = nullptr;
		IDirect3DPixelShader9 *OldPS = nullptr;
		Dev->GetVertexShader(&OldVS);
		Dev->GetPixelShader(&OldPS);
		IDirect3DVertexBuffer9 *OldVB = nullptr;
		UINT OldOff = 0, OldStride = 0;
		Dev->GetStreamSource(0, &OldVB, &OldOff, &OldStride);
		float OldC[1][4];
		Dev->GetPixelShaderConstantF(0, OldC[0], 1);
		DWORD OldTCI[2], OldTTF[2];
		for (DWORD s = 0; s < 2; s++)
		{
			Dev->GetTextureStageState(s, D3DTSS_TEXCOORDINDEX, &OldTCI[s]);
			Dev->GetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, &OldTTF[s]);
		}

		// set: world space, the scene's view, depth tested and not written, plain alpha blending
		static const D3DMATRIX Identity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
		Dev->SetTransform(D3DTS_WORLD, &Identity);
		Dev->SetTransform(D3DTS_VIEW, &View);
		Dev->SetTransform(D3DTS_PROJECTION, &Proj);
		Dev->SetViewport(&Vp);
		Dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
		Dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		Dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
		Dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		Dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		Dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		Dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
		Dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
		Dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		Dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
		Dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		Dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		Dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
		Dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
		Dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		Dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
		Dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
		Dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
		Dev->SetRenderState(D3DRS_DEPTHBIAS, 0);
		Dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
		Dev->SetRenderState(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);
		Dev->SetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
		Dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
		if (HaveFog)
			for (int i = 0; i < 8; i++)
				Dev->SetRenderState(FogRS[i], Fog[i]);
		else
			Dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
		for (DWORD s = 0; s < 2; s++)
		{
			Dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, s);
			Dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		}
		Dev->SetVertexShader(nullptr);
		Dev->SetFVF(VtxFvf);
		Dev->SetPixelShader(PS);
		Dev->SetPixelShaderConstantF(0, Fx, 1);
		Dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, Count / 3, Buf, sizeof(Vtx));

		// put back
		for (int i = 0; i < PassRSCount; i++)
			Dev->SetRenderState(PassRS[i], OldRS[i]);
		Dev->SetTransform(D3DTS_WORLD, &OldW);
		Dev->SetTransform(D3DTS_VIEW, &OldV);
		Dev->SetTransform(D3DTS_PROJECTION, &OldP);
		Dev->SetViewport(&OldVp);
		for (DWORD s = 0; s < 2; s++)
		{
			Dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, OldTCI[s]);
			Dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, OldTTF[s]);
		}
		Dev->SetPixelShaderConstantF(0, OldC[0], 1);
		Dev->SetStreamSource(0, OldVB, OldOff, OldStride);      // (DrawPrimitiveUP unbinds it)
		if (OldFvf != 0 || OldDecl == nullptr)
			Dev->SetFVF(OldFvf);
		else
			Dev->SetVertexDeclaration(OldDecl);
		Dev->SetVertexShader(OldVS);
		Dev->SetPixelShader(OldPS);
		IUnknown *Refs[] = { OldDecl, OldVS, OldPS, OldVB };
		for (IUnknown *R : Refs)
			if (R != nullptr)
				R->Release();
		U2Crash::Where("the game's draws (after the goo strings)");
	}

	// the goo: a wet cylinder by the across coordinate (TEXCOORD0.x, -1..1); TEXCOORD1: x how thin
	// (light through), y alpha (coverage and fade); COLOR0 the goo's colour; c0 = stringfx=
	static const char Source_[] =
		"float4 C : register(c0);\n"
		"float4 main(float4 Col : COLOR0, float2 T0 : TEXCOORD0, float2 T1 : TEXCOORD1) : COLOR\n"
		"{\n"
		"	float u = clamp(T0.x, -1, 1);\n"
		"	float a = abs(u);\n"
		"	float n = sqrt(saturate(1 - u * u));\n"
		"	float Edge = saturate((1 - a) * 5);\n"
		"	float L = C.x;\n"
		"	float3 Body = Col.rgb * L * (0.35 + 0.65 * n) + Col.rgb * L * 0.8 * T1.x * n;\n"
		"	float Spec = pow(saturate(1 - abs(u + 0.4) * 3), 4) * C.y * (0.4 + L);\n"
		"	float Rim = saturate((a - 0.55) * 3) * Edge * C.w * (0.3 + L);\n"
		"	float3 Rgb = Body + Spec * float3(1, 0.92, 0.88) + Rim * (Col.rgb * 2 + 0.15);\n"
		"	float A = C.z * (1 - 0.45 * T1.x) * Edge * T1.y;\n"
		"	A = saturate(A + Spec * 0.5 * Edge * T1.y);\n"
		"	return float4(Rgb, A);\n"
		"}\n";

	inline bool Compile(IDirect3DDevice9 *Dev)
	{
		if (PS != nullptr)
			return true;
		static const char *Profiles[2] = { "ps_2_0", "ps_2_a" };
		for (int p = 0; p < 2 && PS == nullptr; p++)
		{
			ID3DBlob *Code = nullptr, *Err = nullptr;
			const HRESULT hr = D3DCompile(Source_, sizeof(Source_) - 1, "strings", nullptr, nullptr, "main", Profiles[p], 0, 0, &Code, &Err);
			if (SUCCEEDED(hr) && Code != nullptr && SUCCEEDED(Dev->CreatePixelShader((const DWORD *)Code->GetBufferPointer(), &PS)))
				Say("strings: shader ready (%s)", Profiles[p]);
			else
				Say("strings: %s: %s", Profiles[p], Err ? (const char *)Err->GetBufferPointer() : "no shader");
			if (Code) Code->Release();
			if (Err) Err->Release();
		}
		if (PS == nullptr)
			Broken = true;
		return PS != nullptr;
	}

	// the device goes (the game makes a new one on a fullscreen/windowed switch): the shader is its
	inline void Release()
	{
		if (PS) { PS->Release(); PS = nullptr; }
		Broken = false;
		HaveView = HaveFog = false;
	}
}
