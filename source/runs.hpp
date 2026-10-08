// U2Runs: blood running down walls (Advent Rising's AdventMod).
//
// A wall hit leaves a splat; from it drops of blood run down the wall under gravity, leaving
// trails that thin out, slow and stop, sometimes splitting a branch off. The mod lays a run
// region on the wall as a projector decal whose texture is a placeholder (bloodrun=HASH lines in
// U2Shaders.ini, slot order); for a draw with one of those the device swaps in a texture painted
// here from the simulation (as blood.hpp does for floor pools). Commands come through the same
// export as the pools (U2BloodCommand):
//
//   run K kind gx gy       start slot K (kind 0 red, 1 purple); (gx, gy) is "down" across the
//                          texture (the mod works it out from the decal's axes), any length
//   drip K u v amount kind blood lands at (u, v) in 0..1: a splat there, and drops from it
//   rstop K                the region is done: frozen as it is until the slot is reused
//
// Units: cells (the sheet is N x N), seconds. A drop has a mass (about 0.2-1.5); its top speed
// grows with it, it leaves some of it in every cell it crosses and stops when little is left
// (the last bead stays where it stops). The gloss pass (gloss= rules on the placeholders) makes
// the trails wet and lets them dry like the other blood.
#pragma once
#include <d3d9.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

namespace U2Runs
{
	const int MaxSlots = 8;
	const int N = 128;
	const int MaxDrops = 48;
	const float Gravity = 90.0f;               // cells/s^2 (thick blood: friction keeps it far below free fall)

	void (*Log)(const char *) = nullptr;
	static void Say(const char *Fmt, ...)
	{
		if (!Log) return;
		char Buf[256];
		va_list A; va_start(A, Fmt); vsnprintf(Buf, sizeof(Buf), Fmt, A); va_end(A);
		Log(Buf);
	}

	struct Drop { float X, Y, Along, Side, Mass, Wobble; int Kind; bool Live; };
	struct Sheet
	{
		bool Used = false, Frozen = false, Dirty = false;
		float Gx = 0, Gy = 1;                  // "down" across the texture (unit)
		int Kind = 0;
		float T[N * N];                        // blood thickness per cell
		float Mix[N * N];                      // 0 red .. 1 purple
		Drop Drops[MaxDrops];
		IDirect3DTexture9 *Tex = nullptr;
	};
	static Sheet *Slots = new Sheet[MaxSlots];
	static DWORD Hashes[MaxSlots] = {};
	static int Count = 0;
	static DWORD LastMs = 0;
	static unsigned Seed = 12345;

	inline float Rnd() { Seed = Seed * 1664525u + 1013904223u; return (Seed >> 8) / 16777216.0f; }

	inline void AddHash(DWORD H) { if (Count < MaxSlots) Hashes[Count++] = H; }
	inline int SlotOf(DWORD H)
	{
		for (int i = 0; i < Count; i++)
			if (Hashes[i] == H)
				return i;
		return -1;
	}
	inline bool Handles(const char *Word)
	{
		return !_stricmp(Word, "run") || !_stricmp(Word, "drip") || !_stricmp(Word, "rstop");
	}

	inline void Deposit(Sheet &S, float x, float y, float amount, int kind)
	{
		const int i = (int)x, j = (int)y;
		if (i < 0 || j < 0 || i >= N || j >= N || amount <= 0)
			return;
		float &t = S.T[j * N + i];
		S.Mix[j * N + i] = (S.Mix[j * N + i] * t + (float)kind * amount) / fmaxf(t + amount, 1e-6f);
		t += amount;
	}

	inline void AddDrop(Sheet &S, float x, float y, float mass, float side, int kind)
	{
		for (int d = 0; d < MaxDrops; d++)
			if (!S.Drops[d].Live)
			{
				S.Drops[d] = { x, y, 0.0f, side, mass, Rnd() * 6.28f, kind, true };
				return;
			}
	}

	inline int Command(const char *Cmd)
	{
		int K = -1; float A = 0, B = 0, C = 0, D = 0;
		char Word[16] = "";
		if (sscanf_s(Cmd, "%15s %d %f %f %f %f", Word, (unsigned)sizeof(Word), &K, &A, &B, &C, &D) < 2 || K < 0 || K >= MaxSlots)
			return 0;
		Sheet &S = Slots[K];
		if (!_stricmp(Word, "run"))
		{
			memset(S.T, 0, sizeof(S.T));
			memset(S.Drops, 0, sizeof(S.Drops));
			S.Kind = (int)A;
			for (int c = 0; c < N * N; c++)
				S.Mix[c] = (float)S.Kind;
			const float l = sqrtf(B * B + C * C);
			S.Gx = l > 1e-4f ? B / l : 0.0f;
			S.Gy = l > 1e-4f ? C / l : 1.0f;
			S.Used = true; S.Frozen = false; S.Dirty = true;
			Say("runs: slot %d on a wall, down = %.2f %.2f across the texture, kind %d", K, S.Gx, S.Gy, S.Kind);
			return 1;
		}
		if (!S.Used)
			return 0;
		if (!_stricmp(Word, "drip"))
		{
			// a splat where it lands, and one to three drops from its lower edge
			const float cx = A * N, cy = B * N, amount = fmaxf(0.1f, fminf(C, 2.0f));
			const int kind = (int)D;
			const float r = 1.5f + 2.0f * amount;
			for (int j = (int)(cy - r - 1); j <= (int)(cy + r + 1); j++)
				for (int i = (int)(cx - r - 1); i <= (int)(cx + r + 1); i++)
				{
					const float d2 = (i + 0.5f - cx) * (i + 0.5f - cx) + (j + 0.5f - cy) * (j + 0.5f - cy);
					if (d2 < r * r)
						Deposit(S, (float)i, (float)j, 0.5f * amount * (1 - d2 / (r * r)), kind);
				}
			const int n = 1 + (int)(Rnd() * fminf(3.0f, 1.0f + amount * 2));
			for (int k = 0; k < n; k++)
			{
				const float off = (Rnd() - 0.5f) * r * 1.4f;        // across the splat
				const float x = cx + S.Gx * r * 0.7f - S.Gy * off, y = cy + S.Gy * r * 0.7f + S.Gx * off;
				AddDrop(S, x, y, amount * (0.5f + 0.6f * Rnd()) / n * 3.2f, 0.0f, kind);
			}
			S.Frozen = false; S.Dirty = true;
			return 1;
		}
		if (!_stricmp(Word, "rstop"))
		{
			S.Frozen = true;
			for (int d = 0; d < MaxDrops; d++)
				S.Drops[d].Live = false;
			return 1;
		}
		return 0;
	}

	// one step for the drops of a sheet; true while any still runs
	inline bool StepDrops(Sheet &S, float dt)
	{
		bool any = false;
		for (int d = 0; d < MaxDrops; d++)
		{
			Drop &P = S.Drops[d];
			if (!P.Live)
				continue;
			// speed along "down": gravity against a drag that a heavier drop overcomes better
			const float top = 0.8f + 3.0f * P.Mass;           // cells/s: slow, so a run plays out over ~10 s (runs_sim.py)
			P.Along = fminf(top, P.Along + Gravity * dt * fminf(1.0f, P.Mass * 1.5f));
			// a wobble across, and the side speed a branch was given dying away
			P.Wobble += dt * (3.0f + Rnd() * 4.0f);
			P.Side *= powf(0.2f, dt);
			const float across = P.Side + sinf(P.Wobble) * 1.5f * P.Mass;
			const float dx = (S.Gx * P.Along - S.Gy * across) * dt, dy = (S.Gy * P.Along + S.Gx * across) * dt;
			const float dist = sqrtf(dx * dx + dy * dy);
			// the trail: some of the drop stays in every cell it crosses (half to each side for width)
			const float leave = fminf(P.Mass, dist * (0.016f + 0.010f * P.Mass));
			const int steps = 1 + (int)dist;
			for (int s = 0; s < steps; s++)
			{
				const float x = P.X + dx * (s + 0.5f) / steps, y = P.Y + dy * (s + 0.5f) / steps;
				// as wide as the drop: a centre lane and two lanes each side
				const float w = 0.8f + 1.2f * fminf(1.0f, P.Mass);
				Deposit(S, x, y, leave / steps * 0.4f, P.Kind);
				for (int sg = -1; sg <= 1; sg += 2)
				{
					Deposit(S, x - S.Gy * w * 0.5f * sg, y + S.Gx * w * 0.5f * sg, leave / steps * 0.18f, P.Kind);
					Deposit(S, x - S.Gy * w * sg, y + S.Gx * w * sg, leave / steps * 0.12f, P.Kind);
				}
			}
			P.X += dx; P.Y += dy;
			P.Mass -= leave;
			// a heavy drop sometimes splits a branch off to one side
			if (P.Mass > 0.45f && Rnd() < dt * 0.35f * P.Mass)
			{
				const float part = P.Mass * 0.35f;
				P.Mass -= part;
				AddDrop(S, P.X, P.Y, part, (Rnd() < 0.5f ? -1.0f : 1.0f) * (1.5f + Rnd() * 2.5f), P.Kind);
			}
			// stopped: the last of it stays as a bead; off the sheet: gone
			if (P.Mass < 0.06f || P.X < 0 || P.Y < 0 || P.X >= N || P.Y >= N)
			{
				if (P.Mass > 0)
					for (int k = 0; k < 4; k++)
						Deposit(S, P.X + (k & 1) * 0.7f, P.Y + (k >> 1) * 0.7f, P.Mass * 0.25f, P.Kind);
				P.Live = false;
				continue;
			}
			any = true;
		}
		return any;
	}

	// the sheet as the decal texture (the decal multiplies the wall x2: 50% grey = no change)
	inline void Upload(IDirect3DDevice9 *Dev, Sheet &S)
	{
		if (S.Tex == nullptr && FAILED(Dev->CreateTexture(N, N, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &S.Tex, nullptr)))
			return;
		D3DLOCKED_RECT L;
		if (FAILED(S.Tex->LockRect(0, &L, nullptr, 0)))
			return;
		for (int j = 0; j < N; j++)
		{
			DWORD *row = (DWORD *)((BYTE *)L.pBits + j * L.Pitch);
			for (int i = 0; i < N; i++)
			{
				const float t = S.T[j * N + i];
				float cov = fmaxf(0.0f, fminf(1.0f, (t - 0.004f) / 0.02f));    // thin trails show (tuned offline: a trail cell holds about 0.01)
				cov = cov * cov * (3 - 2 * cov);
				const float deep = fmaxf(0.0f, fminf(1.0f, t / 0.3f));
				const float k = 1.0f - 0.55f * deep;
				const float mix = S.Mix[j * N + i];
				const float cr = 86.0f + (70.0f - 86.0f) * mix, cg = 12.0f + (22.0f - 12.0f) * mix, cb = 10.0f + (112.0f - 10.0f) * mix;
				const int r = (int)(128 * (1 - cov) + cr * k * cov + 0.5f);
				const int g = (int)(128 * (1 - cov) + cg * k * cov + 0.5f);
				const int b = (int)(128 * (1 - cov) + cb * k * cov + 0.5f);
				const int a = (int)(250 * cov + 0.5f);
				row[i] = (DWORD)a << 24 | r << 16 | g << 8 | b;
			}
		}
		S.Tex->UnlockRect(0);
		S.Dirty = false;
	}

	inline void Step(IDirect3DDevice9 *Dev)
	{
		const DWORD now = GetTickCount();
		float dt = LastMs ? (now - LastMs) / 1000.0f : 1 / 60.0f;
		LastMs = now;
		dt = fmaxf(1 / 240.0f, fminf(dt, 1 / 20.0f));
		for (int k = 0; k < Count; k++)
		{
			Sheet &S = Slots[k];
			if (!S.Used)
				continue;
			if (!S.Frozen && StepDrops(S, dt))
				S.Dirty = true;
			if (S.Dirty || S.Tex == nullptr)
				Upload(Dev, S);
		}
	}

	inline IDirect3DTexture9 *TextureFor(IDirect3DDevice9 *Dev, DWORD Hash)
	{
		const int k = SlotOf(Hash);
		if (k < 0 || !Slots[k].Used)
			return nullptr;
		if (Slots[k].Tex == nullptr)
			Upload(Dev, Slots[k]);
		static bool Told[MaxSlots];
		if (!Told[k])
		{
			Told[k] = true;
			Say("runs: slot %d's placeholder %08x drawn with the live sheet", k, (unsigned)Hash);
		}
		return Slots[k].Tex;
	}

	inline void Release()
	{
		for (int k = 0; k < MaxSlots; k++)
			if (Slots[k].Tex) { Slots[k].Tex->Release(); Slots[k].Tex = nullptr; }
	}
}
