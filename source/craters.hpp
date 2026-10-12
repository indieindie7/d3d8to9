// U2Craters: SDF craters (Advent Rising's AdventMod, BACKLOG graphics 15).
//
// Holes that read as dug into walls and floors without touching the game's geometry or
// collision. The mod sends each hole as a sphere in world units; a screen pass (craters.hlsl,
// craters= in U2Shaders.ini) finds the pixels of the surface inside a sphere, marches the view
// ray through the union of the spheres to where it leaves them, and draws the inside there:
// the wall's own colour, shaded by the inside's normal and darkened with depth. Visual only:
// no see-through holes, the game's collision stays as it was (Mike Turitzin's lesson: the
// visible hole and the physics hole are separate things).
//
//   crater x y z nx ny nz r lift   a hole on the surface at (x,y,z) with normal n: a sphere of
//                                  radius r whose centre sits `lift` units off the surface
//                                  (lift 0 = a half sphere; lift 0.4r = a shallower bowl)
//   craterclear                    forget every hole (a new level)
#pragma once
#include <d3d9.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace U2Craters
{
	const int Max = 128;                       // holes kept (oldest replaced first)
	const int Draw = 16;                       // holes the pass sees at once: the nearest in view

	struct Hole { float P[3], N[3], R, Lift; };
	Hole Holes[Max];
	int Count = 0, Next = 0;
	int Added = 0;                             // for the log

	inline bool Handles(const char *Word)
	{
		return !strcmp(Word, "crater") || !strcmp(Word, "craterclear");
	}
	inline int Command(const char *Cmd)
	{
		char Word[16] = "";
		sscanf_s(Cmd, "%15s", Word, (unsigned)sizeof(Word));
		if (!strcmp(Word, "craterclear"))
		{
			Count = Next = 0;
			return 1;
		}
		Hole H = {};
		if (sscanf_s(Cmd, "%*s %f %f %f %f %f %f %f %f", &H.P[0], &H.P[1], &H.P[2], &H.N[0], &H.N[1], &H.N[2], &H.R, &H.Lift) < 7)
			return 0;
		const float L = sqrtf(H.N[0] * H.N[0] + H.N[1] * H.N[1] + H.N[2] * H.N[2]);
		if (L < 1e-3f || H.R <= 0.5f)
			return 0;
		for (int i = 0; i < 3; i++)
			H.N[i] /= L;
		H.R = (std::min)(H.R, 400.0f);
		Holes[Next] = H;
		Next = (Next + 1) % Max;
		Count = (std::min)(Count + 1, Max);
		Added++;
		return 1;
	}

	// the nearest holes in front of the camera, into the pass's constants: per hole two float4s,
	// (centre in camera space, r) and (normal in camera space, lift). Returns how many.
	inline int Select(const D3DMATRIX &V, float Out[Draw * 8], float Far)
	{
		struct Pick { float D; int I; float C[3], N[3]; } Picks[Max];
		int n = 0;
		for (int i = 0; i < Count; i++)
		{
			const Hole &H = Holes[i];
			float C[3], N[3];
			const float W[3] = { H.P[0] + H.N[0] * H.Lift, H.P[1] + H.N[1] * H.Lift, H.P[2] + H.N[2] * H.Lift };
			for (int k = 0; k < 3; k++)
			{
				C[k] = W[0] * V.m[0][k] + W[1] * V.m[1][k] + W[2] * V.m[2][k] + V.m[3][k];
				N[k] = H.N[0] * V.m[0][k] + H.N[1] * V.m[1][k] + H.N[2] * V.m[2][k];
			}
			if (C[2] < -H.R)
				continue;                      // behind the camera
			const float D = sqrtf(C[0] * C[0] + C[1] * C[1] + C[2] * C[2]);
			if (D - H.R > Far)
				continue;
			// roughly inside the view cone (90 degrees either way is plenty for any game fov)
			if (C[2] + H.R < 0.5f * (fabsf(C[0]) + fabsf(C[1])) - 2.0f * H.R)
				continue;
			Pick &p = Picks[n++];
			p.D = D; p.I = i;
			memcpy(p.C, C, sizeof(C));
			memcpy(p.N, N, sizeof(N));
		}
		std::sort(Picks, Picks + n, [](const Pick &a, const Pick &b) { return a.D < b.D; });
		n = (std::min)(n, Draw);
		for (int i = 0; i < n; i++)
		{
			const Hole &H = Holes[Picks[i].I];
			float *o = Out + i * 8;
			o[0] = Picks[i].C[0]; o[1] = Picks[i].C[1]; o[2] = Picks[i].C[2]; o[3] = H.R;
			o[4] = Picks[i].N[0]; o[5] = Picks[i].N[1]; o[6] = Picks[i].N[2]; o[7] = H.Lift;
		}
		return n;
	}
}
