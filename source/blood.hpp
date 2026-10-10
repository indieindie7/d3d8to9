// U2Blood: live blood pools (Advent Rising's AdventMod, blood-fluid plan step 2).
//
// The mod draws a spreading pool as a projector decal whose texture is one of a few
// placeholder textures (bloodlive=HASH lines in U2Shaders.ini, in slot order). For a draw with
// one of those textures the device swaps in a texture we fill from a small shallow-water
// sheet simulated here on the CPU, so the pool spreads, finds the slope of the floor and
// splashes outward when something steps in it. The mod drives it with commands through
// AdventNative -> the exported U2BloodCommand():
//
//   pool K size gx gy kind  start slot K: a pool `size` world units across, floor slope gx, gy
//                           (height per unit along the texture's U and V), kind 0 red / 1 purple
//   wet K u v               1 if there is blood at (u, v) (returned, not logged)
//   pour K u v rate secs kind  a body pours in at (u, v) in 0..1, `rate` sheet volume/s for secs (up to 8 at once)
//   bed K u v r h           something lies in the blood: the bed rises under it (radius r, height h)
//   stamp K u v du dv r     something moves through: at (u, v) with velocity (du, dv) in
//                           texture units/s and radius r (texture units), the blood under it
//                           takes its velocity and is pushed to the ring around it
//   stop K                  the pool is done: frozen as it is until the slot is reused
//
// The solver is the one Hydrophobia's water uses (HLL fluxes on hydrostatically reconstructed
// states, CFL sub-steps), as a small viscous liquid: strong friction, a cap on speed, one step
// per frame. Units: cells; depth about 0.5-2 for a pool; the bed is the floor's slope.
#pragma once
#include <d3d9.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <windows.h>

namespace U2Blood
{
	const int MaxSlots = 8;
	const int N = 128;                         // cells across a sheet (texture N x N): a floor REGION, every body in it pours into the same sheet
	const int MaxPours = 8;                    // bodies pouring into one sheet at once
	const float G = 3.0f;                      // gravity, sheet units (as the offline bake)
	const float Friction = 0.9f;               // per second: pow(1 - Friction, dt) on momentum (a thick liquid)
	const float Dry = 1e-4f;
	const float Cfl = 0.45f;
	const int MaxSub = 5;                      // sub-steps per frame at most (128-cell sheets: bounded cost)

	void (*Log)(const char *) = nullptr;
	static void Say(const char *Fmt, ...)
	{
		if (!Log) return;
		char Buf[256];
		va_list A; va_start(A, Fmt); vsnprintf(Buf, sizeof(Buf), Fmt, A); va_end(A);
		Log(Buf);
	}

	struct Sheet
	{
		bool Used = false, Frozen = false, Dirty = false;
		float Size = 100, Gx = 0, Gy = 0;
		float H[N * N], Hu[N * N], Hv[N * N];
		float H2[N * N], Hu2[N * N], Hv2[N * N];  // the write buffer of a step
		float B[N * N];                            // the bed: the floor's slope (+ anything lying in the blood: `bed`)
		float Mix[N * N];                          // per cell: 0 red (human) .. 1 purple (Seeker), set as blood pours in
		struct PourSrc { float U, V, Rate, Left; int Kind; } Pours[MaxPours];
		float PourRate = 0;                        // the sum over the sources (0: nothing pouring)
		int Kind = 0;                              // the region's default kind
		float Still = 0;                           // seconds without motion
		// the cells that were ever wet (a box): steps and uploads skip the dry rest of the sheet,
		// which in a fight was most of the layer's cost (8 full 128x128 sheets solved and uploaded)
		int WI0 = N, WI1 = -1, WJ0 = N, WJ1 = -1;
		bool Full = true;                          // the next upload writes the whole texture
		IDirect3DTexture9 *Tex = nullptr;
	};
	static Sheet *Slots = new Sheet[MaxSlots];       // on the heap: 8 x 100 KB of grids would bloat the dll as static data
	static DWORD Hashes[MaxSlots] = {};
	static int Count = 0;                          // bloodlive= hashes read
	static DWORD LastMs = 0;

	inline void AddHash(DWORD H)
	{
		if (Count < MaxSlots)
			Hashes[Count++] = H;
	}

	inline int SlotOf(DWORD H)
	{
		for (int i = 0; i < Count; i++)
			if (Hashes[i] == H)
				return i;
		return -1;
	}

	inline void Clear(Sheet &S)
	{
		memset(S.H, 0, sizeof(S.H)); memset(S.Hu, 0, sizeof(S.Hu)); memset(S.Hv, 0, sizeof(S.Hv));
		memset(S.Pours, 0, sizeof(S.Pours));
		for (int j = 0; j < N; j++)
			for (int i = 0; i < N; i++)
			{
				S.B[j * N + i] = S.Gx * (i - N / 2) + S.Gy * (j - N / 2);
				S.Mix[j * N + i] = (float)S.Kind;
			}
		S.PourRate = 0; S.Still = 0; S.Frozen = false; S.Dirty = true;
		S.WI0 = N; S.WI1 = -1; S.WJ0 = N; S.WJ1 = -1; S.Full = true;
	}

	// the box around (cx, cy) of radius r in cells, clamped to the sheet
	inline void Box(float cx, float cy, float r, int &i0, int &i1, int &j0, int &j1)
	{
		i0 = (int)fmaxf(0.0f, cx - r - 1); i1 = (int)fminf((float)N, cx + r + 2);
		j0 = (int)fmaxf(0.0f, cy - r - 1); j1 = (int)fminf((float)N, cy + r + 2);
	}

	// U2BloodCommand(): see the header comment
	inline int Command(const char *Cmd)
	{
		int K = -1; float A = 0, Bq = 0, C = 0, D = 0, E = 0;
		char Word[16] = "";
		if (sscanf_s(Cmd, "%15s %d %f %f %f %f %f", Word, (unsigned)sizeof(Word), &K, &A, &Bq, &C, &D, &E) < 2 || K < 0 || K >= MaxSlots)
			return 0;
		Sheet &S = Slots[K];
		if (!_stricmp(Word, "pool"))
		{
			S.Used = true; S.Size = A > 1 ? A : 100; S.Gx = Bq; S.Gy = C; S.Kind = (int)D;
			Clear(S);
			Say("blood: slot %d a pool %.0f units across, slope %.3f %.3f, kind %d", K, S.Size, S.Gx, S.Gy, S.Kind);
			return 1;
		}
		if (!_stricmp(Word, "wet"))
		{
			// is there blood at (u, v)? 1 red, 2 purple, 0 none (the mod asks before giving a walker bloody feet)
			const int i = (int)(A * N), j = (int)(Bq * N);
			if (i < 0 || j < 0 || i >= N || j >= N)
				return 0;
			return S.H[j * N + i] > 0.05f ? (S.Mix[j * N + i] > 0.5f ? 2 : 1) : 0;
		}
		if (!_stricmp(Word, "wetkind"))
		{
			// the blood's colour at (u, v): 1 purple, 0 red (the mod's native call returns a bool, so two queries)
			const int i = (int)(A * N), j = (int)(Bq * N);
			if (i < 0 || j < 0 || i >= N || j >= N)
				return 0;
			return S.Mix[j * N + i] > 0.5f ? 1 : 0;
		}
		if (!S.Used)
			return 0;
		if (!_stricmp(Word, "pour"))
		{
			// a body pours in at (u, v) for D seconds, kind E; the free source, or the one nearest its end
			int best = -1; float least = 1e9f;
			for (int p = 0; p < MaxPours; p++)
				if (S.Pours[p].Left <= 0) { best = p; break; }
				else if (S.Pours[p].Left < least) { least = S.Pours[p].Left; best = p; }
			S.Pours[best].U = A; S.Pours[best].V = Bq; S.Pours[best].Rate = C; S.Pours[best].Left = D; S.Pours[best].Kind = (int)E;
			S.PourRate = fmaxf(S.PourRate, C); S.Frozen = false; S.Still = 0;
			return 1;
		}
		if (!_stricmp(Word, "bed"))
		{
			// something lies in the blood at (u, v), radius C (texture units), height D: the bed
			// rises under it so the blood flows around and against it, not through it
			const float cx = A * N, cy = Bq * N, r = fmaxf(C * N, 1.0f);
			int i0, i1, j0, j1; Box(cx, cy, r, i0, i1, j0, j1);
			for (int j = j0; j < j1; j++)
				for (int i = i0; i < i1; i++)
				{
					const float d2 = (i + 0.5f - cx) * (i + 0.5f - cx) + (j + 0.5f - cy) * (j + 0.5f - cy);
					if (d2 < r * r)
						S.B[j * N + i] += D * (1 - d2 / (r * r));
				}
			S.Frozen = false; S.Still = 0;
			return 1;
		}
		if (!_stricmp(Word, "stamp"))
		{
			// (u, v) and the radius in texture units, velocity in texture units/s -> cells
			const float cx = A * N, cy = Bq * N, r = fmaxf(E * N, 1.0f);
			const float vx = C * N, vy = D * N;
			float moved = 0; int ring = 0;
			int i0, i1, j0, j1; Box(cx, cy, r + 1.5f, i0, i1, j0, j1);
			for (int j = j0; j < j1; j++)
				for (int i = i0; i < i1; i++)
				{
					const float d2 = (i + 0.5f - cx) * (i + 0.5f - cx) + (j + 0.5f - cy) * (j + 0.5f - cy);
					float &h = S.H[j * N + i];
					if (d2 < r * r && h > Dry)
					{
						// the blood under the foot takes its speed (capped blend) and half of it is
						// pushed out to the ring: the displacement of stage 1 in miniature
						const float w = 1 - d2 / (r * r);
						S.Hu[j * N + i] += (h * vx - S.Hu[j * N + i]) * 0.5f * w;
						S.Hv[j * N + i] += (h * vy - S.Hv[j * N + i]) * 0.5f * w;
						const float take = h * 0.5f * w;
						h -= take; moved += take;
					}
					else if (d2 < (r + 1.5f) * (r + 1.5f))
						ring++;
				}
			if (ring > 0 && moved > 0)
				for (int j = j0; j < j1; j++)
					for (int i = i0; i < i1; i++)
					{
						const float d2 = (i + 0.5f - cx) * (i + 0.5f - cx) + (j + 0.5f - cy) * (j + 0.5f - cy);
						if (d2 >= r * r && d2 < (r + 1.5f) * (r + 1.5f))
							S.H[j * N + i] += moved / ring;
					}
			S.Frozen = false; S.Still = 0;
			static int Stamps = 0;
			if (Stamps++ < 3)
				Say("blood: stamp slot %d at %.2f %.2f speed %.2f %.2f radius %.3f (moved %.3f over %d ring cells)", K, A, Bq, C, D, E, moved, ring);
			return 1;
		}
		if (!_stricmp(Word, "stop"))
		{
			S.Frozen = true; S.PourRate = 0; memset(S.Pours, 0, sizeof(S.Pours));
			return 1;
		}
		return 0;
	}

	// HLL flux across one face (L -> R) on hydrostatically reconstructed states; u the normal
	// velocity, v the tangential one. Returns the three fluxes and the two source terms.
	inline void Face(float hL, float hR, float uL, float uR, float vL, float vR, float bL, float bR, float F[3], float &sL, float &sR)
	{
		const float hLs = fmaxf(0.0f, hL + fminf(bL - bR, 0.0f));
		const float hRs = fmaxf(0.0f, hR + fminf(bR - bL, 0.0f));
		const float cL = sqrtf(G * hLs), cR = sqrtf(G * hRs);
		const float us = 0.5f * (uL + uR) + (cL - cR);
		const float cs = 0.5f * (cL + cR) + 0.25f * (uL - uR);
		const float SL = fminf(fminf(uL - cL, us - fabsf(cs)), 0.0f);
		const float SR = fmaxf(fmaxf(uR + cR, us + fabsf(cs)), 0.0f);
		const float qL = hLs * uL, qR = hRs * uR;
		const float fL[3] = { qL, qL * uL + 0.5f * G * hLs * hLs, qL * vL };
		const float fR[3] = { qR, qR * uR + 0.5f * G * hRs * hRs, qR * vR };
		const float UL[3] = { hLs, qL, hLs * vL }, UR[3] = { hRs, qR, hRs * vR };
		const float k = 1.0f / fmaxf(SR - SL, 1e-10f);
		for (int i = 0; i < 3; i++)
			F[i] = k * (SR * fL[i] - SL * fR[i] + SL * SR * (UR[i] - UL[i]));
		sL = 0.5f * G * (hL * hL - hLs * hLs);
		sR = 0.5f * G * (hR * hR - hRs * hRs);
	}

	// one explicit step of at most Dt (the CFL rule may take less); returns what it took
	inline float StepOnce(Sheet &S, float Dt)
	{
		static float U[N * N], V[N * N];
		float smax = 1e-6f;
		int wi0 = N, wi1 = -1, wj0 = N, wj1 = -1;
		for (int c = 0; c < N * N; c++)
		{
			const float h = S.H[c];
			if (h > Dry)
			{
				const int i = c % N, j = c / N;
				if (i < wi0) wi0 = i; if (i > wi1) wi1 = i;
				if (j < wj0) wj0 = j; if (j > wj1) wj1 = j;
				const float cap = 20.0f * h;        // a speed cap: a thick liquid never races
				S.Hu[c] = fmaxf(-cap, fminf(cap, S.Hu[c]));
				S.Hv[c] = fmaxf(-cap, fminf(cap, S.Hv[c]));
				U[c] = S.Hu[c] / h; V[c] = S.Hv[c] / h;
				smax = fmaxf(smax, fabsf(U[c]) + fabsf(V[c]) + sqrtf(G * h));
			}
			else
			{
				U[c] = V[c] = 0; S.Hu[c] = S.Hv[c] = 0;
			}
		}
		if (wi1 < 0)
		{
			// a dry sheet: nothing moves
			if (S.PourRate <= 0)
				S.Still += Dt;
			return Dt;
		}
		// only the wet box and a ring of one dry cell around it: a face between two dry cells
		// carries nothing (both reconstructed depths are 0), so the rest is skipped exactly
		const int i0 = wi0 > 0 ? wi0 - 1 : 0, i1 = wi1 < N - 1 ? wi1 + 1 : N - 1;
		const int j0 = wj0 > 0 ? wj0 - 1 : 0, j1 = wj1 < N - 1 ? wj1 + 1 : N - 1;
		if (i0 < S.WI0) S.WI0 = i0; if (i1 > S.WI1) S.WI1 = i1;
		if (j0 < S.WJ0) S.WJ0 = j0; if (j1 > S.WJ1) S.WJ1 = j1;
		const float dt = fminf(Dt, Cfl / smax);
		const size_t Off = (size_t)j0 * N, Rows = (size_t)(j1 - j0 + 1) * N * sizeof(float);
		memcpy(S.H2 + Off, S.H + Off, Rows); memcpy(S.Hu2 + Off, S.Hu + Off, Rows); memcpy(S.Hv2 + Off, S.Hv + Off, Rows);
		float F[3], sL, sR;
		for (int j = j0; j <= j1; j++)
			for (int i = i0; i <= i1; i++)
			{
				const int c = j * N + i;
				if (i + 1 <= i1)
				{
					const int r = c + 1;
					Face(S.H[c], S.H[r], U[c], U[r], V[c], V[r], S.B[c], S.B[r], F, sL, sR);
					S.H2[c] -= dt * F[0]; S.H2[r] += dt * F[0];
					S.Hu2[c] -= dt * (F[1] - sL); S.Hu2[r] += dt * (F[1] - sR);
					S.Hv2[c] -= dt * F[2]; S.Hv2[r] += dt * F[2];
				}
				if (j + 1 <= j1)
				{
					const int d = c + N;
					Face(S.H[c], S.H[d], V[c], V[d], U[c], U[d], S.B[c], S.B[d], F, sL, sR);
					S.H2[c] -= dt * F[0]; S.H2[d] += dt * F[0];
					S.Hv2[c] -= dt * (F[1] - sL); S.Hv2[d] += dt * (F[1] - sR);
					S.Hu2[c] -= dt * F[2]; S.Hu2[d] += dt * F[2];
				}
			}
		const float damp = powf(1.0f - Friction, dt);
		float motion = 0;
		for (int c = (int)Off; c < (j1 + 1) * N; c++)
		{
			S.H[c] = fmaxf(0.0f, S.H2[c]);
			S.Hu[c] = S.Hu2[c] * damp; S.Hv[c] = S.Hv2[c] * damp;
			motion += fabsf(S.Hu[c]) + fabsf(S.Hv[c]);
		}
		if (motion < 1e-3f * N * N && S.PourRate <= 0)
			S.Still += dt;
		else
			S.Still = 0;
		return dt;
	}

	inline void Pour(Sheet &S, float dt)
	{
		S.PourRate = 0;
		for (int p = 0; p < MaxPours; p++)
		{
			Sheet::PourSrc &P = S.Pours[p];
			if (P.Rate <= 0 || P.Left <= 0)
				continue;
			const float cx = P.U * N, cy = P.V * N, r2 = 9.0f;
			const float d = fminf(dt, P.Left);
			const int i0 = (int)fmaxf(0.0f, cx - 4), i1 = (int)fminf((float)N, cx + 5), j0 = (int)fmaxf(0.0f, cy - 4), j1 = (int)fminf((float)N, cy + 5);
			for (int j = j0; j < j1; j++)
				for (int i = i0; i < i1; i++)
				{
					const float dd = (i + 0.5f - cx) * (i + 0.5f - cx) + (j + 0.5f - cy) * (j + 0.5f - cy);
					if (dd < r2)
					{
						const float add = P.Rate * d * (1 - dd / r2) / (3.14159f * r2 * 0.5f);
						float &h = S.H[j * N + i];
						// the colour follows the volume: a Seeker's purple into a human's red mixes
						S.Mix[j * N + i] = (S.Mix[j * N + i] * h + (float)P.Kind * add) / fmaxf(h + add, 1e-6f);
						h += add;
					}
				}
			P.Left -= d;
			if (P.Left <= 0)
				P.Rate = 0;
			else
				S.PourRate = fmaxf(S.PourRate, P.Rate);
		}
	}

	// the sheet as the decal texture: 50% grey where there's no blood (the decal multiplies
	// the floor x2), the blood's colour darkening with depth (the parallax rule reads darker
	// as deeper), alpha the coverage
	inline void Upload(IDirect3DDevice9 *Dev, Sheet &S)
	{
		if (S.Tex == nullptr)
		{
			if (FAILED(Dev->CreateTexture(N, N, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &S.Tex, nullptr)))
				return;
			S.Full = true;
		}
		// after one full write only the ever-wet box (+1 for the highlight's neighbours) changes
		int ui0 = 0, ui1 = N - 1, uj0 = 0, uj1 = N - 1;
		if (!S.Full)
		{
			if (S.WI1 < 0) { S.Dirty = false; return; }
			ui0 = S.WI0 > 0 ? S.WI0 - 1 : 0; ui1 = S.WI1 < N - 1 ? S.WI1 + 1 : N - 1;
			uj0 = S.WJ0 > 0 ? S.WJ0 - 1 : 0; uj1 = S.WJ1 < N - 1 ? S.WJ1 + 1 : N - 1;
		}
		RECT R = { ui0, uj0, ui1 + 1, uj1 + 1 };
		D3DLOCKED_RECT L;
		if (FAILED(S.Tex->LockRect(0, &L, S.Full ? nullptr : &R, 0)))
			return;
		// a puddle, not paint: dark where deep, a thin lighter meniscus at the wet edge, and a
		// baked highlight from a fixed light over the surface's slope (the decal multiplies the
		// floor x2, so a highlight can reach twice the floor's brightness, no more)
		const float hx = -0.35f, hy = -0.30f, hz = 0.89f;        // the half vector of a light up and to one side
		for (int j = uj0; j <= uj1; j++)
		{
			DWORD *row = (DWORD *)((BYTE *)L.pBits + (j - uj0) * L.Pitch) - ui0;
			for (int i = ui0; i <= ui1; i++)
			{
				const float d = S.H[j * N + i];
				float cov = fmaxf(0.0f, fminf(1.0f, (d - 0.01f) / 0.02f));
				cov = cov * cov * (3 - 2 * cov);
				const float deep = fmaxf(0.0f, fminf(1.0f, d / 0.8f));
				float k = 1.0f - 0.6f * deep;
				// the meniscus: the band just inside the edge is lighter (surface tension catches the light)
				const float edge = fmaxf(0.0f, fminf(1.0f, (d - 0.02f) / 0.06f));
				k *= 1.0f + 0.7f * (1.0f - edge) * cov;
				// the highlight: the slope of the surface against the half vector
				const int i0 = i > 0 ? i - 1 : i, i1 = i < N - 1 ? i + 1 : i, j0 = j > 0 ? j - 1 : j, j1 = j < N - 1 ? j + 1 : j;
				const float gx = (S.H[j * N + i1] - S.H[j * N + i0]) * 6.0f, gy = (S.H[j1 * N + i] - S.H[j0 * N + i]) * 6.0f;
				const float nl = sqrtf(gx * gx + gy * gy + 1.0f);
				float ndh = (-gx * hx - gy * hy + hz) / nl;
				ndh = fmaxf(0.0f, ndh);
				float spec = ndh * ndh; spec *= spec; spec *= spec; spec *= spec; spec *= spec;   // ^32
				spec *= 0.85f * cov;
				const float mix = S.Mix[j * N + i];                               // red .. purple per cell
				const float cr = 82.0f + (70.0f - 82.0f) * mix, cg = 12.0f + (22.0f - 12.0f) * mix, cb = 10.0f + (112.0f - 10.0f) * mix;
				const int r = (int)(128 * (1 - cov) + fminf(255.0f, cr * k + 255.0f * spec) * cov + 0.5f);
				const int g = (int)(128 * (1 - cov) + fminf(255.0f, cg * k + 235.0f * spec) * cov + 0.5f);
				const int b = (int)(128 * (1 - cov) + fminf(255.0f, cb * k + 225.0f * spec) * cov + 0.5f);
				const int a = (int)(250 * cov + 0.5f);
				row[i] = (DWORD)a << 24 | r << 16 | g << 8 | b;
			}
		}
		S.Tex->UnlockRect(0);
		S.Dirty = false; S.Full = false;
	}

	// each frame, from the device's Present: the active sheets step and re-upload
	inline void Step(IDirect3DDevice9 *Dev)
	{
		const DWORD now = GetTickCount();
		if (LastMs == 0)
			LastMs = now - 33;
		// 30 steps a second, not every frame: blood is slow, and the simulation plus a full upload per sheet
		// every frame was most of the layer's end-of-frame time in fights (perf: eof ~5 ms)
		if (now - LastMs < 30)
			return;
		float dt = (now - LastMs) / 1000.0f;
		LastMs = now;
		dt = fmaxf(1 / 120.0f, fminf(dt, 1 / 15.0f));
		for (int k = 0; k < Count; k++)
		{
			Sheet &S = Slots[k];
			if (!S.Used)
				continue;
			if (!S.Frozen && S.Still < 2.0f)
			{
				Pour(S, dt);
				float left = dt;
				for (int n = 0; n < MaxSub && left > 1e-6f; n++)
					left -= StepOnce(S, left);
				S.Dirty = true;
			}
			if (S.Dirty || S.Tex == nullptr)
				Upload(Dev, S);
		}
	}

	// the live texture for a placeholder's hash, or null
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
			float vol = 0; for (int c = 0; c < N * N; c++) vol += Slots[k].H[c];
			Say("blood: slot %d's placeholder %08x drawn with the live sheet (volume %.2f)", k, (unsigned)Hash, vol);
		}
		return Slots[k].Tex;
	}

	inline void Release()
	{
		for (int k = 0; k < MaxSlots; k++)
			if (Slots[k].Tex) { Slots[k].Tex->Release(); Slots[k].Tex = nullptr; }
	}
}
