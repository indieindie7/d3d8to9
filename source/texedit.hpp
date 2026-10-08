#pragma once
/**
 * U2TexEdit - click a surface in the game and adjust its texture live (GM panel, "Texture").
 *
 * Level 1 of games/research_notes/In-game texture editing/design.md:
 *
 *  - Pick: "Pick texture (click)" in the GM panel arms a pick; the next world click sets the cursor
 *    pixel. One frame later every on-screen draw is drawn a second time (the shotmask redraw:
 *    the game's depth still bound, Z test LESSEQUAL, Z write off) into our own target, scissored
 *    to that one pixel, writing its draw number (serial). The topmost visible draw wins as in the
 *    game's own picture; alpha-tested draws (grates) return the texture's alpha so their holes pick
 *    what is behind. The pixel is copied out at that frame's Present and read at the next one.
 *    The serial's draw table says which textures (hashes) were on stages 0-3.
 *  - Adjust: the texture is drawn once through U2Shaders\tex_adjust.hlsl into a render-target
 *    texture (each mip level from the source's own level) and that copy is swapped in for every
 *    draw of the hash, on stages 0-3, through U2Shaders::Replacement. Only the texture changes,
 *    so it works under any stage setup and with every other rule; a slider move re-bakes it.
 *  - Saved as numbers only, never pixels:
 *      U2GM journal (through the panel's command file): "gm tex HASH REV b c g s h sh sm sl sharp"
 *      becomes Ops[k]="@* tex HASH REV b c g s h sh sm sl sharp" (one slot per hash, global: keyed
 *      by the texture's content, not by map), so gm undo / gm redo step through it;
 *      U2Shaders.ini: texadjust=HASH b c g s h sh sm sl sharp (the plain route, read live).
 *    Both are watched here (file times) and re-applied after a map change (lazily, by hash).
 *
 * The numbers: b brightness (added, 0), c contrast (1), g gamma (1; >1 brightens the mids),
 * s saturation (1), h hue shift in degrees (0), sh/sm/sl curves for shadows / mids / highlights
 * (-1..1, 0), sharp sharpness (0; negative softens).
 *
 * Precedence for one hash: the panel's unsaved values > the journal's line > texadjust= in the
 * ini. The base that is adjusted: a replace= DDS or texgrade= copy when there is one (so the
 * adjustment goes on top of it), else the game's own texture. Live blood pools / wall runs
 * (blood.hpp, runs.hpp) are not adjusted. Rules keyed by hash (shader=, layer=, surface=, decal=,
 * glass=, pbr=) still match the original hash and read the adjusted copy from s0, so terrain
 * layers (layer=terrain_hex.hlsl) show the adjustment; gloss= redraws and the glass= cube-map path
 * use the original texture.
 *
 * Hooks (see texedit_hooks.md): U2Shaders::Replacement -> Adjusted(), the four draw calls ->
 * PickBegin()/PickEnd(), Present -> OnPresent(), Reset -> OnLost(), the GM panel -> PanelUi() and
 * WorldClick(). One instance: TexEd().
 */
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include "crash.hpp"
#include "imgui/imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

struct U2TexAdjust
{
	float B = 0, C = 1, G = 1, S = 1, H = 0, Sh = 0, Sm = 0, Sl = 0, Sharp = 0;

	float *Field(int i) { float *F[9] = { &B, &C, &G, &S, &H, &Sh, &Sm, &Sl, &Sharp }; return F[i]; }
	float Get(int i) const { return const_cast<U2TexAdjust *>(this)->Field(i)[0]; }
	bool Same(const U2TexAdjust &O) const
	{
		for (int i = 0; i < 9; i++)
			if (fabsf(Get(i) - O.Get(i)) > (i == 4 ? 0.05f : 0.0005f))
				return false;
		return true;
	}
	bool Neutral() const { return Same(U2TexAdjust()); }
	// "b c g s h sh sm sl sharp" (what the journal and texadjust= hold)
	void Format(char *Out, size_t Size) const
	{
		snprintf(Out, Size, "%.3f %.3f %.3f %.3f %.1f %.3f %.3f %.3f %.3f", B, C, G, S, H, Sh, Sm, Sl, Sharp);
	}
	// fewer numbers than nine: the rest stay neutral; out-of-range values are clamped
	int Parse(const char *Text)
	{
		*this = U2TexAdjust();
		const int n = sscanf_s(Text, "%f %f %f %f %f %f %f %f %f", &B, &C, &G, &S, &H, &Sh, &Sm, &Sl, &Sharp);
		Clamp();
		return n < 0 ? 0 : n;
	}
	void Clamp()
	{
		static const float Lo[9] = { -1, 0, 0.1f, 0, -180, -1, -1, -1, -1 }, Hi[9] = { 1, 4, 10, 4, 180, 1, 1, 1, 4 };
		for (int i = 0; i < 9; i++)
		{
			float &V = *Field(i);
			if (!(V == V)) V = U2TexAdjust().Get(i);   // NaN
			V = V < Lo[i] ? Lo[i] : V > Hi[i] ? Hi[i] : V;
		}
	}
};

class U2TexEdit;
inline U2TexEdit &TexEd();

class U2TexEdit
{
public:
	// ---- one texture hash that has (or may get) an adjustment
	struct Entry
	{
		U2TexAdjust Ini, Journal, Live;
		bool HasIni = false, HasJournal = false, HasLive = false;
		unsigned Rev = 0;                    // the journal line's revision
		int Slot = -1;                       // its Ops[] slot
		IDirect3DTexture9 *Tex = nullptr;    // the baked copy (D3DPOOL_DEFAULT render target, ours)
		U2TexAdjust Baked;
		bool BakedOk = false, BakedFromReplace = false;
		const void *BakedFrom = nullptr;     // only compared, never used
		IDirect3DTexture9 *Pending = nullptr; // the base to bake from at Present (a reference held)
		bool PendingReplace = false;
		bool Failed = false;                 // can't bake (format, memory): not retried until re-picked
		std::string Why;
		UINT W = 0, H = 0, Levels = 0;
		D3DFORMAT Fmt = D3DFMT_UNKNOWN;
		unsigned Uses = 0, UsesLast = 0;     // stage binds this frame / last frame

		const U2TexAdjust &Eff() const { return HasLive ? Live : HasJournal ? Journal : Ini; }
		bool Wanted() const { return (HasLive || HasJournal || HasIni) && !Eff().Neutral(); }
	};
	std::map<DWORD, Entry> Entries;
	DWORD Selected = 0;
	bool Any = false;                        // something to adjust or preview: Replacement must look
	bool Active() const { return Any; }

	std::string Dir;
	IDirect3DDevice9 *Dev = nullptr;         // the last device seen at Present (the thumbnail callback)
	unsigned Frame = 0;

	// ---- the pick
	enum { Idle, Waiting, Armed, Drawing, Reading };
	int PickState = Idle;
	float ClickX = 0, ClickY = 0, ClickW = 0, ClickH = 0;   // the click, in the panel's display units
	int PixX = 0, PixY = 0;                  // the click, in render-target pixels
	bool PickThrough = false;                // skip see-through draws (decals, particles, glass)
	struct PickRow
	{
		DWORD Hash[4];
		UINT W[4], H[4], Levels[4];
		D3DFORMAT Fmt[4];
		bool Blend, AlphaTest, ZWrite, Shader;
	};
	std::vector<PickRow> Rows;
	bool PickCleared = false, PickBroken = false;
	IDirect3DSurface9 *PickRT = nullptr, *PickSmall = nullptr, *PickSys = nullptr, *PickOldRT = nullptr;
	IDirect3DPixelShader9 *PickPS = nullptr, *PickOldPS = nullptr;
	DWORD PickOldRS[7] = {};
	RECT PickOldScissor = {};
	float PickOldC[2][4] = {};
	PickRow Picked = {};                     // the last result
	bool HavePicked = false;
	int PickedStage = 0, PickedSameHash = 0;
	std::string PickMsg;

	// ---- the bake
	IDirect3DPixelShader9 *AdjPS = nullptr;
	bool AdjTried = false;
	std::string AdjWhy;

	// ---- watched files
	FILETIME IniTime = {}, GmTime = {};
	std::string IniMap;
	bool ShowAlpha = false;

	void Log(const char *Format, ...)
	{
		if (Dir.empty())
			return;
		FILE *F = nullptr;
		if (fopen_s(&F, (Dir + "U2Shaders.log").c_str(), "a") || F == nullptr)
			return;
		fputs("texedit: ", F);
		va_list Args;
		va_start(Args, Format);
		vfprintf(F, Format, Args);
		va_end(Args);
		fputc('\n', F);
		fclose(F);
	}

	void UpdateAny()
	{
		Any = Selected != 0;
		for (const auto &It : Entries)
			if (It.second.Wanted())
				Any = true;
	}

	static bool Valid(DWORD Hash) { return Hash != 0 && Hash != 0xFFFFFFFF; }

	static const char *FormatName(D3DFORMAT F)
	{
		switch (F)
		{
		case D3DFMT_DXT1: return "DXT1";
		case D3DFMT_DXT3: return "DXT3";
		case D3DFMT_DXT5: return "DXT5";
		case D3DFMT_A8R8G8B8: return "A8R8G8B8";
		case D3DFMT_X8R8G8B8: return "X8R8G8B8";
		case D3DFMT_R5G6B5: return "R5G6B5";
		case D3DFMT_X1R5G5B5: return "X1R5G5B5";
		case D3DFMT_A1R5G5B5: return "A1R5G5B5";
		case D3DFMT_A4R4G4B4: return "A4R4G4B4";
		case D3DFMT_L8: return "L8";
		case D3DFMT_A8L8: return "A8L8";
		case D3DFMT_A8: return "A8";
		case D3DFMT_V8U8: return "V8U8 (bump)";
		case D3DFMT_P8: return "P8";
		default: return "other";
		}
	}
	// colour formats the bake reads (as a texture) and writes as A8R8G8B8
	static bool Adjustable(D3DFORMAT F)
	{
		switch (F)
		{
		case D3DFMT_DXT1: case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
		case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5:
		case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_L8: case D3DFMT_A8L8:
			return true;
		default:
			return false;
		}
	}

	// ======================================================================================
	// U2Shaders::Replacement: the texture to draw for this hash. Src = the base (a replace= /
	// texgrade= copy when Else is one, else the game's texture); Else = what Replacement would
	// return without us (nullptr = the game's own texture stays).
	IDirect3DTexture9 *Adjusted(IDirect3DDevice9 *D, IDirect3DTexture9 *Src, DWORD Hash, IDirect3DTexture9 *Else)
	{
		(void)D;
		if (!Any || Src == nullptr || !Valid(Hash))
			return Else;
		const auto It = Entries.find(Hash);
		if (It == Entries.end())
			return Else;
		Entry &E = It->second;
		E.Uses++;
		const bool Want = E.Wanted(), Preview = Hash == Selected;
		if (!Want && !Preview)
			return Else;
		const bool FromReplace = Else != nullptr;
		const bool Stale = E.Tex == nullptr || !E.BakedOk || !E.Baked.Same(E.Eff()) || FromReplace != E.BakedFromReplace
			|| (FromReplace && E.BakedFrom != (const void *)Src);
		if (Stale && !E.Failed && E.Pending == nullptr)
		{
			Src->AddRef();                   // alive until Present bakes from it
			E.Pending = Src;
			E.PendingReplace = FromReplace;
		}
		if (!Want || E.Tex == nullptr || E.Failed || !E.BakedOk)
			return Else;
		return E.Tex;                        // (while stale: the previous bake, one frame)
	}

	// ======================================================================================
	// the pick pass: after the game's draw (and U2After), on the pick frame only. Stage: the
	// device's Direct3DTexture8 (GetProxyInterface(), U2Hash); Host: U2Shaders (Known()).
	template <class Host, class Stage>
	bool PickBegin(Host &H, IDirect3DDevice9 *D, Stage *const *St)
	{
		if (PickState != Drawing || PickBroken || Rows.size() >= 65000)
			return false;
		if (Offscreen(D))
			return false;                    // shadow maps, mirrors, scripted textures
		DWORD Fvf = 0;
		D->GetFVF(&Fvf);
		if ((Fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW)
			return false;                    // the HUD and screen quads
		DWORD Blend = 0, ZWrite = 0, ATest = 0;
		D->GetRenderState(D3DRS_ALPHABLENDENABLE, &Blend);
		D->GetRenderState(D3DRS_ZWRITEENABLE, &ZWrite);
		D->GetRenderState(D3DRS_ALPHATESTENABLE, &ATest);
		if (PickThrough && Blend && !ZWrite)
			return false;
		IDirect3DVertexShader9 *VS = nullptr;
		D->GetVertexShader(&VS);
		const bool Shader = VS != nullptr;
		if (VS) VS->Release();
		if (!PickReady(D))
			return false;

		PickRow R = {};
		R.Blend = Blend != 0; R.ZWrite = ZWrite != 0; R.AlphaTest = ATest != 0; R.Shader = Shader;
		for (int s = 0; s < 4; s++)
			if (St[s] != nullptr)
			{
				IDirect3DTexture9 *T = St[s]->GetProxyInterface();
				H.Known(T, St[s]->U2Hash);
				R.Hash[s] = St[s]->U2Hash;
				D3DSURFACE_DESC L = {};
				if (T && SUCCEEDED(T->GetLevelDesc(0, &L)))
				{
					R.W[s] = L.Width; R.H[s] = L.Height; R.Fmt[s] = L.Format; R.Levels[s] = T->GetLevelCount();
				}
			}
		Rows.push_back(R);
		const DWORD Serial = (DWORD)Rows.size();

		D->GetRenderTarget(0, &PickOldRT);
		D3DVIEWPORT9 VP = {};
		D->GetViewport(&VP);
		D->SetRenderTarget(0, PickRT);       // (resets the viewport and the scissor rect)
		if (!PickCleared)
		{
			D3DSURFACE_DESC Pd = {};
			PickRT->GetDesc(&Pd);
			D3DVIEWPORT9 Full = { 0, 0, Pd.Width, Pd.Height, 0, 1 };
			D->SetViewport(&Full);
			D3DRECT C = { PixX, PixY, PixX + 1, PixY + 1 };
			D->Clear(1, &C, D3DCLEAR_TARGET, 0, 1.0f, 0);
			PickCleared = true;
		}
		D->SetViewport(&VP);
		static const D3DRENDERSTATETYPE RS[7] = { D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_COLORWRITEENABLE, D3DRS_FOGENABLE,
			D3DRS_ALPHABLENDENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_SCISSORTESTENABLE };
		for (int i = 0; i < 7; i++)
			D->GetRenderState(RS[i], &PickOldRS[i]);
		D->GetScissorRect(&PickOldScissor);
		D->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		D->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
		D->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
		D->SetRenderState(D3DRS_FOGENABLE, FALSE);
		D->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		D->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
		D->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
		RECT Sc = { PixX, PixY, PixX + 1, PixY + 1 };
		D->SetScissorRect(&Sc);
		D->GetPixelShaderConstantF(0, &PickOldC[0][0], 2);
		const float C[2][4] = {
			{ (Serial & 255) / 255.0f, ((Serial >> 8) & 255) / 255.0f, Check(Serial) / 255.0f, 1 },
			{ (ATest && St[0] != nullptr) ? 1.0f : 0.0f, 0, 0, 0 } };
		D->SetPixelShaderConstantF(0, &C[0][0], 2);
		D->GetPixelShader(&PickOldPS);
		D->SetPixelShader(PickPS);
		return true;
	}
	void PickEnd(IDirect3DDevice9 *D)
	{
		static const D3DRENDERSTATETYPE RS[7] = { D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_COLORWRITEENABLE, D3DRS_FOGENABLE,
			D3DRS_ALPHABLENDENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_SCISSORTESTENABLE };
		for (int i = 0; i < 7; i++)
			D->SetRenderState(RS[i], PickOldRS[i]);
		D->SetPixelShader(PickOldPS);
		if (PickOldPS) { PickOldPS->Release(); PickOldPS = nullptr; }
		D->SetPixelShaderConstantF(0, &PickOldC[0][0], 2);
		D3DVIEWPORT9 VP = {};
		D->GetViewport(&VP);
		D->SetRenderTarget(0, PickOldRT);
		D->SetViewport(&VP);
		D->SetScissorRect(&PickOldScissor);
		if (PickOldRT) { PickOldRT->Release(); PickOldRT = nullptr; }
	}
	static DWORD Check(DWORD Serial) { return ((Serial * 37u) ^ ((Serial >> 8) * 11u) ^ 0xA5u) & 255u; }

	static bool Offscreen(IDirect3DDevice9 *D)
	{
		IDirect3DSurface9 *T = nullptr, *BB = nullptr;
		D3DSURFACE_DESC A = {}, B = {};
		if (FAILED(D->GetRenderTarget(0, &T)) || T == nullptr)
			return true;
		T->GetDesc(&A);
		T->Release();
		if (SUCCEEDED(D->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &BB)) && BB) { BB->GetDesc(&B); BB->Release(); }
		return A.Width != B.Width || A.Height != B.Height;
	}

	static IDirect3DPixelShader9 *CompilePS(IDirect3DDevice9 *D, const std::string &Src, const char *Name, std::string &Why)
	{
		ID3DBlob *Code = nullptr, *Err = nullptr;
		IDirect3DPixelShader9 *PS = nullptr;
		HRESULT hr = D3DCompile(Src.data(), Src.size(), Name, nullptr, nullptr, "main", "ps_2_0", 0, 0, &Code, &Err);
		if (FAILED(hr) || Code == nullptr)
			Why = Err ? std::string((const char *)Err->GetBufferPointer()) : "compile failed";
		else if (FAILED(D->CreatePixelShader((const DWORD *)Code->GetBufferPointer(), &PS)))
		{
			Why = "CreatePixelShader failed";
			PS = nullptr;
		}
		if (Code) Code->Release();
		if (Err) Err->Release();
		return PS;
	}

	// the pick target: the current render target's size and multisampling (the game's depth stays bound)
	bool PickReady(IDirect3DDevice9 *D)
	{
		if (PickPS == nullptr)
		{
			static const char Src[] =
				"sampler2D s0 : register(s0);\n"
				"float4 Id : register(c0);\n"
				"float4 Opt : register(c1);\n"
				"float4 main(float2 uv : TEXCOORD0, float4 d : COLOR0) : COLOR\n"
				"{ return float4(Id.rgb, lerp(1, tex2D(s0, uv).a * d.a, Opt.x)); }\n";
			std::string Why;
			PickPS = CompilePS(D, Src, "texpick", Why);
			if (PickPS == nullptr)
			{
				PickBroken = true;
				PickMsg = "pick shader: " + Why;
				Log("%s", PickMsg.c_str());
				return false;
			}
		}
		IDirect3DSurface9 *RT = nullptr;
		if (FAILED(D->GetRenderTarget(0, &RT)) || RT == nullptr)
			return false;
		D3DSURFACE_DESC A = {};
		RT->GetDesc(&A);
		RT->Release();
		if (PickRT != nullptr)
		{
			D3DSURFACE_DESC M = {};
			PickRT->GetDesc(&M);
			if (M.Width != A.Width || M.Height != A.Height || M.MultiSampleType != A.MultiSampleType || M.MultiSampleQuality != A.MultiSampleQuality)
			{
				PickRT->Release();
				PickRT = nullptr;
				PickCleared = false;
			}
		}
		if (PickRT == nullptr && FAILED(D->CreateRenderTarget(A.Width, A.Height, D3DFMT_A8R8G8B8, A.MultiSampleType, A.MultiSampleQuality, FALSE, &PickRT, nullptr)))
		{
			PickRT = nullptr;
			PickBroken = true;
			PickMsg = "no pick target (out of video memory?)";
			Log("pick: no %ux%u target", A.Width, A.Height);
			return false;
		}
		return true;
	}

	// at Present (frame N, the pick pass drawn): the one pixel copied out of the multisampled target
	void PickResolve(IDirect3DDevice9 *D)
	{
		if (!PickCleared || Rows.empty() || PickRT == nullptr)
		{
			PickMsg = Rows.empty() ? "no 3D draws in the pick frame: click again" : "pick target lost: click again";
			PickState = Idle;
			return;
		}
		if (PickSmall == nullptr && FAILED(D->CreateRenderTarget(1, 1, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &PickSmall, nullptr)))
			PickSmall = nullptr;
		RECT R = { PixX, PixY, PixX + 1, PixY + 1 };
		if (PickSmall == nullptr || FAILED(D->StretchRect(PickRT, &R, PickSmall, nullptr, D3DTEXF_NONE)))
		{
			PickMsg = "pick: the copy out failed";
			Log("%s", PickMsg.c_str());
			PickState = Idle;
			return;
		}
		PickState = Reading;
	}
	// at the next Present: read it (a 1x1 copy; the GPU has had a frame)
	void PickRead(IDirect3DDevice9 *D)
	{
		PickState = Idle;
		if (PickSys == nullptr && FAILED(D->CreateOffscreenPlainSurface(1, 1, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &PickSys, nullptr)))
			PickSys = nullptr;
		D3DLOCKED_RECT L = {};
		if (PickSys == nullptr || PickSmall == nullptr || FAILED(D->GetRenderTargetData(PickSmall, PickSys)) || FAILED(PickSys->LockRect(&L, nullptr, D3DLOCK_READONLY)))
		{
			PickMsg = "pick: the read failed";
			Log("%s", PickMsg.c_str());
			return;
		}
		const DWORD Px = *(const DWORD *)L.pBits;
		PickSys->UnlockRect();
		const DWORD Serial = ((Px >> 16) & 255) | (((Px >> 8) & 255) << 8), Ck = Px & 255;
		if (Serial == 0 && Ck == 0)
		{
			PickMsg = "nothing textured under the cursor (sky, HUD?)";
			return;
		}
		if (Ck != Check(Serial) || Serial > Rows.size())
		{
			PickMsg = "an edge pixel (two draws mixed): click a pixel further in";
			return;
		}
		Picked = Rows[Serial - 1];
		HavePicked = true;
		PickedStage = -1;
		for (int s = 0; s < 4 && PickedStage < 0; s++)
			if (Valid(Picked.Hash[s]))
				PickedStage = s;
		if (PickedStage < 0)
		{
			HavePicked = false;
			PickMsg = Picked.Hash[0] == 0xFFFFFFFF ? "that texture can't be read (made by the game each frame): not adjustable" : "that draw has no 2D texture";
			return;
		}
		Select(Picked.Hash[PickedStage]);
		char B[160];
		snprintf(B, sizeof(B), "picked draw %u of %u: stage %d, %08x", (unsigned)Serial, (unsigned)Rows.size(), PickedStage, (unsigned)Selected);
		PickMsg = B;
		Log("%s %ux%u %s%s%s", B, Picked.W[PickedStage], Picked.H[PickedStage], FormatName(Picked.Fmt[PickedStage]),
			Picked.Blend ? " blended" : "", Picked.AlphaTest ? " alpha-tested" : "");
	}
	void Select(DWORD Hash)
	{
		Selected = Hash;
		Entry &E = Entries[Hash];
		E.Failed = false;
		PickedSameHash = 0;
		for (const PickRow &R : Rows)
			for (int s = 0; s < 4; s++)
				if (R.Hash[s] == Hash)
				{
					PickedSameHash++;
					break;
				}
		UpdateAny();
	}

	// ======================================================================================
	// the bake: Src drawn through tex_adjust.hlsl, level by level, into E.Tex
	bool AdjReady(IDirect3DDevice9 *D)
	{
		if (AdjTried)
			return AdjPS != nullptr;
		AdjTried = true;
		std::string Src;
		FILE *F = nullptr;
		if (fopen_s(&F, (Dir + "U2Shaders\\tex_adjust.hlsl").c_str(), "rb") || F == nullptr)
		{
			AdjWhy = "U2Shaders\\tex_adjust.hlsl not found";
			Log("%s", AdjWhy.c_str());
			return false;
		}
		char Buf[4096];
		size_t n;
		while ((n = fread(Buf, 1, sizeof(Buf), F)) > 0)
			Src.append(Buf, n);
		fclose(F);
		AdjPS = CompilePS(D, Src, "tex_adjust.hlsl", AdjWhy);
		if (AdjPS == nullptr)
			Log("tex_adjust.hlsl: %s", AdjWhy.c_str());
		return AdjPS != nullptr;
	}

	// the shader's constants for one level
	static void Constants(const U2TexAdjust &A, UINT W, UINT H, float C[6][4])
	{
		memset(C, 0, sizeof(float) * 24);
		C[0][0] = 1.0f / W; C[0][1] = 1.0f / H; C[0][2] = A.Sharp;
		C[1][0] = A.B; C[1][1] = A.C; C[1][2] = 1.0f / (A.G < 0.1f ? 0.1f : A.G);
		// saturation (about luma) then hue (a rotation about the grey axis)
		const float Lw[3] = { 0.299f, 0.587f, 0.114f };
		float S[3][3], R[3][3], M[3][3];
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				S[i][j] = (1 - A.S) * Lw[j] + (i == j ? A.S : 0);
		const float a = A.H * 3.14159265f / 180.0f, c = cosf(a), s = sinf(a), k = (1 - c) / 3, q = sqrtf(1.0f / 3) * s;
		R[0][0] = c + k; R[0][1] = k - q; R[0][2] = k + q;
		R[1][0] = k + q; R[1][1] = c + k; R[1][2] = k - q;
		R[2][0] = k - q; R[2][1] = k + q; R[2][2] = c + k;
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				M[i][j] = R[i][0] * S[0][j] + R[i][1] * S[1][j] + R[i][2] * S[2][j];
		for (int i = 0; i < 3; i++)
			for (int j = 0; j < 3; j++)
				C[2 + i][j] = M[i][j];
		C[5][0] = A.Sh * 0.25f; C[5][1] = A.Sm * 0.25f; C[5][2] = A.Sl * 0.25f;
	}

	void Bake(IDirect3DDevice9 *D, DWORD Hash, Entry &E)
	{
		IDirect3DTexture9 *Src = E.Pending;
		E.Pending = nullptr;
		D3DSURFACE_DESC Sd = {};
		if (FAILED(Src->GetLevelDesc(0, &Sd)))
		{
			Src->Release();
			return;
		}
		const UINT Levels = Src->GetLevelCount();
		E.W = Sd.Width; E.H = Sd.Height; E.Levels = Levels; E.Fmt = Sd.Format;
		if (!Adjustable(Sd.Format))
		{
			E.Failed = true;
			E.Why = std::string("format ") + FormatName(Sd.Format) + " is not adjusted";
			Log("%08x: %s", (unsigned)Hash, E.Why.c_str());
			Src->Release();
			return;
		}
		if (E.Tex != nullptr)
		{
			D3DSURFACE_DESC Td = {};
			E.Tex->GetLevelDesc(0, &Td);
			if (Td.Width != Sd.Width || Td.Height != Sd.Height || E.Tex->GetLevelCount() != Levels)
			{
				E.Tex->Release();
				E.Tex = nullptr;
			}
		}
		if (E.Tex == nullptr && FAILED(D->CreateTexture(Sd.Width, Sd.Height, Levels, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &E.Tex, nullptr)))
		{
			E.Tex = nullptr;
			E.Failed = true;
			E.Why = "no render-target texture (out of video memory?)";
			Log("%08x: %ux%u, %u levels: %s", (unsigned)Hash, Sd.Width, Sd.Height, Levels, E.Why.c_str());
			Src->Release();
			return;
		}
		const U2TexAdjust A = E.Eff();
		bool Ok = true;
		D->SetTexture(0, Src);
		for (UINT Lv = 0; Lv < Levels && Ok; Lv++)
		{
			IDirect3DSurface9 *Dst = nullptr;
			if (FAILED(E.Tex->GetSurfaceLevel(Lv, &Dst)) || Dst == nullptr)
			{
				Ok = false;
				break;
			}
			D3DSURFACE_DESC Ld = {};
			Dst->GetDesc(&Ld);
			D->SetRenderTarget(0, Dst);
			D3DVIEWPORT9 VP = { 0, 0, Ld.Width, Ld.Height, 0, 1 };
			D->SetViewport(&VP);
			D->SetSamplerState(0, D3DSAMP_MAXMIPLEVEL, Lv);
			float C[6][4];
			Constants(A, Ld.Width, Ld.Height, C);
			D->SetPixelShaderConstantF(0, &C[0][0], 6);
			const float x0 = -0.5f, y0 = -0.5f, x1 = Ld.Width - 0.5f, y1 = Ld.Height - 0.5f;
			const float V[4][6] = {
				{ x0, y0, 0, 1, 0, 0 }, { x1, y0, 0, 1, 1, 0 },
				{ x0, y1, 0, 1, 0, 1 }, { x1, y1, 0, 1, 1, 1 } };
			Ok = SUCCEEDED(D->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, V, sizeof(V[0])));
			Dst->Release();
		}
		D->SetTexture(0, nullptr);
		E.BakedOk = Ok;
		E.Baked = A;
		E.BakedFromReplace = E.PendingReplace;
		E.BakedFrom = Src;
		if (!Ok)
		{
			E.Failed = true;
			E.Why = "the bake draw failed";
			Log("%08x: %s", (unsigned)Hash, E.Why.c_str());
		}
		Src->Release();
	}

	// all pending bakes, in a scene of our own, the device's state put back after
	void BakePending(IDirect3DDevice9 *D)
	{
		bool Some = false;
		for (auto &It : Entries)
			if (It.second.Pending != nullptr)
				Some = true;
		if (!Some)
			return;
		if (!AdjReady(D))
		{
			for (auto &It : Entries)
				if (It.second.Pending != nullptr)
				{
					It.second.Pending->Release();
					It.second.Pending = nullptr;
					It.second.Failed = true;
					It.second.Why = AdjWhy;
				}
			return;
		}
		U2Crash::Where("texedit: baking adjusted textures");
		IDirect3DStateBlock9 *SB = nullptr;
		if (FAILED(D->CreateStateBlock(D3DSBT_ALL, &SB)) || SB == nullptr)
			return;
		IDirect3DSurface9 *OldRT = nullptr, *OldDS = nullptr;
		D->GetRenderTarget(0, &OldRT);
		D->GetDepthStencilSurface(&OldDS);
		D->SetDepthStencilSurface(nullptr);
		D->SetVertexShader(nullptr);
		D->SetPixelShader(AdjPS);
		D->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		D->SetRenderState(D3DRS_ZENABLE, FALSE);
		D->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		D->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		D->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		D->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		D->SetRenderState(D3DRS_FOGENABLE, FALSE);
		D->SetRenderState(D3DRS_STENCILENABLE, FALSE);
		D->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
		D->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
		D->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
		D->SetRenderState(D3DRS_LIGHTING, FALSE);
		D->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
		D->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
		D->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
		D->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
		D->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
		D->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
		D->SetSamplerState(0, D3DSAMP_MIPMAPLODBIAS, 0);
		D->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
		if (SUCCEEDED(D->BeginScene()))
		{
			int Done = 0;
			for (auto &It : Entries)
				if (It.second.Pending != nullptr && Done < 8)   // at most 8 per frame (a map load with many lines)
				{
					Bake(D, It.first, It.second);
					Done++;
				}
			D->EndScene();
		}
		D->SetRenderTarget(0, OldRT);
		D->SetDepthStencilSurface(OldDS);
		if (OldRT) OldRT->Release();
		if (OldDS) OldDS->Release();
		SB->Apply();
		SB->Release();
	}

	// ======================================================================================
	// the watched files: U2Shaders.ini (texadjust=) and U2GM.ini (the journal's "@* tex" lines)
	static bool Changed(const std::string &Path, FILETIME &Seen)
	{
		WIN32_FILE_ATTRIBUTE_DATA A = {};
		if (!GetFileAttributesExA(Path.c_str(), GetFileExInfoStandard, &A))
		{
			const bool Was = Seen.dwLowDateTime != 0 || Seen.dwHighDateTime != 0;
			Seen = FILETIME();
			return Was;                      // gone: its lines are gone
		}
		if (CompareFileTime(&A.ftLastWriteTime, &Seen) == 0)
			return false;
		Seen = A.ftLastWriteTime;
		return true;
	}
	static std::string ReadAll(const std::string &Path)
	{
		std::string T;
		FILE *F = nullptr;
		if (fopen_s(&F, Path.c_str(), "rb") || F == nullptr)
			return T;
		char Buf[8192];
		size_t n;
		while ((n = fread(Buf, 1, sizeof(Buf), F)) > 0 && T.size() < (4u << 20))
			T.append(Buf, n);
		fclose(F);
		return T;
	}
	template <class Fn>
	static void Lines(const std::string &Text, Fn Each)
	{
		size_t At = 0;
		while (At < Text.size())
		{
			size_t End = Text.find('\n', At);
			if (End == std::string::npos)
				End = Text.size();
			std::string L = Text.substr(At, End - At);
			At = End + 1;
			while (!L.empty() && (L.back() == '\r' || L.back() == ' ' || L.back() == '\t'))
				L.pop_back();
			const size_t S = L.find_first_not_of(" \t");
			if (S != std::string::npos)
				Each(L.substr(S));
		}
	}

	// texadjust=HASH b c g s h sh sm sl sharp; map= sections as U2Shaders::MapSkip reads them
	void ReadIni(const std::string &CurMap)
	{
		const std::string Text = ReadAll(Dir + "U2Shaders.ini");
		for (auto &It : Entries)
			It.second.HasIni = false;
		bool InMap = false, MapHit = false;
		int n = 0;
		Lines(Text, [&](const std::string &L) {
			if (_strnicmp(L.c_str(), "map=", 4) == 0)
			{
				char N[128] = "";
				sscanf_s(L.c_str() + 4, "%127s", N, (unsigned)sizeof(N));
				for (char *c = N; *c; c++)
					*c = (char)tolower((unsigned char)*c);
				InMap = strcmp(N, "*") != 0 && N[0] != 0;
				MapHit = InMap && !CurMap.empty() && CurMap.find(N) != std::string::npos;
				return;
			}
			if ((InMap && !MapHit) || _strnicmp(L.c_str(), "texadjust=", 10) != 0)
				return;
			unsigned H = 0;
			int Used = 0;
			if (sscanf_s(L.c_str() + 10, "%x%n", &H, &Used) < 1 || !Valid(H))
				return;
			Entry &E = Entries[H];
			E.Ini.Parse(L.c_str() + 10 + Used);
			E.HasIni = true;
			n++;
		});
		Log("U2Shaders.ini: %d texadjust= line(s)%s%s", n, CurMap.empty() ? "" : ", map ", CurMap.c_str());
	}

	// Ops[k]="@* tex HASH REV b c g s h sh sm sl sharp" in [U2GM.GMMaster]
	bool ReadJournal()
	{
		const std::string Text = ReadAll(Dir + "U2GM.ini");
		if (Text.empty())
			return false;                    // being written (or gone): the next look
		struct Line { U2TexAdjust A; unsigned Rev; int Slot; };
		std::map<DWORD, Line> Got;
		bool InSec = false;
		Lines(Text, [&](const std::string &L) {
			if (L[0] == '[')
			{
				InSec = _stricmp(L.c_str(), "[U2GM.GMMaster]") == 0;
				return;
			}
			if (!InSec || _strnicmp(L.c_str(), "Ops[", 4) != 0)
				return;
			const size_t Eq = L.find('=');
			if (Eq == std::string::npos)
				return;
			std::string V = L.substr(Eq + 1);
			if (V.size() >= 2 && V.front() == '"' && V.back() == '"')
				V = V.substr(1, V.size() - 2);
			if (strncmp(V.c_str(), "@* tex ", 7) != 0)
				return;
			unsigned H = 0, Rev = 0;
			int Used = 0;
			if (sscanf_s(V.c_str() + 7, "%x %u%n", &H, &Rev, &Used) < 2 || !Valid(H))
				return;
			Line Ln;
			Ln.A.Parse(V.c_str() + 7 + Used);
			Ln.Rev = Rev;
			Ln.Slot = atoi(L.c_str() + 4);
			Got[H] = Ln;
		});
		for (auto &It : Entries)
		{
			Entry &E = It.second;
			const auto G = Got.find(It.first);
			const bool Has = G != Got.end();
			const bool Moved = Has != E.HasJournal || (Has && (G->second.Rev != E.Rev || !G->second.A.Same(E.Journal)));
			if (Moved)
				E.HasLive = false;           // undo / redo / a save came back: the sliders show the journal
			E.HasJournal = Has;
			if (Has)
			{
				E.Journal = G->second.A;
				E.Rev = G->second.Rev;
				E.Slot = G->second.Slot;
			}
			else
				E.Slot = -1;
		}
		for (const auto &G : Got)
			if (Entries.find(G.first) == Entries.end())
			{
				Entry &E = Entries[G.first];
				E.Journal = G.second.A;
				E.HasJournal = true;
				E.Rev = G.second.Rev;
				E.Slot = G.second.Slot;
			}
		Log("U2GM.ini: %u texture line(s) in the journal", (unsigned)Got.size());
		return true;
	}

	// entries with nothing left (no line, not shown): their copies freed
	void Prune()
	{
		for (auto It = Entries.begin(); It != Entries.end();)
		{
			Entry &E = It->second;
			if (!E.HasIni && !E.HasJournal && !E.HasLive && It->first != Selected && E.Pending == nullptr)
			{
				if (E.Tex) E.Tex->Release();
				It = Entries.erase(It);
			}
			else
				++It;
		}
	}

	// ======================================================================================
	// Present, before U2Shaders::OnPresent (so the panel drawn there shows this frame's bakes and
	// arms the pick for the frame after next)
	template <class Host>
	void OnPresent(Host &H, IDirect3DDevice9 *D)
	{
		if (H.Dir.empty() || D == nullptr)
			return;
		U2Crash::Where("texedit at the end of a frame");
		Dir = H.Dir;
		Dev = D;
		Frame++;
		if (Frame % 10 == 3 || IniMap != H.CurMap)
		{
			bool Dirty = false;
			if (Changed(Dir + "U2Shaders.ini", IniTime) || IniMap != H.CurMap)
			{
				IniMap = H.CurMap;
				ReadIni(H.CurMap);
				Dirty = true;
			}
			FILETIME Was = GmTime;
			if (Changed(Dir + "U2GM.ini", GmTime))
			{
				if (ReadJournal())
					Dirty = true;
				else
					GmTime = Was;            // look again next time
			}
			if (Dirty)
			{
				Prune();
				UpdateAny();
			}
		}
		for (auto &It : Entries)
		{
			It.second.UsesLast = It.second.Uses;
			It.second.Uses = 0;
		}
		switch (PickState)
		{
		case Armed:
		{
			// the click in render-target pixels (the panel's display can differ from the back buffer)
			IDirect3DSurface9 *BB = nullptr;
			D3DSURFACE_DESC B = {};
			if (SUCCEEDED(D->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &BB)) && BB) { BB->GetDesc(&B); BB->Release(); }
			const float Sx = ClickW > 0 ? B.Width / ClickW : 1, Sy = ClickH > 0 ? B.Height / ClickH : 1;
			PixX = (int)(ClickX * Sx);
			PixY = (int)(ClickY * Sy);
			if (B.Width == 0 || PixX < 0 || PixY < 0 || PixX >= (int)B.Width || PixY >= (int)B.Height)
			{
				PickMsg = "the click was outside the picture";
				PickState = Idle;
				break;
			}
			Rows.clear();
			PickCleared = false;
			PickState = Drawing;             // the next frame's draws go into the pick target too
			break;
		}
		case Drawing:
			PickResolve(D);
			break;
		case Reading:
			PickRead(D);
			break;
		default:
			break;
		}
		BakePending(D);
		U2Crash::Where("the game's own code or draws (between frames)");
	}

	// Reset: DEFAULT-pool objects go (the bakes are remade the next time each texture is drawn)
	void OnLost()
	{
		for (auto &It : Entries)
		{
			Entry &E = It.second;
			if (E.Tex) { E.Tex->Release(); E.Tex = nullptr; }
			if (E.Pending) { E.Pending->Release(); E.Pending = nullptr; }
			E.BakedOk = false;
		}
		if (PickRT) { PickRT->Release(); PickRT = nullptr; }
		if (PickSmall) { PickSmall->Release(); PickSmall = nullptr; }
		if (PickSys) { PickSys->Release(); PickSys = nullptr; }
		if (PickState == Drawing || PickState == Reading || PickState == Armed)
		{
			PickState = Idle;
			PickMsg = "the device was reset during the pick: click again";
		}
		PickCleared = false;
	}

	// ======================================================================================
	// the GM panel. WorldClick: from GmPanelDraw in place of GmWorldClick while a pick waits for
	// its click (true = the click is ours, GmWorldClick is skipped)
	bool WorldClick()
	{
		if (PickState != Waiting)
			return false;
		ImGuiIO &io = ImGui::GetIO();
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
		{
			PickState = Idle;
			PickMsg = "pick cancelled";
			return true;
		}
		ImGui::GetForegroundDrawList()->AddText(ImVec2(io.MousePos.x + 14, io.MousePos.y + 10), IM_COL32(255, 220, 90, 255), "pick a texture (Esc cancels)");
		if (ImGui::IsMouseClicked(0) && !io.WantCaptureMouse)
		{
			ClickX = io.MousePos.x;
			ClickY = io.MousePos.y;
			ClickW = io.DisplaySize.x;
			ClickH = io.DisplaySize.y;
			PickState = Armed;
			PickMsg = "picking...";
		}
		return true;
	}

	static void OpaqueImage(const ImDrawList *, const ImDrawCmd *)
	{
		if (TexEd().Dev != nullptr)
			TexEd().Dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	}

	// the "Texture" section, inside the GM panel's window. Host: U2Shaders (GmSend, Dir)
	template <class Host>
	void PanelUi(Host &H)
	{
		const ImVec4 Warn(1.0f, 0.65f, 0.25f, 1.0f);
		if (!ImGui::CollapsingHeader("Texture (click to alter)", ImGuiTreeNodeFlags_DefaultOpen))
			return;
		ImGui::PushID("texedit");
		if (PickState == Waiting)
		{
			if (ImGui::Button("Cancel pick"))
			{
				PickState = Idle;
				PickMsg = "pick cancelled";
			}
			ImGui::SameLine();
			ImGui::TextColored(Warn, "click a surface (Esc cancels)");
		}
		else if (PickState == Idle)
		{
			if (ImGui::Button("Pick texture (click)"))
			{
				PickState = Waiting;
				PickMsg.clear();
			}
			ImGui::SameLine();
			ImGui::Checkbox("through decals", &PickThrough);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("skip see-through draws (decals, particles, glass) and pick what is behind them");
		}
		else
			ImGui::TextDisabled("picking...");
		if (!PickMsg.empty())
			ImGui::TextWrapped("%s", PickMsg.c_str());
		if (PickBroken)
			ImGui::TextColored(Warn, "picking is off (see U2Shaders.log)");

		if (HavePicked)
		{
			// the picked draw's stages: choose which texture to edit
			for (int s = 0; s < 4; s++)
			{
				if (Picked.Hash[s] == 0)
					continue;
				char B[96];
				snprintf(B, sizeof(B), "stage %d  %08x  %ux%u %s", s, (unsigned)Picked.Hash[s], Picked.W[s], Picked.H[s], FormatName(Picked.Fmt[s]));
				if (!Valid(Picked.Hash[s]))
					ImGui::TextDisabled("stage %d  unreadable (not adjustable)", s);
				else if (ImGui::RadioButton(B, Selected == Picked.Hash[s]))
				{
					PickedStage = s;
					Select(Picked.Hash[s]);
				}
			}
			ImGui::TextDisabled("%s%s%s", Picked.Blend ? "blended " : "", Picked.AlphaTest ? "alpha-tested " : "", Picked.Shader ? "vertex shader" : "");
		}

		const auto It = Entries.find(Selected);
		if (Selected == 0 || It == Entries.end())
		{
			ImGui::TextDisabled("no texture picked");
			ImGui::PopID();
			return;
		}
		Entry &E = It->second;
		ImGui::Separator();
		const UINT W = E.W ? E.W : (HavePicked ? Picked.W[PickedStage] : 0), Hh = E.H ? E.H : (HavePicked ? Picked.H[PickedStage] : 0);
		ImGui::Text("%08x  %ux%u  %s  %u mips", (unsigned)Selected, W, Hh, FormatName(E.W ? E.Fmt : (HavePicked ? Picked.Fmt[PickedStage] : D3DFMT_UNKNOWN)),
			E.Levels ? E.Levels : (HavePicked ? Picked.Levels[PickedStage] : 0));
		ImGui::Text("used by %u draw(s) last frame%s", E.UsesLast, PickedSameHash ? "" : "");
		if (PickedSameHash > 0)
		{
			ImGui::SameLine();
			ImGui::TextDisabled("(%d in the pick frame)", PickedSameHash);
		}
		if (E.Failed)
			ImGui::TextColored(Warn, "not adjustable: %s", E.Why.c_str());

		// the thumbnail: the adjusted copy (also made while neutral, for this preview)
		if (E.Tex != nullptr && E.BakedOk && W > 0 && Hh > 0)
		{
			const float Box = 160.0f;
			const float Sc = (std::min)(Box / W, Box / Hh);
			if (!ShowAlpha)
				ImGui::GetWindowDrawList()->AddCallback(OpaqueImage, nullptr);
			ImGui::Image((ImTextureID)(intptr_t)E.Tex, ImVec2(W * Sc, Hh * Sc));
			if (!ShowAlpha)
				ImGui::GetWindowDrawList()->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
			ImGui::SameLine();
			ImGui::Checkbox("alpha", &ShowAlpha);
		}
		else if (!E.Failed)
			ImGui::TextDisabled("(the preview appears once the texture is drawn)");

		// the sliders: the panel's own values while edited, else what the journal / ini say
		U2TexAdjust A = E.Eff();
		bool Moved = false;
		ImGui::PushItemWidth(200);
		Moved |= ImGui::SliderFloat("brightness", &A.B, -0.5f, 0.5f, "%.3f");
		Moved |= ImGui::SliderFloat("contrast", &A.C, 0.0f, 2.0f, "%.3f");
		Moved |= ImGui::SliderFloat("gamma", &A.G, 0.2f, 3.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
		Moved |= ImGui::SliderFloat("saturation", &A.S, 0.0f, 2.0f, "%.3f");
		Moved |= ImGui::SliderFloat("hue shift", &A.H, -180.0f, 180.0f, "%.0f deg");
		Moved |= ImGui::SliderFloat("shadows", &A.Sh, -1.0f, 1.0f, "%.3f");
		Moved |= ImGui::SliderFloat("mids", &A.Sm, -1.0f, 1.0f, "%.3f");
		Moved |= ImGui::SliderFloat("highlights", &A.Sl, -1.0f, 1.0f, "%.3f");
		Moved |= ImGui::SliderFloat("sharpness", &A.Sharp, -1.0f, 2.0f, "%.3f");
		ImGui::PopItemWidth();
		if (Moved)
		{
			A.Clamp();
			E.Live = A;
			E.HasLive = true;
			E.Failed = false;
			UpdateAny();
		}
		if (ImGui::Button("Reset"))
		{
			E.Live = U2TexAdjust();
			E.HasLive = true;
			E.Failed = false;
			UpdateAny();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("all sliders neutral (Save to keep it)");
		ImGui::SameLine();
		if (ImGui::Button("Undo"))
			H.GmSend("undo");
		ImGui::SameLine();
		if (ImGui::Button("Redo"))
			H.GmSend("redo");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("the GM journal's undo / redo (the last journal change, whatever it was)");
		ImGui::SameLine();
		const bool Unsaved = E.HasLive && !(E.HasJournal && E.Live.Same(E.Journal));
		if (ImGui::Button("Save"))
		{
			char N[160];
			E.Eff().Format(N, sizeof(N));
			H.GmSend("tex %08x %u %s", (unsigned)Selected, E.Rev + 1, N);
		}
		ImGui::SameLine();
		if (ImGui::Button("Copy ini line"))
		{
			char N[160], L[200];
			E.Eff().Format(N, sizeof(N));
			snprintf(L, sizeof(L), "texadjust=%08x %s", (unsigned)Selected, N);
			ImGui::SetClipboardText(L);
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("texadjust=HASH ... to the clipboard, for U2Shaders.ini");
		if (Unsaved)
			ImGui::TextColored(Warn, "unsaved: live only (Save puts it in the journal)");
		else if (E.HasJournal)
			ImGui::TextDisabled("journal rev %u (Ops[%d])", E.Rev, E.Slot);
		else if (E.HasIni)
			ImGui::TextDisabled("from texadjust= in U2Shaders.ini");
		else
			ImGui::TextDisabled("not adjusted");
		if (E.HasIni && (E.HasJournal || E.HasLive))
			ImGui::TextDisabled("(a texadjust= line for it is overridden)");
		if (!AdjWhy.empty())
			ImGui::TextColored(Warn, "%s", AdjWhy.c_str());
		ImGui::PopID();
	}
};

inline U2TexEdit &TexEd()
{
	static U2TexEdit T;
	return T;
}
