// U2Blood: live blood pools (Advent Rising's AdventMod, blood-fluid plan step 2).
//
// The mod draws a spreading pool as a projector decal whose texture is one of a few
// placeholder textures (bloodlive=HASH lines in U2Shaders.ini, in slot order). For a draw with
// one of those textures the device swaps in a texture we fill from a small shallow-water
// sheet simulated here on the CPU, so the pool spreads, finds the slope of the floor and
// splashes outward when something steps in it. The mod drives it with commands through
// AdventNative -> the exported U2BloodCommand():
//
//   pool K size gx gy       start slot K: a pool `size` world units across, floor slope gx, gy
//                           (height per unit along the texture's U and V), sheet cleared
//   pour K u v rate secs    blood pours in at (u, v) in 0..1, `rate` sheet volume/s for secs
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
	const int N = 64;                          // cells across a sheet (texture N x N)
	const float G = 3.0f;                      // gravity, sheet units (as the offline bake)
	const float Friction = 0.9f;               // per second: pow(1 - Friction, dt) on momentum (a thick liquid)
	const float Dry = 1e-4f;
	const float Cfl = 0.45f;
	const int MaxSub = 8;                      // sub-steps per frame at most

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
		float B[N * N];                            // the bed: the floor's slope
		float PourU = 0.5f, PourV = 0.5f, PourRate = 0, PourLeft = 0;
		float Still = 0;                           // seconds without motion
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
		for (int j = 0; j < N; j++)
			for (int i = 0; i < N; i++)
				S.B[j * N + i] = S.Gx * (i - N / 2) + S.Gy * (j - N / 2);
		S.PourRate = 0; S.PourLeft = 0; S.Still = 0; S.Frozen = false; S.Dirty = true;
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
			S.Used = true; S.Size = A > 1 ? A : 100; S.Gx = Bq; S.Gy = C;
			Clear(S);
			Say("blood: slot %d a pool %.0f units across, slope %.3f %.3f", K, S.Size, S.Gx, S.Gy);
			return 1;
		}
		if (!S.Used)
			return 0;
		if (!_stricmp(Word, "pour"))
		{
			S.PourU = A; S.PourV = Bq; S.PourRate = C; S.PourLeft = D; S.Frozen = false;
			return 1;
		}
		if (!_stricmp(Word, "stamp"))
		{
			// (u, v) and the radius in texture units, velocity in texture units/s -> cells
			const float cx = A * N, cy = Bq * N, r = fmaxf(E * N, 1.0f);
			const float vx = C * N, vy = D * N;
			float moved = 0; int ring = 0;
			for (int j = 0; j < N; j++)
				for (int i = 0; i < N; i++)
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
				for (int j = 0; j < N; j++)
					for (int i = 0; i < N; i++)
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
			S.Frozen = true; S.PourRate = 0;
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
		for (int c = 0; c < N * N; c++)
		{
			const float h = S.H[c];
			if (h > Dry)
			{
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
		const float dt = fminf(Dt, Cfl / smax);
		memcpy(S.H2, S.H, sizeof(S.H)); memcpy(S.Hu2, S.Hu, sizeof(S.Hu)); memcpy(S.Hv2, S.Hv, sizeof(S.Hv));
		float F[3], sL, sR;
		for (int j = 0; j < N; j++)
			for (int i = 0; i < N; i++)
			{
				const int c = j * N + i;
				if (i + 1 < N)
				{
					const int r = c + 1;
					Face(S.H[c], S.H[r], U[c], U[r], V[c], V[r], S.B[c], S.B[r], F, sL, sR);
					S.H2[c] -= dt * F[0]; S.H2[r] += dt * F[0];
					S.Hu2[c] -= dt * (F[1] - sL); S.Hu2[r] += dt * (F[1] - sR);
					S.Hv2[c] -= dt * F[2]; S.Hv2[r] += dt * F[2];
				}
				if (j + 1 < N)
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
		for (int c = 0; c < N * N; c++)
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
		if (S.PourRate <= 0 || S.PourLeft <= 0)
			return;
		const float cx = S.PourU * N, cy = S.PourV * N, r2 = 9.0f;
		const float d = fminf(dt, S.PourLeft);
		for (int j = 0; j < N; j++)
			for (int i = 0; i < N; i++)
			{
				const float dd = (i + 0.5f - cx) * (i + 0.5f - cx) + (j + 0.5f - cy) * (j + 0.5f - cy);
				if (dd < r2)
					S.H[j * N + i] += S.PourRate * d * (1 - dd / r2) / (3.14159f * r2 * 0.5f);
			}
		S.PourLeft -= d;
		if (S.PourLeft <= 0)
			S.PourRate = 0;
	}

	// the sheet as the decal texture: 50% grey where there's no blood (the decal multiplies
	// the floor x2), the blood's colour darkening with depth (the parallax rule reads darker
	// as deeper), alpha the coverage
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
				const float d = S.H[j * N + i];
				float cov = fmaxf(0.0f, fminf(1.0f, (d - 0.01f) / 0.03f));
				cov = cov * cov * (3 - 2 * cov);
				const float deep = fmaxf(0.0f, fminf(1.0f, d / 1.0f));
				const float k = 1.0f - 0.45f * deep;
				const int r = (int)(128 * (1 - cov) + 76 * k * cov + 0.5f);
				const int g = (int)(128 * (1 - cov) + 16 * k * cov + 0.5f);
				const int b = (int)(128 * (1 - cov) + 13 * k * cov + 0.5f);
				const int a = (int)(250 * cov + 0.5f);
				row[i] = (DWORD)a << 24 | r << 16 | g << 8 | b;
			}
		}
		S.Tex->UnlockRect(0);
		S.Dirty = false;
	}

	// each frame, from the device's Present: the active sheets step and re-upload
	inline void Step(IDirect3DDevice9 *Dev)
	{
		const DWORD now = GetTickCount();
		float dt = LastMs ? (now - LastMs) / 1000.0f : 1 / 60.0f;
		LastMs = now;
		dt = fmaxf(1 / 120.0f, fminf(dt, 1 / 30.0f));
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
