/**
 * Multisampled anti-aliasing for games that never asked for it (U2Shaders fork).
 *
 * "msaa=N" in System\U2Shaders.ini (2, 4 or 8; 0 or missing = off): the device's back buffer and
 * its automatic depth buffer are made multisampled in CreateDevice / Reset, where the card can do
 * it (else it stays off and the log says why). Everything the post chain does starts from a copy of
 * the back buffer, and StretchRect resolves the samples on the way.
 *
 * Direct3D 9 refuses to draw when the render target and the depth buffer differ in sampling. The
 * game draws its own off-screen pictures (shadow bitmaps, screen copies) into plain targets while
 * the multisampled depth buffer is still bound: those binds get a plain depth buffer of the target's
 * size instead (U2Shaders::MatchDepth), cleared, and the game is handed its own one back when it asks.
 *
 * The scene's depth can't be read as a texture when it is multisampled, and the cards tried copy it
 * nowhere (no RESZ, StretchRect refused), so each depth-writing draw into the stand-in is drawn a
 * second time into a plain readable depth texture (MsaaDepthBegin in u2shaders.hpp) for gi, ssao,
 * atmos and sss.
 */

#pragma once

#include <Windows.h>
#include <d3d9.h>
#include <string>
#include "fakefull.hpp"

namespace U2Msaa
{
	inline int &Samples() { static int N = -1; return N; }      // -1 not read

	inline int Wanted()
	{
		if (Samples() >= 0)
			return Samples();
		Samples() = 0;
		FILE *F = nullptr;
		if (!fopen_s(&F, (U2FakeFull::Folder() + "U2Shaders.ini").c_str(), "r") && F)
		{
			char Line[256];
			int V = 0;
			while (fgets(Line, sizeof(Line), F))
				if (sscanf_s(Line, " msaa=%d", &V) == 1)
					Samples() = V;
			fclose(F);
		}
		if (Samples() != 2 && Samples() != 4 && Samples() != 8)
			Samples() = 0;
		return Samples();
	}

	// msaashot=1 (test): the pilot's ShotP captures the game's render texture, resolved, instead
	inline bool ShotTest()
	{
		static int On = -1;
		if (On < 0)
		{
			On = 0;
			FILE *F = nullptr;
			if (!fopen_s(&F, (U2FakeFull::Folder() + "U2Shaders.ini").c_str(), "r") && F)
			{
				char Line[256];
				while (fgets(Line, sizeof(Line), F))
					if (!_strnicmp(Line, "msaashot=1", 10) || !_strnicmp(Line, "msaastamp=1", 11))
						On = 1;
				fclose(F);
			}
		}
		return On != 0;
	}

	// CreateDevice / Reset: the presentation made multisampled, if wanted and possible
	inline void Adjust(D3DPRESENT_PARAMETERS &PP, IDirect3D9 *D3D, UINT Adapter, D3DDEVTYPE Type)
	{
		const int N = Wanted();
		if (N == 0 || D3D == nullptr)
			return;
		char Text[256];
		D3DFORMAT BB = PP.BackBufferFormat;
		if (BB == D3DFMT_UNKNOWN)
		{
			D3DDISPLAYMODE DM = {};
			if (SUCCEEDED(D3D->GetAdapterDisplayMode(Adapter, &DM)))
				BB = DM.Format;
		}
		const D3DMULTISAMPLE_TYPE MS = (D3DMULTISAMPLE_TYPE)N;
		if (FAILED(D3D->CheckDeviceMultiSampleType(Adapter, Type, BB, PP.Windowed, MS, nullptr))
			|| (PP.EnableAutoDepthStencil && FAILED(D3D->CheckDeviceMultiSampleType(Adapter, Type, PP.AutoDepthStencilFormat, PP.Windowed, MS, nullptr))))
		{
			sprintf_s(Text, "msaa: %dx not available for back buffer format %u / depth %u, left off", N, (unsigned)BB, (unsigned)PP.AutoDepthStencilFormat);
			U2FakeFull::Note(Text);
			return;
		}
		PP.MultiSampleType = MS;
		PP.MultiSampleQuality = 0;
		PP.SwapEffect = D3DSWAPEFFECT_DISCARD;
		PP.Flags &= ~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
		sprintf_s(Text, "msaa: %dx on (%ux%u)", N, PP.BackBufferWidth, PP.BackBufferHeight);
		U2FakeFull::Note(Text);
	}
}
