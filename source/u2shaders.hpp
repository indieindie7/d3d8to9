/**
 * U2Shaders - replace the fixed-function look of chosen surfaces with HLSL pixel shaders.
 *
 * A surface is chosen by the texture bound to stage 0 when it is drawn (a hash of the top
 * mip level). Settings come from U2Shaders.ini next to the game executable:
 *
 *     glass=HASH file.hlsl        like shader=, but also for solid draws (refractive goo)
 *     log=1                       list alpha-blended textures in U2Shaders.log and
 *                                 save each one to U2Shaders\dump\<hash>_<w>x<h>.dds
 *     tint=1a2b3c4d               draw everything using that texture in flat magenta
 *     zwrite=1a2b3c4d [ref]       alpha-blended draws of that texture also write depth where their
 *                                 alpha > ref (default 24): smoke out at sea stops being painted over
 *                                 by the translucent sea surface drawn after it (Avalon Q59)
 *     shader=1a2b3c4d core.hlsl   draw it with U2Shaders\core.hlsl (entry "main", ps_2_a)
 *     layer=1a2b3c4d terrain_hex.hlsl   bind only the shader: stages, coordinates, transforms,
 *                                 samplers and blending stay the game's (Unreal II terrain layers)
 *     decal=1a2b3c4d decal_parallax.hlsl
 *                                 the same, but the draw keeps its own blending (no frame
 *                                 copy in s1): the shader returns what the texture would
 *                                 have, so overlapping decals still layer
 *     gloss=1a2b3c4d blood_gloss.hlsl
 *                                 after the game draws that texture (a decal), the same
 *                                 geometry is drawn once more with the shader, added on top
 *                                 (ONE/ONE): wet highlights on blood that the game multiplies
 *                                 into the floor. See GlossBegin; glossfx= / glossenv= tune it
 *     surface=1a2b3c4d world_parallax.hlsl
 *                                 a solid surface (wall, floor) drawn with parallax: the
 *                                 shader redoes the texture stages (texture x vertex colour
 *                                 x lightmap in s1, see SurfaceBegin); the lightmap and the
 *                                 texture's panning are left as they are. c2 = how the stages
 *                                 combine, c3 = the texture's brightness levels
 *     replace=1a2b3c4d my.dds     draw U2Shaders\my.dds wherever that texture is used, on any
 *                                 of stages 0-3 (lightmaps too); a DDS, 32-bit or DXT1/3/5,
 *                                 with its own mips (see Replacement)
 *     charlight=1                 light lit solid draws (characters, weapons) per pixel with
 *                                 char_light.hlsl: the game's own D3D lights, softer wrap,
 *                                 ambient lighter from above, rim (see CharBegin)
 *     lmcapture=1                 record the lightmapped geometry as it is drawn, for baking
 *                                 elsewhere: U2Shaders\capture\scene.obj + lightmaps.txt,
 *                                 lightmaps saved in U2Shaders\dump (see CaptureDraw)
 *     post=1                      bloom, sharpening and colour grading before the HUD
 *                                 (bloom=, grade=, colour=, sharpen=, postsplit=, postdebug=;
 *                                 see PostCheck)
 *     charprobe=1                 record how opaque on-screen draws (characters) are lit, in
 *                                 U2Shaders\dump\chars.txt, and whether scene depth can be
 *                                 read as a texture (U2Shaders.log; logging only, see ProbeDraw)
 *
 * What a shader gets:
 *     s0            the original texture, TEXCOORD0 = its (possibly panned) coordinates
 *     s1            a copy of the frame so far (for refraction)
 *     TEXCOORD1     camera-space normal      (only on fixed-function draws, see c0.y)
 *     TEXCOORD2     camera-space position
 *     COLOR0        the vertex lighting
 *     c0            (time in seconds, 1 if normals/positions are valid, 1/width, 1/height)
 *     c1.x          1 if TEXCOORD0 is projected (projector decals): divide .xy by .z
 *     c1.yz         decal= rules: how the draw blends (y: 0 alpha, 1 multiply, 2 multiply x2) and
 *                   the texture's neutral brightness for it (z: 1, or 0.5 for x2); see DecalBlend
 *     c4..c7        the projection matrix (rows), to turn a position into a screen place
 *
 * While a shader draws, alpha blending is off: the shader has the frame behind it in s1
 * and returns the finished colour.
 */

#pragma once

#include "msaa.hpp"
extern "C" __declspec(dllexport) int __cdecl U2BloodCommand(const char *Cmd);   // d3d8to9_device.cpp: the mod's blood commands (gorelink=)
#include "blood.hpp"
#include "runs.hpp"
#include "streaks.hpp"
#include "strings.hpp"
#include "lens.hpp"
#include <d3dcompiler.h>
#include "fakefull.hpp"
#include "crash.hpp"
#include "perf.hpp"
#include "texedit.hpp"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx9.h"
#include "imgui/imgui_impl_win32.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

// sketch=1: the PNG writer (stb_image_write v1.16, public domain / MIT, vendored in source/)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#pragma warning(push, 0)
#pragma warning(disable : 4996)   // stb uses sprintf (an error under /sdl)
#include "stb_image_write.h"
#pragma warning(pop)

#pragma comment(lib, "d3dcompiler.lib")

// gmpanel=1 (the GM panel): ImGui's win32 backend takes the game window's messages through this
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

struct U2TexInfo
{
	UINT W = 0, H = 0;
	D3DFORMAT Fmt = D3DFMT_UNKNOWN;
	unsigned Draws = 0;
};

static const DWORD CubeOnlyHash = 0xC0BE0001;   // glass=c0be0001: draws with only a cube map on stage 0

struct U2Rule
{
	DWORD Hash = 0;
	std::string File;          // empty = the built-in tint
	bool KeepBlend = false;    // decal=: the draw keeps its own blending, no frame copy
	bool Layer = false;        // layer=: only the pixel shader is bound; every stage, coordinate,
	                           // transform, sampler and the blending stay the game's (terrain layers)
	bool Surface = false;      // surface=: solid draws only, see SurfaceBegin
	bool Solid = false;        // glass=: a shader= rule that also takes solid (unblended) draws
	bool PbrBlend = false;     // pbr= with its own shader: alpha-blended draws too (Advent's faces, Seekers)
	bool Pbr = false;          // pbr=: a lit solid draw shaded by char_pbr.hlsl with a material map
	std::string MapFile;       // pbr=: the map (normal xy, roughness, metallic), a DDS in U2Shaders
	IDirect3DTexture9 *Map = nullptr;
	bool MapTried = false;
	bool Refused = false;      // surface=: an unsupported stage setup was logged once (layer=: the stage setup was)
	float Levels[4] = {};      // surface=: the texture's brightness levels (see TextureLevels)
	IDirect3DPixelShader9 *PS = nullptr;
	bool Tried = false;
};

class U2Shaders;
class U2Shaders
{
public:
	bool Loaded = false, Log = false, LogSolid = false, StageLog = false;
	DWORD Aniso = 0;            // aniso=N: smooth texture filtering forced to N x anisotropic (0 = the game's)
	std::string Dir;           // "<exe dir>\"
	std::map<DWORD, U2TexInfo> Seen;
	std::vector<U2Rule> Rules;
	unsigned Frame = 0, SceneFrame = ~0u, LogFrame = 0;

	IDirect3DTexture9 *SceneTex = nullptr;
	UINT SceneW = 0, SceneH = 0;
	D3DFORMAT SceneFmt = D3DFMT_UNKNOWN;

	// state put back after a shader draw
	IDirect3DPixelShader9 *OldPS = nullptr;
	IDirect3DBaseTexture9 *OldTex1 = nullptr;
	DWORD OldTCI[3] = {}, OldTTF[3] = {}, OldBlend = 0, OldSamp1[5] = {};
	D3DMATRIX OldTexMat[3] = {};
	float OldConst[8][4] = {};

	void Message(const char *Format, ...)
	{
		FILE *F = nullptr;
		if (fopen_s(&F, (Dir + "U2Shaders.log").c_str(), "a") || F == nullptr)
			return;
		va_list Args;
		va_start(Args, Format);
		vfprintf(F, Format, Args);
		va_end(Args);
		fputc('\n', F);
		fclose(F);
	}

	// one line of U2Shaders.ini: a setting or a rule (Load, and ReloadRules when the file changes
	// while the game runs; Reload: lines that only make sense once are left alone)
	void ParseLine(char *Line, bool Reload)
	{
		char Name[256] = {};
		unsigned Hash = 0;
		if (sscanf_s(Line, " log=%u", &Hash) == 1)
		{
			Log = Hash != 0;
			LogSolid = Hash >= 2;      // log=2: solid textures too (to find a glass= hash)
		}
		else if (sscanf_s(Line, " pcss=%u", &Hash) == 1)
		{
			Pcss = Hash != 0;
			MapRule.File = "pcss_map.hlsl";
			ProjRule.File = "pcss_proj.hlsl";
		}
		else if (sscanf_s(Line, " pcssparams=%f %f %f %f", &PcssParams[0], &PcssParams[1], &PcssParams[2], &PcssParams[3]) == 4)
			;
		else if (sscanf_s(Line, " pcssdebug=%f", &PcssDebug) == 1)
			;
		else if (sscanf_s(Line, " rtdump=%d", &RtDump) == 1)
			;
		else if (sscanf_s(Line, " shotmask=%u", &Hash) == 1)
			ShotMask = Hash != 0;
		else if (sscanf_s(Line, " live=%u", &Hash) == 1)
			LiveOn = Hash != 0;
		else if (sscanf_s(Line, " gorelink=%u", &Hash) == 1)
			GoreLinkOn = Hash != 0;
		else if (sscanf_s(Line, " gmterrain=%u", &Hash) == 1)
			GmTerrainOn = Hash != 0;
		else if (sscanf_s(Line, " gmpanelscale=%f", &GmPanelScale) == 1)
			;
		else if (sscanf_s(Line, " gmpanel=%u", &Hash) == 1)
			GmPanelMode = (int)Hash;
		else if (_strnicmp(Line + strspn(Line, " 	"), "sketchkey=", 10) == 0)
		{
			// sketchkey=F8 (F1..F24) or a virtual-key code (decimal or 0x..)
			char K[32] = "";
			unsigned V = 0;
			sscanf_s(Line + strspn(Line, " 	") + 10, "%31s", K, (unsigned)sizeof(K));
			if ((K[0] == 'F' || K[0] == 'f') && isdigit((unsigned char)K[1]) && (V = (unsigned)atoi(K + 1)) >= 1 && V <= 24)
				SketchKey = VK_F1 + V - 1;
			else if (sscanf_s(K, "%i", &V) == 1 && V > 0 && V < 256)
				SketchKey = V;
		}
		else if (sscanf_s(Line, " sketchhud=%u", &Hash) == 1)
			SketchHud = Hash != 0;
		else if (sscanf_s(Line, " sketch=%u", &Hash) == 1)
			SketchOn = Hash != 0;
		else if (sscanf_s(Line, " gloss=%x %255s", &Hash, Name, (unsigned)sizeof(Name)) == 2)
		{
			U2Rule R;
			R.Hash = Hash;
			R.File = Name;
			GlossRules.push_back(R);
		}
		else if (sscanf_s(Line, " glossfx=%f %f %f %f", &GlossFx[0], &GlossFx[1], &GlossFx[2], &GlossFx[3]) >= 1)
			;
		else if (strncmp(Line + strspn(Line, " \t"), "sheen=", 6) == 0)
		{
			U2Rule R;
			float L[3] = { 0, 0, 0 };
			if (sscanf_s(Line, " sheen=%x %255s %f %f %f", &Hash, Name, (unsigned)sizeof(Name), &L[0], &L[1], &L[2]) >= 2)
			{
				R.Hash = Hash;
				R.File = Name;
				R.Levels[0] = L[0]; R.Levels[1] = L[1]; R.Levels[2] = L[2];
				SheenRules.push_back(R);
			}
		}
		else if (sscanf_s(Line, " sheenfx=%f %f %f %f", &SheenFx[0], &SheenFx[1], &SheenFx[2], &SheenFx[3]) >= 1)
			;
		else if (sscanf_s(Line, " sheenenv=%f %f %f %f", &SheenEnv[0], &SheenEnv[1], &SheenEnv[2], &SheenEnv[3]) >= 1)
			;
		else if (sscanf_s(Line, " glossenv=%f %f %f %f", &GlossEnv[0], &GlossEnv[1], &GlossEnv[2], &GlossEnv[3]) >= 3)
			;
		else if (sscanf_s(Line, " glossdry=%f %f %f %f", &GlossDry[0], &GlossDry[1], &GlossDry[2], &GlossDry[3]) >= 2)
			;
		else if (sscanf_s(Line, " glossreflect=%f %f", &GlossReflect[0], &GlossReflect[1]) >= 1)
			;
		else if (sscanf_s(Line, " streakparams=%f %f %f %f", &U2Streaks::Params[0], &U2Streaks::Params[1], &U2Streaks::Params[2], &U2Streaks::Params[3]) >= 1)
			;
		else if (sscanf_s(Line, " streakfx=%f %f %f %f", &U2Streaks::Fx[0], &U2Streaks::Fx[1], &U2Streaks::Fx[2], &U2Streaks::Fx[3]) >= 1)
			;
		else if (sscanf_s(Line, " streaks=%u", &Hash) == 1)
			U2Streaks::On = Hash != 0;        // blood running down characters (streaks.hpp)
		else if (sscanf_s(Line, " handsparams=%f %f %f %f", &U2Streaks::HandsParams[0], &U2Streaks::HandsParams[1], &U2Streaks::HandsParams[2], &U2Streaks::HandsParams[3]) >= 1)
			;
		else if (sscanf_s(Line, " hands=%u", &Hash) == 1)
			U2Streaks::HandsOn = Hash != 0;   // blood on the player's hands and weapon (streaks.hpp)
		else if (sscanf_s(Line, " lensparams=%f %f %f %f", &U2Lens::Params[0], &U2Lens::Params[1], &U2Lens::Params[2], &U2Lens::Params[3]) >= 1)
			;
		else if (sscanf_s(Line, " lensfx=%f %f %f %f", &U2Lens::Fx[0], &U2Lens::Fx[1], &U2Lens::Fx[2], &U2Lens::Fx[3]) >= 1)
			;
		else if (sscanf_s(Line, " lens=%u", &Hash) == 1)
			U2Lens::On = Hash != 0;           // blood drops on the lens (lens.hpp; needs post=1)
		else if (sscanf_s(Line, " stringparams=%f %f %f %f", &U2Strings::Params[0], &U2Strings::Params[1], &U2Strings::Params[2], &U2Strings::Params[3]) >= 1)
			;
		else if (sscanf_s(Line, " stringfx=%f %f %f %f", &U2Strings::Fx[0], &U2Strings::Fx[1], &U2Strings::Fx[2], &U2Strings::Fx[3]) >= 1)
			;
		else if (sscanf_s(Line, " strings=%u", &Hash) == 1)
			U2Strings::On = Hash != 0;        // goo strings between body parts (strings.hpp)
		else if (sscanf_s(Line, " relight=%u", &Hash) == 1)
			Relight = Hash != 0;
		else if (sscanf_s(Line, " pcssprobe=%u", &Hash) == 1)
			PcssProbe = (int)Hash;
		else if (sscanf_s(Line, " psreplace=%x %255s", &Hash, Name, (unsigned)sizeof(Name)) == 2)
		{
			// a pixel shader the game made (D3D8 tokens, hashed in CreatePixelShader) drawn with
			// ours instead: Advent's terrain (PS_Terrain3Layer/4Layer) -> terrain3/4.hlsl
			U2Rule R;
			R.Hash = Hash;
			R.File = Name;
			PsReplace[Hash] = R;
		}
		else if (sscanf_s(Line, " pslog=%u", &Hash) == 1 && (PsLogCode = Hash >= 2, true))
			PsLog = Hash != 0;
		else if (sscanf_s(Line, " shadowtint=%f %f %f", &ShadowTint[0], &ShadowTint[1], &ShadowTint[2]) == 3)
			;
		else if (sscanf_s(Line, " charprobe=%u", &Hash) == 1)
			CharProbe = Hash != 0;
		else if (sscanf_s(Line, " lmcapture=%u", &Hash) == 1)
			Capture = Hash != 0;
		else if (sscanf_s(Line, " charlight=%u", &Hash) == 1)
		{
			CharLight = Hash != 0;
			CharRule.File = "char_light.hlsl";
		}
		else if (sscanf_s(Line, " post=%u", &Hash) == 1)
		{
			Post = Hash != 0;
			PostBright.File = "post_bright.hlsl";
			PostBlur.File = "post_blur.hlsl";
			PostFinal.File = "post_final.hlsl";
			PostDown.File = "post_down.hlsl";
			PostUp.File = "post_up.hlsl";
		}
		else if (_strnicmp(Line + strspn(Line, " \t"), "posthud=z0", 10) == 0)
			PostHudZ0 = true;
		else if (sscanf_s(Line, " posttrace=%u", &Hash) == 1)
			PostTrace = (int)Hash;
		else if (sscanf_s(Line, " postdebug=%u", &Hash) == 1)
			PostDebug = Hash != 0;
		else if (sscanf_s(Line, " postsplit=%u", &Hash) == 1)
			PostSplit = Hash != 0 ? 1.0f : 0.0f;
		else if (sscanf_s(Line, " bloom=%f %f", &PostBloom[0], &PostBloom[1]) == 2)
			;
		else if (sscanf_s(Line, " colorblind=%f %f", &PostBloom[2], &PostBloom[3]) >= 1)
			;  // post_final: c1.z = 1 protanopia, 2 deuteranopia, 3 tritanopia; c1.w = strength
		else if (sscanf_s(Line, " grade=%f %f %f %f", &PostGrade[0], &PostGrade[1], &PostGrade[2], &PostGrade[3]) == 4)
			;
		else if (sscanf_s(Line, " colour=%f %f %f", &PostBalance[0], &PostBalance[1], &PostBalance[2]) == 3)
			;
		else if (sscanf_s(Line, " sharpen=%f", &PostBalance[3]) == 1)
			;
		else if (sscanf_s(Line, " postfx=%f %f %f %f", &PostFx[0], &PostFx[1], &PostFx[2], &PostFx[3]) >= 1)
			;
		else if (strncmp(Line + strspn(Line, " \t"), "lut=", 4) == 0)
		{
			char F[260] = "";
			sscanf_s(Line + strspn(Line, " \t") + 4, "%259s", F, (unsigned)sizeof(F));
			if (LutFile != F)
			{
				LutFile = F;
				if (LutTex) { LutTex->Release(); LutTex = nullptr; }
		if (TerrainDetailTex) { TerrainDetailTex->Release(); TerrainDetailTex = nullptr; }
		TerrainDetailTried = false;
				LutTried = false;
			}
		}
		else if (strncmp(Line + strspn(Line, " 	"), "zwrite=", 7) == 0)
		{
			DWORD Ref = 24;
			if (sscanf_s(Line, " zwrite=%x %u", &Hash, &Ref) >= 1)
				ZWriteTex[Hash] = Ref;
		}
		else if (sscanf_s(Line, " tint=%x", &Hash) == 1)
		{
			U2Rule R;
			R.Hash = Hash;
			Rules.push_back(R);
		}
		else if (sscanf_s(Line, " shader=%x %255s", &Hash, Name, (unsigned)sizeof(Name)) == 2)
		{
			U2Rule R;
			R.Hash = Hash;
			R.File = Name;
			Rules.push_back(R);
		}
		else if (sscanf_s(Line, " lagfix=%u", &Hash) == 1)
			U2LagFix() = Hash != 0;
		else if (sscanf_s(Line, " aniso=%u", &Hash) == 1)
			Aniso = Hash > 16 ? 16 : Hash;
		else if (sscanf_s(Line, " stagelog=%u", &Hash) == 1)
			StageLog = Hash != 0;
		else if (sscanf_s(Line, " stagetrace=%u", &Hash) == 1)
		{
			StageLog = Hash != 0;          // stagetrace=N: every draw of N frames (from frame 600)
			StageTrace = (int)Hash;
		}
		else if (sscanf_s(Line, " pbr=%x %255s", &Hash, Name, (unsigned)sizeof(Name)) == 2)
		{
			// (after the map: highlight strength and normal map strength, 1 1 if left out)
			float Spec = 1, Bump = 1;
			char Shader[256] = "";
			sscanf_s(Line, " pbr=%*x %*s %f %f %255s", &Spec, &Bump, Shader, (unsigned)sizeof(Shader));
			// a character's (or weapon's) texture shaded as a physically based material:
			// char_pbr.hlsl with the D3D lights the game set, and <Name> as its material map
			U2Rule R;
			R.Hash = Hash;
			R.File = "char_pbr.hlsl";
			// a fifth value names another shader with the same inputs (char_skin.hlsl); such a
			// rule also takes the texture's alpha-blended draws, the shader returning its alpha
			if (strstr(Shader, ".hlsl") != nullptr)
			{
				R.File = Shader;
				R.PbrBlend = true;
			}
			R.MapFile = Name;
			R.Pbr = true;
			R.Levels[0] = Spec;
			R.Levels[1] = Bump;
			Rules.push_back(R);
		}
		else if (sscanf_s(Line, " glass=%x %255s", &Hash, Name, (unsigned)sizeof(Name)) == 2)
		{
			// like shader=, for things the game draws solid (the Bio Rifle's goo): the
			// shader gets the frame behind in s1 and draws the whole surface itself
			U2Rule R;
			R.Hash = Hash;
			R.File = Name;
			R.Solid = true;
			Rules.push_back(R);
		}
		else if (sscanf_s(Line, " layer=%x %255s", &Hash, Name, (unsigned)sizeof(Name)) == 2)
		{
			U2Rule R;
			R.Hash = Hash;
			R.File = Name;
			R.Layer = true;
			Rules.push_back(R);
		}
		else if (sscanf_s(Line, " decal=%x %255s", &Hash, Name, (unsigned)sizeof(Name)) == 2)
		{
			U2Rule R;
			R.Hash = Hash;
			R.File = Name;
			R.KeepBlend = true;
			Rules.push_back(R);
		}
		else if (sscanf_s(Line, " replace=%x %255s", &Hash, Name, (unsigned)sizeof(Name)) == 2)
			Replacements[Hash].File = Name;
		else if (strncmp(Line + strspn(Line, " \t"), "texgrade=", 9) == 0)
		{
			float g[5] = { 1, 0, 1, 1, 1 };
			if (sscanf_s(Line, " texgrade=%x %f %f %f %f %f", &Hash, &g[0], &g[1], &g[2], &g[3], &g[4]) >= 2)
			{
				U2Replace &R = Replacements[Hash];
				R.Grade = true;
				memcpy(R.G, g, sizeof(g));
			}
		}
		else if (sscanf_s(Line, " bloodlive=%x", &Hash) == 1)
		{
			if (!Reload)
				U2Blood::AddHash(Hash);         // the slots are fixed once pools exist
		}
		else if (sscanf_s(Line, " bloodrun=%x", &Hash) == 1)
		{
			if (!Reload)
				U2Runs::AddHash(Hash);          // a wall run region's placeholder texture (runs.hpp), slot order
		}             // a live blood pool's placeholder texture (blood.hpp), slot order
		else if (sscanf_s(Line, " surface=%x %255s", &Hash, Name, (unsigned)sizeof(Name)) == 2)
		{
			U2Rule R;
			R.Hash = Hash;
			R.File = Name;
			R.Surface = true;
			Rules.push_back(R);
		}
	}

	void Load()
	{
		Loaded = true;

		char Path[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, Path, MAX_PATH);
		Dir = Path;
		Dir.erase(Dir.find_last_of("\\/") + 1);
		DeleteFileA((Dir + "U2Shaders.log").c_str());
		U2Crash::Install(Dir);   // a C-runtime abort leaves a log line and a minidump (crash.hpp)
		if (!U2FakeFull::Pending().empty())
			Message("%s", U2FakeFull::Pending().substr(0, U2FakeFull::Pending().size() - 1).c_str());

		FILE *F = nullptr;
		if (fopen_s(&F, (Dir + "U2Shaders.ini").c_str(), "r") || F == nullptr)
			return;
		char Line[512];
		bool InMap = false, MapHit = false;
		while (fgets(Line, sizeof(Line), F))
			if (!MapSkip(Line, InMap, MapHit))
				ParseLine(Line, false);
		fclose(F);
		GmPanelFileEmpty(false);   // U2GM execs System\U2GMPanel.txt every poll: a missing file logs a warning each time
		Message("U2Shaders: log %d, %u rule(s), %u replacement(s), pcss %d (%g %g %g %g), shadow tint %g %g %g", (int)Log, (unsigned)Rules.size(), (unsigned)Replacements.size(), (int)Pcss,
			PcssParams[0], PcssParams[1], PcssParams[2], PcssParams[3], ShadowTint[0], ShadowTint[1], ShadowTint[2]);
		if (Log || CharProbe || Capture)
		{
			CreateDirectoryA((Dir + "U2Shaders").c_str(), nullptr);
			CreateDirectoryA((Dir + "U2Shaders\\dump").c_str(), nullptr);
		}
		if (Capture)
			CreateDirectoryA((Dir + "U2Shaders\\capture").c_str(), nullptr);
	}

	static UINT LevelBytes(const D3DSURFACE_DESC &Desc, INT Pitch)
	{
		switch (Desc.Format)
		{
		case D3DFMT_DXT1:
			return (std::max)(1u, Desc.Width / 4) * (std::max)(1u, Desc.Height / 4) * 8;
		case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
			return (std::max)(1u, Desc.Width / 4) * (std::max)(1u, Desc.Height / 4) * 16;
		default:
			return (UINT)Pitch * Desc.Height;
		}
	}

	// hash of the top mip (0 = could not be read); with Dump, also saved as a .dds
	DWORD Hash(IDirect3DTexture9 *Tex, U2TexInfo &Info, bool Dump)
	{
		D3DSURFACE_DESC Desc;
		D3DLOCKED_RECT Lock;
		if (FAILED(Tex->GetLevelDesc(0, &Desc)) || FAILED(Tex->LockRect(0, &Lock, nullptr, D3DLOCK_READONLY)))
			return 0;
		const UINT Bytes = LevelBytes(Desc, Lock.Pitch);
		DWORD H = 2166136261u ^ Desc.Width ^ (Desc.Height << 12);
		const BYTE *P = static_cast<const BYTE *>(Lock.pBits);
		for (UINT i = 0, n = (std::min)(Bytes, 16384u); i < n; i++)
			H = (H ^ P[i]) * 16777619u;
		if (H == 0)
			H = 1;
		Info.W = Desc.Width; Info.H = Desc.Height; Info.Fmt = Desc.Format;
		if (Dump)
			SaveDDS(H, Desc, P, Bytes, Lock.Pitch);
		Tex->UnlockRect(0);
		return H;
	}

	void SaveDDS(DWORD H, const D3DSURFACE_DESC &Desc, const BYTE *Bits, UINT Bytes, INT Pitch)
	{
		DWORD Header[32] = {};
		Header[0] = 0x20534444;            // "DDS "
		Header[1] = 124;
		Header[2] = 0x1007;                // caps | height | width | pixelformat
		Header[3] = Desc.Height;
		Header[4] = Desc.Width;
		Header[19] = 32;                   // pixel format size
		const bool Compressed = Desc.Format == D3DFMT_DXT1 || (Desc.Format >= D3DFMT_DXT2 && Desc.Format <= D3DFMT_DXT5 && (Desc.Format & 0xFF) == 'D');
		if (Compressed)
		{
			Header[2] |= 0x80000;          // linear size
			Header[5] = Bytes;
			Header[20] = 0x4;              // fourcc
			Header[21] = Desc.Format;
		}
		else if (Desc.Format == D3DFMT_A8R8G8B8 || Desc.Format == D3DFMT_X8R8G8B8)
		{
			Header[2] |= 0x8;              // pitch
			Header[5] = Pitch;
			Header[20] = Desc.Format == D3DFMT_A8R8G8B8 ? 0x41 : 0x40;
			Header[22] = 32;
			Header[23] = 0xFF0000; Header[24] = 0xFF00; Header[25] = 0xFF;
			Header[26] = Desc.Format == D3DFMT_A8R8G8B8 ? 0xFF000000 : 0;
		}
		else
			return;
		Header[27] = 0x1000;               // texture

		char Name[64];
		sprintf_s(Name, "U2Shaders\\dump\\%08x_%ux%u.dds", H, Desc.Width, Desc.Height);
		FILE *F = nullptr;
		if (fopen_s(&F, (Dir + Name).c_str(), "wb") || F == nullptr)
			return;
		fwrite(Header, 4, 32, F);
		fwrite(Bits, 1, Bytes, F);
		fclose(F);
	}

	IDirect3DPixelShader9 *Compile(IDirect3DDevice9 *Dev, U2Rule &R)
	{
		if (R.Tried)
			return R.PS;
		R.Tried = true;

		static const char TintSource[] = "float4 main() : COLOR { return float4(1, 0, 1, 1); }";
		std::string Source = TintSource;
		if (!R.File.empty())
		{
			FILE *F = nullptr;
			if (fopen_s(&F, (Dir + "U2Shaders\\" + R.File).c_str(), "rb") || F == nullptr)
			{
				Message("shader %s: file not found", R.File.c_str());
				return nullptr;
			}
			Source.clear();
			char Buf[4096];
			size_t n;
			while ((n = fread(Buf, 1, sizeof(Buf), F)) > 0)
				Source.append(Buf, n);
			fclose(F);
		}

		// one compile per shader source: rules on many textures share the file (decal/gloss/layer rules), and a
		// compile takes 100-200 ms - done per texture, each new blood texture in a fight was a hitch
		const auto Hit = PsCache.find(Source);
		if (Hit != PsCache.end())
		{
			R.PS = Hit->second;
			if (R.PS != nullptr)
				R.PS->AddRef();
			return R.PS;
		}
		ID3DBlob *Code = nullptr, *Errors = nullptr;
		HRESULT hr = D3DCompile(Source.data(), Source.size(), R.File.c_str(), nullptr, nullptr, "main", "ps_2_a", 0, 0, &Code, &Errors);
		if (FAILED(hr))
		{
			// too big for ps_2_a (22 temporaries): ps_2_b has 32, still usable with fixed-function vertices
			if (Errors != nullptr) { Errors->Release(); Errors = nullptr; }
			hr = D3DCompile(Source.data(), Source.size(), R.File.c_str(), nullptr, nullptr, "main", "ps_2_b", 0, 0, &Code, &Errors);
		}
		if (Errors != nullptr)
		{
			Message("shader %s: %s", R.File.c_str(), (const char *)Errors->GetBufferPointer());
			Errors->Release();
		}
		if (FAILED(hr) || Code == nullptr)
		{
			PsCache[Source] = nullptr;      // a broken file isn't compiled again on every rule
			return nullptr;
		}
		if (FAILED(Dev->CreatePixelShader(static_cast<const DWORD *>(Code->GetBufferPointer()), &R.PS)))
			Message("shader %s: CreatePixelShader failed", R.File.c_str());
		else
		{
			Message("shader %s: ready for texture %08x", R.File.empty() ? "(tint)" : R.File.c_str(), R.Hash);
			R.PS->AddRef();                 // the cache's reference
			PsCache[Source] = R.PS;
		}
		Code->Release();
		return R.PS;
	}
	std::map<std::string, IDirect3DPixelShader9 *> PsCache;   // shader source text -> its compiled shader

	// at load: every rule's shader file compiled once into the cache, so the first draw of each texture
	// in play costs nothing (the rules then pick theirs from the cache)
	bool Warmed = false;
	void WarmShaders(IDirect3DDevice9 *Dev)
	{
		if (Warmed || Dev == nullptr)
			return;
		Warmed = true;
		std::set<std::string> Done;
		unsigned n = 0;
		LARGE_INTEGER T0, T1, Fq;
		QueryPerformanceCounter(&T0);
		auto Warm = [&](const U2Rule &Src)
		{
			if (Src.File.empty() || Done.count(Src.File))
				return;
			Done.insert(Src.File);
			U2Rule Tmp;
			Tmp.File = Src.File;
			Tmp.Hash = Src.Hash;
			if (Compile(Dev, Tmp) != nullptr)
				n++;
			if (Tmp.PS)
				Tmp.PS->Release();
		};
		for (const U2Rule &R : Rules) Warm(R);
		for (const U2Rule &R : GlossRules) Warm(R);
		for (const U2Rule &R : SheenRules) Warm(R);
		if (Soft)
		{
			U2Rule S;
			S.File = "soft.hlsl";
			Warm(S);
		}
		for (const auto &It : PsReplace) Warm(It.second);
		if (U2Streaks::On)
			U2Streaks::Compile(Dev);   // the body-streak shader (first wound was a 130 ms hitch)
		if (U2Strings::On)
			U2Strings::Compile(Dev);   // and the goo strings'
		if (U2Streaks::HandsOn)
			U2Streaks::CompileHands(Dev);   // the hands pass's
		if (U2Lens::On)
			U2Lens::Compile(Dev);      // and the lens drops'
		QueryPerformanceCounter(&T1);
		QueryPerformanceFrequency(&Fq);
		Message("shaders: %u rule shader file(s) compiled at load in %.0f ms", n, (T1.QuadPart - T0.QuadPart) * 1000.0 / Fq.QuadPart);
	}

	// Copies the current render target into SceneTex; false if the copy failed (SceneTex then
	// holds whatever was in that memory: never draw it). LastCopyHr says why.
	HRESULT LastCopyHr = S_OK;
	bool CopyScene(IDirect3DDevice9 *Dev, bool Force = false)
	{
		IDirect3DSurface9 *Target = nullptr, *Copy = nullptr;
		bool Ok = !Force;                  // not forced and already copied this frame: fine
		if (FAILED(Dev->GetRenderTarget(0, &Target)) || Target == nullptr)
			return false;
		D3DSURFACE_DESC Desc;
		Target->GetDesc(&Desc);
		if (SceneTex != nullptr && (SceneW != Desc.Width || SceneH != Desc.Height || SceneFmt != Desc.Format))
		{
			SceneTex->Release();
			SceneTex = nullptr;
		}
		if (SceneTex == nullptr)
		{
			SceneW = Desc.Width; SceneH = Desc.Height; SceneFmt = Desc.Format;
			if (FAILED(Dev->CreateTexture(SceneW, SceneH, 1, D3DUSAGE_RENDERTARGET, SceneFmt, D3DPOOL_DEFAULT, &SceneTex, nullptr)))
			{
				SceneTex = nullptr;
				Message("scene copy: CreateTexture %ux%u failed", SceneW, SceneH);
			}
		}
		if (SceneTex != nullptr && (Force || SceneFrame != Frame) && SUCCEEDED(SceneTex->GetSurfaceLevel(0, &Copy)))
		{
			LastCopyHr = Dev->StretchRect(Target, nullptr, Copy, nullptr, D3DTEXF_NONE);
			if (FAILED(LastCopyHr))
			{
				// some wrappers won't copy from the current target object: try the back buffer itself
				IDirect3DSurface9 *BB = nullptr;
				if (SUCCEEDED(Dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &BB)) && BB != nullptr)
				{
					const HRESULT Again = Dev->StretchRect(BB, nullptr, Copy, nullptr, D3DTEXF_NONE);
					static int Told = 0;
					if (Told++ < 3)
						Message("scene copy: from the render target failed (%08lx), from the back buffer %s (%08lx)",
							(unsigned long)LastCopyHr, SUCCEEDED(Again) ? "worked" : "failed too", (unsigned long)Again);
					LastCopyHr = Again;
					BB->Release();
				}
			}
			Ok = SUCCEEDED(LastCopyHr);
			Copy->Release();
			SceneFrame = Frame;
		}
		else if (SceneTex == nullptr)
			Ok = false;
		Target->Release();
		return Ok;
	}

	// Draws that render into something other than the back buffer (shadow maps are built this way)
	// called before every draw (despite the name: it is the per-draw hook)
	std::map<std::string, bool> StageSeen;
	int StageTrace = 0;
	void StageLogDraw(IDirect3DDevice9 *Dev, const char *Key)
	{
		if (StageTrace > 0 && Frame >= 600 && Frame < 600 + (unsigned)StageTrace)
		{
			Message("stagetrace f%u: %s", Frame, Key);   // every draw, in order, for a few frames
			return;
		}
		if (StageTrace > 0 || Offscreen(Dev) || StageSeen.size() > 400 || StageSeen[Key])
			return;
		StageSeen[Key] = true;
		Message("stagelog: %s", Key);
	}

	void LogTargetDraw(IDirect3DDevice9 *Dev, bool FixedFunction, bool HasTex0)
	{
		if (!Loaded)
			Load();
		if (U2Strings::On && U2Strings::Live > 0 && !U2Strings::Drawn)
			StringsCheck(Dev);               // strings=1: goo strings, after the world, before the post chain
		PostCheck(Dev);
		if (SkGrabWant)
			SketchDraw(Dev);                 // sketch=1: a frame to freeze, at its first 2D draw
		GmCaptureView(Dev);                  // gmpanel=1, while shown: the scene's view for the gizmo
		if (CharProbe && !HasTex0 && Offscreen(Dev))
		{
			DWORD blend = 0, op0 = 0, arg1 = 0;
			Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
			Dev->GetTextureStageState(0, D3DTSS_COLOROP, &op0);
			Dev->GetTextureStageState(0, D3DTSS_COLORARG1, &arg1);
			if (!blend && op0 == D3DTOP_SELECTARG1 && (arg1 & 0xF) == D3DTA_TFACTOR)
				MapsThisFrame++;           // a shadow silhouette (same test as PcssBegin's map pass)
		}
		if (!Log)
			return;
		IDirect3DSurface9 *T = nullptr, *BB = nullptr;
		if (FAILED(Dev->GetRenderTarget(0, &T)) || T == nullptr)
			return;
		D3DSURFACE_DESC D = {}, B = {};
		T->GetDesc(&D);
		if (SUCCEEDED(Dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &BB)) && BB) { BB->GetDesc(&B); BB->Release(); }
		T->Release();
		if (D.Width == B.Width && D.Height == B.Height)
			return;
		DWORD rs[4] = {}, ts[6] = {}, tf = 0;
		Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &rs[0]);
		Dev->GetRenderState(D3DRS_SRCBLEND, &rs[1]);
		Dev->GetRenderState(D3DRS_DESTBLEND, &rs[2]);
		Dev->GetRenderState(D3DRS_ZENABLE, &rs[3]);
		Dev->GetRenderState(D3DRS_TEXTUREFACTOR, &tf);
		Dev->GetTextureStageState(0, D3DTSS_COLOROP, &ts[0]);
		Dev->GetTextureStageState(0, D3DTSS_COLORARG1, &ts[1]);
		Dev->GetTextureStageState(0, D3DTSS_COLORARG2, &ts[2]);
		Dev->GetTextureStageState(0, D3DTSS_TEXCOORDINDEX, &ts[3]);
		Dev->GetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, &ts[4]);
		DWORD fvf = 0;
		Dev->GetFVF(&fvf);
		char key[300];
		sprintf_s(key, "INTO %ux%u | %s fvf %x tex0 %d | blend %u src %u dst %u z %u tfactor %08x | st0 op %u a1 %u a2 %u tci %x ttf %x",
			D.Width, D.Height, FixedFunction ? "ff" : "vs", fvf, (int)HasTex0, rs[0], rs[1], rs[2], rs[3], tf, ts[0], ts[1], ts[2], ts[3], ts[4]);
		DrawKinds[key]++;
	}

	// Fills in a texture's cached hash the first time (0xFFFFFFFF: unreadable, don't try again)
	void Known(IDirect3DTexture9 *Tex, DWORD &Hash)
	{
		if (Hash != 0)
			return;
		U2TexInfo Info;
		Hash = this->Hash(Tex, Info, false);
		if (Hash == 0)
			Hash = 0xFFFFFFFF;
		else
			Seen[Hash] = Info;
	}

	// ---- replace=: our own texture in place of one of the game's -------------------------
	// The device swaps it in for each draw and back after (so the game still sees its own
	// texture when it asks), on stages 0-3. The rules keyed by hash still match the original.
	struct U2Replace
	{
		std::string File;
		IDirect3DTexture9 *Tex = nullptr;
		bool Tried = false;
		bool Grade = false;            // texgrade=: a graded copy of the game's own texture, made in memory
		float G[5] = { 1, 0, 1, 1, 1 };  // lift, desaturate 0..1, tint r g b
	};

	// texgrade=HASH lift desat r g b: the texture's colours lifted, desaturated toward grey and
	// tinted, in a copy made in memory the first time it is drawn (nothing is read from or written
	// to disk). Works under any stage setup, since only the texture changes. 32-bit textures per
	// pixel; DXT1/3/5 per block, by grading the two endpoint colours (the interpolated ones follow,
	// as the grade is linear until it clips); a DXT1 block's endpoint order decides whether it has
	// a transparent colour, so the order is kept and the indices remapped where the grade swaps it.
	static void GradeRGB(const float G[5], float &r, float &g, float &b)
	{
		const float l = 0.30f * r + 0.59f * g + 0.11f * b;
		r = (r + (l - r) * G[1]) * G[2] * G[0];
		g = (g + (l - g) * G[1]) * G[3] * G[0];
		b = (b + (l - b) * G[1]) * G[4] * G[0];
		r = r < 0 ? 0 : r > 1 ? 1 : r; g = g < 0 ? 0 : g > 1 ? 1 : g; b = b < 0 ? 0 : b > 1 ? 1 : b;
	}
	static WORD Grade565(const float G[5], WORD v)
	{
		float r = ((v >> 11) & 31) / 31.0f, g = ((v >> 5) & 63) / 63.0f, b = (v & 31) / 31.0f;
		GradeRGB(G, r, g, b);
		return (WORD)(((int)(r * 31 + 0.5f) << 11) | ((int)(g * 63 + 0.5f) << 5) | (int)(b * 31 + 0.5f));
	}
	static void GradeDXT1Block(const float G[5], BYTE *B, bool Alone)
	{
		WORD &c0 = *reinterpret_cast<WORD *>(B), &c1 = *reinterpret_cast<WORD *>(B + 2);
		DWORD &ix = *reinterpret_cast<DWORD *>(B + 4);
		const bool Four = !Alone || c0 > c1;      // DXT3/5 colour blocks are always four-colour
		WORD n0 = Grade565(G, c0), n1 = Grade565(G, c1);
		if (!Alone)
		{
			c0 = n0; c1 = n1;
			return;
		}
		if (Four)
		{
			if (n0 < n1)
			{
				WORD t = n0; n0 = n1; n1 = t;
				ix ^= 0x55555555;                  // 0<->1, 2<->3
			}
			else if (n0 == n1)
			{
				ix = 0;                            // one colour: all texels take c0
				if (n0 < 0xFFFF) n0++; else n1--;
			}
		}
		else if (n0 > n1)
		{
			WORD t = n0; n0 = n1; n1 = t;
			DWORD out = 0;                         // 0<->1; 2 (the middle) and 3 (transparent) stay
			for (int k = 0; k < 16; k++)
			{
				DWORD i = (ix >> (2 * k)) & 3;
				if (i < 2) i ^= 1;
				out |= i << (2 * k);
			}
			ix = out;
		}
		c0 = n0; c1 = n1;
	}
	IDirect3DTexture9 *GradeCopy(IDirect3DDevice9 *Dev, IDirect3DTexture9 *Src, DWORD Hash, const float G[5])
	{
		D3DSURFACE_DESC D;
		if (Src == nullptr || FAILED(Src->GetLevelDesc(0, &D)))
			return nullptr;
		const bool Dxt = D.Format == D3DFMT_DXT1 || D.Format == D3DFMT_DXT3 || D.Format == D3DFMT_DXT5;
		if (!Dxt && D.Format != D3DFMT_A8R8G8B8 && D.Format != D3DFMT_X8R8G8B8)
		{
			Message("grade %08x: format %u not graded", Hash, (unsigned)D.Format);
			return nullptr;
		}
		const UINT Levels = Src->GetLevelCount();
		IDirect3DTexture9 *Out = nullptr;
		if (FAILED(Dev->CreateTexture(D.Width, D.Height, Levels, 0, D.Format, D3DPOOL_MANAGED, &Out, nullptr)))
			return nullptr;
		for (UINT lv = 0; lv < Levels; lv++)
		{
			D3DSURFACE_DESC L;
			D3DLOCKED_RECT A, B;
			Src->GetLevelDesc(lv, &L);
			if (FAILED(Src->LockRect(lv, &A, nullptr, D3DLOCK_READONLY)))
			{
				Out->Release();
				Message("grade %08x: level %u not readable", Hash, lv);
				return nullptr;
			}
			if (FAILED(Out->LockRect(lv, &B, nullptr, 0)))
			{
				Src->UnlockRect(lv);
				Out->Release();
				return nullptr;
			}
			const UINT Block = D.Format == D3DFMT_DXT1 ? 8 : 16;
			const UINT Rows = Dxt ? (std::max)(1u, (L.Height + 3) / 4) : L.Height;
			const UINT Bytes = Dxt ? (std::max)(1u, (L.Width + 3) / 4) * Block : L.Width * 4;
			for (UINT y = 0; y < Rows; y++)
			{
				const BYTE *s = static_cast<const BYTE *>(A.pBits) + y * A.Pitch;
				BYTE *d = static_cast<BYTE *>(B.pBits) + y * B.Pitch;
				memcpy(d, s, Bytes);
				if (Dxt)
					for (UINT x = 0; x < Bytes; x += Block)
						GradeDXT1Block(G, d + x + (Block == 16 ? 8 : 0), Block == 8);
				else
					for (UINT x = 0; x < Bytes; x += 4)
					{
						float r = d[x + 2] / 255.0f, g = d[x + 1] / 255.0f, b = d[x] / 255.0f;
						GradeRGB(G, r, g, b);
						d[x + 2] = (BYTE)(r * 255 + 0.5f); d[x + 1] = (BYTE)(g * 255 + 0.5f); d[x] = (BYTE)(b * 255 + 0.5f);
					}
			}
			Out->UnlockRect(lv);
			Src->UnlockRect(lv);
		}
		Message("grade %08x: graded copy made (%ux%u, %u levels, lift %.2f desaturate %.2f tint %.2f %.2f %.2f)",
			Hash, D.Width, D.Height, Levels, G[0], G[1], G[2], G[3], G[4]);
		return Out;
	}
	std::map<DWORD, U2Replace> Replacements;

	IDirect3DTexture9 *Replacement(IDirect3DDevice9 *Dev, IDirect3DTexture9 *Tex, DWORD &Hash)
	{
		if (!Loaded)
			Load();
		if (Tex == nullptr || (Replacements.empty() && U2Blood::Count == 0 && U2Runs::Count == 0 && !TexEd().Active()))
			return nullptr;
		Known(Tex, Hash);
		if (U2Blood::Count > 0 && Seen.count(Hash) && Seen[Hash].W == 64 && Seen[Hash].H == 64)
		{
			static std::set<DWORD> Told64;
			if (Told64.insert(Hash).second)
				Message("blood: a 64x64 texture %08x (format %u) drawn, slot %d", (unsigned)Hash, (unsigned)Seen[Hash].Fmt, U2Blood::SlotOf(Hash));
		}
		if (IDirect3DTexture9 *Live = U2Blood::TextureFor(Dev, Hash))
			return Live;                        // a live blood pool (blood.hpp)
		if (IDirect3DTexture9 *Run = U2Runs::TextureFor(Dev, Hash))
			return Run;                         // blood running down a wall (runs.hpp)
		const auto It = Replacements.find(Hash);
		if (It == Replacements.end())
			return TexEd().Adjusted(Dev, Tex, Hash, nullptr);       // texadjust: the game's texture adjusted
		U2Replace &R = It->second;
		if (!R.Tried)
		{
			R.Tried = true;
			if (R.Grade)
				R.Tex = GradeCopy(Dev, Tex, Hash, R.G);
			else
			{
				R.Tex = LoadDDS(Dev, R.File);
				if (R.Tex != nullptr)
					Message("replace %08x: %s loaded", Hash, R.File.c_str());
			}
		}
		return TexEd().Adjusted(Dev, R.Tex ? R.Tex : Tex, Hash, R.Tex);   // texadjust on top of replace= / texgrade=
	}

	// A DDS file as a managed texture (survives device resets): 32-bit (A8R8G8B8/X8R8G8B8) or
	// DXT1/3/5, all the mip levels the file has.
	IDirect3DTexture9 *LoadDDS(IDirect3DDevice9 *Dev, const std::string &File)
	{
		std::string Data;
		FILE *F = nullptr;
		if (fopen_s(&F, (Dir + "U2Shaders\\" + File).c_str(), "rb") || F == nullptr)
		{
			Message("replace: %s not found", File.c_str());
			return nullptr;
		}
		char Buf[65536];
		size_t n;
		while ((n = fread(Buf, 1, sizeof(Buf), F)) > 0)
			Data.append(Buf, n);
		fclose(F);
		const DWORD *H = reinterpret_cast<const DWORD *>(Data.data());
		if (Data.size() < 128 || H[0] != 0x20534444 || H[1] != 124)
		{
			Message("replace: %s is not a DDS file", File.c_str());
			return nullptr;
		}
		const UINT Height = H[3], Width = H[4], Levels = (H[2] & 0x20000) && H[7] ? H[7] : 1;
		const DWORD PfFlags = H[20], FourCC = H[21], Bits = H[22], RMask = H[23], AMask = H[26];
		D3DFORMAT Fmt = D3DFMT_UNKNOWN;
		UINT BlockBytes = 0;          // DXT: bytes per 4x4 block; else 0
		if ((PfFlags & 0x4) && (FourCC == D3DFMT_DXT1 || FourCC == D3DFMT_DXT3 || FourCC == D3DFMT_DXT5))
		{
			Fmt = (D3DFORMAT)FourCC;
			BlockBytes = FourCC == D3DFMT_DXT1 ? 8 : 16;
		}
		else if ((PfFlags & 0x40) && Bits == 32 && RMask == 0xFF0000)
			Fmt = (PfFlags & 0x1) && AMask ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8;
		if (Fmt == D3DFMT_UNKNOWN || Width == 0 || Height == 0)
		{
			Message("replace: %s: unsupported format (use 32-bit BGRA or DXT1/3/5)", File.c_str());
			return nullptr;
		}
		IDirect3DTexture9 *T = nullptr;
		if (FAILED(Dev->CreateTexture(Width, Height, Levels, 0, Fmt, D3DPOOL_MANAGED, &T, nullptr)))
		{
			Message("replace: %s: CreateTexture %ux%u failed", File.c_str(), Width, Height);
			return nullptr;
		}
		size_t At = 128;
		for (UINT L = 0; L < Levels; L++)
		{
			const UINT W = (std::max)(1u, Width >> L), Hh = (std::max)(1u, Height >> L);
			const UINT Rows = BlockBytes ? (std::max)(1u, (Hh + 3) / 4) : Hh;
			const UINT RowBytes = BlockBytes ? (std::max)(1u, (W + 3) / 4) * BlockBytes : W * 4;
			D3DLOCKED_RECT Lock;
			if (At + (size_t)Rows * RowBytes > Data.size() || FAILED(T->LockRect(L, &Lock, nullptr, 0)))
			{
				Message("replace: %s: file ends early (level %u)", File.c_str(), L);
				T->Release();
				return nullptr;
			}
			for (UINT y = 0; y < Rows; y++)
				memcpy(static_cast<BYTE *>(Lock.pBits) + y * Lock.Pitch, Data.data() + At + (size_t)y * RowBytes, RowBytes);
			T->UnlockRect(L);
			At += (size_t)Rows * RowBytes;
		}
		return T;
	}

	// decal=: what kind of decal the draw is, from its blending. Alpha decals (SRCALPHA,
	// INVSRCALPHA) are deeper where more opaque; multiplying ones (the wall times the texture)
	// are deeper where darker than their neutral colour: white for DESTCOLOR x ZERO, mid-grey for
	// DESTCOLOR x SRCCOLOR (twice the wall times the texture). Logged once per rule.
	void DecalBlend(IDirect3DDevice9 *Dev, U2Rule &R, float &Mode, float &Neutral)
	{
		DWORD Src = 0, Dst = 0;
		Dev->GetRenderState(D3DRS_SRCBLEND, &Src);
		Dev->GetRenderState(D3DRS_DESTBLEND, &Dst);
		const char *Kind = "alpha";
		Mode = 0; Neutral = 1;
		if ((Src == D3DBLEND_DESTCOLOR && Dst == D3DBLEND_SRCCOLOR) || (Src == D3DBLEND_SRCCOLOR && Dst == D3DBLEND_DESTCOLOR))
		{ Mode = 2; Neutral = 0.5f; Kind = "multiply x2 (grey = no change)"; }
		else if ((Src == D3DBLEND_DESTCOLOR && Dst == D3DBLEND_ZERO) || (Src == D3DBLEND_ZERO && Dst == D3DBLEND_SRCCOLOR))
		{ Mode = 1; Neutral = 1; Kind = "multiply (white = no change)"; }
		else if (!(Src == D3DBLEND_SRCALPHA && (Dst == D3DBLEND_INVSRCALPHA || Dst == D3DBLEND_ONE)))
			Kind = "unknown, treated as alpha";
		if (!R.Refused)
			Message("decal %08x: blend src %u dst %u: %s", R.Hash, Src, Dst, Kind);
		R.Refused = true;              // (reused as "logged" for decal rules)
	}

	// Called before a draw. Hash = the stage 0 texture's hash (0: not yet known, read it from
	// Tex). Returns true if a shader was put in place; End() must then follow the draw.
	// ---- layer= : a shader that only replaces how the game's own stages combine -----------------
	// Unreal II draws a terrain layer as a fixed-function pass: stage 0 = the layer texture (its
	// coordinates generated and transformed by the fixed-function vertex pipeline), stage 1 = the
	// layer's alpha map, both MODULATE2X, alpha-blended over the layers below (the first solid).
	// Here nothing of that is touched: TEXCOORD0/1 reach the shader already transformed, s0/s1 keep
	// their samplers, the blend stays on. The shader gets c0 = (time, 1, 1/width, 1/height).
	// The shader reproduces whatever stage setup the draw has, read here and passed in c1:
	//   c1.x stage 0 colour: 0 texture only, 1 texture x diffuse, 2 texture x diffuse x 2
	//   c1.y stage 1 colour: 0 unchanged, 1 x alpha map, 2 x alpha map x 2
	//   c1.z alpha: 0 stage 0's (texture alpha x diffuse alpha), 1 that x alpha map alpha, 2 alpha map alpha
	//   c1.w 1 when stage 1 is used (s1/TEXCOORD1 valid)
	//   c2.x stage 0 alpha: 0 texture, 1 texture x diffuse, 2 diffuse, 3 texture x diffuse x 2
	// Stage 2 must be off; game vertex shaders and render-to-texture passes are left alone.
	// Setups outside that are left to the game (logged once per hash and setup).
	std::set<std::pair<DWORD, unsigned long long>> LayerSeen;   // (hash, packed stage setup) already logged
	bool LayerBegin(IDirect3DDevice9 *Dev, U2Rule &Rule, bool FixedFunction)
	{
		if (!FixedFunction || Offscreen(Dev))       // a game vertex shader, or a render-to-texture pass: stock
			return false;
		DWORD c2op = 0, a0a1 = 0, a0a2 = 0;
		Dev->GetTextureStageState(2, D3DTSS_COLOROP, &c2op);
		Dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &a0a1);
		Dev->GetTextureStageState(0, D3DTSS_ALPHAARG2, &a0a2);
		DWORD c0op = 0, c0a1 = 0, c0a2 = 0, a0op = 0, c1op = 0, c1a1 = 0, c1a2 = 0, a1op = 0, a1a1 = 0, a1a2 = 0, ttf0 = 0, ttf1 = 0;
		Dev->GetTextureStageState(0, D3DTSS_COLOROP, &c0op);
		Dev->GetTextureStageState(0, D3DTSS_COLORARG1, &c0a1);
		Dev->GetTextureStageState(0, D3DTSS_COLORARG2, &c0a2);
		Dev->GetTextureStageState(0, D3DTSS_ALPHAOP, &a0op);
		Dev->GetTextureStageState(1, D3DTSS_COLOROP, &c1op);
		Dev->GetTextureStageState(1, D3DTSS_COLORARG1, &c1a1);
		Dev->GetTextureStageState(1, D3DTSS_COLORARG2, &c1a2);
		Dev->GetTextureStageState(1, D3DTSS_ALPHAOP, &a1op);
		Dev->GetTextureStageState(1, D3DTSS_ALPHAARG1, &a1a1);
		Dev->GetTextureStageState(1, D3DTSS_ALPHAARG2, &a1a2);
		Dev->GetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, &ttf0);
		Dev->GetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, &ttf1);
		float C1[4] = { -1, 0, 0, 0 };
		// stage 0: the layer texture, alone or with the vertex lighting
		bool texdif = (c0a1 == D3DTA_TEXTURE && c0a2 == D3DTA_DIFFUSE) || (c0a1 == D3DTA_DIFFUSE && c0a2 == D3DTA_TEXTURE);
		if (c0op == D3DTOP_SELECTARG1 && c0a1 == D3DTA_TEXTURE) C1[0] = 0;
		else if (c0op == D3DTOP_MODULATE && texdif) C1[0] = 1;
		else if (c0op == D3DTOP_MODULATE2X && texdif) C1[0] = 2;
		// stage 0 alpha (c2.x): 0 texture, 1 texture x diffuse, 2 diffuse, 3 texture x diffuse x 2
		float C2[4] = { -1, 0, 0, 0 };
		bool atexdif = (a0a1 == D3DTA_TEXTURE && a0a2 == D3DTA_DIFFUSE) || (a0a1 == D3DTA_DIFFUSE && a0a2 == D3DTA_TEXTURE);
		if ((a0op == D3DTOP_SELECTARG1 && a0a1 == D3DTA_TEXTURE) || (a0op == D3DTOP_SELECTARG2 && a0a2 == D3DTA_TEXTURE)) C2[0] = 0;
		else if (a0op == D3DTOP_MODULATE && atexdif) C2[0] = 1;
		else if ((a0op == D3DTOP_SELECTARG1 && a0a1 == D3DTA_DIFFUSE) || (a0op == D3DTOP_SELECTARG2 && a0a2 == D3DTA_DIFFUSE)) C2[0] = 2;
		else if (a0op == D3DTOP_MODULATE2X && atexdif) C2[0] = 3;
		bool ok = C1[0] >= 0 && C2[0] >= 0 && c2op == D3DTOP_DISABLE && !(ttf0 & D3DTTFF_PROJECTED) && !(ttf1 & D3DTTFF_PROJECTED);
		IDirect3DBaseTexture9 *t1 = nullptr;
		Dev->GetTexture(1, &t1);
		bool st1 = c1op != D3DTOP_DISABLE && t1 != nullptr;
		if (t1) t1->Release();
		if (ok && st1)
		{
			bool curtex = (c1a1 == D3DTA_CURRENT && c1a2 == D3DTA_TEXTURE) || (c1a1 == D3DTA_TEXTURE && c1a2 == D3DTA_CURRENT);
			if ((c1op == D3DTOP_SELECTARG1 && c1a1 == D3DTA_CURRENT) || (c1op == D3DTOP_SELECTARG2 && c1a2 == D3DTA_CURRENT)) C1[1] = 0;
			else if (c1op == D3DTOP_MODULATE && curtex) C1[1] = 1;
			else if (c1op == D3DTOP_MODULATE2X && curtex) C1[1] = 2;
			else ok = false;
			bool acurtex = (a1a1 == D3DTA_CURRENT && a1a2 == D3DTA_TEXTURE) || (a1a1 == D3DTA_TEXTURE && a1a2 == D3DTA_CURRENT);
			if (a1op == D3DTOP_DISABLE || (a1op == D3DTOP_SELECTARG1 && a1a1 == D3DTA_CURRENT) || (a1op == D3DTOP_SELECTARG2 && a1a2 == D3DTA_CURRENT)) C1[2] = 0;
			else if (a1op == D3DTOP_MODULATE && acurtex) C1[2] = 1;
			else if ((a1op == D3DTOP_SELECTARG1 && a1a1 == D3DTA_TEXTURE) || (a1op == D3DTOP_SELECTARG2 && a1a2 == D3DTA_TEXTURE)) C1[2] = 2;
			else ok = false;
			C1[3] = 1;
		}
		// the message only when this setup is new for this texture (a packed key: every value is < 32 but the flags)
		unsigned long long K = 0;
		const DWORD Vals[13] = { c0op, c0a1, c0a2, a0op, a0a1, a0a2, c1op, c1a1, c1a2, a1op, a1a1, a1a2, c2op };
		for (DWORD v : Vals)
			K = K * 32 + (v & 31);
		K = K * 4 + (ttf0 & 3);
		K = K * 4 + (ttf1 & 3);
		K = K * 2 + (st1 ? 1 : 0);
		if (LayerSeen.insert(std::make_pair(Rule.Hash, K)).second)
			Message("layer %08x: st0 %u(%u,%u) a%u(%u,%u) | st1 %s %u(%u,%u) a%u(%u,%u) | st2 %u | ttf %u %u -> %s (c1 %.0f %.0f %.0f %.0f, c2 %.0f)",
				Rule.Hash, c0op, c0a1, c0a2, a0op, a0a1, a0a2, st1 ? "on" : "off", c1op, c1a1, c1a2, a1op, a1a1, a1a2, c2op, ttf0, ttf1,
				ok ? "shader" : "left to the game", C1[0], C1[1], C1[2], C1[3], C2[0]);
		if (!ok)
			return false;
		IDirect3DPixelShader9 *PS = Compile(Dev, Rule);
		if (PS == nullptr)
			return false;
		Dev->GetPixelShader(&OldPS);
		Dev->GetPixelShaderConstantF(0, OldConst[0], 8);
		float C0[4] = { (GetTickCount() % 3600000) / 1000.0f, 1.0f, SceneW ? 1.0f / SceneW : 0, SceneH ? 1.0f / SceneH : 0 };
		Dev->SetPixelShaderConstantF(0, C0, 1);
		Dev->SetPixelShaderConstantF(1, C1, 1);
		Dev->SetPixelShaderConstantF(2, C2, 1);
		Dev->SetPixelShader(PS);
		Mode = 8;
		return true;
	}

	// zwrite=: the texture's blended draws write depth where they're solid enough (alpha test)
	std::map<DWORD, DWORD> ZWriteTex;
	bool ZwOn = false;
	DWORD ZwOld[4] = {};
	void ZWriteBegin(IDirect3DDevice9 *Dev, DWORD Hash)
	{
		if (ZWriteTex.empty())
			return;
		auto It = ZWriteTex.find(Hash);
		if (It == ZWriteTex.end())
			return;
		DWORD Blend = 0;
		Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &Blend);
		if (!Blend)
			return;
		Dev->GetRenderState(D3DRS_ZWRITEENABLE, &ZwOld[0]);
		Dev->GetRenderState(D3DRS_ALPHATESTENABLE, &ZwOld[1]);
		Dev->GetRenderState(D3DRS_ALPHAREF, &ZwOld[2]);
		Dev->GetRenderState(D3DRS_ALPHAFUNC, &ZwOld[3]);
		Dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		Dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
		Dev->SetRenderState(D3DRS_ALPHAREF, It->second);
		Dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
		ZwOn = true;
	}
	void ZWriteEnd(IDirect3DDevice9 *Dev)
	{
		if (!ZwOn)
			return;
		ZwOn = false;
		Dev->SetRenderState(D3DRS_ZWRITEENABLE, ZwOld[0]);
		Dev->SetRenderState(D3DRS_ALPHATESTENABLE, ZwOld[1]);
		Dev->SetRenderState(D3DRS_ALPHAREF, ZwOld[2]);
		Dev->SetRenderState(D3DRS_ALPHAFUNC, ZwOld[3]);
	}

	bool Begin(IDirect3DDevice9 *Dev, IDirect3DTexture9 *Tex, DWORD &Hash, bool FixedFunction)
	{
		if (!Loaded)
			Load();
		// Tex == nullptr with Hash already set: a draw without a 2D texture that a rule can still
		// name (CubeOnlyHash: a cube map alone on stage 0, see the device's U2Begin)
		if ((Tex == nullptr && Hash == 0) || (!Log && Rules.empty() && !CharProbe && !CharLight))
			return false;
		if (Tex == nullptr)
		{
			U2Rule *Cube = nullptr;
			for (U2Rule &R : Rules)
				if (R.Hash == Hash)
					Cube = &R;
			if (Cube == nullptr)
				return false;
		}
		else

		Known(Tex, Hash);
		if (Hash == 0xFFFFFFFF)
		{
			if (Log)
				LogUnreadableDraw(Dev, Tex);   // render targets (e.g. shadow maps): note how they're drawn
			return false;
		}
		if (CharProbe && Tex != nullptr)
			ProbeDraw(Dev, Tex, Hash, FixedFunction);

		if (Log && Tex != nullptr)
		{
			DWORD Blend = 0;
			Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &Blend);
			if (Blend || LogSolid)
			{
				U2TexInfo &Info = Seen[Hash];
				if (Info.Draws++ == 0)
				{
					U2TexInfo Dummy;
					this->Hash(Tex, Dummy, true);
				}
			}
		}

		U2Rule *Rule = nullptr;
		for (U2Rule &R : Rules)
			if (R.Hash == Hash)
				Rule = &R;
		if (Rule == nullptr)
			return CharLight ? CharBegin(Dev, FixedFunction) : false;
		if (Rule->Pbr)
			return CharBegin(Dev, FixedFunction, Rule);
		if (Rule->Surface)
			return SurfaceBegin(Dev, *Rule, FixedFunction);
		if (Rule->Layer)
			return LayerBegin(Dev, *Rule, FixedFunction);
		// only the see-through parts: an atlas is often shared with solid ones
		DWORD Blending = 0;
		Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &Blending);
		if (!Blending && !Rule->Solid)
			return false;
		IDirect3DPixelShader9 *PS = Compile(Dev, *Rule);
		if (PS == nullptr)
			return false;

		if (!Rule->KeepBlend)
			CopyScene(Dev);

		Dev->GetPixelShader(&OldPS);
		Dev->GetTexture(1, &OldTex1);
		Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &OldBlend);
		Dev->GetPixelShaderConstantF(0, OldConst[0], 8);
		static const D3DSAMPLERSTATETYPE Samp[5] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER };
		static const DWORD SampValue[5] = { D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE };
		for (int i = 0; i < 5; i++)
		{
			Dev->GetSamplerState(1, Samp[i], &OldSamp1[i]);
			Dev->SetSamplerState(1, Samp[i], SampValue[i]);
		}
		bool Projected = false;
		if (FixedFunction)
		{
			// raw texture coordinates on stage 0 (no panning): the shader animates itself.
			// Projector draws (decals) keep their transform: it is the projection itself,
			// and the shader divides by z (c1.x = 1), as pixel shaders ignore PROJECTED
			Dev->GetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, &OldTTF[0]);
			Projected = (OldTTF[0] & D3DTTFF_PROJECTED) != 0;
			if (!Projected)
				Dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
			static const D3DMATRIX Identity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
			for (DWORD s = 1; s <= 2; s++)
			{
				Dev->GetTextureStageState(s, D3DTSS_TEXCOORDINDEX, &OldTCI[s]);
				Dev->GetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, &OldTTF[s]);
				Dev->GetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), &OldTexMat[s]);
				Dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, (s == 1 ? D3DTSS_TCI_CAMERASPACENORMAL : D3DTSS_TCI_CAMERASPACEPOSITION) | s);
				Dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
				Dev->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), &Identity);
			}
		}

		float Const[8][4] = {};
		Const[0][0] = (GetTickCount() % 3600000) / 1000.0f;
		Const[0][1] = FixedFunction ? 1.0f : 0.0f;
		Const[0][2] = SceneW ? 1.0f / SceneW : 0;
		Const[0][3] = SceneH ? 1.0f / SceneH : 0;
		Const[1][0] = Projected ? 1.0f : 0.0f;
		if (Rule->KeepBlend)
			DecalBlend(Dev, *Rule, Const[1][1], Const[1][2]);
		D3DMATRIX Proj;
		Dev->GetTransform(D3DTS_PROJECTION, &Proj);
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 4; c++)
				Const[4 + r][c] = Proj.m[r][c];
		Dev->SetPixelShaderConstantF(0, Const[0], 8);
		if (!Rule->KeepBlend)
		{
			Dev->SetTexture(1, SceneTex);
			Dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		}
		Dev->SetPixelShader(PS);
		WasFixedFunction = FixedFunction;
		Mode = 1;
		return true;
	}

	// ---- contact-hardening shadows (PCSS) --------------------------------------------------
	// Unreal II builds each dynamic shadow by drawing the character as a flat silhouette
	// (colour = texture factor) into a small render target, then projects that target onto
	// the ground (stage 0 projected, DESTCOLOR*SRCCOLOR, stages 1-2 fade to the neutral
	// texture factor by their gradient textures' alpha). With "pcss=1":
	//   shadow-map pass: pcss_map.hlsl keeps the colour and writes the distance from the light
	//                    into the target's otherwise unused alpha
	//   projector pass:  pcss_proj.hlsl searches for blockers, sizes the blur by the gap
	//                    between them and the ground, filters, and applies the two fades
	// The engine's own shadow blur must be off (WinDrv BlurShadows=False): it would mix the
	// stored distances. pcssparams=A B C D goes to c2 (search radius, min radius, radius per
	// unit of gap, max radius; UV units). shadowtint=R G B goes to c5: how strongly the
	// projector darkens each channel (1 1 1 = the engine's grey; lower blue = bluer shadows).
	bool Pcss = false;
	U2Rule MapRule, ProjRule;
	float PcssParams[4] = { 0.05f, 0.004f, 0.0006f, 0.06f };
	float ShadowTint[4] = { 1, 1, 1, 1 };   // w = 1 tells pcss_proj.hlsl the tint is set
	float PcssDebug = 0;          // pcssdebug=1: colour the shadows by the measured gap
	int Mode = 0;                 // what End() undoes: 1 surface shader, 2 shadow map, 3 projector, 5 solid surface, 9 soft particle
	DWORD OldCWE = 0;
	std::map<void *, D3DMATRIX> MapViews;   // shadow-map surface -> the light's view it was drawn with
	IDirect3DSurface9 *MapTarget = nullptr; // the shadow map drawn last (compared only, not referenced)
	std::map<void *, IDirect3DTexture9 *> BlurSource;   // blurred shadow surface (B) -> our snapshot of its sharp map (A)
	bool MapDirty = false;                  // a silhouette was drawn since the last snapshot
	void *CopiedFor = nullptr;              // the B the last snapshot was taken for
	IDirect3DBaseTexture9 *OldTex3 = nullptr;
	DWORD OldSamp3[5] = {};

	void DumpRaw(IDirect3DDevice9 *Dev, IDirect3DTexture9 *Tex, const char *What, UINT Size)
	{
		D3DSURFACE_DESC D = {};
		Tex->GetLevelDesc(0, &D);
		IDirect3DSurface9 *RT = nullptr, *Sys = nullptr;
		if (SUCCEEDED(Tex->GetSurfaceLevel(0, &RT)) &&
			SUCCEEDED(Dev->CreateOffscreenPlainSurface(D.Width, D.Height, D.Format, D3DPOOL_SYSTEMMEM, &Sys, nullptr)) &&
			SUCCEEDED(U2PerfReadback(Dev, RT, Sys)))
		{
			D3DLOCKED_RECT L;
			if (SUCCEEDED(Sys->LockRect(&L, nullptr, D3DLOCK_READONLY)))
			{
				char name[64];
				sprintf_s(name, "U2Shaders\\dump\\pcss_%s_%u.raw", What, Size);
				FILE *F = nullptr;
				if (!fopen_s(&F, (Dir + name).c_str(), "wb") && F)
				{
					for (UINT y = 0; y < D.Height; y++)
						fwrite((const BYTE *)L.pBits + y * L.Pitch, 4, D.Width, F);
					fclose(F);
				}
				Sys->UnlockRect();
			}
		}
		else
			Message("pcss dump of %s %u failed\n", What, Size);
		if (Sys) Sys->Release();
		if (RT) RT->Release();
	}

	// pcssprobe=N: for the first N shadow-map (silhouette) draws, log what happens around the draw:
	// the HRESULTs of SetPixelShader and of the draw, stages 2/3 texgen before / during / after,
	// and A read back right after the draw (pixels with alpha < 255 = silhouette). For finding
	// why a map draw with our pixel shader can write nothing (Advent Rising, sun on terrain).
	int PcssProbe = 0, ProbeCount = 0, SnapProbes = 0;
	bool ProbePending = false;
	HRESULT LastDrawHR = S_OK, ProbePSHR = S_OK;
	DWORD ProbeBefore[4] = {};              // stage 2 TCI/TTF, stage 3 TCI/TTF before the draw
	IDirect3DSurface9 *ProbeRT = nullptr;
	DWORD OldMapTCI3 = 0, OldMapTTF3 = 0;

	// silhouette pixels (alpha < 255) and how many pixels were read, -1 if the read failed
	int CountShadowed(IDirect3DDevice9 *Dev, IDirect3DSurface9 *RT, int &Total)
	{
		D3DSURFACE_DESC D = {};
		RT->GetDesc(&D);
		Total = 0;
		if (D.Format != D3DFMT_A8R8G8B8)
			return -2;
		IDirect3DSurface9 *Sys = nullptr;
		int n = -1;
		if (SUCCEEDED(Dev->CreateOffscreenPlainSurface(D.Width, D.Height, D.Format, D3DPOOL_SYSTEMMEM, &Sys, nullptr)) &&
			SUCCEEDED(U2PerfReadback(Dev, RT, Sys)))
		{
			D3DLOCKED_RECT L;
			if (SUCCEEDED(Sys->LockRect(&L, nullptr, D3DLOCK_READONLY)))
			{
				n = 0;
				for (UINT y = 0; y < D.Height; y++)
				{
					const BYTE *Row = (const BYTE *)L.pBits + y * L.Pitch;
					for (UINT x = 0; x < D.Width; x++)
						n += Row[x * 4 + 3] < 255;
				}
				Total = D.Width * D.Height;
				Sys->UnlockRect();
			}
		}
		if (Sys) Sys->Release();
		return n;
	}

	void ProbeAfter(IDirect3DDevice9 *Dev)
	{
		if (!ProbePending)
			return;
		ProbePending = false;
		DWORD a[4] = {};
		Dev->GetTextureStageState(2, D3DTSS_TEXCOORDINDEX, &a[0]);
		Dev->GetTextureStageState(2, D3DTSS_TEXTURETRANSFORMFLAGS, &a[1]);
		Dev->GetTextureStageState(3, D3DTSS_TEXCOORDINDEX, &a[2]);
		Dev->GetTextureStageState(3, D3DTSS_TEXTURETRANSFORMFLAGS, &a[3]);
		int Total = 0, n = ProbeRT ? CountShadowed(Dev, ProbeRT, Total) : -3;
		Message("pcssprobe %d: draw hr %08x, A shadowed %d of %d; after: st2 %x/%x st3 %x/%x\n",
			ProbeCount, (unsigned)LastDrawHR, n, Total, a[0], a[1], a[2], a[3]);
		if (ProbeRT) { ProbeRT->Release(); ProbeRT = nullptr; }
	}

	// lightprobe: when U2Shaders\lightprobe.req appears, log the next two frames event by event
	// (lights set / enabled, draws with their target and active lights) to dump\lightprobe.txt.
	// For "a character loses its lamp while one of our shadows uses that lamp".
	int LightProbeFrames = 0;
	FILE *LightProbeFile = nullptr;
	void LightProbeCheck()
	{
		if (LightProbeFrames > 0 && --LightProbeFrames == 0 && LightProbeFile)
		{
			fclose(LightProbeFile);
			LightProbeFile = nullptr;
			Message("lightprobe: written");
		}
		if (Frame % 30 != 0 || LightProbeFrames > 0 || Dir.empty())
			return;
		std::string Req = Dir + "U2Shaders\\lightprobe.req";
		if (GetFileAttributesA(Req.c_str()) == INVALID_FILE_ATTRIBUTES)
			return;
		DeleteFileA(Req.c_str());
		if (!fopen_s(&LightProbeFile, (Dir + "U2Shaders\\dump\\lightprobe.txt").c_str(), "w") && LightProbeFile)
			LightProbeFrames = 3;          // the rest of this frame, then two whole frames
	}
	void LightProbeLight(IDirect3DDevice9 *Dev, DWORD Index, const D3DLIGHT9 &L)
	{
		if (LightProbeFile)
			fprintf(LightProbeFile, "setlight %u %s type %u dif %.2f %.2f %.2f pos %.0f %.0f %.0f range %.0f\n", Index,
				Offscreen(Dev) ? "OFF" : "main", (unsigned)L.Type, L.Diffuse.r, L.Diffuse.g, L.Diffuse.b,
				L.Position.x, L.Position.y, L.Position.z, L.Range);
	}
	void LightProbeEnable(DWORD Index, BOOL On)
	{
		if (LightProbeFile)
			fprintf(LightProbeFile, "enable %u %d\n", Index, (int)On);
	}
	void LightProbeDraw(IDirect3DDevice9 *Dev, DWORD Hash, bool HasTex, bool FixedFunction)
	{
		if (!LightProbeFile)
			return;
		DWORD lighting = 0, op0 = 0, arg1 = 0, blend = 0;
		Dev->GetRenderState(D3DRS_LIGHTING, &lighting);
		Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
		Dev->GetTextureStageState(0, D3DTSS_COLOROP, &op0);
		Dev->GetTextureStageState(0, D3DTSS_COLORARG1, &arg1);
		bool off = Offscreen(Dev);
		bool map = off && !HasTex && !blend && op0 == D3DTOP_SELECTARG1 && (arg1 & 0xF) == D3DTA_TFACTOR;
		if (!lighting && !map && FixedFunction)
			return;                         // unlit draws (world with lightmaps, HUD) don't matter here
		fprintf(LightProbeFile, "draw %s %08x%s lighting %u %s", off ? "OFF" : "main", HasTex ? Hash : 0, map ? " SILHOUETTE" : "", lighting, FixedFunction ? "ff" : "VS");
		{
			DWORD amb = 0, cv = 0, dsrc = 0, asrc = 0, norm = 0, spec = 0, op[2] = {}, a1[2] = {}, a2[2] = {};
			Dev->GetRenderState(D3DRS_AMBIENT, &amb);
			Dev->GetRenderState(D3DRS_COLORVERTEX, &cv);
			Dev->GetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, &dsrc);
			Dev->GetRenderState(D3DRS_AMBIENTMATERIALSOURCE, &asrc);
			Dev->GetRenderState(D3DRS_NORMALIZENORMALS, &norm);
			Dev->GetRenderState(D3DRS_SPECULARENABLE, &spec);
			for (DWORD st = 0; st < 2; st++)
			{
				Dev->GetTextureStageState(st, D3DTSS_COLOROP, &op[st]);
				Dev->GetTextureStageState(st, D3DTSS_COLORARG1, &a1[st]);
				Dev->GetTextureStageState(st, D3DTSS_COLORARG2, &a2[st]);
			}
			D3DMATERIAL9 M = {};
			Dev->GetMaterial(&M);
			D3DMATRIX W = {};
			Dev->GetTransform(D3DTS_WORLD, &W);
			DWORD fvf = 0;
			Dev->GetFVF(&fvf);
			fprintf(LightProbeFile, " | amb %08x cv %u dsrc %u asrc %u norm %u spec %u st0 %u %x %x st1 %u %x %x mat d %.2f a %.2f e %.2f | W %.2f %.2f %.2f t %.0f %.0f %.0f fvf %x",
				amb, cv, dsrc, asrc, norm, spec, op[0], a1[0], a2[0], op[1], a1[1], a2[1], M.Diffuse.r, M.Ambient.r, M.Emissive.r,
				W._11, W._22, W._33, W._41, W._42, W._43, fvf);
		}
		if (!FixedFunction)
		{
			// a vertex shader lights it: its constants carry the lights
			float C[24][4] = {};
			Dev->GetVertexShaderConstantF(0, C[0], 24);
			fprintf(LightProbeFile, " | vsc");
			for (int i = 0; i < 24; i++)
				fprintf(LightProbeFile, " c%d(%.2f %.2f %.2f %.2f)", i, C[i][0], C[i][1], C[i][2], C[i][3]);
		}
		for (DWORD i = 0; i < 8; i++)
		{
			BOOL on = FALSE;
			D3DLIGHT9 L = {};
			if (SUCCEEDED(Dev->GetLightEnable(i, &on)) && on && SUCCEEDED(Dev->GetLight(i, &L)))
				fprintf(LightProbeFile, " | L%u %.2f %.2f %.2f r%.0f", i, L.Diffuse.r, L.Diffuse.g, L.Diffuse.b, L.Range);
		}
		fprintf(LightProbeFile, "\n");
	}

	// relight (on unless relight=0): when an actor casts a shadow from a real light, Unreal II leaves that
	// light out when it draws the actor (it lights it with the next lights instead, or none), so a character
	// under one lamp turns black. Each shadow silhouette (drawn offscreen with the actor's own world matrix)
	// remembers the lights that were on; a lit draw in the main view with the same world matrix gets any of
	// them that is missing turned on in a free slot, for that draw only.
	bool Relight = true;
	struct RelightSeen { D3DMATRIX W; D3DLIGHT9 L[8]; DWORD Mask; unsigned Frame; };
	std::vector<RelightSeen> RelightList;
	DWORD RelightOn = 0;                  // lights turned on for the current draw (RelightEnd turns them off)
	unsigned RelightCount = 0;
	static bool SameWorld(const D3DMATRIX &A, const D3DMATRIX &B)
	{
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 3; c++)
				if (fabsf(A.m[r][c] - B.m[r][c]) > (r == 3 ? 0.5f : 0.01f))
					return false;
		return true;
	}
	void RelightBegin(IDirect3DDevice9 *Dev, bool HasTex, bool FixedFunction)
	{
		RelightOn = 0;
		if (!Relight || !FixedFunction)
			return;
		DWORD lighting = 0;
		Dev->GetRenderState(D3DRS_LIGHTING, &lighting);
		if (!lighting)
		{
			DWORD op0 = 0, arg1 = 0, blend = 0;
			if (HasTex)
				return;
			Dev->GetTextureStageState(0, D3DTSS_COLOROP, &op0);
			Dev->GetTextureStageState(0, D3DTSS_COLORARG1, &arg1);
			if (op0 != D3DTOP_SELECTARG1 || (arg1 & 0xF) != D3DTA_TFACTOR)
				return;
			Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
			if (blend || !Offscreen(Dev))
				return;
			RelightSeen S = {};
			Dev->GetTransform(D3DTS_WORLD, &S.W);
			for (DWORD i = 0; i < 8; i++)
			{
				BOOL on = FALSE;
				if (SUCCEEDED(Dev->GetLightEnable(i, &on)) && on && SUCCEEDED(Dev->GetLight(i, &S.L[i])))
					S.Mask |= 1u << i;
			}
			if (!S.Mask)
				return;
			S.Frame = Frame;
			for (auto &E : RelightList)
				if (SameWorld(E.W, S.W)) { E = S; return; }
			if (RelightList.size() < 64)
				RelightList.push_back(S);
			return;
		}
		if (RelightList.empty())
			return;
		D3DMATRIX W;
		Dev->GetTransform(D3DTS_WORLD, &W);
		for (auto &E : RelightList)
		{
			if (Frame - E.Frame > 1 || !SameWorld(E.W, W))
				continue;
			if (Offscreen(Dev))
				return;
			// which of the silhouette's lights is missing now (the engine may light it with others instead)
			D3DLIGHT9 On[8] = {};
			DWORD OnMask = 0;
			for (DWORD i = 0; i < 8; i++)
			{
				BOOL on = FALSE;
				if (SUCCEEDED(Dev->GetLightEnable(i, &on)) && on && SUCCEEDED(Dev->GetLight(i, &On[i])))
					OnMask |= 1u << i;
			}
			for (DWORD k = 0; k < 8; k++)
			{
				if (!(E.Mask & (1u << k)))
					continue;
				bool have = false;
				for (DWORD i = 0; i < 8 && !have; i++)
					have = (OnMask & (1u << i)) && fabsf(On[i].Position.x - E.L[k].Position.x) + fabsf(On[i].Position.y - E.L[k].Position.y)
						+ fabsf(On[i].Position.z - E.L[k].Position.z) < 1.0f && fabsf(On[i].Diffuse.r - E.L[k].Diffuse.r) < 0.01f;
				if (have)
					continue;
				DWORD slot = 0;
				while (slot < 8 && ((OnMask | RelightOn) & (1u << slot)))
					slot++;
				if (slot == 8)
					break;
				Dev->SetLight(slot, &E.L[k]);
				Dev->LightEnable(slot, TRUE);
				RelightOn |= 1u << slot;
			}
			if (RelightOn && RelightCount++ == 0)
				Message("relight: gave an actor back the light its shadow is cast from");
			return;
		}
	}
	void RelightEnd(IDirect3DDevice9 *Dev)
	{
		for (DWORD i = 0; i < 8; i++)
			if (RelightOn & (1u << i))
				Dev->LightEnable(i, FALSE);
		RelightOn = 0;
	}
	void RelightFrame()
	{
		RelightList.erase(std::remove_if(RelightList.begin(), RelightList.end(), [&](const RelightSeen &E) { return Frame - E.Frame > 2; }), RelightList.end());
	}

	void ClearBlurSources() { for (auto &It : BlurSource) if (It.second) It.second->Release(); BlurSource.clear(); }
	DWORD OldTCI3 = 0, OldTTF3 = 0, OldSrc = 0, OldDst = 0;
	bool RawDebug = false;
	D3DMATRIX OldTexMat3 = {};

	static D3DMATRIX Mul(const D3DMATRIX &A, const D3DMATRIX &B)
	{
		D3DMATRIX R = {};
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 4; c++)
				for (int k = 0; k < 4; k++)
					R.m[r][c] += A.m[r][k] * B.m[k][c];
		return R;
	}

	// inverse of a view matrix (rotation + translation, row-vector convention)
	static D3DMATRIX InvView(const D3DMATRIX &V)
	{
		D3DMATRIX R = {};
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				R.m[r][c] = V.m[c][r];
		for (int c = 0; c < 3; c++)
			R.m[3][c] = -(V.m[3][0] * R.m[0][c] + V.m[3][1] * R.m[1][c] + V.m[3][2] * R.m[2][c]);
		R.m[3][3] = 1;
		return R;
	}

	// the game draws a shadow into a render surface and copies it into the projector's texture
	void OnCopy(IDirect3DSurface9 *From, IDirect3DSurface9 *To)
	{
		auto It = MapViews.find(From);
		if (It != MapViews.end())
		{
			MapViews[To] = It->second;
			static std::map<UINT, bool> told;
			D3DSURFACE_DESC A = {}, B = {};
			From->GetDesc(&A); To->GetDesc(&B);
			if (!told[A.Width])
			{
				told[A.Width] = true;
				Message("pcss copy %ux%u fmt %u -> %ux%u fmt %u usage %x pool %u\n", A.Width, A.Height, (unsigned)A.Format, B.Width, B.Height, (unsigned)B.Format, (unsigned)B.Usage, (unsigned)B.Pool);
			}
		}
	}

	// ---- gloss= : wet highlights added over a decal -----------------------------------------
	// Advent's blood decals multiply the floor (twice the floor times the texture, mid-grey =
	// unchanged), which can darken and redden but never shine. After such a draw the device
	// draws the same geometry again (U2GlossBegin) with the rule's shader, blended ONE/ONE: what
	// the shader returns is added. The shader gets what decal= shaders get (s0 the decal,
	// TEXCOORD0 its coordinates, projected when c1.x = 1; TEXCOORD2 the camera-space position;
	// c0 time, 1, 1/w, 1/h; c1.yz blend kind and neutral brightness; c4..c7 the projection) and:
	//   c2  glossfx=  strength, highlight sharpness (Blinn exponent), light gain, sheen gain
	//   c3  glossenv= the colour a grazing look reflects (r g b, and a multiplier)
	//   c1.w how wet this decal still is (1 fresh .. 0 dry), from its age (see GlossAge)
	//   c8..c15 up to 4 of the game's lights in camera space, nearest the camera first:
	//       c8+2i = position (or the direction toward a directional light) and range (0:
	//       directional), c9+2i = colour and 1 if used. The lights are the ones the game set for
	//       lit draws (characters) in the frame before (gi's light gathering: gi=1 or ssao=1).
	// Fog stays on with a black fog colour, so highlights fade with distance like the rest.
	// The pass blends ONE / SRCALPHA: the shader's colour is added and the alpha multiplies what
	// is there, so a drying decal can also darken (blood going brown-black as it dries).
	std::vector<U2Rule> GlossRules;
	// Drying: each projector decal is told apart by where it sits in the world (its texture
	// matrix with the view taken out: the projector's own, fixed while it stays put) and aged
	// from the first time it was drawn. A growing pool changes a little each frame and is
	// matched to the nearest known one, so it stays fresh while it spreads and ages from then.
	// glossdry= seconds wet, seconds until dry, how much a dry decal darkens (0-1).
	float GlossDry[4] = { 60.0f, 240.0f, 0.4f, 0.0f };
	// Puddle reflections: glossreflect= strength, reach (world units). With a strength above 0
	// the pass also gets the frame drawn so far in s1 (CopyScene, once a frame: decals come
	// after the level, so it holds what stands around the pool) and c16.yz = strength, reach;
	// the shader follows the reflected view ray a few steps and samples the frame where they
	// land on screen: a soft reflection of what is above the pool.
	float GlossReflect[2] = { 0.0f, 300.0f };
	IDirect3DBaseTexture9 *GlossOldTex1 = nullptr;
	DWORD GlossOldSamp1[5] = {};
	bool GlossBound1 = false;
	struct GlossDecal { float M[16]; DWORD Born, Seen; };
	std::vector<GlossDecal> GlossDecals;
	float GlossAge(IDirect3DDevice9 *Dev)
	{
		D3DMATRIX T, V;
		Dev->GetTransform(D3DTS_TEXTURE0, &T);
		Dev->GetTransform(D3DTS_VIEW, &V);
		// texture coordinates = camera position x T, camera position = world x V: world x (V T)
		float W[16];
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 4; c++)
			{
				float x = 0;
				for (int k = 0; k < 4; k++)
					x += V.m[r][k] * T.m[k][c];
				W[r * 4 + c] = x;
			}
		const DWORD Now = GetTickCount();
		GlossDecal *Best = nullptr;
		float BestD = 1e30f;
		for (GlossDecal &D : GlossDecals)
		{
			// the rotation/scale part relative to its size, the translation part as is
			float Scale = 1e-6f, Diff = 0;
			for (int i = 0; i < 12; i++)
			{
				Scale = (std::max)(Scale, fabsf(D.M[i]));
				Diff = (std::max)(Diff, fabsf(D.M[i] - W[i]));
			}
			// the translation row is large far from the level's origin: compared relative to its size
			float Rel = Diff / Scale, Move = 0, Big = 1;
			for (int i = 12; i < 16; i++)
			{
				Move = (std::max)(Move, fabsf(D.M[i] - W[i]));
				Big = (std::max)(Big, fabsf(D.M[i]));
			}
			Move /= Big;
			const float Dist = Rel + Move;
			if (Rel < 0.03f && Move < 0.01f && Dist < BestD)
			{
				BestD = Dist;
				Best = &D;
			}
		}
		if (Best == nullptr)
		{
			// forget decals not drawn for 10 minutes (and the oldest past 4096)
			GlossDecals.erase(std::remove_if(GlossDecals.begin(), GlossDecals.end(), [Now](const GlossDecal &D) { return Now - D.Seen > 600000; }), GlossDecals.end());
			if (GlossDecals.size() >= 4096)
				GlossDecals.erase(GlossDecals.begin());
			GlossDecal N;
			memcpy(N.M, W, sizeof(W));
			N.Born = N.Seen = Now;
			GlossDecals.push_back(N);
			return 0;
		}
		memcpy(Best->M, W, sizeof(W));       // a growing pool: follow it
		Best->Seen = Now;
		return (Now - Best->Born) / 1000.0f;
	}
	std::vector<D3DLIGHT9> GlossLights;
	float GlossFx[4] = { 1.0f, 90.0f, 1.0f, 0.35f };
	float GlossEnv[4] = { 0.6f, 0.58f, 0.56f, 1.0f };
	IDirect3DPixelShader9 *GlossOldPS = nullptr;
	IDirect3DBaseTexture9 *GlossOldTex0 = nullptr;
	float GlossOldConst[17][4];
	DWORD GlossOldRS[6], GlossOldTCI[3], GlossOldTTF[3];
	D3DMATRIX GlossOldTexMat[3];
	bool GlossSwapped0 = false;
	// before the second draw: false = no gloss for this one
	bool GlossBegin(IDirect3DDevice9 *Dev, IDirect3DTexture9 *Tex, DWORD Hash, bool FixedFunction)
	{
		U2Rule *R = nullptr;
		for (U2Rule &G : GlossRules)
			if (G.Hash == Hash)
				R = &G;
		if (R == nullptr || !FixedFunction || Offscreen(Dev))
			return false;
		DWORD Blending = 0;
		Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &Blending);
		if (!Blending)
			return false;
		IDirect3DPixelShader9 *PS = Compile(Dev, *R);
		if (PS == nullptr)
			return false;
		float C[16][4] = {};
		// the decal's blending, as decal= rules read it (before ours replaces it)
		{
			DWORD Src = 0, Dst = 0;
			Dev->GetRenderState(D3DRS_SRCBLEND, &Src);
			Dev->GetRenderState(D3DRS_DESTBLEND, &Dst);
			C[1][1] = 0; C[1][2] = 1;
			if ((Src == D3DBLEND_DESTCOLOR && Dst == D3DBLEND_SRCCOLOR) || (Src == D3DBLEND_SRCCOLOR && Dst == D3DBLEND_DESTCOLOR)) { C[1][1] = 2; C[1][2] = 0.5f; }
			else if ((Src == D3DBLEND_DESTCOLOR && Dst == D3DBLEND_ZERO) || (Src == D3DBLEND_ZERO && Dst == D3DBLEND_SRCCOLOR)) { C[1][1] = 1; C[1][2] = 1; }
		}
		static const D3DRENDERSTATETYPE RS[6] = { D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_FOGCOLOR, D3DRS_ZWRITEENABLE };
		for (int i = 0; i < 6; i++)
			Dev->GetRenderState(RS[i], &GlossOldRS[i]);
		Dev->GetPixelShader(&GlossOldPS);
		Dev->GetPixelShaderConstantF(0, GlossOldConst[0], 17);
		// the live blood pools' simulated sheet in place of their placeholder, as for the draw itself
		GlossSwapped0 = false;
		DWORD H = Hash;
		if (IDirect3DTexture9 *Rep = Replacement(Dev, Tex, H))
		{
			Dev->GetTexture(0, &GlossOldTex0);
			Dev->SetTexture(0, Rep);
			GlossSwapped0 = true;
		}
		// camera-space normal and position from stages 1 and 2 (as Begin does)
		static const D3DMATRIX Identity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
		Dev->GetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, &GlossOldTTF[0]);
		for (DWORD s = 1; s <= 2; s++)
		{
			Dev->GetTextureStageState(s, D3DTSS_TEXCOORDINDEX, &GlossOldTCI[s]);
			Dev->GetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, &GlossOldTTF[s]);
			Dev->GetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), &GlossOldTexMat[s]);
			Dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, (s == 1 ? D3DTSS_TCI_CAMERASPACENORMAL : D3DTSS_TCI_CAMERASPACEPOSITION) | s);
			Dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
			Dev->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), &Identity);
		}
		C[0][0] = (GetTickCount() % 3600000) / 1000.0f;
		C[0][1] = 1;
		C[0][2] = SceneW ? 1.0f / SceneW : 0;
		C[0][3] = SceneH ? 1.0f / SceneH : 0;
		C[1][0] = (GlossOldTTF[0] & D3DTTFF_PROJECTED) ? 1.0f : 0.0f;
		C[1][3] = 1;
		if (C[1][0] > 0.5f && GlossDry[1] > GlossDry[0])
		{
			const float Age = GlossAge(Dev);
			float t = (Age - GlossDry[0]) / (GlossDry[1] - GlossDry[0]);
			t = t < 0 ? 0 : t > 1 ? 1 : t;
			C[1][3] = 1 - t * t * (3 - 2 * t);
		}
		memcpy(C[2], GlossFx, sizeof(GlossFx));
		memcpy(C[3], GlossEnv, sizeof(GlossEnv));
		D3DMATRIX P, V;
		Dev->GetTransform(D3DTS_PROJECTION, &P);
		Dev->GetTransform(D3DTS_VIEW, &V);
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 4; c++)
				C[4 + r][c] = P.m[r][c];
		PackLights(V, C);                  // the lights in camera space, the nearest four
		Dev->SetPixelShaderConstantF(0, C[0], 16);
		GlossBound1 = false;
		if (GlossReflect[0] > 0 && CopyScene(Dev) && SceneTex != nullptr)
		{
			static const D3DSAMPLERSTATETYPE Samp[5] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER };
			static const DWORD Val[5] = { D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE };
			Dev->GetTexture(1, &GlossOldTex1);
			for (int i = 0; i < 5; i++)
			{
				Dev->GetSamplerState(1, Samp[i], &GlossOldSamp1[i]);
				Dev->SetSamplerState(1, Samp[i], Val[i]);
			}
			Dev->SetTexture(1, SceneTex);
			GlossBound1 = true;
		}
		const float DryC[4] = { GlossDry[2], GlossBound1 ? GlossReflect[0] : 0.0f, GlossReflect[1], 0 };
		Dev->SetPixelShaderConstantF(16, DryC, 1);     // c16.x: how much a dry decal darkens
		Dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		Dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
		Dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCALPHA);
		Dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
		Dev->SetRenderState(D3DRS_FOGCOLOR, 0);
		Dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		Dev->SetPixelShader(PS);
		static bool Told = false;
		if (!Told)
		{
			Told = true;
			Message("gloss: first draw (%08x with %s, %d of the game's lights)", Hash, R->File.c_str(), (int)GlossLights.size());
		}
		static DWORD LastTold = 0;
		if (GetTickCount() - LastTold > 20000)
		{
			LastTold = GetTickCount();
			Message("gloss: %d decals known (drying), this one wet %.2f", (int)GlossDecals.size(), C[1][3]);
		}
		return true;
	}
	void GlossEnd(IDirect3DDevice9 *Dev)
	{
		static const D3DRENDERSTATETYPE RS[6] = { D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_FOGCOLOR, D3DRS_ZWRITEENABLE };
		for (int i = 0; i < 6; i++)
			Dev->SetRenderState(RS[i], GlossOldRS[i]);
		Dev->SetPixelShader(GlossOldPS);
		if (GlossOldPS) { GlossOldPS->Release(); GlossOldPS = nullptr; }
		Dev->SetPixelShaderConstantF(0, GlossOldConst[0], 17);
		for (DWORD s = 1; s <= 2; s++)
		{
			Dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, GlossOldTCI[s]);
			Dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, GlossOldTTF[s]);
			Dev->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), &GlossOldTexMat[s]);
		}
		if (GlossBound1)
		{
			static const D3DSAMPLERSTATETYPE Samp[5] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER };
			Dev->SetTexture(1, GlossOldTex1);
			for (int i = 0; i < 5; i++)
				Dev->SetSamplerState(1, Samp[i], GlossOldSamp1[i]);
			if (GlossOldTex1) { GlossOldTex1->Release(); GlossOldTex1 = nullptr; }
			GlossBound1 = false;
		}
		if (GlossSwapped0)
		{
			Dev->SetTexture(0, GlossOldTex0);
			if (GlossOldTex0) { GlossOldTex0->Release(); GlossOldTex0 = nullptr; }
			GlossSwapped0 = false;
		}
	}

	// ---- sheen= : highlights and a grazing reflection added over solid world surfaces --------
	// The world is lightmapped: metal panels and polished floors come out as matte as concrete.
	// sheen=HASH sheen.hlsl [strength sharpness metal] draws a solid fixed-function draw with that
	// texture once more with the shader, added on top (ONE/ONE, depth equal, fog black): the
	// texture (s0, TEXCOORD0) and the draw's own lightmap (s1, TEXCOORD1, when stage 1 has one) or
	// vertex light (COLOR0) say how lit the spot is; TEXCOORD4/5 are the camera-space normal and
	// position. What it adds: Blinn highlights from the game's lights near the camera (the ones it
	// set for lit draws, as gloss= gets them) and a Fresnel reflection of the "room" whose
	// brightness is the baked light at that spot, brighter when the reflection looks up (lamps,
	// sky). The texture's own brightness masks it (scuffs and seams shine less), and "metal"
	// tints the reflection with the texture's colour.
	//   c0  time, 1, 1/w, 1/h       c2 sheenfx= strength, sharpness, Fresnel, mask contrast
	//   c3  sheenenv= the room's colour (r g b), its multiplier
	//   c8..c15 lights (as gloss=)  c16 1 if a lightmap is on stage 1, the lightmap's scale (x2/x4)
	//   c17 the world's up in camera space   c18 the rule's own strength, sharpness, metal (0 = global)
	std::vector<U2Rule> SheenRules;
	float SheenFx[4] = { 1.0f, 60.0f, 1.0f, 0.6f };
	float SheenEnv[4] = { 0.55f, 0.55f, 0.6f, 1.0f };
	IDirect3DPixelShader9 *SheenOldPS = nullptr;
	float SheenOldConst[19][4];
	DWORD SheenOldRS[7], SheenOldTCI[2], SheenOldTTF[2];
	D3DMATRIX SheenOldTexMat[2];
	unsigned SheenDraws = 0;

	// the frame's lights in camera space, the nearest four, into C[8..15] (gloss= and sheen=)
	void PackLights(const D3DMATRIX &V, float C[][4])
	{
		struct Near { float D; int I; };
		std::vector<Near> Order;
		for (size_t i = 0; i < GlossLights.size(); i++)
		{
			const D3DLIGHT9 &L = GlossLights[i];
			float D = 0;
			if (L.Type != D3DLIGHT_DIRECTIONAL)
			{
				const D3DVECTOR &p = L.Position;
				const float cx = p.x * V._11 + p.y * V._21 + p.z * V._31 + V._41;
				const float cy = p.x * V._12 + p.y * V._22 + p.z * V._32 + V._42;
				const float cz = p.x * V._13 + p.y * V._23 + p.z * V._33 + V._43;
				D = sqrtf(cx * cx + cy * cy + cz * cz);
			}
			Order.push_back({ D, (int)i });
		}
		std::sort(Order.begin(), Order.end(), [](const Near &A, const Near &B) { return A.D < B.D; });
		for (size_t k = 0; k < Order.size() && k < 4; k++)
		{
			const D3DLIGHT9 &L = GlossLights[Order[k].I];
			float *Pos = C[8 + 2 * k], *Col = C[9 + 2 * k];
			if (L.Type == D3DLIGHT_DIRECTIONAL)
			{
				const D3DVECTOR &d = L.Direction;
				Pos[0] = -(d.x * V._11 + d.y * V._21 + d.z * V._31);
				Pos[1] = -(d.x * V._12 + d.y * V._22 + d.z * V._32);
				Pos[2] = -(d.x * V._13 + d.y * V._23 + d.z * V._33);
				Pos[3] = 0;
			}
			else
			{
				const D3DVECTOR &p = L.Position;
				Pos[0] = p.x * V._11 + p.y * V._21 + p.z * V._31 + V._41;
				Pos[1] = p.x * V._12 + p.y * V._22 + p.z * V._32 + V._42;
				Pos[2] = p.x * V._13 + p.y * V._23 + p.z * V._33 + V._43;
				Pos[3] = L.Range > 1 ? L.Range : 1000;
			}
			Col[0] = L.Diffuse.r; Col[1] = L.Diffuse.g; Col[2] = L.Diffuse.b; Col[3] = 1;
		}
	}

	bool SheenBegin(IDirect3DDevice9 *Dev, DWORD Hash, bool FixedFunction)
	{
		U2Rule *R = nullptr;
		for (U2Rule &G : SheenRules)
			if (G.Hash == Hash)
				R = &G;
		if (R == nullptr || !FixedFunction || Offscreen(Dev))
			return false;
		DWORD Blending = 0, Z = 0;
		Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &Blending);
		Dev->GetRenderState(D3DRS_ZENABLE, &Z);
		if (Blending || !Z)
			return false;
		D3DMATRIX P, V;
		Dev->GetTransform(D3DTS_PROJECTION, &P);
		if (P._34 != 1.0f || P._44 != 0.0f)
			return false;
		IDirect3DPixelShader9 *PS = Compile(Dev, *R);
		if (PS == nullptr)
			return false;
		Dev->GetTransform(D3DTS_VIEW, &V);
		float C[19][4] = {};
		C[0][0] = (GetTickCount() % 3600000) / 1000.0f;
		C[0][1] = 1;
		C[0][2] = SceneW ? 1.0f / SceneW : 0;
		C[0][3] = SceneH ? 1.0f / SceneH : 0;
		memcpy(C[2], SheenFx, sizeof(SheenFx));
		memcpy(C[3], SheenEnv, sizeof(SheenEnv));
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 4; c++)
				C[4 + r][c] = P.m[r][c];
		PackLights(V, C);
		// stage 1: the lightmap (its own coordinates stay), and how it was combined
		DWORD Op1 = D3DTOP_DISABLE;
		IDirect3DBaseTexture9 *T1 = nullptr;
		Dev->GetTextureStageState(1, D3DTSS_COLOROP, &Op1);
		Dev->GetTexture(1, &T1);
		if (T1 != nullptr && Op1 != D3DTOP_DISABLE)
		{
			C[16][0] = 1;
			C[16][1] = Op1 == D3DTOP_MODULATE4X ? 4.0f : Op1 == D3DTOP_MODULATE2X ? 2.0f : 1.0f;
		}
		if (T1) T1->Release();
		C[17][0] = V._31; C[17][1] = V._32; C[17][2] = V._33;     // world +z, turned into camera space
		memcpy(C[18], R->Levels, sizeof(R->Levels));
		static const D3DRENDERSTATETYPE RS[7] = { D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_FOGCOLOR, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC };
		for (int i = 0; i < 7; i++)
			Dev->GetRenderState(RS[i], &SheenOldRS[i]);
		Dev->GetPixelShader(&SheenOldPS);
		Dev->GetPixelShaderConstantF(0, SheenOldConst[0], 19);
		static const D3DMATRIX Identity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
		for (DWORD s = 4; s <= 5; s++)
		{
			Dev->GetTextureStageState(s, D3DTSS_TEXCOORDINDEX, &SheenOldTCI[s - 4]);
			Dev->GetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, &SheenOldTTF[s - 4]);
			Dev->GetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), &SheenOldTexMat[s - 4]);
			Dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, (s == 4 ? D3DTSS_TCI_CAMERASPACENORMAL : D3DTSS_TCI_CAMERASPACEPOSITION) | s);
			Dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
			Dev->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), &Identity);
		}
		Dev->SetPixelShaderConstantF(0, C[0], 19);
		Dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		Dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
		Dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
		Dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
		Dev->SetRenderState(D3DRS_FOGCOLOR, 0);
		Dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		Dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
		Dev->SetPixelShader(PS);
		if (SheenDraws++ == 0)
			Message("sheen: first draw (%08x with %s, lightmap %s x%.0f, %d of the game's lights)", Hash, R->File.c_str(),
				C[16][0] > 0 ? "on stage 1" : "none (vertex light)", C[16][1], (int)GlossLights.size());
		return true;
	}
	void SheenEnd(IDirect3DDevice9 *Dev)
	{
		static const D3DRENDERSTATETYPE RS[7] = { D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_FOGCOLOR, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC };
		for (int i = 0; i < 7; i++)
			Dev->SetRenderState(RS[i], SheenOldRS[i]);
		Dev->SetPixelShader(SheenOldPS);
		if (SheenOldPS) { SheenOldPS->Release(); SheenOldPS = nullptr; }
		Dev->SetPixelShaderConstantF(0, SheenOldConst[0], 19);
		for (DWORD s = 4; s <= 5; s++)
		{
			Dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, SheenOldTCI[s - 4]);
			Dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, SheenOldTTF[s - 4]);
			Dev->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), &SheenOldTexMat[s - 4]);
		}
	}

	// ---- live command channel: console commands from outside, run on the game's thread ---------
	// A tool (U2Avalon's live.py, AdventMod's live tools) writes lines to System\U2Live.cmd
	// (atomically: a temporary file renamed into place). At the next Present the fork reads the
	// file and deletes it; the lines are run from the game window's message loop (a message
	// posted to it, handled by a hook on its window procedure), outside rendering: run from
	// Present itself, the first command hung Advent (the viewport is locked mid-frame). Each
	// line runs as a console command of the player's viewport
	// (UViewport::Exec, the path a typed command takes: exec functions of the player, its
	// interactions and mutators' ExecManagers all see it). Blank lines and lines starting with
	// # or ; are skipped. Replies go to the game's own log.
	//   System\U2Live.ack     the last "<word> batch N" line run (N), written after each file
	//   System\U2Live.status  "up <unix seconds> <exe> <map>", rewritten every 2 seconds while
	//                         the game runs: the channel is up when it is fresh
	// Off unless U2Shaders.ini has live=1 (any program that can write the game's System folder
	// could run console commands through it); every command run is logged.
	bool LiveOn = false, LiveBroken = false, LiveTold = false;
	DWORD LiveStatusTick = 0;
	void *LiveViewport = nullptr;
	typedef int (__fastcall *ViewportExec_t)(void *This, void *Edx, const wchar_t *Cmd, void *Out);
	typedef const wchar_t *(__fastcall *ObjGetName_t)(void *This, void *Edx);
	typedef void *(__fastcall *ObjGetClass_t)(void *This, void *Edx);
	struct ObjArray { void **Data; int Num, Max; };
	ViewportExec_t LiveExec = nullptr;
	ObjGetName_t LiveGetName = nullptr;
	ObjGetClass_t LiveGetClass = nullptr;
	ObjArray *LiveObjs = nullptr;
	void **LiveLog = nullptr;
	bool LiveBind()
	{
		if (LiveExec != nullptr)
			return true;
		HMODULE Core = GetModuleHandleA("Core.dll"), Engine = GetModuleHandleA("Engine.dll");
		if (!Core || !Engine)
			return false;
		LiveObjs = (ObjArray *)GetProcAddress(Core, "?GObjObjects@UObject@@1V?$TArray@PAVUObject@@@@A");
		if (LiveObjs == nullptr)                   // Unreal II exports it as private (0V), Advent as protected (1V)
			LiveObjs = (ObjArray *)GetProcAddress(Core, "?GObjObjects@UObject@@0V?$TArray@PAVUObject@@@@A");
		LiveGetName = (ObjGetName_t)GetProcAddress(Core, "?GetName@UObject@@QBEPBGXZ");
		LiveGetClass = (ObjGetClass_t)GetProcAddress(Core, "?GetClass@UObject@@QBEPAVUClass@@XZ");
		LiveLog = (void **)GetProcAddress(Core, "?GLog@@3PAVFOutputDevice@@A");
		LiveExec = (ViewportExec_t)GetProcAddress(Engine, "?Exec@UViewport@@UAEHPBGAAVFOutputDevice@@@Z");
		if (!LiveObjs || !LiveGetName || !LiveGetClass || !LiveLog || !LiveExec)
		{
			Message("live: the engine's exports aren't there (Core/Engine.dll): no command channel");
			LiveExec = nullptr;
			LiveBroken = true;
			return false;
		}
		return true;
	}
	// the player's viewport: the first object whose class is a Viewport (WindowsViewport)
	void *LiveFindViewport()
	{
		if (LiveViewport != nullptr && Readable(LiveViewport, 64))
		{
			void *C = LiveGetClass(LiveViewport, nullptr);
			const wchar_t *N = C ? LiveGetName(C, nullptr) : nullptr;
			if (N && wcsstr(N, L"Viewport"))
				return LiveViewport;
		}
		LiveViewport = nullptr;
		for (int i = 0; i < LiveObjs->Num; i++)
		{
			void *O = LiveObjs->Data[i];
			if (O == nullptr || !Readable(O, 64))
				continue;
			void *C = LiveGetClass(O, nullptr);
			if (C == nullptr)
				continue;
			const wchar_t *CN = LiveGetName(C, nullptr);
			if (CN == nullptr || wcscmp(CN, L"Class") == 0)
				continue;
			const size_t L = wcslen(CN);
			if (L >= 8 && wcscmp(CN + L - 8, L"Viewport") == 0)
			{
				LiveViewport = O;
				Message("live: player viewport %ls", LiveGetName(O, nullptr));
				break;
			}
		}
		return LiveViewport;
	}
	std::vector<std::string> LiveQueue;
	long long LiveQueueBatch = -1;
	HWND LiveWnd = nullptr;
	WNDPROC LiveOldProc = nullptr;
	static const UINT LiveMsg = WM_APP + 0x75;
	static U2Shaders *&LiveSelf() { static U2Shaders *P = nullptr; return P; }
	static LRESULT CALLBACK LiveWndProc(HWND W, UINT M, WPARAM A, LPARAM B)
	{
		if (LiveSelf() != nullptr && (LiveSelf()->GmPanelMode != 0 || LiveSelf()->SketchOn))
		{
			LRESULT R = 0;
			if (LiveSelf()->GmPanelInput(W, M, A, B, R))
				return R;
		}
		if (M == LiveMsg && LiveSelf() != nullptr)
		{
			LiveSelf()->LiveRun();
			return 0;
		}
		if (M == GmMsg && LiveSelf() != nullptr)
		{
			LiveSelf()->GmRun();
			return 0;
		}
		return CallWindowProcW(LiveSelf() ? LiveSelf()->LiveOldProc : DefWindowProcW, W, M, A, B);
	}
	bool LiveHook(IDirect3DDevice9 *Dev)
	{
		if (LiveOldProc != nullptr)
			return true;
		D3DDEVICE_CREATION_PARAMETERS P = {};
		if (FAILED(Dev->GetCreationParameters(&P)) || P.hFocusWindow == nullptr)
			return false;
		LiveSelf() = this;
		LiveWnd = P.hFocusWindow;
		LiveOldProc = (WNDPROC)SetWindowLongPtrW(LiveWnd, GWLP_WNDPROC, (LONG_PTR)LiveWndProc);
		if (LiveOldProc == nullptr)
			return false;
		Message("live: commands run from window %p's message loop", (void *)LiveWnd);
		return true;
	}
	// in the message loop: the queued lines through the console
	void LiveRun()
	{
		if (LiveQueue.empty())
			return;
		std::vector<std::string> Lines;
		Lines.swap(LiveQueue);
		if (!LiveBind() || LiveFindViewport() == nullptr)
		{
			Message("live: commands arrived but there is no viewport yet; dropped");
			return;
		}
		// UViewport::Exec overrides FExec's: MSVC hands it the object's FExec part, not its start,
		// and calls go through that part's table (WindowsViewport overrides it in WinDrv.dll).
		// The FExec part is found by proof, not by layout: a table pointer in the object whose
		// first entry IS the exported UViewport::Exec, or calls or jumps to it (an override ends
		// by handing unknown commands to its base). Candidates that don't pass are never called.
		void *Self = nullptr;
		ViewportExec_t Call = nullptr;
		{
			char Seen[200] = "";
			for (int Off = 4; Off <= 0x80 && Self == nullptr; Off += 4)
			{
				void **Slot = (void **)((BYTE *)LiveViewport + Off);
				if (!Readable(Slot, 4) || !Readable(*Slot, 8))
					continue;
				BYTE *Fn = (BYTE *)((void **)*Slot)[0];
				HMODULE FnMod = nullptr;
				if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)Fn, &FnMod) || FnMod == nullptr)
					continue;
				bool Proven = Fn == (BYTE *)LiveExec;
				for (int i = 0; !Proven && i < 4096 && Readable(Fn + i, 6); i++)
				{
					if ((Fn[i] == 0xE8 || Fn[i] == 0xE9) && Fn + i + 5 + *(INT32 *)(Fn + i + 1) == (BYTE *)LiveExec)
						Proven = true;                       // call / jmp rel32
					else if (Fn[i] == 0xFF && (Fn[i + 1] == 0x15 || Fn[i + 1] == 0x25))
					{
						void **Imp = *(void ***)(Fn + i + 2);  // call / jmp [import]
						if (Readable(Imp, 4) && *Imp == (void *)LiveExec)
							Proven = true;
					}
				}
				snprintf(Seen + strlen(Seen), sizeof(Seen) - strlen(Seen), " +0x%x%s", Off, Proven ? "(Exec)" : "");
				if (Proven)
				{
					Self = Slot;
					Call = (ViewportExec_t)Fn;
				}
			}
			if (!LiveTold)
			{
				LiveTold = true;
				Message("live: table pointers in the viewport:%s", Seen);
			}
		}
		if (Self == nullptr || Call == nullptr)
		{
			Message("live: the viewport's Exec part wasn't found: commands not run (no guessing)");
			LiveBroken = true;
			return;
		}
		// (calling the exported UPlayer::Exec on this part froze Advent: the viewport's own Exec only)
		for (const std::string &L : Lines)
		{
			Message("live: > %s", L.c_str());
			std::wstring Cmd(L.begin(), L.end());
			const int Done = Call(Self, nullptr, Cmd.c_str(), *LiveLog);
			Message("live:   %s", Done ? "taken" : "not recognised");
		}
		FILE *A = nullptr;
		if (LiveQueueBatch >= 0 && !fopen_s(&A, (Dir + "U2Live.ack").c_str(), "w") && A)
		{
			fprintf(A, "%lld\n", LiveQueueBatch);
			fclose(A);
		}
		Message("live: ran %d command(s)%s", (int)Lines.size(), LiveQueueBatch >= 0 ? (" (batch " + std::to_string(LiveQueueBatch) + ")").c_str() : "");
		LiveQueueBatch = -1;
	}
	void LiveStatus()
	{
		if (GetTickCount() - LiveStatusTick < 2000)
			return;
		LiveStatusTick = GetTickCount();
		char Exe[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, Exe, MAX_PATH);
		const char *Base = strrchr(Exe, '\\') ? strrchr(Exe, '\\') + 1 : Exe;
		FILE *F = nullptr;
		if (!fopen_s(&F, (Dir + "U2Live.status").c_str(), "w") && F)
		{
			fprintf(F, "up %lld %s %s\n", (long long)time(nullptr), Base, CurMap.empty() ? "?" : CurMap.c_str());
			fclose(F);
		}
	}
	// ---- gorelink=1: blood commands from UnrealScript without a native DLL -------------------
	// Advent's script reaches U2BloodCommand() through AdventNative.dll; Unreal II has no native
	// bridge. Instead the mod keeps one actor of a class named GoreLink with a string property
	// (any name) that it rewrites every tick as
	//     "GL1 <seq>;<command>;<command>;..."
	// (the commands of blood.hpp/runs.hpp/streaks.hpp/strings.hpp/lens.hpp, ';' between them).
	// Each frame the layer finds that object in GObjects (the live channel's exports), finds the
	// string by its "GL1 " prefix (an FString {Data, Num, Max} inside the object; the offset is
	// remembered), and runs the commands once per new <seq>. The layer only reads: it never
	// writes the game's memory. No return values (wet, lens's 1/0): the mod decides by its own
	// settings. At most GoreLinkMax characters a frame; longer text is cut at the last ';'.
	bool GoreLinkOn = false, GoreLinkTold = false;
	void *GoreLinkObj = nullptr;
	int GoreLinkIdx = -1, GoreLinkOff = -1;
	DWORD GoreLinkScanTick = 0;
	std::wstring GoreLinkSeq;
	unsigned GoreLinkRuns = 0, GoreLinkCmds = 0;
	static const int GoreLinkMax = 16384;
	static bool GoreLinkIsLink(U2Shaders *S, void *O)
	{
		if (O == nullptr || !Readable(O, 64))
			return false;
		void *C = S->LiveGetClass(O, nullptr);
		const wchar_t *CN = C ? S->LiveGetName(C, nullptr) : nullptr;
		if (CN == nullptr || wcscmp(CN, L"GoreLink") != 0)
			return false;
		const wchar_t *N = S->LiveGetName(O, nullptr);
		return N != nullptr && wcsncmp(N, L"Default__", 9) != 0;
	}
	// the FString at Off in the object, if it is one and starts with "GL1 "
	static const wchar_t *GoreLinkText(void *O, int Off, int &Len)
	{
		struct FStr { wchar_t *Data; int Num, Max; };
		const FStr *F = (const FStr *)((BYTE *)O + Off);
		if (!Readable(F, sizeof(FStr)) || F->Data == nullptr || F->Num < 5 || F->Num > (1 << 20) || F->Max < F->Num)
			return nullptr;
		if (!Readable(F->Data, F->Num * sizeof(wchar_t)) || wcsncmp(F->Data, L"GL1 ", 4) != 0)
			return nullptr;
		Len = F->Num - 1;                        // Num counts the terminating zero
		return F->Data;
	}
	void GoreLinkPoll()
	{
		if (!LiveBind())
			return;
		// the cached object: still in its GObjects slot and still a GoreLink (a new map frees it)
		if (GoreLinkObj != nullptr && (GoreLinkIdx >= LiveObjs->Num || LiveObjs->Data[GoreLinkIdx] != GoreLinkObj || !GoreLinkIsLink(this, GoreLinkObj)))
		{
			Message("gorelink: the GoreLink object went (%u batches, %u commands run)", GoreLinkRuns, GoreLinkCmds);
			GoreLinkObj = nullptr;
			GoreLinkOff = -1;
		}
		if (GoreLinkObj == nullptr)
		{
			const DWORD Now = GetTickCount();
			if (Now - GoreLinkScanTick < 1000)
				return;
			GoreLinkScanTick = Now;
			for (int i = 0; i < LiveObjs->Num; i++)
				if (GoreLinkIsLink(this, LiveObjs->Data[i]))
				{
					GoreLinkObj = LiveObjs->Data[i];
					GoreLinkIdx = i;
					Message("gorelink: found %ls", LiveGetName(GoreLinkObj, nullptr));
					break;
				}
			if (GoreLinkObj == nullptr)
				return;
		}
		int Len = 0;
		const wchar_t *T = GoreLinkOff >= 0 ? GoreLinkText(GoreLinkObj, GoreLinkOff, Len) : nullptr;
		if (T == nullptr)
		{
			// find the string: past UObject's header, within the first 8 KB that can be read
			GoreLinkOff = -1;
			for (int Off = 0x24; Off < 0x2000 && T == nullptr; Off += 4)
				if ((T = GoreLinkText(GoreLinkObj, Off, Len)) != nullptr)
					GoreLinkOff = Off;
			if (T == nullptr)
				return;                          // nothing sent yet (or "" this tick)
			Message("gorelink: commands string at +0x%x", GoreLinkOff);
		}
		// "GL1 <seq>;": once per new seq
		int i = 4;
		while (i < Len && T[i] != L';')
			i++;
		const std::wstring Seq(T + 4, T + i);
		if (Seq == GoreLinkSeq)
			return;
		GoreLinkSeq = Seq;
		GoreLinkRuns++;
		if (Len > GoreLinkMax)
		{
			int Cut = GoreLinkMax;
			while (Cut > i && T[Cut] != L';')
				Cut--;
			if (!GoreLinkTold)
				Message("gorelink: a batch of %d characters cut to %d (the cap is %d a frame)", Len, Cut, GoreLinkMax);
			GoreLinkTold = true;
			Len = Cut;
		}
		std::string Cmd;
		for (int k = i + 1; k <= Len; k++)
		{
			if (k == Len || T[k] == L';')
			{
				size_t a = Cmd.find_first_not_of(' '), b = Cmd.find_last_not_of(' ');
				if (a != std::string::npos && Cmd.size() < 1024)
				{
					U2BloodCommand(Cmd.substr(a, b - a + 1).c_str());
					GoreLinkCmds++;
				}
				Cmd.clear();
			}
			else
				Cmd += T[k] < 128 ? (char)T[k] : ' ';
		}
	}
	void LivePoll(IDirect3DDevice9 *Dev)
	{
		if (!LiveOn || LiveBroken || Dir.empty())
			return;
		if (!LiveHook(Dev))
			return;
		LiveStatus();
		const std::string Path = Dir + "U2Live.cmd";
		if (GetFileAttributesA(Path.c_str()) == INVALID_FILE_ATTRIBUTES)
			return;
		std::string Text;
		{
			FILE *F = nullptr;
			if (fopen_s(&F, Path.c_str(), "rb") || F == nullptr)
				return;                         // still being written: the next frame
			char Buf[4096];
			size_t n;
			while ((n = fread(Buf, 1, sizeof(Buf), F)) > 0)
				Text.append(Buf, n);
			fclose(F);
		}
		DeleteFileA(Path.c_str());
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
			if (S == std::string::npos || L[S] == '#' || L[S] == ';')
				continue;
			L = L.substr(S);
			{
				char W1[64] = "", W2[64] = "";
				long long N = 0;
				if (sscanf_s(L.c_str(), "%63s %63s %lld", W1, (unsigned)sizeof(W1), W2, (unsigned)sizeof(W2), &N) == 3 && _stricmp(W2, "batch") == 0)
					LiveQueueBatch = N;
			}
			LiveQueue.push_back(L);
		}
		if (!LiveQueue.empty() && LiveWnd != nullptr)
			PostMessageW(LiveWnd, LiveMsg, 0, 0);
	}

	// ---- gmterrain=1: the GM journal's terrain lines, applied to the level's terrain ---------------
	// U2GM (script) writes "terrain raise|lower|flatten|smooth X Y R H" lines into its journal
	// (System\U2GM.ini, [U2GM.GMMaster] Ops[k]=@family terrain ...); script can't reach the
	// heightmap, so the fork applies them. Every 10 frames the ini's time is checked; on a change
	// or a new map the current map family's terrain lines are read (in slot order) and handed to
	// the game window's message loop (the live channel's window hook), where:
	//   - the level's TerrainInfo objects are found through GObjObjects (class TerrainInfo, the
	//     outermost package = the current map), each checked (readable, G16 heightmap of the
	//     terrain's own size, resident);
	//   - the first time a terrain is touched its heightmap is copied (the original);
	//   - the result = the original + every line in order (so undo/redo just work), with a
	//     cosine falloff; it is compared with what the engine holds and only the changed heights
	//     are written (ATerrainInfo::SetHeightmap);
	//   - the changed rectangle (+1 quad of normals round it) is rebuilt as UnrealEd's brushes do
	//     (Update = CalcVertices + UpdateVertexBuffers(..., relight 1)), but without Update's
	//     game-only tail, which unloads the heightmap's lazy array (the next touch would re-read
	//     it from the map file). CalcVertices rebuilds the Vertices array that LineCheck and
	//     PointCheck collide with, so collision follows; UpdateVertexBuffer recomputes the sectors'
	//     bounds and bumps their revision; StaticLight relights them with the baked light visibility.
	// Off unless U2Shaders.ini has gmterrain=1; never in UnrealEd (it would bake into a saved map).
	bool GmTerrainOn = false, GmBroken = false, GmTold = false;
	FILETIME GmIniTime = {};
	std::string GmMap, GmLastText;
	std::vector<std::string> GmPending;
	bool GmHasPending = false;
	static const UINT GmMsg = WM_APP + 0x76;
	struct GmTerrain { void *Obj; int W, H; std::vector<WORD> Orig; std::string Name; };
	std::vector<GmTerrain> GmTerrains;
	typedef WORD (__fastcall *GmGetH_t)(void *This, void *Edx, int X, int Y);
	typedef void (__fastcall *GmSetH_t)(void *This, void *Edx, int X, int Y, WORD H);
	typedef void (__fastcall *GmCalc_t)(void *This, void *Edx, float T, int X1, int Y1, int X2, int Y2);
	typedef void (__fastcall *GmVB_t)(void *This, void *Edx, int X1, int Y1, int X2, int Y2, int Relight);
	typedef float *(__fastcall *GmXform_t)(void *This, void *Edx, float *Ret, float X, float Y, float Z);
	typedef void (__fastcall *GmLoad_t)(void *This, void *Edx);
	GmGetH_t GmGetH = nullptr;
	GmSetH_t GmSetH = nullptr;
	GmCalc_t GmCalc = nullptr;
	GmVB_t GmVB = nullptr;
	GmXform_t GmToWorld = nullptr, GmToHeight = nullptr;
	bool GmBound = false;
	bool GmBind()
	{
		if (GmBound)
			return true;
		HMODULE Engine = GetModuleHandleA("Engine.dll");
		if (Engine == nullptr)
			return false;
		GmGetH = (GmGetH_t)GetProcAddress(Engine, "?GetHeightmap@ATerrainInfo@@QAEGHH@Z");
		GmSetH = (GmSetH_t)GetProcAddress(Engine, "?SetHeightmap@ATerrainInfo@@QAEXHHG@Z");
		GmCalc = (GmCalc_t)GetProcAddress(Engine, "?CalcVertices@ATerrainInfo@@QAEXMHHHH@Z");
		GmVB = (GmVB_t)GetProcAddress(Engine, "?UpdateVertexBuffers@ATerrainInfo@@QAEXHHHHH@Z");
		GmToWorld = (GmXform_t)GetProcAddress(Engine, "?HeightmapToWorld@ATerrainInfo@@QAE?AVFVector@@V2@@Z");
		GmToHeight = (GmXform_t)GetProcAddress(Engine, "?WorldToHeightmap@ATerrainInfo@@QAE?AVFVector@@V2@@Z");
		if (!GmGetH || !GmSetH || !GmCalc || !GmVB || !GmToWorld || !GmToHeight)
		{
			Message("gm terrain: Engine.dll's ATerrainInfo exports aren't there: off");
			GmBroken = true;
			return false;
		}
		GmBound = true;
		return true;
	}
	// engine calls under a structured exception guard (no C++ objects in these frames)
	static bool GmSafeXform(GmXform_t F, void *T, float X, float Y, float Z, float Out[3])
	{
		__try
		{
			float R[3] = { 0, 0, 0 };
			F(T, nullptr, R, X, Y, Z);
			Out[0] = R[0]; Out[1] = R[1]; Out[2] = R[2];
			return _finite(R[0]) && _finite(R[1]) && _finite(R[2]);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	static bool GmSafeRead(GmGetH_t F, void *T, int W, int H, WORD *Out)
	{
		__try
		{
			for (int y = 0; y < H; y++)
				for (int x = 0; x < W; x++)
					Out[y * W + x] = F(T, nullptr, x, y);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	static bool GmSafeWrite(GmSetH_t F, void *T, int W, const WORD *Want, const WORD *Have, int X1, int Y1, int X2, int Y2)
	{
		__try
		{
			for (int y = Y1; y <= Y2; y++)
				for (int x = X1; x <= X2; x++)
					if (Want[y * W + x] != Have[y * W + x])
						F(T, nullptr, x, y, Want[y * W + x]);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	static bool GmSafeUpdate(GmCalc_t C, GmVB_t V, void *T, int X1, int Y1, int X2, int Y2)
	{
		__try
		{
			C(T, nullptr, 0.0f, X1, Y1, X2, Y2);
			V(T, nullptr, X1, Y1, X2, Y2, 1);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	static bool GmSafeLoad(void *Lazy)
	{
		__try
		{
			void **Vt = *(void ***)Lazy;
			((GmLoad_t)Vt[0])(Lazy, nullptr);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	static bool InModule(const void *P)
	{
		HMODULE M = nullptr;
		return P != nullptr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)P, &M) && M != nullptr;
	}
	// ATerrainInfo fields (from the Engine.dll decompile: SetupSectors, CalcVertices, GetHeightmap):
	// TerrainMap 0x3a0, Sectors (TArray) 0x114c, HeightmapX 0x1164, HeightmapY 0x1168.
	// UTexture: Format (BYTE) 0x3c, USize 0x44, VSize 0x48, Mips (TArray<FMipmap>) 0xa0.
	// FMipmap: 0x10 bytes of base, then TLazyArray<BYTE> DataArray: vtable (Load, Unload), SavedAr,
	// SavedPos, then its TArray (Data at 0x1c, Num at 0x20 from the mip's start)
	// A terrain's heightmap, checked: G16, the terrain's own size, resident (loaded if it can be)
	const char *GmCheck(void *T, int &W, int &H)
	{
		if (!Readable(T, 0x1210))
			return "object not readable";
		W = *(int *)((BYTE *)T + 0x1164);
		H = *(int *)((BYTE *)T + 0x1168);
		if (W < 2 || H < 2 || W > 4096 || H > 4096)
			return "heightmap size out of range";
		BYTE *Tex = *(BYTE **)((BYTE *)T + 0x3a0);
		if (!Readable(Tex, 0xac))
			return "no TerrainMap";
		if (Tex[0x3c] != 10)
			return "TerrainMap isn't G16 (P8 heightmaps aren't handled)";
		if (*(int *)(Tex + 0x44) != W || *(int *)(Tex + 0x48) != H)
			return "TerrainMap size isn't the terrain's";
		BYTE *Mip = *(BYTE **)(Tex + 0xa0);
		if (*(int *)(Tex + 0xa4) < 1 || !Readable(Mip, 0x24))
			return "TerrainMap has no mip 0";
		const int Bytes = W * H * 2;
		if (*(int *)(Mip + 0x20) < Bytes || !Readable(*(BYTE **)(Mip + 0x1c), Bytes))
		{
			// not resident: the lazy array loads itself (as CalcVertices does first thing), only
			// when it still has a loader and a place in the file
			void *Lazy = Mip + 0x10;
			void **Vt = *(void ***)Lazy;
			if (!Readable(Vt, 8) || !InModule(Vt[0]) || *(int *)(Mip + 0x18) <= 0 || !Readable(*(void **)(Mip + 0x14), 8))
				return "heightmap not resident and can't be loaded";
			if (!GmSafeLoad(Lazy))
				return "heightmap load faulted";
			if (*(int *)(Mip + 0x20) < Bytes || !Readable(*(BYTE **)(Mip + 0x1c), Bytes))
				return "heightmap still not resident after loading";
			Message("gm terrain: loaded the heightmap's lazy array");
		}
		if (*(int *)((BYTE *)T + 0x114c + 4) < 1)
			return "no sectors";
		return nullptr;
	}
	static bool GmExeIsEditor()
	{
		char Exe[MAX_PATH] = {};
		GetModuleFileNameA(nullptr, Exe, MAX_PATH);
		for (char *c = Exe; *c; c++)
			*c = (char)tolower((unsigned char)*c);
		return strstr(Exe, "unrealed") != nullptr;
	}
	// the map's name without "_liveN", as GMMaster.Family()
	std::string GmFamily() const
	{
		std::string F = CurMap;
		const size_t L = F.find("_live");
		if (L != std::string::npos)
			F = F.substr(0, L);
		return F;
	}
	// System\U2GM.ini as text (UTF-16: the low bytes)
	std::string GmIniText()
	{
		std::string Text;
		FILE *F = nullptr;
		if (fopen_s(&F, (Dir + "U2GM.ini").c_str(), "rb") || F == nullptr)
			return Text;
		char Buf[4096];
		size_t n;
		while ((n = fread(Buf, 1, sizeof(Buf), F)) > 0)
			Text.append(Buf, n);
		fclose(F);
		if (Text.find('\0') != std::string::npos)
		{
			std::string N;
			for (char c : Text)
				if (c != '\0' && (unsigned char)c != 0xff && (unsigned char)c != 0xfe)
					N += c;
			Text.swap(N);
		}
		return Text;
	}
	// this map family's terrain lines from U2GM.ini, in slot order ("raise X Y R H")
	std::vector<std::string> GmReadLines()
	{
		std::vector<std::string> Out;
		std::string Text = GmIniText();
		if (Text.empty())
			return Out;
		const std::string Tag = "@" + GmFamily() + " ";
		std::map<int, std::string> BySlot;
		bool InSec = false;
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
			if (S == std::string::npos)
				continue;
			L = L.substr(S);
			if (L[0] == '[')
			{
				InSec = _stricmp(L.c_str(), "[U2GM.GMMaster]") == 0;
				continue;
			}
			if (!InSec || _strnicmp(L.c_str(), "ops[", 4) != 0)
				continue;
			const size_t Close = L.find("]=");
			if (Close == std::string::npos || Close <= 4)
				continue;
			const int K = atoi(L.c_str() + 4);
			std::string V = L.substr(Close + 2);
			if (V.size() >= 2 && V.front() == '"' && V.back() == '"')
				V = V.substr(1, V.size() - 2);
			if (K < 0 || V.size() <= Tag.size() || _strnicmp(V.c_str(), Tag.c_str(), Tag.size()) != 0)
				continue;
			V = V.substr(Tag.size());
			if (_strnicmp(V.c_str(), "terrain ", 8) != 0)
				continue;
			BySlot[K] = V.substr(8);
		}
		for (const auto &It : BySlot)
			Out.push_back(It.second);
		return Out;
	}
	// every 10 frames (OnPresent): the journal or the map changed -> the message loop applies it
	void GmPoll(IDirect3DDevice9 *Dev)
	{
		if (!GmTerrainOn || GmBroken || Dir.empty() || CurMap.empty())
			return;
		if (!GmTold)
		{
			GmTold = true;
			if (GmExeIsEditor())
			{
				Message("gm terrain: not in UnrealEd (the edits would be saved into the map): off");
				GmBroken = true;
				return;
			}
			Message("gm terrain: on, watching %sU2GM.ini", Dir.c_str());
		}
		bool NewMap = false;
		if (GmMap != CurMap)
		{
			GmMap = CurMap;
			GmTerrains.clear();             // a new level: new objects, new originals
			GmLastText.clear();
			NewMap = true;
		}
		WIN32_FILE_ATTRIBUTE_DATA A = {};
		if (!GetFileAttributesExA((Dir + "U2GM.ini").c_str(), GetFileExInfoStandard, &A))
			return;
		if (!NewMap && CompareFileTime(&A.ftLastWriteTime, &GmIniTime) == 0)
			return;
		GmIniTime = A.ftLastWriteTime;
		std::vector<std::string> Lines = GmReadLines();
		std::string Text;
		for (const std::string &L : Lines)
			Text += L + "\n";
		if (!NewMap && Text == GmLastText)
			return;                         // the game saved something else (moves, palette...)
		GmLastText = Text;
		if (Lines.empty() && GmTerrains.empty())
			return;                         // nothing journalled, nothing touched
		if (!LiveHook(Dev))
			return;
		GmPending = Lines;
		GmHasPending = true;
		Message("gm terrain: %d terrain line(s) for %s%s", (int)Lines.size(), GmFamily().c_str(), NewMap ? " (map loaded)" : "");
		PostMessageW(LiveWnd, GmMsg, 0, 0);
	}
	struct GmBrush { char Kind[16]; float X, Y, R, H; };
	// in the message loop: the original + every line, written where it differs, rebuilt there
	void GmRun()
	{
		if (!GmHasPending)
			return;
		GmHasPending = false;
		std::vector<std::string> Lines;
		Lines.swap(GmPending);
		if (!GmBind())
			return;
		EngineMap();                        // binds the object list
		if (EngObjs == nullptr || !Readable(EngObjs, sizeof(ObjList)))
		{
			Message("gm terrain: the engine's object list isn't there: not applied");
			return;
		}
		std::vector<GmBrush> Brushes;
		for (const std::string &L : Lines)
		{
			GmBrush B = {};
			if (sscanf_s(L.c_str(), "%15s %f %f %f %f", B.Kind, (unsigned)sizeof(B.Kind), &B.X, &B.Y, &B.R, &B.H) < 4 || !(B.R > 0 && B.R <= 65536))
			{
				Message("gm terrain: skipped '%s' (want raise|lower|flatten|smooth X Y R H)", L.c_str());
				continue;
			}
			if (strcmp(B.Kind, "raise") && strcmp(B.Kind, "lower") && strcmp(B.Kind, "flatten") && strcmp(B.Kind, "smooth"))
			{
				Message("gm terrain: skipped '%s' (unknown brush)", L.c_str());
				continue;
			}
			if (strcmp(B.Kind, "smooth") == 0 && B.H == 0)
				B.H = 1;
			Brushes.push_back(B);
		}
		// the level's terrains: class TerrainInfo, outermost package = this map
		std::vector<void *> Found;
		for (int i = 0; i < EngObjs->Num; i++)
		{
			void *O = EngObjs->Data[i];
			if (O == nullptr)
				continue;
			void *C = EngClass(O, nullptr);
			const wchar_t *CN = C ? EngName(C, nullptr) : nullptr;
			if (CN == nullptr || wcscmp(CN, L"TerrainInfo") != 0)
				continue;
			void *Pkg = O;
			for (int d = 0; d < 8; d++)
			{
				void *Up = EngOuter(Pkg, nullptr);
				if (Up == nullptr)
					break;
				Pkg = Up;
			}
			const wchar_t *PN = EngName(Pkg, nullptr);
			std::string P;
			for (const wchar_t *c = PN ? PN : L""; *c; c++)
				P += (char)towlower(*c);
			if (P == GmMap)
				Found.push_back(O);
		}
		if (Found.empty())
		{
			Message("gm terrain: %d line(s) on %s but the map has no TerrainInfo", (int)Brushes.size(), GmMap.c_str());
			return;
		}
		for (void *T : Found)
		{
			const wchar_t *TN = EngName(T, nullptr);
			std::string Name;
			for (const wchar_t *c = TN ? TN : L"?"; *c; c++)
				Name += (char)*c;
			int W = 0, H = 0;
			if (const char *Why = GmCheck(T, W, H))
			{
				Message("gm terrain: %s skipped: %s", Name.c_str(), Why);
				continue;
			}
			// heightmap <-> world is affine (FCoords): measured once through the engine's own transform
			float O[3], PX[3], PY[3], PZ[3];
			if (!GmSafeXform(GmToWorld, T, 0, 0, 0, O) || !GmSafeXform(GmToWorld, T, 1, 0, 0, PX) ||
				!GmSafeXform(GmToWorld, T, 0, 1, 0, PY) || !GmSafeXform(GmToWorld, T, 0, 0, 1, PZ))
			{
				Message("gm terrain: %s skipped: HeightmapToWorld failed", Name.c_str());
				continue;
			}
			float AX[3], AY[3], AZ[3];
			for (int k = 0; k < 3; k++)
			{
				AX[k] = PX[k] - O[k];
				AY[k] = PY[k] - O[k];
				AZ[k] = PZ[k] - O[k];
			}
			const float CellX = sqrtf(AX[0] * AX[0] + AX[1] * AX[1]), CellY = sqrtf(AY[0] * AY[0] + AY[1] * AY[1]);
			if (CellX < 1e-3f || CellY < 1e-3f || fabsf(AZ[2]) < 1e-6f)
			{
				Message("gm terrain: %s skipped: degenerate scale (cell %.3f x %.3f, height step %g)", Name.c_str(), CellX, CellY, AZ[2]);
				continue;
			}
			GmTerrain *S = nullptr;
			for (GmTerrain &G : GmTerrains)
				if (G.Obj == T && G.W == W && G.H == H)
					S = &G;
			if (S == nullptr && Brushes.empty())
				continue;                   // never touched, nothing to do
			std::vector<WORD> Have((size_t)W * H);
			if (!GmSafeRead(GmGetH, T, W, H, &Have[0]))
			{
				Message("gm terrain: %s skipped: reading the heightmap faulted", Name.c_str());
				continue;
			}
			if (S == nullptr)
			{
				GmTerrain G;
				G.Obj = T; G.W = W; G.H = H; G.Orig = Have; G.Name = Name;
				GmTerrains.push_back(G);
				S = &GmTerrains.back();
				Message("gm terrain: %s %dx%d, cell %.1f x %.1f, height step %.4f: original kept", Name.c_str(), W, H, CellX, CellY, AZ[2]);
			}
			// the result: the original + every line in order
			std::vector<float> Work(S->Orig.begin(), S->Orig.end());
			std::vector<float> Prev;
			int Applied = 0;
			for (const GmBrush &B : Brushes)
			{
				float C[3];
				if (!GmSafeXform(GmToHeight, T, B.X, B.Y, 0, C))
					continue;
				const float RX = B.R / CellX, RY = B.R / CellY;
				const int X1 = (std::max)(0, (int)floorf(C[0] - RX - 1)), X2 = (std::min)(W - 1, (int)ceilf(C[0] + RX + 1));
				const int Y1 = (std::max)(0, (int)floorf(C[1] - RY - 1)), Y2 = (std::min)(H - 1, (int)ceilf(C[1] + RY + 1));
				if (X1 > X2 || Y1 > Y2)
					continue;               // not on this terrain
				const bool Smooth = B.Kind[0] == 's';
				if (Smooth)
					Prev = Work;
				const float Strength = (std::min)((std::max)(B.H, 0.0f), 1.0f);
				bool Touched = false;
				for (int y = Y1; y <= Y2; y++)
					for (int x = X1; x <= X2; x++)
					{
						const float WX = O[0] + x * AX[0] + y * AY[0], WY = O[1] + x * AX[1] + y * AY[1];
						const float D = sqrtf((WX - B.X) * (WX - B.X) + (WY - B.Y) * (WY - B.Y));
						if (D >= B.R)
							continue;
						const float Fall = 0.5f * (1.0f + cosf(3.14159265f * D / B.R));
						float &V = Work[(size_t)y * W + x];
						if (B.Kind[0] == 'r')
							V += Fall * B.H / AZ[2];
						else if (B.Kind[0] == 'l')
							V -= Fall * B.H / AZ[2];
						else if (B.Kind[0] == 'f')
						{
							const float Base = O[2] + x * AX[2] + y * AY[2];
							V += Fall * ((B.H - Base) / AZ[2] - V);
						}
						else
						{
							float Sum = 0;
							int N = 0;
							for (int dy = -1; dy <= 1; dy++)
								for (int dx = -1; dx <= 1; dx++)
									if (x + dx >= 0 && x + dx < W && y + dy >= 0 && y + dy < H)
									{
										Sum += Prev[(size_t)(y + dy) * W + x + dx];
										N++;
									}
							V += Fall * Strength * (Sum / N - V);
						}
						Touched = true;
					}
				Applied += Touched ? 1 : 0;
			}
			std::vector<WORD> Want((size_t)W * H);
			int MinX = W, MinY = H, MaxX = -1, MaxY = -1, Changed = 0;
			for (int y = 0; y < H; y++)
				for (int x = 0; x < W; x++)
				{
					const size_t i = (size_t)y * W + x;
					const float V = (std::min)((std::max)(Work[i], 0.0f), 65535.0f);
					Want[i] = (WORD)(V + 0.5f);
					if (Want[i] != Have[i])
					{
						Changed++;
						MinX = (std::min)(MinX, x); MaxX = (std::max)(MaxX, x);
						MinY = (std::min)(MinY, y); MaxY = (std::max)(MaxY, y);
					}
				}
			if (Changed == 0)
			{
				Message("gm terrain: %s: %d line(s) on %s, already as journalled", Name.c_str(), Applied, GmMap.c_str());
				continue;
			}
			const DWORD T0 = GetTickCount();
			if (!GmSafeWrite(GmSetH, T, W, &Want[0], &Have[0], MinX, MinY, MaxX, MaxY))
			{
				Message("gm terrain: %s: writing the heightmap faulted: off", Name.c_str());
				GmBroken = true;
				return;
			}
			// the vertices one past the change (CalcVertices' ends are exclusive; a vertex's pass
			// remakes the normals of the quad left/above it)
			const int UX1 = (std::max)(0, MinX - 1), UY1 = (std::max)(0, MinY - 1);
			const int UX2 = (std::min)(W, MaxX + 2), UY2 = (std::min)(H, MaxY + 2);
			if (!GmSafeUpdate(GmCalc, GmVB, T, UX1, UY1, UX2, UY2))
			{
				Message("gm terrain: %s: rebuilding the terrain faulted: off", Name.c_str());
				GmBroken = true;
				return;
			}
			Message("gm terrain: %s on %s: %d line(s) applied, %d height(s) changed, rebuilt x %d..%d y %d..%d in %u ms",
				Name.c_str(), GmMap.c_str(), Applied, Changed, UX1, UX2, UY1, UY2, (unsigned)(GetTickCount() - T0));
		}
	}

	// ---- gmpanel=1: the GM panel and gizmo (Dear ImGui), drawn over the game ---------------------
	// U2GM (script) does the editing; this is its front end. F7 (seen by the game window's message
	// hook, LiveWndProc) shows or hides a panel drawn at Present, after the HUD, on the back buffer.
	//   - Shown, the game's mouse is held by U2Input (System\dinput8.dll, its exported
	//     U2InputHoldMouse): the DirectInput mouse is unacquired, so the OS cursor is free and the
	//     game gets no look or fire. Key presses and mouse messages go to the panel only; key
	//     releases still reach the game, so nothing held sticks. Without U2Input the messages are
	//     still kept from the game, but the mouse keeps turning the view (the game reads the mouse
	//     through DirectInput, not window messages).
	//   - Buttons send "gm ..." commands as lines "gm q SESSION K CMD" in System\U2GMPanel.txt
	//     (written whole and renamed into place). GMMaster execs that file every 2 s, 0.25 s while
	//     the panel is open ("gm panel 1"), and runs each line once (K above its last). Its
	//     PanelState line in U2GM.ini says which K it reached and what is picked; lines it has
	//     taken leave the file. Only commands made of [A-Za-z0-9 ._-#:,+] are ever written.
	//   - The picked actor gets a gizmo: X/Y/Z arrows and a yaw ring, projected with the scene's
	//     view and projection (the last perspective draw of the frame on the back buffer with an
	//     identity world matrix, else the last perspective draw). Dragging previews the change
	//     ("gm preview", at most every 150 ms) and the release sends "gm moveto X Y Z" or
	//     "gm turn DEG", snapped to U2GM.ini's GridSize / YawStep (Escape cancels).
	//   - A click in the world (not on the panel or gizmo) sends "gm ray" (the line under the mouse)
	//     and the chosen action aimed along it: pick, spawn, move here, or a terrain brush.
	// gmpanel=2: the same with the cursor drawn by ImGui; gmpanelscale=F sizes the panel. The panel's
	// window place is kept in System\U2GMPanel.imgui.ini. Never in UnrealEd.
	int GmPanelMode = 0;
	float GmPanelScale = 1.0f;
	bool GmPanelShown = false, GmPanelBroken = false, GmPanelStarted = false;
	bool GmImReady = false;
	IDirect3DDevice9 *GmImDev = nullptr;
	std::string GmImIni;
	int GmCursorShows = 0;
	typedef int (WINAPI *GmHold_t)(int);
	GmHold_t GmHold = nullptr;
	bool GmHoldLooked = false;
	unsigned GmSession = 0;
	long GmOutK = 0;
	struct GmOutLine { long K; std::string Cmd; bool Preview; };
	std::vector<GmOutLine> GmOut;
	std::string GmOutWritten;
	struct GmStateT
	{
		bool Valid = false, On = false, Poss = false, Frz = false, CamOk = false;
		unsigned Session = 0;
		long Seq = 0;
		std::string Pick = "-", Cls = "-", Mesh = "-";
		float Loc[3] = { 0, 0, 0 }, Yaw = 0, Scale = 1, Cam[3] = { 0, 0, 0 };
		long CommitStamp = 0;                // commit=STAMP:STATE:MAP (gm commit, gm_commit.py --watch)
		std::string CommitState = "none", CommitMap = "-";
		int Con = 0;                         // con=N: the console (1 the full one, 2 the one-line one; Console.ui's triggers)
		bool ViewOk = false;
		float View[2] = { 0, 0 };            // view=YAW,PITCH (degrees): where the player looks
	};
	GmStateT GmSt;
	std::vector<std::pair<int, std::string>> GmPalette, GmJournal;
	std::vector<std::array<float, 3>> GmDrawPts;    // U2GM.ini DrawState: the draw being made
	char GmNote[112] = {};                           // the draw's note
	std::string GmCommitWatch;                       // System\U2GMCommit.status: the watcher's last word
	DWORD GmCommitWatchAge = 0xffffffff;             // its age in ms when read
	float GmGrid = 32;
	int GmYawStep = 15;
	FILETIME GmPanelIniTime = {};
	std::string GmPanelMap;
	D3DMATRIX GmView = {}, GmProj = {}, GmViewNext = {}, GmProjNext = {};
	bool GmViewOk = false, GmViewNextOk = false, GmViewNextIdent = false;
	float GmVP[16] = {}, GmInvVP[16] = {}, GmCam[3] = {};
	float GmDispW = 0, GmDispH = 0;
	float GmStep = 32, GmRadius = 512, GmHeight = 128;
	int GmPaletteSel = 0, GmClickMode = 0, GmHot = -1;
	bool GmGizmoOn = true, GmPreviewOn = true, GmSnapOn = true;
	struct GmDragT
	{
		int Axis = -1;                     // 0..2 move along X/Y/Z, 3 the yaw ring
		float Start[3] = { 0, 0, 0 }, Now[3] = { 0, 0, 0 };
		float StartYaw = 0, NowYaw = 0, Amount = 0, Previewed = 0, L = 0;
		float M0[2] = { 0, 0 }, AnglePrev = 0, AngleAcc = 0;
		DWORD LastPreview = 0;
	};
	GmDragT GmDrag;
	float GmPoseLoc[3] = { 0, 0, 0 }, GmPoseYaw = 0;   // where the last drag left it, until the game says so
	long GmPoseK = 0;

	long GmAcked() const { return (GmSt.Valid && GmSt.Session == GmSession) ? GmSt.Seq : 0; }

	// one "gm" command for the game (printf-style); returns its K, 0 if refused
	long GmSend(const char *Fmt, ...)
	{
		char Buf[256];
		va_list Args;
		va_start(Args, Fmt);
		vsnprintf(Buf, sizeof(Buf), Fmt, Args);
		va_end(Args);
		Buf[sizeof(Buf) - 1] = 0;
		// "draw done ... NOTE", "sketch mark NAME NOTE": the note may also hold !?()/%& (never quotes, semicolons or |)
		const char *Extra = (strncmp(Buf, "draw done ", 10) == 0 || strncmp(Buf, "sketch mark ", 12) == 0) ? " ._-#:,+!?()/%&" : " ._-#:,+";
		for (const char *c = Buf; *c; c++)
			if (!isalnum((unsigned char)*c) && strchr(Extra, *c) == nullptr)
			{
				Message("gm panel: not sent (only letters, digits and %s): %s", Extra + 1, Buf);
				return 0;
			}
		const bool Preview = strncmp(Buf, "preview ", 8) == 0;
		if (Preview)                         // a newer preview makes the waiting ones pointless
			GmOut.erase(std::remove_if(GmOut.begin(), GmOut.end(), [](const GmOutLine &L) { return L.Preview; }), GmOut.end());
		if (GmOut.size() >= 200)
			GmOut.erase(GmOut.begin());      // the game isn't taking them (U2GM not loaded?)
		GmOutLine L;
		L.K = ++GmOutK;
		L.Cmd = Buf;
		L.Preview = Preview;
		GmOut.push_back(L);
		return L.K;
	}
	// the lines the game hasn't taken yet, into System\U2GMPanel.txt (whole, renamed into place)
	void GmFlushOut()
	{
		const long Ack = GmAcked();
		GmOut.erase(std::remove_if(GmOut.begin(), GmOut.end(), [Ack](const GmOutLine &L) { return L.K <= Ack; }), GmOut.end());
		std::string Text;
		for (const GmOutLine &L : GmOut)
			Text += "gm q " + std::to_string(GmSession) + " " + std::to_string(L.K) + " " + L.Cmd + "\r\n";
		if (Text == GmOutWritten)
			return;
		const std::string Mine = Text;
		const std::string Tmp = Dir + "U2GMPanel.tmp", Path = Dir + "U2GMPanel.txt";
		// lines of another session (gm_commit.py's watcher: "gm q <2^30 and up> ...") stay in the
		// file; the watcher takes its own out once the game ran them
		{
			FILE *Old = nullptr;
			if (fopen_s(&Old, Path.c_str(), "rb") == 0 && Old != nullptr)
			{
				std::string Was;
				char Rd[4096];
				size_t n;
				while ((n = fread(Rd, 1, sizeof(Rd), Old)) > 0 && Was.size() < 65536)
					Was.append(Rd, n);
				fclose(Old);
				const std::string Own = "gm q " + std::to_string(GmSession) + " ";
				size_t At = 0;
				while (At < Was.size())
				{
					size_t End = Was.find('\n', At);
					if (End == std::string::npos)
						End = Was.size();
					std::string L = Was.substr(At, End - At);
					At = End + 1;
					while (!L.empty() && (L.back() == '\r' || L.back() == ' '))
						L.pop_back();
					if (L.compare(0, 5, "gm q ") == 0 && L.compare(0, Own.size(), Own) != 0)
						Text += L + "\r\n";
				}
			}
		}
		FILE *F = nullptr;
		if (fopen_s(&F, Tmp.c_str(), "wb") || F == nullptr)
			return;
		const bool Ok = fwrite(Text.data(), 1, Text.size(), F) == Text.size();
		fclose(F);
		if (Ok && MoveFileExA(Tmp.c_str(), Path.c_str(), MOVEFILE_REPLACE_EXISTING))
			GmOutWritten = Mine;            // else (the game reading it): the next frame
		else
			DeleteFileA(Tmp.c_str());
	}

	void GmParseState(const std::string &V)
	{
		GmStateT S;
		S.Valid = true;
		size_t At = 0;
		while (At < V.size())
		{
			size_t End = V.find(' ', At);
			if (End == std::string::npos)
				End = V.size();
			const std::string T = V.substr(At, End - At);
			At = End + 1;
			const size_t Eq = T.find('=');
			if (Eq == std::string::npos)
				continue;
			const std::string K = T.substr(0, Eq), X = T.substr(Eq + 1);
			if (K == "seq")
			{
				unsigned Se = 0;
				long Sq = 0;
				if (sscanf_s(X.c_str(), "%u:%ld", &Se, &Sq) == 2)
				{
					S.Session = Se;
					S.Seq = Sq;
				}
			}
			else if (K == "on") S.On = atoi(X.c_str()) != 0;
			else if (K == "poss") S.Poss = atoi(X.c_str()) != 0;
			else if (K == "frz") S.Frz = atoi(X.c_str()) != 0;
			else if (K == "pick") S.Pick = X;
			else if (K == "cls") S.Cls = X;
			else if (K == "mesh") S.Mesh = X;
			else if (K == "loc") sscanf_s(X.c_str(), "%f,%f,%f", &S.Loc[0], &S.Loc[1], &S.Loc[2]);
			else if (K == "yaw") S.Yaw = (float)atof(X.c_str());
			else if (K == "scale") S.Scale = (float)atof(X.c_str());
			else if (K == "cam") S.CamOk = sscanf_s(X.c_str(), "%f,%f,%f", &S.Cam[0], &S.Cam[1], &S.Cam[2]) == 3;
			else if (K == "con") S.Con = atoi(X.c_str());
			else if (K == "view") S.ViewOk = sscanf_s(X.c_str(), "%f,%f", &S.View[0], &S.View[1]) == 2;
			else if (K == "commit")
			{
				const size_t C1 = X.find(':'), C2 = C1 == std::string::npos ? C1 : X.find(':', C1 + 1);
				S.CommitStamp = atol(X.c_str());
				if (C1 != std::string::npos)
					S.CommitState = X.substr(C1 + 1, C2 == std::string::npos ? std::string::npos : C2 - C1 - 1);
				if (C2 != std::string::npos)
					S.CommitMap = X.substr(C2 + 1);
			}
		}
		if (S.Pick.empty())
			S.Pick = "-";
		GmSt = S;
	}
	// U2GM.ini when it changed: PanelState, the palette, the grid, this map's journal
	void GmPanelReadIni()
	{
		WIN32_FILE_ATTRIBUTE_DATA A = {};
		if (!GetFileAttributesExA((Dir + "U2GM.ini").c_str(), GetFileExInfoStandard, &A))
			return;
		if (GmPanelMap == CurMap && CompareFileTime(&A.ftLastWriteTime, &GmPanelIniTime) == 0)
			return;
		const std::string Text = GmIniText();
		if (Text.empty())
			return;                         // being written: the next look
		GmPanelIniTime = A.ftLastWriteTime;
		GmPanelMap = CurMap;
		GmPalette.clear();
		GmJournal.clear();
		GmDrawPts.clear();
		const std::string Tag = "@" + GmFamily() + " ";
		bool InSec = false;
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
			if (S == std::string::npos)
				continue;
			L = L.substr(S);
			if (L[0] == '[')
			{
				InSec = _stricmp(L.c_str(), "[U2GM.GMMaster]") == 0;
				continue;
			}
			const size_t Eq = L.find('=');
			if (!InSec || Eq == std::string::npos)
				continue;
			const std::string Key = L.substr(0, Eq);
			std::string V = L.substr(Eq + 1);
			if (V.size() >= 2 && V.front() == '"' && V.back() == '"')
				V = V.substr(1, V.size() - 2);
			if (_stricmp(Key.c_str(), "PanelState") == 0)
				GmParseState(V);
			else if (_stricmp(Key.c_str(), "DrawState") == 0)
			{
				size_t P = 0;
				while (P < V.size())
				{
					size_t E = V.find(' ', P);
					if (E == std::string::npos)
						E = V.size();
					std::array<float, 3> Pt = { 0, 0, 0 };
					if (sscanf_s(V.substr(P, E - P).c_str(), "%f,%f,%f", &Pt[0], &Pt[1], &Pt[2]) == 3)
						GmDrawPts.push_back(Pt);
					P = E + 1;
				}
			}
			else if (_stricmp(Key.c_str(), "GridSize") == 0)
				GmGrid = (float)atof(V.c_str());
			else if (_stricmp(Key.c_str(), "YawStep") == 0)
				GmYawStep = atoi(V.c_str());
			else if (_strnicmp(Key.c_str(), "Palette[", 8) == 0 && !V.empty())
				GmPalette.push_back(std::make_pair(atoi(Key.c_str() + 8), V));
			else if (_strnicmp(Key.c_str(), "Ops[", 4) == 0 && V.size() > Tag.size() && _strnicmp(V.c_str(), Tag.c_str(), Tag.size()) == 0)
				GmJournal.push_back(std::make_pair(atoi(Key.c_str() + 4), V.substr(Tag.size())));
		}
		std::sort(GmPalette.begin(), GmPalette.end());
		std::sort(GmJournal.begin(), GmJournal.end());
	}

	// ---- the view: world (Unreal units) -> screen, and back
	static bool GmInvert(const float m[16], float out[16])
	{
		float inv[16];
		inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
		inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
		inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
		inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
		inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
		inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
		inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
		inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
		inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
		inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
		inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
		inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
		inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
		inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
		inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
		inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
		const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
		if (det == 0 || !_finite(det))
			return false;
		for (int i = 0; i < 16; i++)
			out[i] = inv[i] / det;
		return true;
	}
	// per draw (LogTargetDraw), only while the panel is shown: the scene's view and projection
	void GmCaptureView(IDirect3DDevice9 *Dev)
	{
		if (!GmPanelShown)
			return;
		D3DMATRIX P;
		if (FAILED(Dev->GetTransform(D3DTS_PROJECTION, &P)) || P._34 != 1.0f || P._44 != 0.0f)
			return;
		DWORD Fvf = 0;
		Dev->GetFVF(&Fvf);
		if ((Fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW)
			return;
		IDirect3DVertexShader9 *VS = nullptr;
		Dev->GetVertexShader(&VS);
		if (VS != nullptr)
		{
			VS->Release();
			return;
		}
		D3DMATRIX W;
		Dev->GetTransform(D3DTS_WORLD, &W);
		bool Ident = true;
		for (int r = 0; r < 4 && Ident; r++)
			for (int c = 0; c < 4; c++)
				if (fabsf(W.m[r][c] - (r == c ? 1.0f : 0.0f)) > 1e-4f)
				{
					Ident = false;
					break;
				}
		if (!Ident && GmViewNextIdent)
			return;                          // this frame's world draws already gave the view
		if (Offscreen(Dev))
			return;
		GmProjNext = P;
		Dev->GetTransform(D3DTS_VIEW, &GmViewNext);
		GmViewNextOk = true;
		GmViewNextIdent = GmViewNextIdent || Ident;
	}
	void GmTakeView()
	{
		if (GmViewNextOk)
		{
			GmView = GmViewNext;
			GmProj = GmProjNext;
			for (int r = 0; r < 4; r++)
				for (int c = 0; c < 4; c++)
				{
					float x = 0;
					for (int k = 0; k < 4; k++)
						x += GmView.m[r][k] * GmProj.m[k][c];
					GmVP[r * 4 + c] = x;
				}
			GmViewOk = GmInvert(GmVP, GmInvVP);
			const D3DMATRIX Inv = InvView(GmView);
			for (int k = 0; k < 3; k++)
				GmCam[k] = Inv.m[3][k];
		}
		GmViewNextOk = GmViewNextIdent = false;
	}
	bool GmProject(const float P[3], ImVec2 &Out) const
	{
		float c[4];
		for (int k = 0; k < 4; k++)
			c[k] = P[0] * GmVP[k] + P[1] * GmVP[4 + k] + P[2] * GmVP[8 + k] + GmVP[12 + k];
		if (c[3] < 1.0f)
			return false;                    // behind the camera
		Out.x = (c[0] / c[3] * 0.5f + 0.5f) * GmDispW;
		Out.y = (0.5f - c[1] / c[3] * 0.5f) * GmDispH;
		return _finite(Out.x) && _finite(Out.y);
	}
	// the line under a screen point: from the camera, unit direction
	bool GmMouseRay(const ImVec2 &M, float S[3], float D[3]) const
	{
		const float x = M.x / GmDispW * 2 - 1, y = 1 - M.y / GmDispH * 2;
		float p[4];
		for (int k = 0; k < 4; k++)
			p[k] = x * GmInvVP[k] + y * GmInvVP[4 + k] + GmInvVP[12 + k];   // (x, y, 0, 1): the near plane
		if (fabsf(p[3]) < 1e-12f)
			return false;
		float Len = 0;
		for (int k = 0; k < 3; k++)
		{
			S[k] = GmCam[k];
			D[k] = p[k] / p[3] - GmCam[k];
			Len += D[k] * D[k];
		}
		Len = sqrtf(Len);
		if (!(Len > 1e-6f))
			return false;
		for (int k = 0; k < 3; k++)
			D[k] /= Len;
		return true;
	}
	// where the line under M meets the horizontal plane through C: its angle round C (degrees)
	bool GmRingAngle(const ImVec2 &M, const float C[3], float &Deg) const
	{
		float S[3], D[3];
		if (!GmMouseRay(M, S, D) || fabsf(D[2]) < 1e-4f)
			return false;
		const float t = (C[2] - S[2]) / D[2];
		if (t <= 0)
			return false;
		Deg = atan2f(S[1] + D[1] * t - C[1], S[0] + D[0] * t - C[0]) * 57.2957795f;
		return true;
	}
	static float GmSegDist(const ImVec2 &P, const ImVec2 &A, const ImVec2 &B)
	{
		const float dx = B.x - A.x, dy = B.y - A.y, L2 = dx * dx + dy * dy;
		float t = L2 > 0 ? ((P.x - A.x) * dx + (P.y - A.y) * dy) / L2 : 0;
		t = t < 0 ? 0 : t > 1 ? 1 : t;
		const float ex = A.x + dx * t - P.x, ey = A.y + dy * t - P.y;
		return sqrtf(ex * ex + ey * ey);
	}
	// the picked actor's place: mid-drag, just dropped (the game hasn't said yet), or the game's
	void GmPose(float P[3], float &Yaw) const
	{
		const float *L = GmSt.Loc;
		Yaw = GmSt.Yaw;
		if (GmDrag.Axis >= 0)
		{
			L = GmDrag.Now;
			Yaw = GmDrag.NowYaw;
		}
		else if (GmPoseK > GmAcked())
		{
			L = GmPoseLoc;
			Yaw = GmPoseYaw;
		}
		for (int k = 0; k < 3; k++)
			P[k] = L[k];
	}

	// ---- the gizmo
	void GmDragCancel()
	{
		if (GmDrag.Axis >= 0 && GmDrag.Previewed != 0)
			GmSend("preview %.1f %.1f %.1f %.1f", GmDrag.Start[0], GmDrag.Start[1], GmDrag.Start[2], GmDrag.StartYaw);
		GmDrag.Axis = -1;
	}
	void GmDragEnd()
	{
		const GmDragT D = GmDrag;
		GmDrag.Axis = -1;
		if (D.Axis < 3)
		{
			if (D.Amount != 0)
			{
				GmPoseK = GmSend("moveto %.1f %.1f %.1f", D.Now[0], D.Now[1], D.Now[2]);
				memcpy(GmPoseLoc, D.Now, sizeof(GmPoseLoc));
				GmPoseYaw = D.StartYaw;
			}
			else if (D.Previewed != 0)
				GmSend("preview %.1f %.1f %.1f %.1f", D.Start[0], D.Start[1], D.Start[2], D.StartYaw);
			return;
		}
		if (D.Previewed != 0)                // back where it was, then one journalled turn
			GmSend("preview %.1f %.1f %.1f %.1f", D.Start[0], D.Start[1], D.Start[2], D.StartYaw);
		if (D.Amount != 0)
		{
			GmPoseK = GmSend("turn %g", D.Amount);
			memcpy(GmPoseLoc, D.Start, sizeof(GmPoseLoc));
			GmPoseYaw = D.StartYaw + D.Amount;
		}
	}
	void GmGizmo()
	{
		GmHot = -1;
		if (!GmGizmoOn || !GmViewOk || GmSt.Pick == "-")
		{
			if (GmDrag.Axis >= 0)
				GmDragCancel();
			return;
		}
		ImDrawList *DL = ImGui::GetBackgroundDrawList();
		ImGuiIO &io = ImGui::GetIO();
		const ImVec2 M = io.MousePos;
		float P[3], Yaw;
		GmPose(P, Yaw);
		ImVec2 C;
		if (!GmProject(P, C))
			return;
		float L = GmDrag.L;
		if (GmDrag.Axis < 0)
		{
			const float dx = P[0] - GmCam[0], dy = P[1] - GmCam[1], dz = P[2] - GmCam[2];
			L = (std::max)(sqrtf(dx * dx + dy * dy + dz * dz) * 0.15f, 8.0f);
		}
		static const ImU32 Col[4] = { IM_COL32(235, 70, 70, 255), IM_COL32(70, 215, 70, 255), IM_COL32(80, 130, 255, 255), IM_COL32(245, 205, 50, 255) };
		static const ImU32 White = IM_COL32(255, 255, 255, 255);
		ImVec2 E[3];
		bool EOk[3];
		for (int a = 0; a < 3; a++)
		{
			float Q[3] = { P[0], P[1], P[2] };
			Q[a] += L;
			EOk[a] = GmProject(Q, E[a]) && fabsf(E[a].x - C.x) + fabsf(E[a].y - C.y) > 6;
		}
		const int N = 48;
		ImVec2 Ring[N];
		bool RingOk = true;
		for (int i = 0; i < N && RingOk; i++)
		{
			const float t = i * 6.2831853f / N;
			const float Q[3] = { P[0] + cosf(t) * L * 0.75f, P[1] + sinf(t) * L * 0.75f, P[2] };
			RingOk = GmProject(Q, Ring[i]);
		}
		if (GmDrag.Axis < 0 && !io.WantCaptureMouse)
		{
			float Best = 9;
			for (int a = 0; a < 3; a++)
				if (EOk[a] && GmSegDist(M, C, E[a]) < Best)
				{
					Best = GmSegDist(M, C, E[a]);
					GmHot = a;
				}
			for (int i = 0; i < N && RingOk; i++)
				if (GmSegDist(M, Ring[i], Ring[(i + 1) % N]) < Best)
				{
					Best = GmSegDist(M, Ring[i], Ring[(i + 1) % N]);
					GmHot = 3;
				}
		}
		const int Lit = GmDrag.Axis >= 0 ? GmDrag.Axis : GmHot;
		if (RingOk)
		{
			DL->AddPolyline(Ring, N, Lit == 3 ? White : Col[3], ImDrawFlags_Closed, 2.0f);
			const float Q[3] = { P[0] + cosf(Yaw / 57.2957795f) * L * 0.75f, P[1] + sinf(Yaw / 57.2957795f) * L * 0.75f, P[2] };
			ImVec2 T;
			if (GmProject(Q, T))
				DL->AddCircleFilled(T, 5, Lit == 3 ? White : Col[3]);     // the way it faces
		}
		for (int a = 0; a < 3; a++)
			if (EOk[a])
			{
				const ImU32 K = Lit == a ? White : Col[a];
				DL->AddLine(C, E[a], K, 3.0f);
				DL->AddCircleFilled(E[a], 6, K);
				const char Name[2] = { "XYZ"[a], 0 };
				DL->AddText(ImVec2(E[a].x + 8, E[a].y - 16), K, Name);
			}
		DL->AddCircleFilled(C, 4, IM_COL32(255, 255, 255, 220));
		if (GmHot >= 0 && ImGui::IsMouseClicked(0))
		{
			GmDrag = GmDragT();
			GmDrag.Axis = GmHot;
			memcpy(GmDrag.Start, P, sizeof(GmDrag.Start));
			memcpy(GmDrag.Now, P, sizeof(GmDrag.Now));
			GmDrag.StartYaw = GmDrag.NowYaw = Yaw;
			GmDrag.M0[0] = M.x;
			GmDrag.M0[1] = M.y;
			GmDrag.L = L;
			float A0 = 0;
			if (GmHot == 3 && GmRingAngle(M, P, A0))
				GmDrag.AnglePrev = A0;
			return;
		}
		if (GmDrag.Axis < 0)
			return;
		if (ImGui::IsKeyPressed(ImGuiKey_Escape))
		{
			GmDragCancel();
			return;
		}
		const int a = GmDrag.Axis;
		const float Was = GmDrag.Amount;
		char Label[64];
		if (a < 3)
		{
			ImVec2 C0, E0;
			float Q[3] = { GmDrag.Start[0], GmDrag.Start[1], GmDrag.Start[2] };
			Q[a] += GmDrag.L;
			if (GmProject(GmDrag.Start, C0) && GmProject(Q, E0))
			{
				const float dx = E0.x - C0.x, dy = E0.y - C0.y, Len = sqrtf(dx * dx + dy * dy);
				if (Len > 1)
				{
					float W = ((M.x - GmDrag.M0[0]) * dx + (M.y - GmDrag.M0[1]) * dy) / Len / Len * GmDrag.L;
					if (GmSnapOn && GmGrid > 0)
						W = GmGrid * floorf(W / GmGrid + 0.5f);
					GmDrag.Amount = W;
					memcpy(GmDrag.Now, GmDrag.Start, sizeof(GmDrag.Now));
					GmDrag.Now[a] += W;
				}
			}
			snprintf(Label, sizeof(Label), "%c %+.0f", "XYZ"[a], GmDrag.Amount);
			ImVec2 S0, N0;
			if (GmProject(GmDrag.Start, S0) && GmProject(GmDrag.Now, N0))
				DL->AddLine(S0, N0, IM_COL32(255, 255, 255, 110), 1.5f);
		}
		else
		{
			float Ang = 0;
			if (GmRingAngle(M, GmDrag.Start, Ang))
			{
				float d = Ang - GmDrag.AnglePrev;
				while (d > 180) d -= 360;
				while (d < -180) d += 360;
				GmDrag.AngleAcc += d;
				GmDrag.AnglePrev = Ang;
			}
			float T = GmDrag.AngleAcc;
			if (GmSnapOn && GmYawStep > 0)
				T = GmYawStep * floorf(T / GmYawStep + 0.5f);
			GmDrag.Amount = T;
			GmDrag.NowYaw = GmDrag.StartYaw + T;
			snprintf(Label, sizeof(Label), "turn %+.0f", T);
		}
		DL->AddText(ImVec2(M.x + 16, M.y + 10), White, Label);
		if (GmPreviewOn && GmDrag.Amount != Was && GetTickCount() - GmDrag.LastPreview > 150)
		{
			GmDrag.LastPreview = GetTickCount();
			GmSend("preview %.1f %.1f %.1f %.1f", GmDrag.Now[0], GmDrag.Now[1], GmDrag.Now[2], GmDrag.NowYaw);
			GmDrag.Previewed = 1;
		}
		if (ImGui::IsMouseReleased(0))
			GmDragEnd();
	}
	// a click in the world: the line under the mouse, then the chosen action along it
	void GmWorldClick()
	{
		ImGuiIO &io = ImGui::GetIO();
		if (!ImGui::IsMouseClicked(0) || io.WantCaptureMouse || GmHot >= 0 || GmDrag.Axis >= 0 || !GmViewOk)
			return;
		float S[3], D[3];
		if (!GmMouseRay(io.MousePos, S, D))
			return;
		GmSend("ray %.1f %.1f %.1f %.1f %.1f %.1f", S[0], S[1], S[2], S[0] + D[0] * 60000, S[1] + D[1] * 60000, S[2] + D[2] * 60000);
		switch (GmClickMode)
		{
		case 0: GmSend("pick"); break;
		case 1: GmSend("spawn %d", GmPaletteSel); break;
		case 2: GmSend("moveto here"); break;
		case 3: GmSend("raise %.0f %.0f", GmRadius, GmHeight); break;
		case 4: GmSend("lower %.0f %.0f", GmRadius, GmHeight); break;
		case 5: GmSend("flatten %.0f", GmRadius); break;
		case 6: GmSend("smooth %.0f", GmRadius); break;
		default: GmSend("draw add"); break;   // a point of the draw at the hit along the ray
		}
	}
	// the draw being made: screen-space lines between its projected points (closing edge faint)
	void GmDrawLines()
	{
		if (GmDrawPts.empty() || !GmViewOk)
			return;
		ImDrawList *DL = ImGui::GetBackgroundDrawList();
		const ImU32 Col = IM_COL32(255, 120, 230, 255), Faint = IM_COL32(255, 120, 230, 90);
		std::vector<ImVec2> S(GmDrawPts.size());
		std::vector<char> Ok(GmDrawPts.size());
		for (size_t i = 0; i < GmDrawPts.size(); i++)
			Ok[i] = GmProject(GmDrawPts[i].data(), S[i]) ? 1 : 0;
		for (size_t i = 0; i < S.size(); i++)
		{
			if (Ok[i])
			{
				DL->AddCircleFilled(S[i], 4.0f, Col);
				char N[8];
				snprintf(N, sizeof(N), "%d", (int)i + 1);
				DL->AddText(ImVec2(S[i].x + 6, S[i].y - 14), Col, N);
			}
			if (i > 0 && Ok[i] && Ok[i - 1])
				DL->AddLine(S[i - 1], S[i], Col, 2.0f);
		}
		if (S.size() > 2 && Ok.front() && Ok.back())
			DL->AddLine(S.back(), S.front(), Faint, 1.0f);
	}
	// System\U2GMCommit.status (the commit watcher's progress line), read about twice a second while shown
	void GmReadCommitStatus()
	{
		const std::string Path = Dir + "U2GMCommit.status";
		WIN32_FILE_ATTRIBUTE_DATA A = {};
		if (!GetFileAttributesExA(Path.c_str(), GetFileExInfoStandard, &A))
		{
			GmCommitWatch.clear();
			return;
		}
		FILETIME Now;
		GetSystemTimeAsFileTime(&Now);
		const ULONGLONG T0 = ((ULONGLONG)A.ftLastWriteTime.dwHighDateTime << 32) | A.ftLastWriteTime.dwLowDateTime;
		const ULONGLONG T1 = ((ULONGLONG)Now.dwHighDateTime << 32) | Now.dwLowDateTime;
		GmCommitWatchAge = T1 > T0 ? (DWORD)(std::min)((T1 - T0) / 10000, (ULONGLONG)0xfffffffe) : 0;
		FILE *F = nullptr;
		if (fopen_s(&F, Path.c_str(), "rb") || F == nullptr)
			return;
		char Buf[256] = {};
		const size_t n = fread(Buf, 1, sizeof(Buf) - 1, F);
		fclose(F);
		Buf[n] = 0;
		for (char *c = Buf; *c; c++)
			if ((unsigned char)*c < 32)
			{
				*c = 0;
				break;
			}
		GmCommitWatch = Buf;
	}

	// ---- the panel
	void GmPanelUi()
	{
		const ImVec4 Warn(1.0f, 0.65f, 0.25f, 1.0f);
		ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Game master   (F7 hides)"))
		{
			ImGui::End();
			return;
		}
		ImGui::TextDisabled("%s | game took line %ld, %d waiting", CurMap.empty() ? "?" : GmFamily().c_str(), GmAcked(), (int)GmOut.size());
		if (!GmSt.Valid)
			ImGui::TextColored(Warn, "no PanelState in U2GM.ini yet: is U2GM loaded?");
		if (GmHold == nullptr)
			ImGui::TextColored(Warn, "U2Input not found: the mouse still turns the view");
		if (!GmViewOk)
			ImGui::TextColored(Warn, "no scene view yet: no gizmo, no world clicks");
		bool On = GmSt.On;
		if (ImGui::Checkbox("GM mode", &On))
			GmSend(On ? "on" : "off");
		ImGui::SameLine();
		bool Frz = GmSt.Frz;
		if (ImGui::Checkbox("Freeze", &Frz))
			GmSend("freeze");
		ImGui::SameLine();
		if (GmSt.Poss ? ImGui::Button("Release") : ImGui::Button("Possess"))
			GmSend(GmSt.Poss ? "release" : "possess");
		ImGui::SameLine();
		if (ImGui::Button("Undo"))
			GmSend("undo");
		ImGui::SameLine();
		if (ImGui::Button("Redo"))
			GmSend("redo");

		ImGui::SeparatorText("Picked");
		if (GmSt.Pick == "-")
			ImGui::TextDisabled("nothing: Pick (crosshair) or click the world");
		else
		{
			ImGui::Text("%s  (%s)", GmSt.Pick.c_str(), GmSt.Cls.c_str());
			ImGui::TextWrapped("mesh %s", GmSt.Mesh.c_str());
			ImGui::Text("at %.0f %.0f %.0f   yaw %.1f   scale %.2f", GmSt.Loc[0], GmSt.Loc[1], GmSt.Loc[2], GmSt.Yaw, GmSt.Scale);
		}
		if (ImGui::Button("Pick (crosshair)"))
			GmSend("pick");
		ImGui::SameLine();
		if (ImGui::Button("Hide"))
			GmSend("hide");
		ImGui::SetNextItemWidth(200);
		ImGui::Combo("click in world", &GmClickMode, "pick\0spawn the palette entry\0move the picked here\0raise terrain\0lower terrain\0flatten terrain\0smooth terrain\0draw (add a point)\0\0");

		ImGui::SeparatorText("Move / turn / scale");
		ImGui::SetNextItemWidth(90);
		ImGui::InputFloat("step", &GmStep, 0, 0, "%.0f");
		for (int a = 0; a < 3; a++)
			for (int s = -1; s <= 1; s += 2)
			{
				char B[16];
				snprintf(B, sizeof(B), "%c%c", s < 0 ? '-' : '+', "XYZ"[a]);
				if (a != 0 || s > 0)
					ImGui::SameLine();
				if (ImGui::Button(B, ImVec2(40, 0)))
					GmSend("move %g %g %g", a == 0 ? s * GmStep : 0.0f, a == 1 ? s * GmStep : 0.0f, a == 2 ? s * GmStep : 0.0f);
			}
		if (ImGui::Button("Turn -15"))
			GmSend("turn -15");
		ImGui::SameLine();
		if (ImGui::Button("Turn +15"))
			GmSend("turn 15");
		ImGui::SameLine();
		if (ImGui::Button("Scale -10%"))
			GmSend("scale %.3f", GmSt.Scale * 0.9f);
		ImGui::SameLine();
		if (ImGui::Button("Scale +10%"))
			GmSend("scale %.3f", GmSt.Scale * 1.1f);
		ImGui::Checkbox("gizmo", &GmGizmoOn);
		ImGui::SameLine();
		ImGui::Checkbox("live preview", &GmPreviewOn);
		ImGui::SameLine();
		ImGui::Checkbox("snap", &GmSnapOn);
		ImGui::TextDisabled("grid %g, yaw step %d (gm snap / gm yawstep)", GmGrid, GmYawStep);

		ImGui::SeparatorText("Spawn");
		if (GmPalette.empty())
			ImGui::TextDisabled("palette empty: gm palette add Package.Group.Mesh");
		else
		{
			ImGui::BeginChild("palette", ImVec2(0, 90), ImGuiChildFlags_Borders);
			for (const auto &It : GmPalette)
			{
				char B[300];
				snprintf(B, sizeof(B), "%d: %s", It.first, It.second.c_str());
				if (ImGui::Selectable(B, GmPaletteSel == It.first))
					GmPaletteSel = It.first;
			}
			ImGui::EndChild();
		}
		if (ImGui::Button("Spawn at crosshair"))
			GmSend("spawn %d", GmPaletteSel);

		ImGui::SeparatorText("Terrain (at the crosshair)");
		ImGui::SetNextItemWidth(90);
		ImGui::InputFloat("radius", &GmRadius, 0, 0, "%.0f");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(90);
		ImGui::InputFloat("height", &GmHeight, 0, 0, "%.0f");
		if (ImGui::Button("Raise"))
			GmSend("raise %.0f %.0f", GmRadius, GmHeight);
		ImGui::SameLine();
		if (ImGui::Button("Lower"))
			GmSend("lower %.0f %.0f", GmRadius, GmHeight);
		ImGui::SameLine();
		if (ImGui::Button("Flatten"))
			GmSend("flatten %.0f", GmRadius);
		ImGui::SameLine();
		if (ImGui::Button("Smooth"))
			GmSend("smooth %.0f", GmRadius);

		ImGui::SeparatorText("Draw (notes for the level team)");
		ImGui::Text("%d point(s)%s", (int)GmDrawPts.size(), GmClickMode == 7 ? ": world clicks add points" : ": click in world = draw adds points");
		if (ImGui::Button("Point at crosshair"))
			GmSend("draw add");
		ImGui::SameLine();
		if (ImGui::Button("Undo point"))
			GmSend("draw undo");
		ImGui::SameLine();
		if (ImGui::Button("Clear"))
			GmSend("draw clear");
		ImGui::SetNextItemWidth(260);
		ImGui::InputText("note", GmNote, sizeof(GmNote));
		for (int c = 0; c < 2; c++)
		{
			if (c)
				ImGui::SameLine();
			if (ImGui::Button(c ? "Done (closed)" : "Done (open)"))
			{
				// the note through the command file's filter: anything else becomes a space
				std::string N;
				for (const char *p = GmNote; *p; p++)
					N += (isalnum((unsigned char)*p) || strchr(" ._-#:,+!?()/%&", *p)) ? *p : ' ';
				while (!N.empty() && N.back() == ' ')
					N.pop_back();
				GmSend("draw done %s%s%s", c ? "closed" : "open", N.empty() ? "" : " ", N.c_str());
				GmNote[0] = 0;
			}
		}

		ImGui::SeparatorText("Commit (bake into a new map)");
		{
			const bool Pending = GmSt.CommitState == "pending";
			if (!Pending && ImGui::Button("Commit"))
				GmSend("commit");
			if (Pending && ImGui::Button("Cancel commit"))
				GmSend("commit cancel");
			ImGui::SameLine();
			if (GmSt.CommitStamp == 0)
				ImGui::TextDisabled("no commit yet");
			else if (GmSt.CommitState == "done")
				ImGui::Text("commit %ld done: %s", GmSt.CommitStamp, GmSt.CommitMap.c_str());
			else
				ImGui::Text("commit %ld %s%s%s", GmSt.CommitStamp, GmSt.CommitState.c_str(), GmSt.CommitMap != "-" ? ": " : "", GmSt.CommitMap != "-" ? GmSt.CommitMap.c_str() : "");
			if (Frame % 30 == 0 || GmCommitWatchAge == 0xffffffff)
				GmReadCommitStatus();
			if (!GmCommitWatch.empty() && GmCommitWatchAge < 90000)   // the watcher beats every 30 s
				ImGui::TextWrapped("watcher: %s", GmCommitWatch.c_str());
			else if (Pending)
				ImGui::TextColored(Warn, "no watcher answered: run U2GM/tools/gm_commit.py --watch");
		}

		TexEd().PanelUi(*this);              // texedit.hpp: pick a texture, sliders, Save to the journal
		ImGui::SeparatorText("Journal (this map)");
		ImGui::BeginChild("journal", ImVec2(0, 140), ImGuiChildFlags_Borders);
		if (GmJournal.empty())
			ImGui::TextDisabled("no lines");
		for (const auto &It : GmJournal)
			ImGui::Text("%d: %s", It.first, It.second.c_str());
		ImGui::EndChild();
		if (GmViewOk && GmSt.CamOk)
			ImGui::TextDisabled("view check: camera %.0f %.0f %.0f, game eye %.0f %.0f %.0f", GmCam[0], GmCam[1], GmCam[2], GmSt.Cam[0], GmSt.Cam[1], GmSt.Cam[2]);
		ImGui::End();
	}

	// ---- show / hide, input
	// the game's mouse held and the cursor shown while the panel or the sketch is open
	bool GmCaptured = false;
	void GmCapture(bool S)
	{
		if (S == GmCaptured)
			return;
		GmCaptured = S;
		if (!GmHoldLooked)
		{
			GmHoldLooked = true;
			if (HMODULE U2I = GetModuleHandleA((Dir + "dinput8.dll").c_str()))
				GmHold = (GmHold_t)GetProcAddress(U2I, "U2InputHoldMouse");
			Message("gm panel: U2Input's U2InputHoldMouse %s", GmHold ? "found: the game's mouse is held while the panel is open" : "not found (System\\dinput8.dll missing or older): mouse look isn't held");
		}
		if (GmHold != nullptr)
			GmHold(S ? 1 : 0);
		if (S)
		{
			ClipCursor(nullptr);
			int n = ShowCursor(TRUE);
			GmCursorShows = 1;
			while (n < 0 && GmCursorShows < 64)
			{
				n = ShowCursor(TRUE);
				GmCursorShows++;
			}
			RECT R;
			if (LiveWnd != nullptr && GetClientRect(LiveWnd, &R))
			{
				POINT C = { R.right / 2, R.bottom / 2 };
				ClientToScreen(LiveWnd, &C);
				SetCursorPos(C.x, C.y);
			}
		}
		else
		{
			for (; GmCursorShows > 0; GmCursorShows--)
				ShowCursor(FALSE);
		}
		if (GmImReady)
			ImGui::GetIO().AddFocusEvent(S);
	}
	void GmSetShown(bool S)
	{
		if (S == GmPanelShown)
			return;
		GmPanelShown = S;
		if (!S)
			GmDragCancel();
		GmCapture(GmPanelShown || SkOpen);
		GmSend(S ? "panel 1" : "panel 0");
		Message("gm panel: %s", S ? "shown" : "hidden");
	}
	// from LiveWndProc (the game window's messages): F7, and while shown the input goes to ImGui
	bool GmPanelInput(HWND W, UINT M, WPARAM A, LPARAM B, LRESULT &R)
	{
		R = 0;
		if (GmPanelBroken || (GmPanelMode == 0 && !SketchOn))
			return false;
		if (GmPanelMode != 0 && (M == WM_KEYDOWN || M == WM_SYSKEYDOWN) && A == VK_F7)
		{
			if (!(B & (1 << 30)))                // not a repeat
				GmSetShown(!GmPanelShown);
			return true;
		}
		if (GmPanelMode != 0 && (M == WM_KEYUP || M == WM_SYSKEYUP) && A == VK_F7)
			return true;
		// the console from its keys (Avalon Q72c): Console.ui's triggers reach U2GM only when the console closes,
		// and PC.bIsTyping never changes in Unreal II. User.ini: Tilde = ShowConsole (the big console), Tab = Type
		// (the one-line one); the big one closes on Escape or Tilde, the one-line one on Enter or Escape. Each change
		// goes to U2GM as "gm con big|quick 1|0" (GMCine pauses a cinematic while it is open)
		if (GmPanelStarted && !GmPanelShown && !SkOpen && (M == WM_KEYDOWN || M == WM_SYSKEYDOWN) && !(B & (1 << 30)))
		{
			int Next = KeyCon;
			if (KeyCon == 0 && A == VK_OEM_3) Next = 1;
			else if (KeyCon == 0 && A == VK_TAB) Next = 2;
			else if (KeyCon == 1 && (A == VK_ESCAPE || A == VK_OEM_3)) Next = 0;
			else if (KeyCon == 2 && (A == VK_RETURN || A == VK_ESCAPE)) Next = 0;
			if (Next != KeyCon)
			{
				const char *Which = (Next == 2 || KeyCon == 2) ? "quick" : "big";
				KeyCon = Next;
				GmSend("con %s %d", Which, Next != 0 ? 1 : 0);
			}
		}
		if (SketchOn && GmPanelStarted)
		{
			// sketch=1: its key (never reaches the game: U2 binds F8 to QuickLoad), the console's strip, Escape
			if ((M == WM_KEYDOWN || M == WM_SYSKEYDOWN) && A == SketchKey)
			{
				if (!(B & (1 << 30)))
				{
					if (SkOpen)
						SkSetOpen(false);
					else
						SkRequestOpen();
				}
				return true;
			}
			if ((M == WM_KEYUP || M == WM_SYSKEYUP) && A == SketchKey)
				return true;
			if (!SkOpen && SkStripShown && (M == WM_LBUTTONDOWN || M == WM_LBUTTONDBLCLK))
			{
				const float x = (float)(short)LOWORD(B), y = (float)(short)HIWORD(B);
				if (x >= SkStrip.x && y >= SkStrip.y && x <= SkStrip.z && y <= SkStrip.w)
				{
					SkRequestOpen();
					return true;
				}
			}
			if (SkOpen && M == WM_KEYDOWN && A == VK_ESCAPE && !(GmImReady && ImGui::GetIO().WantTextInput))
			{
				SkSetOpen(false);
				return true;
			}
		}
		if (!GmPanelShown && !SkOpen)
			return false;
		const bool Im = GmImReady;
		switch (M)
		{
		case WM_SETCURSOR:
			if (LOWORD(B) != HTCLIENT)
				return false;
			if (!(Im && ImGui_ImplWin32_WndProcHandler(W, M, A, B)))
				SetCursor(GmPanelMode == 2 ? nullptr : LoadCursor(nullptr, IDC_ARROW));
			R = TRUE;
			return true;
		case WM_KEYUP:
		case WM_SYSKEYUP:
			if (Im)
				ImGui_ImplWin32_WndProcHandler(W, M, A, B);
			return false;                    // the game sees releases: nothing stays held
		case WM_SYSKEYDOWN:
			if (Im)
				ImGui_ImplWin32_WndProcHandler(W, M, A, B);
			return A != VK_F4;               // Alt+F4 still closes the game
		case WM_KEYDOWN:
		case WM_CHAR:
		case WM_SYSCHAR:
		case WM_MOUSEMOVE:
		case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
		case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
		case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
		case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
		case WM_MOUSEWHEEL:
		case WM_MOUSEHWHEEL:
			if (Im)
				ImGui_ImplWin32_WndProcHandler(W, M, A, B);
			return true;                     // the panel's only
		default:
			if (Im)
				ImGui_ImplWin32_WndProcHandler(W, M, A, B);   // focus, mouse leave...: both see it
			return false;
		}
	}

	bool GmImSetup(IDirect3DDevice9 *Dev)
	{
		if (!GmImReady)
		{
			IMGUI_CHECKVERSION();
			ImGui::CreateContext();
			ImGuiIO &io = ImGui::GetIO();
			GmImIni = Dir + "U2GMPanel.imgui.ini";
			io.IniFilename = GmImIni.c_str();
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
			io.MouseDrawCursor = GmPanelMode == 2;
			ImGui::StyleColorsDark();
			ImGuiStyle &St = ImGui::GetStyle();
			St.WindowRounding = 4;
			St.Colors[ImGuiCol_WindowBg].w = 0.92f;
			if (GmPanelScale > 0.5f && GmPanelScale < 4 && GmPanelScale != 1)
			{
				St.ScaleAllSizes(GmPanelScale);
				St.FontScaleMain = GmPanelScale;
			}
			if (!ImGui_ImplWin32_Init(LiveWnd))
			{
				Message("gm panel: ImGui's win32 backend didn't start: off");
				GmPanelBroken = true;
				return false;
			}
			GmImReady = true;
		}
		if (GmImDev != Dev)
		{
			if (GmImDev != nullptr)
				ImGui_ImplDX9_Shutdown();
			GmImDev = nullptr;
			if (!ImGui_ImplDX9_Init(Dev))
			{
				Message("gm panel: ImGui's dx9 backend didn't start: off");
				GmPanelBroken = true;
				return false;
			}
			GmImDev = Dev;
		}
		return true;
	}
	void GmPanelDraw(IDirect3DDevice9 *Dev)
	{
		U2Crash::Where("drawing the GM panel or the sketch");
		ImGui_ImplDX9_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGuiIO &io = ImGui::GetIO();
		GmDispW = io.DisplaySize.x;
		GmDispH = io.DisplaySize.y;
		ImGui::NewFrame();
		if (SkOpen)
			SketchUi(Dev);                   // sketch mode covers the screen: the panel waits
		else
		{
			if (GmPanelShown)
			{
				GmPanelUi();
				if (GmDispW >= 16 && GmDispH >= 16)
				{
					GmGizmo();
					GmDrawLines();
					if (!TexEd().WorldClick())   // texedit: a pick waiting for its click (Esc cancels)
						GmWorldClick();
				}
			}
			SketchStrip();
		}
		if (SkSaveWant != 0)
			SkBuildSave();                   // before Render: glyphs it bakes are uploaded by this frame's render
		ImGui::Render();
		IDirect3DSurface9 *OldRT = nullptr, *OldDS = nullptr, *BB = nullptr;
		D3DVIEWPORT9 OldVp = {};
		Dev->GetViewport(&OldVp);
		Dev->GetRenderTarget(0, &OldRT);
		Dev->GetDepthStencilSurface(&OldDS);
		Dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &BB);
		if (BB != nullptr)
		{
			static bool Told = false;
			D3DSURFACE_DESC D = {};
			BB->GetDesc(&D);
			if (!Told && (fabsf(D.Width - GmDispW) > 1 || fabsf(D.Height - GmDispH) > 1))
			{
				Told = true;
				Message("gm panel: back buffer %ux%u but the window is %.0fx%.0f: the panel's mouse may be off", D.Width, D.Height, GmDispW, GmDispH);
			}
			if (BB != OldRT)
				Dev->SetRenderTarget(0, BB);
			Dev->SetDepthStencilSurface(nullptr);
			if (SUCCEEDED(Dev->BeginScene()))
			{
				ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
				Dev->EndScene();
			}
			if (OldRT != nullptr && BB != OldRT)
				Dev->SetRenderTarget(0, OldRT);
			Dev->SetDepthStencilSurface(OldDS);
			Dev->SetViewport(&OldVp);
		}
		if (BB) BB->Release();
		if (OldRT) OldRT->Release();
		if (OldDS) OldDS->Release();
		if (SkSaveWant != 0)
			SkSave(Dev);
	}
	// every Present while gmpanel or sketch is on
	void GmPanelFrame(IDirect3DDevice9 *Dev)
	{
		if (GmPanelBroken || Dir.empty() || (GmPanelMode == 0 && !GmPanelShown && !SketchOn))
			return;
		if (!GmPanelStarted)
		{
			GmPanelStarted = true;
			if (GmExeIsEditor())
			{
				Message("gm panel: not in UnrealEd: off");
				GmPanelBroken = true;
				return;
			}
			GmSession = ((GetTickCount() ^ (GetCurrentProcessId() << 12)) & 0x3fffffff) | 1;
			GmPanelFileEmpty(true);   // an old session's lines must not run (emptied, not deleted: U2GM execs it every poll)
			Message("gm panel: %s, session %u, commands through %sU2GMPanel.txt", GmPanelMode != 0 ? "on (F7)" : "off", GmSession, Dir.c_str());
			if (SketchOn)
				Message("sketch: on (key 0x%02X%s, frozen frame %s), files in %sSketch", SketchKey, SketchKey >= VK_F1 && SketchKey <= VK_F24 ? (" = F" + std::to_string(SketchKey - VK_F1 + 1)).c_str() : "",
					SketchHud ? "= the presented frame (sketchhud=1)" : "= the world before the HUD and console", Dir.c_str());
		}
		if (GmPanelMode == 0)
			GmSetShown(false);               // switched off in U2Shaders.ini while open
		if (!LiveHook(Dev))
			return;
		if (Frame % 10 == 7 || ((GmPanelShown || SkOpen) && Frame % 3 == 0))
			GmPanelReadIni();
		GmTakeView();
		SketchFrame(Dev);                    // the console's state, frames frozen this frame
		if ((GmPanelShown || SkOpen || (SketchOn && SkConsole)) && GmImSetup(Dev))
			GmPanelDraw(Dev);
		else
			SkStripShown = false;
		GmFlushOut();
	}

	// ---- sketch=1: screenshot markup over the game (Dear ImGui) ---------------------------------
	// "Image editing tools when I'm on the console": a frozen frame marked up with pen, highlighter,
	// arrow, rectangle, ellipse, text labels and a crop, saved as a PNG with a sidecar .txt, and if
	// wanted sent as an "avalon mark" so the level team gets it with a normal mark.
	//   - The console: U2's console is a UI component (UIScripts\Console.ui), not visible to script.
	//     U2GM/tools/sketch_console_ui.py adds TriggerEvent lines to its two states, so the UI itself
	//     runs "gm con big|quick 1|0" when either console opens or closes; GMMaster keeps it as con=N
	//     in PanelState (U2GM.ini, written on change), read here every 10 frames (GmPanelReadIni).
	//   - The frozen frame: the world of the next frame, copied at its first 2D draw (where post
	//     runs, so post is in it): the console and the HUD are 2D draws drawn later, so they're never
	//     in it, however late the console's flag arrives. A frame without a 2D draw is copied at
	//     Present. sketchhud=1: the whole presented frame instead (HUD and an open console in it).
	//   - While the console is open a "Sketch" strip shows on the right; a click on it or the key
	//     (sketchkey=, default F8; U2 binds F8 to QuickLoad, which then never reaches the game) opens
	//     sketch mode: the frozen frame full screen, a tool bar, the mouse held as for the GM panel
	//     (U2Input's U2InputHoldMouse). The key outside the console freezes a fresh frame first.
	//     Escape (outside a text field) or the key again closes it; the strokes stay with the frame.
	//   - Strokes are vectors in the frame's pixels; undo/redo are snapshots of the list. Save draws
	//     the frame and the strokes with ImGui's own renderer into a target of the frame's size,
	//     reads it back, cuts the crop, and writes System\Sketch\sketch-YYYYMMDD-HHMMSS.png
	//     (stb_image_write, on a thread) and sketch-....txt beside it (map, eye, view, note, strokes).
	//   - Save as mark: the same, then "gm sketch mark NAME NOTE" through U2GMPanel.txt; GMMaster logs
	//     it and, when AvalonCards is loaded, runs "avalon mark NOTE sketch:NAME" (a normal mark with
	//     its own Shot*.bmp; U2Avalon/tools/live.py --marks copies the sketch next to it).
	// Off unless U2Shaders.ini has sketch=1; never in UnrealEd (GmPanelFrame's check).
	bool SketchOn = false, SketchHud = false;
	UINT SketchKey = VK_F8;
	enum { SkPen, SkHigh, SkArrow, SkRect, SkEllipse, SkText, SkCrop, SkToolCount };
	struct SkStroke
	{
		int Tool = 0;
		ImU32 Col = IM_COL32(255, 59, 48, 255);
		float Size = 5;                      // frame pixels
		std::vector<ImVec2> Pts;             // frame pixels: pen/highlight all, shapes start and end, text its place
		std::string Text;
	};
	std::vector<SkStroke> SkStrokes;
	std::vector<std::vector<SkStroke>> SkUndo, SkRedo;
	SkStroke SkCur;                          // the stroke being drawn
	bool SkDrawing = false;
	bool SkOpen = false, SkConsole = false, SkStripShown = false;
	ImVec4 SkStrip = ImVec4(0, 0, 0, 0);     // the strip, client pixels (x0 y0 x1 y1)
	unsigned SkConsoleSeq = 0, SkPixSeq = 0; // console openings; the one the frozen frame is from (0 none)
	D3DMATRIX SkPixView = {};                // the camera when the frame was frozen
	bool SkPixViewOk = false, SkConStale = false; // con= said open but the camera moved since: a missed close
	D3DMATRIX SkConView = {};                // the camera when con= went to open
	int KeyCon = 0;                          // the console from its keys: 0 closed, 1 the big one, 2 the one-line one
	bool SkConViewOk = false;
	bool SkGrabWant = false, SkGrabSaw3D = false, SkGrabPending = false, SkGrabFull = false;
	bool SkGrabForConsole = false, SkOpenOnGrab = false;
	int SkGrabAge = 0;
	IDirect3DSurface9 *SkGrabRT = nullptr;   // the copy, until it is read back at Present
	std::vector<DWORD> SkPix;                // the frozen frame, B G R A (A = 255)
	UINT SkW = 0, SkH = 0;
	bool SkPixFull = false;
	IDirect3DTexture9 *SkTex = nullptr;      // managed: survives a reset
	bool SkTexTold = false;
	int SkTool = 0, SkColor = 0;
	float SkSize = 5;
	char SkLabel[128] = {}, SkNote[200] = {};
	int SkSaveWant = 0;                      // 1 a PNG, 2 a PNG and a mark
	std::string SkStatus, SkLastName;
	ImDrawList *SkSaveList = nullptr;
	ImVec2 SkO = ImVec2(0, 0);               // the frame on screen: top left, scale
	float SkS = 1;

	static ImU32 SkPal(int i)
	{
		static const ImU32 P[8] = { IM_COL32(255, 59, 48, 255), IM_COL32(255, 149, 0, 255), IM_COL32(255, 214, 10, 255), IM_COL32(52, 199, 89, 255),
			IM_COL32(50, 210, 245, 255), IM_COL32(10, 132, 255, 255), IM_COL32(255, 45, 149, 255), IM_COL32(255, 255, 255, 255) };
		return P[i & 7];
	}
	static const char *SkPalName(int i)
	{
		static const char *N[8] = { "red", "orange", "yellow", "green", "cyan", "blue", "magenta", "white" };
		return N[i & 7];
	}
	static const char *SkToolName(int i)
	{
		static const char *N[SkToolCount] = { "Pen", "Highlight", "Arrow", "Rect", "Ellipse", "Text", "Crop" };
		return (i >= 0 && i < SkToolCount) ? N[i] : "?";
	}
	std::string SkKeyName() const
	{
		char B[16];
		if (SketchKey >= VK_F1 && SketchKey <= VK_F24)
			snprintf(B, sizeof(B), "F%u", SketchKey - VK_F1 + 1);
		else
			snprintf(B, sizeof(B), "key 0x%02X", SketchKey);
		return B;
	}

	// ---- the frozen frame
	// per draw while a frame is wanted (LogTargetDraw): the first 2D draw after the world
	void SketchDraw(IDirect3DDevice9 *Dev)
	{
		if (SketchHud || Offscreen(Dev))
			return;
		DWORD Fvf = 0;
		Dev->GetFVF(&Fvf);
		IDirect3DVertexShader9 *VS = nullptr;
		Dev->GetVertexShader(&VS);
		const bool Prog = VS != nullptr;
		if (VS) VS->Release();
		D3DMATRIX P = {};
		Dev->GetTransform(D3DTS_PROJECTION, &P);
		const bool Rhw = (Fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
		const bool Ortho = !Prog && !Rhw && P._34 == 0.0f && P._44 == 1.0f;
		if (!Rhw && !Ortho)
		{
			SkGrabSaw3D = true;
			return;
		}
		if (!SkGrabSaw3D)
			return;
		if (PostHudZ0)
		{
			DWORD Z = 0;
			Dev->GetRenderState(D3DRS_ZENABLE, &Z);
			if (Z)
				return;                      // as the post: z-tested 2D draws are still the scene
		}
		IDirect3DSurface9 *RT = nullptr;
		if (SUCCEEDED(Dev->GetRenderTarget(0, &RT)) && RT)
		{
			SkGrab(Dev, RT, false);
			RT->Release();
		}
	}
	void SkGrab(IDirect3DDevice9 *Dev, IDirect3DSurface9 *Src, bool Full)
	{
		SkGrabWant = false;
		D3DSURFACE_DESC D = {};
		Src->GetDesc(&D);
		if (SkGrabRT != nullptr)
		{
			D3DSURFACE_DESC G = {};
			SkGrabRT->GetDesc(&G);
			if (G.Width != D.Width || G.Height != D.Height || G.Format != D.Format)
			{
				SkGrabRT->Release();
				SkGrabRT = nullptr;
			}
		}
		if (SkGrabRT == nullptr && FAILED(Dev->CreateRenderTarget(D.Width, D.Height, D.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &SkGrabRT, nullptr)))
		{
			SkGrabRT = nullptr;
			Message("sketch: couldn't make a %ux%u copy (format %u): no frame", D.Width, D.Height, (unsigned)D.Format);
			SkOpenOnGrab = false;
			return;
		}
		const HRESULT hr = Dev->StretchRect(Src, nullptr, SkGrabRT, nullptr, D3DTEXF_NONE);
		if (FAILED(hr))
		{
			Message("sketch: couldn't copy the frame (%08x): no frame", (unsigned)hr);
			SkOpenOnGrab = false;
			return;
		}
		SkGrabPending = true;
		SkGrabFull = Full;
		SkPixView = GmView;
		SkPixViewOk = GmViewOk;
	}
	static bool SkToBgra(const D3DSURFACE_DESC &D, const D3DLOCKED_RECT &L, std::vector<DWORD> &Out)
	{
		Out.resize((size_t)D.Width * D.Height);
		for (UINT y = 0; y < D.Height; y++)
		{
			const BYTE *Row = static_cast<const BYTE *>(L.pBits) + (size_t)y * L.Pitch;
			DWORD *O = &Out[(size_t)y * D.Width];
			switch (D.Format)
			{
			case D3DFMT_X8R8G8B8:
			case D3DFMT_A8R8G8B8:
				for (UINT x = 0; x < D.Width; x++)
					O[x] = reinterpret_cast<const DWORD *>(Row)[x] | 0xFF000000u;
				break;
			case D3DFMT_R5G6B5:
				for (UINT x = 0; x < D.Width; x++)
				{
					const DWORD p = reinterpret_cast<const WORD *>(Row)[x];
					O[x] = 0xFF000000u | ((((p >> 11) & 31) * 255 / 31) << 16) | ((((p >> 5) & 63) * 255 / 63) << 8) | ((p & 31) * 255 / 31);
				}
				break;
			case D3DFMT_X1R5G5B5:
			case D3DFMT_A1R5G5B5:
				for (UINT x = 0; x < D.Width; x++)
				{
					const DWORD p = reinterpret_cast<const WORD *>(Row)[x];
					O[x] = 0xFF000000u | ((((p >> 10) & 31) * 255 / 31) << 16) | ((((p >> 5) & 31) * 255 / 31) << 8) | ((p & 31) * 255 / 31);
				}
				break;
			default:
				Out.clear();
				return false;
			}
		}
		return true;
	}
	void SkReadBack(IDirect3DDevice9 *Dev)
	{
		SkGrabPending = false;
		if (SkGrabRT == nullptr)
			return;
		D3DSURFACE_DESC D = {};
		SkGrabRT->GetDesc(&D);
		IDirect3DSurface9 *Sys = nullptr;
		D3DLOCKED_RECT L = {};
		bool Ok = false;
		HRESULT hr = Dev->CreateOffscreenPlainSurface(D.Width, D.Height, D.Format, D3DPOOL_SYSTEMMEM, &Sys, nullptr);
		if (SUCCEEDED(hr))
			hr = U2PerfReadback(Dev, SkGrabRT, Sys);
		if (SUCCEEDED(hr))
			hr = Sys->LockRect(&L, nullptr, D3DLOCK_READONLY);
		std::vector<DWORD> Pix;
		if (SUCCEEDED(hr))
		{
			Ok = SkToBgra(D, L, Pix);
			Sys->UnlockRect();
		}
		if (Sys) Sys->Release();
		SkGrabRT->Release();                 // a grab is rare: no full-screen copy kept around
		SkGrabRT = nullptr;
		if (!Ok)
		{
			Message("sketch: couldn't read the frame (%08x, format %u)", (unsigned)hr, (unsigned)D.Format);
			SkOpenOnGrab = false;
			SkGrabForConsole = false;
			return;
		}
		SkPix.swap(Pix);
		SkW = D.Width;
		SkH = D.Height;
		SkPixFull = SkGrabFull;
		SkPixSeq = SkGrabForConsole ? SkConsoleSeq : 0;
		SkGrabForConsole = false;
		if (SkTex) { SkTex->Release(); SkTex = nullptr; }
		SkStrokes.clear();                   // a new frame: a new sketch
		SkUndo.clear();
		SkRedo.clear();
		SkDrawing = false;
		SkStatus.clear();
		if (SkOpenOnGrab)
		{
			SkOpenOnGrab = false;
			SkSetOpen(true);
		}
	}
	bool SkEnsureTex(IDirect3DDevice9 *Dev)
	{
		if (SkTex != nullptr)
			return true;
		if (SkPix.empty() || SkW == 0 || SkH == 0)
			return false;
		if (FAILED(Dev->CreateTexture(SkW, SkH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &SkTex, nullptr)) || SkTex == nullptr)
		{
			SkTex = nullptr;
			if (!SkTexTold)
				Message("sketch: couldn't make a %ux%u texture for the frame", SkW, SkH);
			SkTexTold = true;
			return false;
		}
		D3DLOCKED_RECT L = {};
		if (SUCCEEDED(SkTex->LockRect(0, &L, nullptr, 0)))
		{
			for (UINT y = 0; y < SkH; y++)
				memcpy(static_cast<BYTE *>(L.pBits) + (size_t)y * L.Pitch, &SkPix[(size_t)y * SkW], SkW * 4);
			SkTex->UnlockRect(0);
		}
		return true;
	}
	ImTextureRef SkTexRef() const { return ImTextureRef((ImTextureID)(intptr_t)SkTex); }

	// every Present (GmPanelFrame), before anything of ours is drawn: the console, the frame
	void SketchFrame(IDirect3DDevice9 *Dev)
	{
		if (!SketchOn)
			return;
		U2Crash::Where("the sketch (frozen frame, console state)");
		// still wanted: no 2D draw came in a whole frame (or sketchhud=1): the frame as presented
		if (SkGrabWant && (SketchHud || ++SkGrabAge >= 2))
		{
			IDirect3DSurface9 *BB = nullptr;
			if (SUCCEEDED(Dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &BB)) && BB)
			{
				SkGrab(Dev, BB, true);
				BB->Release();
			}
		}
		SkGrabSaw3D = false;
		if (SkGrabPending)
			SkReadBack(Dev);
		// the console (U2GM's PanelState con=): opened -> freeze a frame for the strip
		const bool ConRaw = GmSt.Valid && GmSt.Con != 0;
		// Console.ui's close trigger can be missed (a cutscene or map change with the console open),
		// leaving con= stuck at open. The open console holds the camera still, so a camera that moved
		// away from the console's frame means it is shut: no strip, and the key freezes a fresh frame.
		if (!ConRaw)
			SkConStale = false;
		else if (SkConsole && !SkOpen && SkConMoved())
		{
			SkConStale = true;
			Message("sketch: the camera moved with con= still open: taken as a missed console close");
		}
		const bool Con = ConRaw && !SkConStale;
		if (Con && !SkConsole)
		{
			SkConView = GmView;
			SkConViewOk = GmViewOk;
			SkConsoleSeq++;
			if (!SkOpen && !SkGrabWant)
				SkWantGrab(true);
		}
		SkConsole = Con;
	}
	// the camera moved away from where it was when con= went to open (the open console holds the camera still)
	bool SkConMoved() const
	{
		if (!GmViewOk || !SkConViewOk)
			return false;
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 3; c++)
				if (fabsf(GmView.m[r][c] - SkConView.m[r][c]) > (r == 3 ? 4.0f : 0.01f))
					return true;
		return false;
	}
	bool SkViewMoved() const
	{
		if (!GmViewOk || !SkPixViewOk)
			return false;
		for (int r = 0; r < 4; r++)
			for (int c = 0; c < 3; c++)
				if (fabsf(GmView.m[r][c] - SkPixView.m[r][c]) > (r == 3 ? 4.0f : 0.01f))
					return true;
		return false;
	}
	void SkWantGrab(bool ForConsole)
	{
		SkGrabWant = true;
		SkGrabAge = 0;
		SkGrabSaw3D = false;
		SkGrabForConsole = ForConsole;
	}
	// the key or the strip: always a fresh frame, the game camera as it is now (the user's rule: never
	// an older frame, e.g. the one frozen when the console opened to type "avalon mark")
	void SkRequestOpen()
	{
		if (!(SkGrabWant && SkGrabForConsole))
			SkWantGrab(SkConsole);
		SkOpenOnGrab = true;
	}
	void SkSetOpen(bool S)
	{
		if (S == SkOpen || (S && SkPix.empty()))
			return;
		SkOpen = S;
		SkDrawing = false;
		if (S)
			GmDragCancel();
		GmCapture(GmPanelShown || SkOpen);
		Message("sketch: %s (%ux%u, %d stroke(s))", S ? "open" : "closed", SkW, SkH, (int)SkStrokes.size());
	}

	// ---- strokes
	void SkPushUndo()
	{
		SkUndo.push_back(SkStrokes);
		if (SkUndo.size() > 200)
			SkUndo.erase(SkUndo.begin());
		SkRedo.clear();
	}
	void SkUndoStep()
	{
		if (SkUndo.empty())
			return;
		SkRedo.push_back(SkStrokes);
		SkStrokes = SkUndo.back();
		SkUndo.pop_back();
	}
	void SkRedoStep()
	{
		if (SkRedo.empty())
			return;
		SkUndo.push_back(SkStrokes);
		SkStrokes = SkRedo.back();
		SkRedo.pop_back();
	}
	// the last crop, in frame pixels (x0 y0 x1 y1); false: none (or too small)
	bool SkCropRect(ImVec4 &R) const
	{
		for (size_t i = SkStrokes.size(); i-- > 0;)
		{
			const SkStroke &S = SkStrokes[i];
			if (S.Tool != SkCrop || S.Pts.size() < 2)
				continue;
			const ImVec2 &a = S.Pts[0], &b = S.Pts[1];
			R.x = floorf((std::max)(0.0f, (std::min)(a.x, b.x)));
			R.y = floorf((std::max)(0.0f, (std::min)(a.y, b.y)));
			R.z = ceilf((std::min)((float)SkW, (std::max)(a.x, b.x)));
			R.w = ceilf((std::min)((float)SkH, (std::max)(a.y, b.y)));
			return R.z - R.x >= 4 && R.w - R.y >= 4;
		}
		return false;
	}
	// one stroke into a draw list: O + frame pixels * Sc (the screen, or 0 and 1 for the saved PNG)
	void SkDrawStroke(ImDrawList *DL, const SkStroke &S, ImVec2 O, float Sc, bool Live)
	{
		if (S.Pts.empty())
			return;
		const float Th = (std::max)(1.0f, S.Size * Sc);
		const ImVec2 A(O.x + S.Pts[0].x * Sc, O.y + S.Pts[0].y * Sc);
		const ImVec2 B = S.Pts.size() > 1 ? ImVec2(O.x + S.Pts[1].x * Sc, O.y + S.Pts[1].y * Sc) : A;
		switch (S.Tool)
		{
		case SkPen:
		case SkHigh:
		{
			const bool Hi = S.Tool == SkHigh;
			const ImU32 C = Hi ? ((S.Col & ~IM_COL32_A_MASK) | (0x66u << IM_COL32_A_SHIFT)) : S.Col;
			const float T = Hi ? Th * 4.0f : Th;
			std::vector<ImVec2> P;
			P.reserve(S.Pts.size());
			for (const ImVec2 &p : S.Pts)
			{
				const ImVec2 q(O.x + p.x * Sc, O.y + p.y * Sc);
				if (P.empty() || q.x != P.back().x || q.y != P.back().y)
					P.push_back(q);
			}
			if (P.size() == 1)
			{
				if (Hi)
					DL->AddRectFilled(ImVec2(P[0].x - T * 0.5f, P[0].y - T * 0.5f), ImVec2(P[0].x + T * 0.5f, P[0].y + T * 0.5f), C);
				else
					DL->AddCircleFilled(P[0], T * 0.5f, C);
				break;
			}
			DL->AddPolyline(P.data(), (int)P.size(), C, ImDrawFlags_None, T);
			if (!Hi && T > 2.5f)
				for (const ImVec2 &p : P)
					DL->AddCircleFilled(p, T * 0.5f, C);   // round joins and caps
			break;
		}
		case SkArrow:
		{
			float dx = B.x - A.x, dy = B.y - A.y;
			const float L = sqrtf(dx * dx + dy * dy);
			if (L < 0.5f)
				break;
			dx /= L;
			dy /= L;
			const float H = (std::min)((std::max)(S.Size * 3.5f, 12.0f) * Sc, L);
			const ImVec2 Base(B.x - dx * H, B.y - dy * H);
			DL->AddLine(A, ImVec2(B.x - dx * H * 0.7f, B.y - dy * H * 0.7f), S.Col, Th);
			DL->AddCircleFilled(A, Th * 0.5f, S.Col);
			DL->AddTriangleFilled(B, ImVec2(Base.x - dy * H * 0.55f, Base.y + dx * H * 0.55f), ImVec2(Base.x + dy * H * 0.55f, Base.y - dx * H * 0.55f), S.Col);
			break;
		}
		case SkRect:
			DL->AddRect(ImVec2((std::min)(A.x, B.x), (std::min)(A.y, B.y)), ImVec2((std::max)(A.x, B.x), (std::max)(A.y, B.y)), S.Col, 0.0f, 0, Th);
			break;
		case SkEllipse:
		{
			const ImVec2 R(fabsf(B.x - A.x) * 0.5f, fabsf(B.y - A.y) * 0.5f);
			if (R.x >= 0.5f && R.y >= 0.5f)
				DL->AddEllipse(ImVec2((A.x + B.x) * 0.5f, (A.y + B.y) * 0.5f), R, S.Col, 0.0f, 0, Th);
			break;
		}
		case SkText:
		{
			if (S.Text.empty())
				break;
			const float Fs = (16.0f + S.Size * 2.5f) * Sc;
			const float o = (std::max)(1.0f, Fs / 14.0f);
			static const float D[8][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 }, { 1, -1 } };
			for (int k = 0; k < 8; k++)              // a dark outline: readable on any part of the frame
				DL->AddText(nullptr, Fs, ImVec2(A.x + D[k][0] * o, A.y + D[k][1] * o), IM_COL32(0, 0, 0, 210), S.Text.c_str());
			DL->AddText(nullptr, Fs, A, S.Col, S.Text.c_str());
			break;
		}
		case SkCrop:
			if (Live)                                // a kept crop shows as the dimmed outside (SkDrawCrop)
				DL->AddRect(ImVec2((std::min)(A.x, B.x), (std::min)(A.y, B.y)), ImVec2((std::max)(A.x, B.x), (std::max)(A.y, B.y)), IM_COL32(255, 255, 255, 230), 0.0f, 0, 1.5f);
			break;
		}
	}
	void SkDrawCrop(ImDrawList *DL)
	{
		ImVec4 R;
		if (!SkCropRect(R))
			return;
		const ImVec2 a(SkO.x + R.x * SkS, SkO.y + R.y * SkS), b(SkO.x + R.z * SkS, SkO.y + R.w * SkS);
		const ImVec2 i0 = SkO, i1(SkO.x + SkW * SkS, SkO.y + SkH * SkS);
		const ImU32 Dim = IM_COL32(0, 0, 0, 150);
		DL->AddRectFilled(i0, ImVec2(i1.x, a.y), Dim);
		DL->AddRectFilled(ImVec2(i0.x, b.y), i1, Dim);
		DL->AddRectFilled(ImVec2(i0.x, a.y), ImVec2(a.x, b.y), Dim);
		DL->AddRectFilled(ImVec2(b.x, a.y), ImVec2(i1.x, b.y), Dim);
		DL->AddRect(a, b, IM_COL32(255, 255, 255, 230), 0.0f, 0, 1.5f);
	}

	// ---- on screen
	// while the console is open (and sketch mode isn't): the strip to click
	void SketchStrip()
	{
		SkStripShown = false;
		if (!SketchOn || !SkConsole || SkOpen)
			return;
		const ImGuiIO &io = ImGui::GetIO();
		const float Sc = (GmPanelScale > 0.5f && GmPanelScale < 4) ? GmPanelScale : 1.0f;
		const std::string T = "Sketch this frame  (" + SkKeyName() + ")";
		const ImVec2 TS = ImGui::CalcTextSize(T.c_str());
		const ImVec2 Sz(TS.x + 28 * Sc, TS.y + 18 * Sc);
		const ImVec2 A(io.DisplaySize.x - Sz.x - 16 * Sc, (float)(int)(io.DisplaySize.y * 0.3f));
		const ImVec2 B(A.x + Sz.x, A.y + Sz.y);
		bool Hot = false;
		POINT M = {};
		if (LiveWnd != nullptr && GetCursorPos(&M) && ScreenToClient(LiveWnd, &M))
			Hot = M.x >= A.x && M.y >= A.y && M.x <= B.x && M.y <= B.y;
		ImDrawList *FG = ImGui::GetForegroundDrawList();
		FG->AddRectFilled(A, B, Hot ? IM_COL32(40, 110, 200, 235) : IM_COL32(18, 22, 28, 220), 6 * Sc);
		FG->AddRect(A, B, SkPal(2), 6 * Sc, 0, 1.5f * Sc);
		FG->AddText(ImVec2(A.x + 14 * Sc, A.y + 9 * Sc), IM_COL32_WHITE, T.c_str());
		SkStrip = ImVec4(A.x, A.y, B.x, B.y);
		SkStripShown = true;
	}
	void SketchUi(IDirect3DDevice9 *Dev)
	{
		const ImGuiIO &io = ImGui::GetIO();
		const float DW = io.DisplaySize.x, DH = io.DisplaySize.y;
		if (DW < 16 || DH < 16 || !SkEnsureTex(Dev))
			return;
		SkS = (std::min)(DW / SkW, DH / SkH);
		SkO = ImVec2((float)(int)((DW - SkW * SkS) * 0.5f), (float)(int)((DH - SkH * SkS) * 0.5f));
		ImDrawList *BG = ImGui::GetBackgroundDrawList();
		BG->AddRectFilled(ImVec2(0, 0), ImVec2(DW, DH), IM_COL32(0, 0, 0, 255));
		BG->AddImage(SkTexRef(), SkO, ImVec2(SkO.x + SkW * SkS, SkO.y + SkH * SkS));
		for (const SkStroke &S : SkStrokes)
			SkDrawStroke(BG, S, SkO, SkS, false);
		SkDrawCrop(BG);
		if (SkDrawing)
			SkDrawStroke(BG, SkCur, SkO, SkS, true);
		SkToolbar();
		SkMouse();
		SkKeys();
	}
	void SkToolbar()
	{
		const ImVec4 Warn(1.0f, 0.65f, 0.25f, 1.0f);
		ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, 10), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0));
		ImGui::SetNextWindowBgAlpha(0.9f);
		if (!ImGui::Begin("Sketch   (Esc closes)", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
		{
			ImGui::End();
			return;
		}
		for (int i = 0; i < SkToolCount; i++)
		{
			if (i)
				ImGui::SameLine();
			const bool Sel = SkTool == i;
			if (Sel)
				ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
			char L[32];
			snprintf(L, sizeof(L), "%d %s", i + 1, SkToolName(i));
			if (ImGui::Button(L))
				SkTool = i;
			if (Sel)
				ImGui::PopStyleColor();
		}
		for (int i = 0; i < 8; i++)
		{
			if (i)
				ImGui::SameLine();
			ImGui::PushID(i);
			const bool Sel = SkColor == i;
			if (Sel)
			{
				ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 3.0f);
				ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 1));
			}
			if (ImGui::ColorButton(SkPalName(i), ImGui::ColorConvertU32ToFloat4(SkPal(i)), ImGuiColorEditFlags_NoAlpha, ImVec2(26, 26)))
				SkColor = i;
			if (Sel)
			{
				ImGui::PopStyleColor();
				ImGui::PopStyleVar();
			}
			ImGui::PopID();
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(130);
		ImGui::SliderFloat("size", &SkSize, 1.0f, 24.0f, "%.0f px");
		ImGui::SetNextItemWidth(300);
		ImGui::InputTextWithHint("##label", "label for the Text tool", SkLabel, sizeof(SkLabel));
		ImGui::SameLine();
		ImGui::BeginDisabled(SkUndo.empty());
		if (ImGui::Button("Erase last"))
			SkUndoStep();
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(SkRedo.empty());
		if (ImGui::Button("Redo"))
			SkRedoStep();
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(SkStrokes.empty());
		if (ImGui::Button("Clear"))
		{
			SkPushUndo();
			SkStrokes.clear();
		}
		ImGui::EndDisabled();
		ImGui::SetNextItemWidth(470);
		ImGui::InputTextWithHint("##note", "note (goes into the .txt and the mark)", SkNote, sizeof(SkNote));
		if (ImGui::Button("Save PNG"))
			SkSaveWant = 1;
		ImGui::SameLine();
		if (ImGui::Button("Save as mark"))
			SkSaveWant = 2;
		ImGui::SameLine();
		if (ImGui::Button("Close"))
			SkSetOpen(false);
		ImGui::SameLine();
		ImVec4 Cr;
		ImGui::TextDisabled("%ux%u %s | %d stroke(s)%s", SkW, SkH, SkPixFull ? "presented frame" : "world (no HUD/console)", (int)SkStrokes.size(),
			SkCropRect(Cr) ? " | cropped" : "");
		if (GmHold == nullptr)
			ImGui::TextColored(Warn, "U2Input not found: the mouse still turns the view");
		if (!SkStatus.empty())
			ImGui::TextWrapped("%s", SkStatus.c_str());
		ImGui::TextDisabled("1-7 tools | Ctrl+Z erase last, Ctrl+Y redo, Ctrl+S save | right click drops a stroke");
		ImGui::End();
	}
	void SkMouse()
	{
		const ImGuiIO &io = ImGui::GetIO();
		const ImVec2 M = io.MousePos;
		ImVec2 P((M.x - SkO.x) / SkS, (M.y - SkO.y) / SkS);
		const bool Over = P.x >= 0 && P.y >= 0 && P.x <= SkW && P.y <= SkH;
		P.x = (std::min)((std::max)(P.x, 0.0f), (float)SkW);
		P.y = (std::min)((std::max)(P.y, 0.0f), (float)SkH);
		if (!SkDrawing && Over && !io.WantCaptureMouse && ImGui::IsMouseClicked(0))
		{
			if (SkTool == SkText)
			{
				if (SkLabel[0] == 0)
					SkStatus = "Text: type the label first (the field under the colours), then click where it goes";
				else
				{
					SkStroke S;
					S.Tool = SkText;
					S.Col = SkPal(SkColor);
					S.Size = SkSize;
					S.Pts.push_back(P);
					S.Text = SkLabel;
					SkPushUndo();
					SkStrokes.push_back(S);
				}
			}
			else
			{
				SkCur = SkStroke();
				SkCur.Tool = SkTool;
				SkCur.Col = SkPal(SkColor);
				SkCur.Size = SkSize;
				SkCur.Pts.push_back(P);
				SkCur.Pts.push_back(P);
				SkDrawing = true;
			}
		}
		if (!SkDrawing)
			return;
		if (ImGui::IsMouseClicked(1))
		{
			SkDrawing = false;                       // right click: drop the stroke being drawn
			return;
		}
		if (ImGui::IsMouseDown(0))
		{
			if (SkCur.Tool == SkPen || SkCur.Tool == SkHigh)
			{
				const ImVec2 &L = SkCur.Pts.back();
				if (fabsf(P.x - L.x) + fabsf(P.y - L.y) >= 2.0f / SkS)
					SkCur.Pts.push_back(P);
			}
			else
				SkCur.Pts[1] = P;
			return;
		}
		SkDrawing = false;
		const bool Shape = SkCur.Tool != SkPen && SkCur.Tool != SkHigh;
		if (Shape && fabsf(SkCur.Pts[1].x - SkCur.Pts[0].x) + fabsf(SkCur.Pts[1].y - SkCur.Pts[0].y) < 3.0f)
			return;                                  // a click, not a shape
		if (!Shape && SkCur.Pts.size() > 2 && SkCur.Pts[0].x == SkCur.Pts[1].x && SkCur.Pts[0].y == SkCur.Pts[1].y)
			SkCur.Pts.erase(SkCur.Pts.begin());      // the press's doubled first point
		SkPushUndo();
		SkStrokes.push_back(SkCur);
		if (SkCur.Tool == SkCrop)
			SkStatus = "crop set: Save writes only this part (Erase last takes it back)";
	}
	void SkKeys()
	{
		const ImGuiIO &io = ImGui::GetIO();
		if (io.WantTextInput)
			return;
		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z))
			SkUndoStep();
		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y))
			SkRedoStep();
		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
			SkSaveWant = 1;
		if (!io.KeyCtrl && !io.KeyAlt)
			for (int i = 0; i < SkToolCount; i++)
				if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + i), false))
					SkTool = i;
	}

	// ---- save
	// in the ImGui frame (before Render): the frame and the strokes at 1:1, for SkSave
	void SkBuildSave()
	{
		if (!SkOpen || SkTex == nullptr || SkW == 0 || SkH == 0)
		{
			SkSaveWant = 0;
			return;
		}
		if (SkSaveList == nullptr)
			SkSaveList = IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData());
		ImDrawList *DL = SkSaveList;
		DL->_ResetForNewFrame();
		DL->PushTexture(ImGui::GetIO().Fonts->TexRef);
		DL->PushClipRect(ImVec2(0, 0), ImVec2((float)SkW, (float)SkH));
		DL->AddImage(SkTexRef(), ImVec2(0, 0), ImVec2((float)SkW, (float)SkH));
		for (const SkStroke &S : SkStrokes)
			SkDrawStroke(DL, S, ImVec2(0, 0), 1.0f, false);
		DL->_PopUnusedDrawCmd();
	}
	// the note for the mark: U+00C0..U+00FF as plain letters, only what a q-line may hold
	static std::string SkMarkNote(const char *In)
	{
		static const char *Latin = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
		std::string Out;
		for (const unsigned char *c = (const unsigned char *)In; *c;)
		{
			char ch = ' ';
			if (*c < 0x80)
				ch = (char)*c++;
			else if ((*c & 0xE0) == 0xC0 && (c[1] & 0xC0) == 0x80)
			{
				const unsigned cp = ((c[0] & 0x1Fu) << 6) | (c[1] & 0x3Fu);
				if (cp >= 0xC0 && cp <= 0xFF)
					ch = Latin[cp - 0xC0];
				c += 2;
			}
			else
				for (c++; (*c & 0xC0) == 0x80; c++)
					;
			if (!isalnum((unsigned char)ch) && strchr(" ._-#:,+!?()/%&", ch) == nullptr)
				ch = ' ';
			if (ch == ' ' && (Out.empty() || Out.back() == ' '))
				continue;
			Out += ch;
		}
		while (!Out.empty() && Out.back() == ' ')
			Out.pop_back();
		if (Out.size() > 180)
			Out.resize(180);
		return Out;
	}
	// after the frame's ImGui render: SkSaveList into a target, read back, the files written
	void SkSave(IDirect3DDevice9 *Dev)
	{
		const int Want = SkSaveWant;
		SkSaveWant = 0;
		if (SkSaveList == nullptr || SkW == 0 || SkH == 0)
			return;
		U2Crash::Where("saving a sketch");
		IDirect3DSurface9 *RT = nullptr, *Sys = nullptr, *OldRT = nullptr, *OldDS = nullptr;
		D3DVIEWPORT9 OldVp = {};
		HRESULT hr = Dev->CreateRenderTarget(SkW, SkH, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &RT, nullptr);
		if (SUCCEEDED(hr))
			hr = Dev->CreateOffscreenPlainSurface(SkW, SkH, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &Sys, nullptr);
		if (SUCCEEDED(hr))
		{
			Dev->GetViewport(&OldVp);
			Dev->GetRenderTarget(0, &OldRT);
			Dev->GetDepthStencilSurface(&OldDS);
			Dev->SetRenderTarget(0, RT);
			Dev->SetDepthStencilSurface(nullptr);
			if (SUCCEEDED(Dev->BeginScene()))
			{
				Dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
				ImDrawData DD;
				DD.Valid = true;
				DD.CmdLists.push_back(SkSaveList);
				DD.CmdListsCount = 1;
				DD.TotalVtxCount = SkSaveList->VtxBuffer.Size;
				DD.TotalIdxCount = SkSaveList->IdxBuffer.Size;
				DD.DisplayPos = ImVec2(0, 0);
				DD.DisplaySize = ImVec2((float)SkW, (float)SkH);
				DD.FramebufferScale = ImVec2(1, 1);
				DD.OwnerViewport = ImGui::GetMainViewport();
				DD.Textures = nullptr;            // this frame's render already brought the font up to date
				ImGui_ImplDX9_RenderDrawData(&DD);
				Dev->EndScene();
			}
			Dev->SetRenderTarget(0, OldRT);
			Dev->SetDepthStencilSurface(OldDS);
			Dev->SetViewport(&OldVp);
			hr = U2PerfReadback(Dev, RT, Sys);
		}
		ImVec4 Cr(0, 0, (float)SkW, (float)SkH);
		const bool Cropped = SkCropRect(Cr);
		if (!Cropped)
			Cr = ImVec4(0, 0, (float)SkW, (float)SkH);
		const UINT X0 = (UINT)Cr.x, Y0 = (UINT)Cr.y, CW = (UINT)(Cr.z - Cr.x), CH = (UINT)(Cr.w - Cr.y);
		std::vector<unsigned char> Rgb;
		D3DLOCKED_RECT L = {};
		if (SUCCEEDED(hr) && SUCCEEDED(hr = Sys->LockRect(&L, nullptr, D3DLOCK_READONLY)))
		{
			Rgb.resize((size_t)CW * CH * 3);
			for (UINT y = 0; y < CH; y++)
			{
				const BYTE *In = static_cast<const BYTE *>(L.pBits) + (size_t)(Y0 + y) * L.Pitch + (size_t)X0 * 4;
				unsigned char *O = &Rgb[(size_t)y * CW * 3];
				for (UINT x = 0; x < CW; x++)
				{
					O[x * 3 + 0] = In[x * 4 + 2];
					O[x * 3 + 1] = In[x * 4 + 1];
					O[x * 3 + 2] = In[x * 4 + 0];
				}
			}
			Sys->UnlockRect();
		}
		if (Sys) Sys->Release();
		if (RT) RT->Release();
		if (OldRT) OldRT->Release();
		if (OldDS) OldDS->Release();
		if (Rgb.empty())
		{
			SkStatus = "couldn't save: the drawing wasn't readable (see U2Shaders.log)";
			Message("sketch: save failed (%08x)", (unsigned)hr);
			return;
		}
		// the name: sketch-YYYYMMDD-HHMMSS(-N)
		const std::string Folder = Dir + "Sketch\\";
		CreateDirectoryA(Folder.c_str(), nullptr);
		SYSTEMTIME T = {};
		GetLocalTime(&T);
		char Stamp[64];
		snprintf(Stamp, sizeof(Stamp), "sketch-%04u%02u%02u-%02u%02u%02u", T.wYear, T.wMonth, T.wDay, T.wHour, T.wMinute, T.wSecond);
		std::string Name = Stamp;
		for (int k = 2; k < 100 && (Name == SkLastName || GetFileAttributesA((Folder + Name + ".png").c_str()) != INVALID_FILE_ATTRIBUTES); k++)
			Name = std::string(Stamp) + "-" + std::to_string(k);
		SkLastName = Name;
		// the mark (U2GM's q-line channel), then the sidecar that says what became of it
		std::string MarkLine = "not asked (Save PNG)";
		const std::string Note = SkMarkNote(SkNote);
		if (Want == 2)
		{
			if (!GmSt.Valid)
				MarkLine = "not sent: no PanelState in U2GM.ini (is U2GM loaded?)";
			else
			{
				const long K = Note.empty() ? GmSend("sketch mark %s", Name.c_str()) : GmSend("sketch mark %s %s", Name.c_str(), Note.c_str());
				MarkLine = K > 0 ? ("sent as gm q line " + std::to_string(K) + ": gm sketch mark " + Name + (Note.empty() ? "" : " " + Note)
					+ " (GMMaster runs: avalon mark " + Note + (Note.empty() ? "" : " ") + "sketch:" + Name + ")") : std::string("not sent (refused, see U2Shaders.log)");
			}
		}
		std::string Txt;
		char Line[512];
		snprintf(Line, sizeof(Line), "sketch    %s.png\r\ntime      %04u-%02u-%02u %02u:%02u:%02u\r\nmap       %s\r\n", Name.c_str(), T.wYear, T.wMonth, T.wDay, T.wHour, T.wMinute, T.wSecond,
			CurMap.empty() ? "?" : CurMap.c_str());
		Txt += Line;
		if (GmSt.Valid && GmSt.CamOk)
			snprintf(Line, sizeof(Line), "eye       %.0f %.0f %.0f   (U2GM PanelState cam=: the player's eye, up to 1 s old)\r\n", GmSt.Cam[0], GmSt.Cam[1], GmSt.Cam[2]);
		else
			snprintf(Line, sizeof(Line), "eye       unknown (no PanelState: U2GM isn't loaded)\r\n");
		Txt += Line;
		if (GmSt.Valid && GmSt.ViewOk)
			snprintf(Line, sizeof(Line), "view      yaw %.0f pitch %.0f   (degrees, PanelState view=)\r\n", GmSt.View[0], GmSt.View[1]);
		else
			snprintf(Line, sizeof(Line), "view      unknown\r\n");
		Txt += Line;
		snprintf(Line, sizeof(Line), "frame     %ux%u, %s\r\n", SkW, SkH, SkPixFull ? "the presented frame" : "the world before the HUD and the console were drawn");
		Txt += Line;
		if (Cropped)
			snprintf(Line, sizeof(Line), "crop      x %u y %u w %u h %u (the PNG is this part)\r\n", X0, Y0, CW, CH);
		else
			snprintf(Line, sizeof(Line), "crop      none\r\n");
		Txt += Line;
		int Count[SkToolCount] = {};
		std::string Labels;
		for (const SkStroke &S : SkStrokes)
		{
			if (S.Tool >= 0 && S.Tool < SkToolCount)
				Count[S.Tool]++;
			if (S.Tool == SkText)
				Labels += (Labels.empty() ? "\"" : " | \"") + S.Text + "\"";
		}
		std::string Kinds;
		for (int i = 0; i < SkToolCount; i++)
			if (Count[i] > 0)
				Kinds += (Kinds.empty() ? "" : ", ") + std::string(SkToolName(i)) + " " + std::to_string(Count[i]);
		Txt += "strokes   " + std::to_string(SkStrokes.size()) + (Kinds.empty() ? std::string() : " (" + Kinds + ")") + "\r\n";
		Txt += "labels    " + (Labels.empty() ? std::string("none") : Labels) + "\r\n";
		Txt += "note      " + std::string(SkNote) + "\r\n";
		Txt += "mark      " + MarkLine + "\r\n";
		const std::string Png = Folder + Name + ".png", TxtPath = Folder + Name + ".txt";
		const int PW = (int)CW, PH = (int)CH;
		std::thread([Rgb, PW, PH, Png, TxtPath, Txt]()
		{
			// a 1080p PNG takes a moment to compress: not on the game's thread
			stbi_write_png(Png.c_str(), PW, PH, 3, Rgb.data(), PW * 3);
			FILE *F = nullptr;
			if (!fopen_s(&F, TxtPath.c_str(), "wb") && F)
			{
				fwrite(Txt.data(), 1, Txt.size(), F);
				fclose(F);
			}
		}).detach();
		SkStatus = "saved System\\Sketch\\" + Name + ".png (+ .txt)";
		if (Want == 2)
			SkStatus += "; mark: " + MarkLine;
		Message("sketch: saved %s.png (%ux%u%s, %d stroke(s)); mark: %s", Name.c_str(), CW, CH, Cropped ? ", cropped" : "", (int)SkStrokes.size(), MarkLine.c_str());
	}

	// ---- shotmask=1: a character mask with each shotp frame ---------------------------------
	// For visual QA (tools/python/visualqa): figure-ground and silhouette checks need to know which
	// pixels are characters. When a shotp is asked for, the frame after it is drawn with a mask
	// pass: each lit, solid, textured draw on screen (characters, their weapons; the level is
	// lightmapped and unlit) is drawn again in flat white into a screen-sized target, tested
	// against the scene's depth so walls in front still hide it. That frame is saved as
	// ShotP#####.bmp as usual and the mask beside it as ShotP#####_mask.bmp (white = character).
	bool ShotMask = false;
	int MaskState = 0;                       // 0 idle, 1 this frame draws the mask
	bool MaskCleared = false;
	IDirect3DSurface9 *MaskRT = nullptr, *MaskOldRT = nullptr;
	IDirect3DPixelShader9 *MaskPS = nullptr, *MaskOldPS = nullptr;
	DWORD MaskOldRS[6];
	bool MaskBroken = false;
	std::string LastShotPath;
	std::set<std::string> MaskSeen;          // shotmask: each kind of draw taken into the mask, logged once
	// streaks=1: blood running down characters, a pass (or more) over a character draw (streaks.hpp)
	bool StreakBegin(IDirect3DDevice9 *Dev, int Pass, bool FixedFunction, bool Textured)
	{
		if (Pass == 0 && (!U2Streaks::Wants(Dev, FixedFunction, Textured) || Offscreen(Dev)))
			return false;
		return U2Streaks::Begin(Dev, Pass);
	}
	bool MaskBegin(IDirect3DDevice9 *Dev, bool FixedFunction, bool Textured, DWORD Hash, UINT Prims)
	{
		if (MaskState != 1 || MaskBroken || !FixedFunction || !Textured || Offscreen(Dev))
			return false;
		DWORD Lit = 0, Blend = 0, Z = 0;
		Dev->GetRenderState(D3DRS_LIGHTING, &Lit);
		Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &Blend);
		Dev->GetRenderState(D3DRS_ZENABLE, &Z);
		if (!Lit || Blend || !Z)
			return false;
		{
			// Advent draws its level's static meshes through vertex declarations (no FVF) and
			// lights them like characters; characters and their weapons come with an FVF
			// (position, normal, texture). Only those go into the mask
			DWORD Fvf = 0, Vb = 0;
			Dev->GetFVF(&Fvf);
			if (Fvf == 0)
				return false;
			Dev->GetRenderState(D3DRS_VERTEXBLEND, &Vb);
			char K[96];
			sprintf_s(K, "%08x fvf %x vb %u", Hash, Fvf, Vb);
			if (MaskSeen.size() < 80 && MaskSeen.insert(K).second)
				Message("shotmask: draw %s, %u triangles", K, Prims);
		}
		if (MaskPS == nullptr)
		{
			static const char Src[] = "float4 main() : COLOR { return float4(1, 1, 1, 1); }";
			ID3DBlob *Code = nullptr, *Err = nullptr;
			if (FAILED(D3DCompile(Src, sizeof(Src) - 1, "mask", nullptr, nullptr, "main", "ps_2_0", 0, 0, &Code, &Err)) || Code == nullptr
				|| FAILED(Dev->CreatePixelShader((const DWORD *)Code->GetBufferPointer(), &MaskPS)))
				MaskBroken = true;
			if (Code) Code->Release();
			if (Err) Err->Release();
			if (MaskBroken)
				return false;
		}
		IDirect3DSurface9 *RT = nullptr;
		if (FAILED(Dev->GetRenderTarget(0, &RT)) || RT == nullptr)
			return false;
		D3DSURFACE_DESC D = {};
		RT->GetDesc(&D);
		if (MaskRT != nullptr)
		{
			D3DSURFACE_DESC M = {};
			MaskRT->GetDesc(&M);
			if (M.Width != D.Width || M.Height != D.Height) { MaskRT->Release(); MaskRT = nullptr; MaskCleared = false; }
		}
		if (MaskRT == nullptr && FAILED(Dev->CreateRenderTarget(D.Width, D.Height, D3DFMT_A8R8G8B8, D.MultiSampleType, D.MultiSampleQuality, FALSE, &MaskRT, nullptr)))
		{
			RT->Release();
			MaskBroken = true;
			Message("shotmask: no %ux%u mask target", D.Width, D.Height);
			return false;
		}
		MaskOldRT = RT;                      // keeps the reference until MaskEnd
		D3DVIEWPORT9 VP = {};
		Dev->GetViewport(&VP);
		Dev->SetRenderTarget(0, MaskRT);
		Dev->SetViewport(&VP);               // SetRenderTarget resets it
		if (!MaskCleared)
		{
			Dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
			MaskCleared = true;
		}
		static const D3DRENDERSTATETYPE RS[6] = { D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_COLORWRITEENABLE, D3DRS_FOGENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRGBWRITEENABLE };
		for (int i = 0; i < 6; i++)
			Dev->GetRenderState(RS[i], &MaskOldRS[i]);
		Dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
		Dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
		Dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
		Dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
		Dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
		Dev->GetPixelShader(&MaskOldPS);
		Dev->SetPixelShader(MaskPS);
		return true;
	}
	void MaskEnd(IDirect3DDevice9 *Dev)
	{
		static const D3DRENDERSTATETYPE RS[6] = { D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_COLORWRITEENABLE, D3DRS_FOGENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRGBWRITEENABLE };
		for (int i = 0; i < 6; i++)
			Dev->SetRenderState(RS[i], MaskOldRS[i]);
		Dev->SetPixelShader(MaskOldPS);
		if (MaskOldPS) { MaskOldPS->Release(); MaskOldPS = nullptr; }
		D3DVIEWPORT9 VP = {};
		Dev->GetViewport(&VP);
		Dev->SetRenderTarget(0, MaskOldRT);
		Dev->SetViewport(&VP);
		if (MaskOldRT) { MaskOldRT->Release(); MaskOldRT = nullptr; }
	}
	// after the frame's shot: the mask beside it
	void MaskSave(IDirect3DDevice9 *Dev)
	{
		MaskState = 0;
		const bool Drawn = MaskCleared;
		MaskCleared = false;
		if (LastShotPath.empty())
			return;
		std::string Path = LastShotPath.substr(0, LastShotPath.size() - 4) + "_mask.bmp";
		if (!Drawn || MaskRT == nullptr)
		{
			// nothing lit on screen: an all-black mask, so the frame still has one
			FILE *F = nullptr;
			if (!fopen_s(&F, Path.c_str(), "wb") && F)
			{
				BITMAPFILEHEADER FH = {}; BITMAPINFOHEADER IH = {};
				FH.bfType = 0x4D42; FH.bfOffBits = sizeof(FH) + sizeof(IH); FH.bfSize = FH.bfOffBits + 4;
				IH.biSize = sizeof(IH); IH.biWidth = 1; IH.biHeight = 1; IH.biPlanes = 1; IH.biBitCount = 24; IH.biSizeImage = 4;
				const BYTE Px[4] = {};
				fwrite(&FH, sizeof(FH), 1, F); fwrite(&IH, sizeof(IH), 1, F); fwrite(Px, 1, 4, F);
				fclose(F);
			}
			return;
		}
		D3DSURFACE_DESC D = {};
		MaskRT->GetDesc(&D);
		IDirect3DSurface9 *Src = MaskRT, *Plain = nullptr, *Sys = nullptr;
		if (D.MultiSampleType != D3DMULTISAMPLE_NONE
			&& SUCCEEDED(Dev->CreateRenderTarget(D.Width, D.Height, D.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &Plain, nullptr))
			&& SUCCEEDED(Dev->StretchRect(MaskRT, nullptr, Plain, nullptr, D3DTEXF_NONE)))
			Src = Plain;
		D3DLOCKED_RECT L = {};
		if (SUCCEEDED(Dev->CreateOffscreenPlainSurface(D.Width, D.Height, D.Format, D3DPOOL_SYSTEMMEM, &Sys, nullptr))
			&& SUCCEEDED(U2PerfReadback(Dev, Src, Sys)) && SUCCEEDED(Sys->LockRect(&L, nullptr, D3DLOCK_READONLY)))
		{
			FILE *F = nullptr;
			if (!fopen_s(&F, Path.c_str(), "wb") && F)
			{
				const DWORD Row = (D.Width * 3 + 3) & ~3u, Size = Row * D.Height;
				BITMAPFILEHEADER FH = {}; BITMAPINFOHEADER IH = {};
				FH.bfType = 0x4D42; FH.bfOffBits = sizeof(FH) + sizeof(IH); FH.bfSize = FH.bfOffBits + Size;
				IH.biSize = sizeof(IH); IH.biWidth = (LONG)D.Width; IH.biHeight = (LONG)D.Height; IH.biPlanes = 1; IH.biBitCount = 24; IH.biSizeImage = Size;
				fwrite(&FH, sizeof(FH), 1, F); fwrite(&IH, sizeof(IH), 1, F);
				std::vector<BYTE> Out(Row, 0);
				for (UINT y = D.Height; y-- > 0;)
				{
					const BYTE *In = static_cast<const BYTE *>(L.pBits) + (size_t)y * L.Pitch;
					for (UINT x = 0; x < D.Width; x++)
						Out[x * 3] = Out[x * 3 + 1] = Out[x * 3 + 2] = In[x * 4 + 1];
					fwrite(Out.data(), 1, Row, F);
				}
				fclose(F);
			}
			Sys->UnlockRect();
		}
		if (Sys) Sys->Release();
		if (Plain) Plain->Release();
	}

	// rtdump=N (testing): the first N times the game leaves a 512x512 render target (a
	// ScriptedTexture, e.g. AdventMod's gib card atlas), what it holds goes to
	// System\RtDump##.bmp, and the clears made into it are logged
	int RtDump = 0, RtDumped = 0;
	bool RtIs512(IDirect3DDevice9 *Dev)
	{
		IDirect3DSurface9 *T = nullptr;
		D3DSURFACE_DESC D = {};
		if (FAILED(Dev->GetRenderTarget(0, &T)) || T == nullptr)
			return false;
		T->GetDesc(&D);
		T->Release();
		return D.Width == 512 && D.Height == 512;
	}
	void RtClear(IDirect3DDevice9 *Dev, DWORD Count, DWORD Flags, D3DCOLOR Color)
	{
		if (RtDumped < RtDump && RtIs512(Dev))
		{
			D3DVIEWPORT9 V = {};
			Dev->GetViewport(&V);
			Message("rtdump: clear %u rects flags %x colour %08x, viewport %u,%u %ux%u (frame %u)", Count, Flags, Color, V.X, V.Y, V.Width, V.Height, Frame);
		}
	}
	void RtLeave(IDirect3DDevice9 *Dev)
	{
		if (RtDumped >= RtDump || !RtIs512(Dev))
			return;
		IDirect3DSurface9 *T = nullptr, *Sys = nullptr;
		Dev->GetRenderTarget(0, &T);
		D3DSURFACE_DESC D = {};
		T->GetDesc(&D);
		D3DLOCKED_RECT L = {};
		HRESULT hr = Dev->CreateOffscreenPlainSurface(D.Width, D.Height, D.Format, D3DPOOL_SYSTEMMEM, &Sys, nullptr);
		if (SUCCEEDED(hr))
			hr = U2PerfReadback(Dev, T, Sys);
		if (SUCCEEDED(hr))
			hr = Sys->LockRect(&L, nullptr, D3DLOCK_READONLY);
		if (SUCCEEDED(hr) && (D.Format == D3DFMT_A8R8G8B8 || D.Format == D3DFMT_X8R8G8B8))
		{
			char Path[MAX_PATH];
			snprintf(Path, sizeof(Path), "%sRtDump%02d.bmp", Dir.c_str(), RtDumped);
			FILE *F = nullptr;
			if (!fopen_s(&F, Path, "wb") && F)
			{
				const DWORD Row = D.Width * 3, Size = Row * D.Height;
				BITMAPFILEHEADER FH = {};
				BITMAPINFOHEADER IH = {};
				FH.bfType = 0x4D42;
				FH.bfOffBits = sizeof(FH) + sizeof(IH);
				FH.bfSize = FH.bfOffBits + Size;
				IH.biSize = sizeof(IH);
				IH.biWidth = (LONG)D.Width;
				IH.biHeight = (LONG)D.Height;
				IH.biPlanes = 1;
				IH.biBitCount = 24;
				IH.biSizeImage = Size;
				fwrite(&FH, sizeof(FH), 1, F);
				fwrite(&IH, sizeof(IH), 1, F);
				std::vector<BYTE> Out(Row, 0);
				double Sum = 0;
				for (UINT y = D.Height; y-- > 0;)
				{
					const BYTE *In = static_cast<const BYTE *>(L.pBits) + (size_t)y * L.Pitch;
					for (UINT x = 0; x < D.Width; x++)
					{
						Out[x * 3] = In[x * 4]; Out[x * 3 + 1] = In[x * 4 + 1]; Out[x * 3 + 2] = In[x * 4 + 2];
						Sum += In[x * 4] + In[x * 4 + 1] + In[x * 4 + 2];
					}
					fwrite(Out.data(), 1, Row, F);
				}
				fclose(F);
				Message("rtdump: %s (format %u, mean brightness %.1f, frame %u)", Path, (unsigned)D.Format, Sum / (3.0 * D.Width * D.Height), Frame);
			}
			Sys->UnlockRect();
		}
		else
			Message("rtdump: couldn't read the target (%08x, format %u)", (unsigned)hr, (unsigned)D.Format);
		if (Sys) Sys->Release();
		T->Release();
		RtDumped++;
	}

	bool Offscreen(IDirect3DDevice9 *Dev)
	{
		IDirect3DSurface9 *T = nullptr, *BB = nullptr;
		D3DSURFACE_DESC D = {}, B = {};
		if (FAILED(Dev->GetRenderTarget(0, &T)) || T == nullptr)
			return false;
		T->GetDesc(&D);
		T->Release();
		if (SUCCEEDED(Dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &BB)) && BB) { BB->GetDesc(&B); BB->Release(); }
		return D.Width != B.Width || D.Height != B.Height;
	}

	bool PcssBegin(IDirect3DDevice9 *Dev, IDirect3DTexture9 *Tex, bool FixedFunction)
	{
		if (!Loaded)
			Load();
		if (!Pcss)
			return false;
		DWORD blend = 0, src = 0, op0 = 0, arg1 = 0, ttf0 = 0, tf = 0;
		Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
		Dev->GetRenderState(D3DRS_SRCBLEND, &src);
		Dev->GetTextureStageState(0, D3DTSS_COLOROP, &op0);
		Dev->GetTextureStageState(0, D3DTSS_COLORARG1, &arg1);
		Dev->GetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, &ttf0);
		Dev->GetRenderState(D3DRS_TEXTUREFACTOR, &tf);
		const float tfc[4] = { ((tf >> 16) & 255) / 255.0f, ((tf >> 8) & 255) / 255.0f, (tf & 255) / 255.0f, (tf >> 24) / 255.0f };

		if (PcssDebug > 4.5 && Offscreen(Dev))
		{
			// every distinct state set drawn into an offscreen target, once
			static std::map<std::string, bool> told;
			DWORD aop = 0, aa1 = 0, cwe = 0, zen = 0, dst = 0, op1 = 0, fvf = 0;
			Dev->GetTextureStageState(0, D3DTSS_ALPHAOP, &aop);
			Dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &aa1);
			Dev->GetTextureStageState(1, D3DTSS_COLOROP, &op1);
			Dev->GetRenderState(D3DRS_COLORWRITEENABLE, &cwe);
			Dev->GetRenderState(D3DRS_ZENABLE, &zen);
			Dev->GetRenderState(D3DRS_DESTBLEND, &dst);
			Dev->GetFVF(&fvf);
			IDirect3DSurface9 *T = nullptr, *Z = nullptr; D3DSURFACE_DESC D = {}, ZD = {};
			if (SUCCEEDED(Dev->GetRenderTarget(0, &T)) && T) { T->GetDesc(&D); T->Release(); }
			const bool HasZ = SUCCEEDED(Dev->GetDepthStencilSurface(&Z)) && Z;
			if (HasZ) { Z->GetDesc(&ZD); Z->Release(); }
			DWORD zfunc = 0, zwrite = 0;
			Dev->GetRenderState(D3DRS_ZFUNC, &zfunc);
			Dev->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
			DWORD tci0 = 0, addu = 0, minf = 0;
			Dev->GetTextureStageState(0, D3DTSS_TEXCOORDINDEX, &tci0);
			Dev->GetSamplerState(0, D3DSAMP_ADDRESSU, &addu);
			Dev->GetSamplerState(0, D3DSAMP_MINFILTER, &minf);
			bool Self = false;
			if (Tex != nullptr)
			{
				IDirect3DSurface9 *S0 = nullptr, *RT = nullptr;
				if (SUCCEEDED(Tex->GetSurfaceLevel(0, &S0)) && S0)
				{
					if (SUCCEEDED(Dev->GetRenderTarget(0, &RT)) && RT) { Self = RT == S0; RT->Release(); }
					S0->Release();
				}
			}
			char k[400];
			sprintf_s(k, "rt %u tex %d%s ff %d blend %u %u/%u cop %u a1 %x aop %u aa1 %x st1 %u cwe %x z %u (func %u write %u, depth %ux%u%s) fvf %x tci0 %x ttf0 %x addr %u min %u tf %08x",
				D.Width, Tex != nullptr, Self ? " (self)" : "", FixedFunction, blend, src, dst, op0, arg1, aop, aa1, op1, cwe, zen, zfunc, zwrite, ZD.Width, ZD.Height, HasZ ? "" : " none", fvf, tci0, ttf0, addu, minf, tf);
			if (!told[k]) { told[k] = true; Message("offscreen draw: %s\n", k); }
		}
		bool map = Tex == nullptr && !blend && op0 == D3DTOP_SELECTARG1 && (arg1 & 0xF) == D3DTA_TFACTOR && Offscreen(Dev);
		if (map)
			PassMap++;
		else if (Tex != nullptr && blend && src == D3DBLEND_SRCALPHA && Offscreen(Dev))
			PassBlur++;
		else if (Tex != nullptr && blend && src == D3DBLEND_DESTCOLOR && (ttf0 & D3DTTFF_PROJECTED))
			PassProj++;
		if (map)
		{
			IDirect3DSurface9 *T = nullptr;
			if (SUCCEEDED(Dev->GetRenderTarget(0, &T)) && T) { MapTarget = T; T->Release(); }
			MapDirty = true;
		}
		else if (Tex != nullptr && blend && src == D3DBLEND_SRCALPHA && MapTarget != nullptr)
		{
			// the engine blurs the silhouette map (A) into a second target (B) with additive
			// textured passes; the projector then samples B. A is a scratch target shared by
			// every shadow, B is kept per shadow (and only redrawn when needed). So take a
			// snapshot of A for this B now, which the projector pass reads instead of B.
			IDirect3DSurface9 *T = nullptr, *S0 = nullptr;
			if (SUCCEEDED(Dev->GetRenderTarget(0, &T)) && T && SUCCEEDED(Tex->GetSurfaceLevel(0, &S0)) && S0)
			{
				if (S0 == MapTarget && T != MapTarget && (MapDirty || CopiedFor != T))
				{
					D3DSURFACE_DESC D = {};
					S0->GetDesc(&D);
					IDirect3DTexture9 *&Snap = BlurSource[T];
					if (Snap != nullptr)
					{
						D3DSURFACE_DESC SD = {};
						Snap->GetLevelDesc(0, &SD);
						if (SD.Width != D.Width || SD.Height != D.Height || SD.Format != D.Format) { Snap->Release(); Snap = nullptr; }
					}
					if (Snap == nullptr && FAILED(Dev->CreateTexture(D.Width, D.Height, 1, D3DUSAGE_RENDERTARGET, D.Format, D3DPOOL_DEFAULT, &Snap, nullptr)))
						Snap = nullptr;
					IDirect3DSurface9 *Dst = nullptr;
					if (Snap != nullptr && SUCCEEDED(Snap->GetSurfaceLevel(0, &Dst)) && Dst)
					{
						const HRESULT SR = Dev->StretchRect(S0, nullptr, Dst, nullptr, D3DTEXF_NONE);
						if (PcssProbe > 0 && SnapProbes < PcssProbe)
						{
							// pcssprobe=N: does the snapshot get the silhouette? (shadowed pixels in A and the copy)
							SnapProbes++;
							int TA = 0, TS = 0;
							const int NA = CountShadowed(Dev, S0, TA), NS = CountShadowed(Dev, Dst, TS);
							Message("pcss snapshot %d: StretchRect hr %08x, A shadowed %d, snapshot shadowed %d (of %d), for target %p\n",
								SnapProbes, (unsigned)SR, NA, NS, TA, T);
						}
						Dst->Release();
					}
					MapDirty = false;
					CopiedFor = T;
				}
			}
			if (S0) S0->Release();
			if (T) T->Release();
			return false;
		}
		bool proj = Tex != nullptr && blend && src == D3DBLEND_DESTCOLOR && (ttf0 & D3DTTFF_PROJECTED);
		if (proj && PcssDebug > 5.5)
		{
			// stages 1 and 2 of the projector draw (the gradient fades), once per distinct setup
			static std::map<std::string, bool> told;
			for (DWORD st = 1; st <= 2; st++)
			{
				DWORD tci = 0, ttf = 0, op = 0, a1 = 0, a2 = 0, aop = 0, aa1 = 0, aa2 = 0, au = 0, av = 0, bc = 0;
				Dev->GetTextureStageState(st, D3DTSS_TEXCOORDINDEX, &tci);
				Dev->GetTextureStageState(st, D3DTSS_TEXTURETRANSFORMFLAGS, &ttf);
				Dev->GetTextureStageState(st, D3DTSS_COLOROP, &op);
				Dev->GetTextureStageState(st, D3DTSS_COLORARG1, &a1);
				Dev->GetTextureStageState(st, D3DTSS_COLORARG2, &a2);
				Dev->GetTextureStageState(st, D3DTSS_ALPHAOP, &aop);
				Dev->GetTextureStageState(st, D3DTSS_ALPHAARG1, &aa1);
				Dev->GetTextureStageState(st, D3DTSS_ALPHAARG2, &aa2);
				Dev->GetSamplerState(st, D3DSAMP_ADDRESSU, &au);
				Dev->GetSamplerState(st, D3DSAMP_ADDRESSV, &av);
				Dev->GetSamplerState(st, D3DSAMP_BORDERCOLOR, &bc);
				D3DMATRIX M = {};
				Dev->GetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + st), &M);
				IDirect3DBaseTexture9 *B = nullptr;
				D3DSURFACE_DESC D = {};
				char cube[200] = "";
				if (SUCCEEDED(Dev->GetTexture(st, &B)) && B)
				{
					if (B->GetType() == D3DRTYPE_TEXTURE) static_cast<IDirect3DTexture9 *>(B)->GetLevelDesc(0, &D);
					else if (B->GetType() == D3DRTYPE_CUBETEXTURE)
					{
						auto *C = static_cast<IDirect3DCubeTexture9 *>(B);
						C->GetLevelDesc(0, &D);
						// alpha range of each face's top level (managed/system textures can be locked)
						int n = sprintf_s(cube, " cube levels %u pool %u usage %x alpha", C->GetLevelCount(), (unsigned)D.Pool, (unsigned)D.Usage);
						for (int f = 0; f < 6 && D.Format == D3DFMT_A8R8G8B8; f++)
						{
							D3DLOCKED_RECT L;
							if (SUCCEEDED(C->LockRect((D3DCUBEMAP_FACES)f, 0, &L, nullptr, D3DLOCK_READONLY)))
							{
								int lo = 255, hi = 0;
								for (UINT y = 0; y < D.Height; y++)
									for (UINT x = 0; x < D.Width; x++)
									{
										int a = ((const BYTE *)L.pBits)[y * L.Pitch + x * 4 + 3];
										lo = a < lo ? a : lo; hi = a > hi ? a : hi;
									}
								C->UnlockRect((D3DCUBEMAP_FACES)f, 0);
								n += sprintf_s(cube + n, sizeof(cube) - n, " %d-%d", lo, hi);
							}
							else
								n += sprintf_s(cube + n, sizeof(cube) - n, " nolock");
						}
					}
					B->Release();
				}
				char k[900];
				sprintf_s(k, "proj stage %u: tex %ux%u fmt %u%s, tci %x ttf %x, cop %u %x %x, aop %u %x %x, addr %u %u border %08x, "
					"m [%.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g]",
					st, D.Width, D.Height, (unsigned)D.Format, cube, tci, ttf, op, a1, a2, aop, aa1, aa2, au, av, bc,
					M._11, M._12, M._13, M._14, M._21, M._22, M._23, M._24, M._31, M._32, M._33, M._34, M._41, M._42, M._43, M._44);
				const char *e = strstr(k, ", m [");
				std::string key(k, e ? e - k : strlen(k));
				if (!told[key] && told.size() < 12) { told[key] = true; Message("%s\n", k); }
			}
		}
		if ((!map && !proj) || !FixedFunction)
			return false;
		IDirect3DPixelShader9 *PS = Compile(Dev, map ? MapRule : ProjRule);
		if (PS == nullptr)
			return false;
		{
			// once per pass kind and size: the formats involved (alpha must survive to the projector)
			static std::map<UINT, bool> told;
			D3DSURFACE_DESC D = {};
			IDirect3DSurface9 *T = nullptr;
			if (map && SUCCEEDED(Dev->GetRenderTarget(0, &T)) && T) { T->GetDesc(&D); T->Release(); }
			if (proj) Tex->GetLevelDesc(0, &D);
			UINT key = (map ? 0x10000 : 0x20000) + D.Width;
			if (!told[key])
			{
				told[key] = true;
				Message("pcss %s %ux%u format %u usage %x\n", map ? "map target" : "projector texture", D.Width, D.Height, (unsigned)D.Format, (unsigned)D.Usage);
			}
		}

		Dev->GetPixelShader(&OldPS);
		Dev->GetPixelShaderConstantF(0, OldConst[0], 8);
		float c[4][4] = {};
		c[0][0] = (GetTickCount() % 3600000) / 1000.0f;
		c[0][1] = PcssDebug;
		memcpy(c[1], tfc, sizeof(tfc));
		memcpy(c[2], PcssParams, sizeof(PcssParams));
		IDirect3DTexture9 *Sharp = nullptr;
		if (proj)
		{
			// the sharp silhouette map this blurred shadow was made from (see the blur passes above)
			IDirect3DSurface9 *S0 = nullptr;
			if (SUCCEEDED(Tex->GetSurfaceLevel(0, &S0)) && S0)
			{
				auto It = BlurSource.find(S0);
				if (It != BlurSource.end())
					Sharp = It->second;
				S0->Release();
			}
			if (Sharp == nullptr)
			{
				static int told = 0;
				if (told++ < 4)
					Message("pcss: projector without a known sharp map, left stock\n");
				return false;
			}
			// stage 2 fades the shadow by the receiver's facing: a cube map looked up by the
			// camera-space normal. pcss_proj.hlsl samples it as a cube (texCUBE); a projector
			// with anything else there is left to the engine
			IDirect3DBaseTexture9 *F2 = nullptr;
			const bool Cube = SUCCEEDED(Dev->GetTexture(2, &F2)) && F2 && F2->GetType() == D3DRTYPE_CUBETEXTURE;
			if (F2) F2->Release();
			// pcss_proj.hlsl doesn't read stage 2 any more, so a projector with a plain texture
			// there (Advent) is taken over too. Leaving it to the engine while the map pass was
			// still replaced projected the map's red height channel onto the floor (2026-10-05).
			if (!Cube)
			{
				static int toldCube = 0;
				if (toldCube++ < 2)
					Message("pcss: projector whose stage 2 isn't a cube map (taken over all the same)\n");
			}
		}
		if (proj && PcssDebug > 2.5)
		{
			// debug: both maps, raw BGRA, once per size
			static std::map<UINT, bool> dumped;
			D3DSURFACE_DESC D = {};
			Tex->GetLevelDesc(0, &D);
			if (!dumped[D.Width] && Frame > 300)
			{
				dumped[D.Width] = true;
				DumpRaw(Dev, Tex, "blurred", D.Width);
				DumpRaw(Dev, Sharp, "sharp", D.Width);
			}
		}
		if (proj)
		{
			// the sharp map on sampler 3
			static const D3DSAMPLERSTATETYPE Samp[5] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER };
			static const DWORD Want[5] = { D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE };
			Dev->GetTexture(3, &OldTex3);
			for (int i = 0; i < 5; i++)
			{
				Dev->GetSamplerState(3, Samp[i], &OldSamp3[i]);
				Dev->SetSamplerState(3, Samp[i], Want[i]);
			}
			Dev->SetTexture(3, Sharp);
		}
		if (proj)
		{
			D3DSURFACE_DESC D = {};
			Tex->GetLevelDesc(0, &D);
			c[3][0] = D.Width ? 1.0f / D.Width : 0;
			c[3][1] = D.Height ? 1.0f / D.Height : 0;
		}
		// c4: camera space -> world height (z), from the inverse of the current view. In the
		// shadow-map pass the view is the light's, in the projector pass the player's: both
		// passes then measure world height, and their difference is how far the shadowing
		// part is above the ground it shadows.
		float zrow[4] = { 0, 0, 0, 0 };
		{
			D3DMATRIX V;
			Dev->GetTransform(D3DTS_VIEW, &V);
			D3DMATRIX Inv = InvView(V);
			zrow[0] = Inv.m[0][2]; zrow[1] = Inv.m[1][2]; zrow[2] = Inv.m[2][2]; zrow[3] = Inv.m[3][2];
			c[0][2] = 1.0f;
		}
		Dev->SetPixelShaderConstantF(0, c[0], 4);
		Dev->SetPixelShaderConstantF(4, zrow, 1);
		if (proj)
			Dev->SetPixelShaderConstantF(5, ShadowTint, 1);   // restored with c0-c7 in End()
		if (!map)
		{
			// camera-space position on TEXCOORD3 for the receiver depth
			static const D3DMATRIX Identity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
			Dev->GetTextureStageState(3, D3DTSS_TEXCOORDINDEX, &OldTCI3);
			Dev->GetTextureStageState(3, D3DTSS_TEXTURETRANSFORMFLAGS, &OldTTF3);
			Dev->GetTransform(D3DTS_TEXTURE3, &OldTexMat3);
			Dev->SetTextureStageState(3, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION | 3);
			Dev->SetTextureStageState(3, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
			Dev->SetTransform(D3DTS_TEXTURE3, &Identity);
		}
		if (map)
		{
			// the silhouette's camera-space position (the light's view) on TEXCOORD2
			static const D3DMATRIX Identity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
			Dev->GetTextureStageState(2, D3DTSS_TEXCOORDINDEX, &OldTCI[2]);
			Dev->GetTextureStageState(2, D3DTSS_TEXTURETRANSFORMFLAGS, &OldTTF[2]);
			Dev->GetTransform(D3DTS_TEXTURE2, &OldTexMat[2]);
			Dev->SetTextureStageState(2, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION | 2);
			Dev->SetTextureStageState(2, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
			Dev->SetTransform(D3DTS_TEXTURE2, &Identity);
			Dev->GetRenderState(D3DRS_COLORWRITEENABLE, &OldCWE);
			Dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
			// stage 3 may carry texgen left over from the terrain (Advent outdoors): off for the map
			Dev->GetTextureStageState(3, D3DTSS_TEXCOORDINDEX, &OldMapTCI3);
			Dev->GetTextureStageState(3, D3DTSS_TEXTURETRANSFORMFLAGS, &OldMapTTF3);
			Dev->SetTextureStageState(3, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU | 3);
			Dev->SetTextureStageState(3, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		}
		if (proj && PcssDebug > 1.5 && PcssDebug < 2.5)
		{
			// raw-value debug view: write the shader output as is (no multiply with the ground)
			Dev->GetRenderState(D3DRS_SRCBLEND, &OldSrc);
			Dev->GetRenderState(D3DRS_DESTBLEND, &OldDst);
			Dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
			Dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
			RawDebug = true;
		}
		HRESULT PSHR = Dev->SetPixelShader(PS);
		if (map && ProbeCount < PcssProbe)
		{
			ProbeCount++;
			IDirect3DSurface9 *T = nullptr;
			if (SUCCEEDED(Dev->GetRenderTarget(0, &T)) && T) { if (ProbeRT) ProbeRT->Release(); ProbeRT = T; }
			DWORD d[4] = {}, fog = 0, ate = 0;
			Dev->GetTextureStageState(2, D3DTSS_TEXCOORDINDEX, &d[0]);
			Dev->GetTextureStageState(2, D3DTSS_TEXTURETRANSFORMFLAGS, &d[1]);
			Dev->GetTextureStageState(3, D3DTSS_TEXCOORDINDEX, &d[2]);
			Dev->GetTextureStageState(3, D3DTSS_TEXTURETRANSFORMFLAGS, &d[3]);
			Dev->GetRenderState(D3DRS_FOGENABLE, &fog);
			Dev->GetRenderState(D3DRS_ALPHATESTENABLE, &ate);
			D3DVIEWPORT9 VP = {};
			Dev->GetViewport(&VP);
			int Total = 0, n = ProbeRT ? CountShadowed(Dev, ProbeRT, Total) : -3;
			Message("pcssprobe %d: map rt %p vp %u,%u %ux%u, A shadowed before %d, SetPixelShader hr %08x, fog %u alphatest %u; "
				"stage2 %x/%x stage3 %x/%x before, during st2 %x/%x st3 %x/%x\n",
				ProbeCount, ProbeRT, VP.X, VP.Y, VP.Width, VP.Height, n, (unsigned)PSHR, fog, ate,
				OldTCI[2], OldTTF[2], OldMapTCI3, OldMapTTF3, d[0], d[1], d[2], d[3]);
			ProbePending = true;
		}
		Mode = map ? 2 : 3;
		return true;
	}

	// surface=: a solid world surface drawn with parallax (world_parallax.hlsl). A pixel shader
	// replaces all the texture stages, so the shader redoes what they did; only the usual setups
	// are taken (stage 0: the texture, alone or times the vertex colour; stage 1: nothing, or a
	// second texture such as the lightmap times the result), as c2 = (vertex colour factor,
	// stage 1 factor; 0 = not used). Anything else is drawn as before and noted once in the log.
	// Stages 0 and 1 keep their own coordinates and transforms (panning, the lightmap's
	// mapping); stage 2's coordinates carry the camera-space position (TEXCOORD2).
	static float ModulateFactor(DWORD Op)
	{
		return Op == D3DTOP_MODULATE ? 1.0f : Op == D3DTOP_MODULATE2X ? 2.0f : Op == D3DTOP_MODULATE4X ? 4.0f : 0.0f;
	}
	static bool ArgsAre(DWORD A1, DWORD A2, DWORD X, DWORD Y)
	{
		return (A1 == X && A2 == Y) || (A1 == Y && A2 == X);   // no modifiers (complement, alpha replicate)
	}
	// The brightness a surface's texture mostly has (its median: taken as the surface itself) and
	// its darkest few percent (the bottom of its gaps), so world_parallax.hlsl needs no tuning per
	// texture. Read once from the top mip: 32-bit textures per pixel, DXT per block (the mean of
	// its two end colours). Other formats: false, and the shader uses its own settings.
	bool TextureLevels(IDirect3DTexture9 *Tex, float &Flat, float &Deep)
	{
		D3DSURFACE_DESC Desc;
		D3DLOCKED_RECT Lock;
		if (FAILED(Tex->GetLevelDesc(0, &Desc)))
			return false;
		const bool Dxt1 = Desc.Format == D3DFMT_DXT1;
		const bool Dxt = Dxt1 || Desc.Format == D3DFMT_DXT2 || Desc.Format == D3DFMT_DXT3 || Desc.Format == D3DFMT_DXT4 || Desc.Format == D3DFMT_DXT5;
		if (!(Desc.Format == D3DFMT_A8R8G8B8 || Desc.Format == D3DFMT_X8R8G8B8 || Dxt) || FAILED(Tex->LockRect(0, &Lock, nullptr, D3DLOCK_READONLY)))
			return false;
		unsigned Hist[256] = {}, Count = 0;
		auto Add = [&](unsigned r, unsigned g, unsigned b) { Hist[(r * 77 + g * 151 + b * 29) >> 8]++; Count++; };
		const BYTE *Bits = static_cast<const BYTE *>(Lock.pBits);
		if (Dxt)
		{
			const UINT Bw = (std::max)(1u, Desc.Width / 4), Bh = (std::max)(1u, Desc.Height / 4), Size = Dxt1 ? 8 : 16;
			for (UINT y = 0; y < Bh; y++)
				for (UINT x = 0; x < Bw; x++)
				{
					const BYTE *B = Bits + y * Lock.Pitch + x * Size + (Dxt1 ? 0 : 8);
					const unsigned c0 = B[0] | (B[1] << 8), c1 = B[2] | (B[3] << 8);
					Add((((c0 >> 11) & 31) + ((c1 >> 11) & 31)) * 255 / 62, (((c0 >> 5) & 63) + ((c1 >> 5) & 63)) * 255 / 126,
						((c0 & 31) + (c1 & 31)) * 255 / 62);
				}
		}
		else
			for (UINT y = 0; y < Desc.Height; y++)
				for (UINT x = 0; x < Desc.Width; x++)
				{
					const BYTE *P = Bits + y * Lock.Pitch + x * 4;
					Add(P[2], P[1], P[0]);
				}
		Tex->UnlockRect(0);
		if (Count == 0)
			return false;
		int DeepAt = -1, FlatAt = -1;
		for (unsigned i = 0, Sum = 0; i < 256; i++)
		{
			Sum += Hist[i];
			if (DeepAt < 0 && Sum * 100 >= Count * 4)
				DeepAt = i;
			if (FlatAt < 0 && Sum * 2 >= Count)
				FlatAt = i;
		}
		// a low-contrast texture gets little depth, not its small differences stretched to full depth
		Flat = (FlatAt + 0.5f) / 255;
		Deep = (std::min)((DeepAt + 0.5f) / 255, Flat - 0.15f);
		return true;
	}

	bool SurfaceBegin(IDirect3DDevice9 *Dev, U2Rule &R, bool FixedFunction)
	{
		DWORD Blending = 0;
		Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &Blending);
		if (Blending || !FixedFunction || Offscreen(Dev))
		{
			if (!R.Refused && !Offscreen(Dev))
				Message("surface %08x: not drawn by this rule (%s)", R.Hash, Blending ? "alpha-blended draw" : "vertex-shader draw");
			R.Refused = true;
			return false;
		}
		DWORD op[3] = {}, a1[3] = {}, a2[3] = {};
		for (DWORD st = 0; st < 3; st++)
		{
			Dev->GetTextureStageState(st, D3DTSS_COLOROP, &op[st]);
			Dev->GetTextureStageState(st, D3DTSS_COLORARG1, &a1[st]);
			Dev->GetTextureStageState(st, D3DTSS_COLORARG2, &a2[st]);
		}
		float Vert = -1, Second = -1, Factor = 0;
		if (op[0] == D3DTOP_SELECTARG1 && a1[0] == D3DTA_TEXTURE)
			Vert = 0;
		else if (ModulateFactor(op[0]) > 0 && (ArgsAre(a1[0], a2[0], D3DTA_TEXTURE, D3DTA_DIFFUSE) || ArgsAre(a1[0], a2[0], D3DTA_TEXTURE, D3DTA_CURRENT)))
			Vert = ModulateFactor(op[0]);    // on stage 0, CURRENT is the vertex colour
		if (op[1] == D3DTOP_DISABLE)
			Second = 0;
		else if (ModulateFactor(op[1]) > 0 && ArgsAre(a1[1], a2[1], D3DTA_TEXTURE, D3DTA_CURRENT) && op[2] == D3DTOP_DISABLE)
			Second = ModulateFactor(op[1]);
		else if (ModulateFactor(op[1]) > 0 && ArgsAre(a1[1], a2[1], D3DTA_TFACTOR, D3DTA_CURRENT) && op[2] == D3DTOP_DISABLE
			&& R.File.find("_tone") != std::string::npos)
		{
			// stage 1 multiplies by the texture factor (a constant colour; Advent's rock meshes):
			// only for shaders that read it (*_tone.hlsl: c1 = the colour, c2.z = 1)
			Second = ModulateFactor(op[1]);
			Factor = 1;
		}
		if (Vert < 0 || Second < 0)
		{
			if (!R.Refused)
				Message("surface %08x: stage setup not supported, drawn as before (st0 op %u %x %x | st1 op %u %x %x | st2 op %u)",
					R.Hash, op[0], a1[0], a2[0], op[1], a1[1], a2[1], op[2]);
			R.Refused = true;
			return false;
		}
		const bool First = !R.Tried;
		IDirect3DPixelShader9 *PS = Compile(Dev, R);
		if (PS == nullptr)
			return false;
		if (First)
		{
			IDirect3DTexture9 *Tex = nullptr;
			if (SUCCEEDED(Dev->GetTexture(0, reinterpret_cast<IDirect3DBaseTexture9 **>(&Tex))) && Tex != nullptr)
			{
				if (TextureLevels(Tex, R.Levels[0], R.Levels[1]))
				{
					R.Levels[3] = 1;
					Message("surface %08x: brightness levels %.2f (surface) %.2f (deepest)", R.Hash, R.Levels[0], R.Levels[1]);
				}
				else
					Message("surface %08x: texture format not read, using the shader's own levels", R.Hash);
				Tex->Release();
			}
		}

		Dev->GetPixelShader(&OldPS);
		Dev->GetPixelShaderConstantF(0, OldConst[0], 8);
		static const D3DMATRIX Identity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
		Dev->GetTextureStageState(2, D3DTSS_TEXCOORDINDEX, &OldTCI[2]);
		Dev->GetTextureStageState(2, D3DTSS_TEXTURETRANSFORMFLAGS, &OldTTF[2]);
		Dev->GetTransform(D3DTS_TEXTURE2, &OldTexMat[2]);
		Dev->SetTextureStageState(2, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION | 2);
		Dev->SetTextureStageState(2, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
		Dev->SetTransform(D3DTS_TEXTURE2, &Identity);

		float Const[4][4] = {};
		Const[0][0] = (GetTickCount() % 3600000) / 1000.0f;
		Const[0][1] = 1;
		Const[0][2] = SceneW ? 1.0f / SceneW : 0;
		Const[0][3] = SceneH ? 1.0f / SceneH : 0;
		Const[2][0] = Vert;
		Const[2][1] = Second;
		Const[2][2] = Factor;
		if (Factor > 0)
		{
			DWORD Tf = 0xffffffff;
			Dev->GetRenderState(D3DRS_TEXTUREFACTOR, &Tf);
			Const[1][0] = ((Tf >> 16) & 255) / 255.0f; Const[1][1] = ((Tf >> 8) & 255) / 255.0f;
			Const[1][2] = (Tf & 255) / 255.0f; Const[1][3] = (Tf >> 24) / 255.0f;
		}
		memcpy(Const[3], R.Levels, sizeof(R.Levels));
		Dev->SetPixelShaderConstantF(0, Const[0], 4);
		Dev->SetPixelShader(PS);
		Mode = 5;
		return true;
	}

	// charlight=1: a solid, fixed-function, lit draw (characters, weapons, pickups: the level
	// itself is lightmapped or vertex-coloured, unlit) gets its lighting per pixel from the same
	// D3D lights, in char_light.hlsl. Taken only when the shader can redo what the stages did:
	//   stage 0: texture x lit colour (x1/2/4), or the lit colour alone (SELECTARG2 DIFFUSE)
	//   stage 1: off; passing the result on (SELECT CURRENT); x a 2D texture (x1/2/4); or
	//            MODULATEALPHA_ADDCOLOR (result + result alpha x texture: a masked shine)
	//   stage 2: off
	// with material colours from the material (not the vertices). Anything else is drawn as
	// before; each setup is logged once, taken or not ("charlight: ...").
	// Constants: c2 (stage 0 factor, light count, stage 1 kind: 0 none, 1 multiply, 2 masked add;
	// stage 1 factor), c3 ambient + emissive, c4 world up in view space, c5.x 1 = stage 0 uses
	// its texture, c8.. 4 per light (up to 4), in view space:
	//   position xyz, type (1 point, 2 spot, 3 directional) | direction xyz, cos(phi / 2)
	//   colour (light x material diffuse) rgb, range | attenuation 0, 1, 2, cos(theta / 2)
	// Stage 1 keeps its own coordinates (TEXCOORD1); stages 2 and 3 carry the camera-space
	// normal and position (TEXCOORD2/3).
	// pbr=: the same for one texture's draws, with char_pbr.hlsl and the rule's material map on
	// sampler 3. The game may draw these with a pixel shader of its own (Advent's characters:
	// ps_1_1 over fixed-function vertices), which this replaces: the stages aren't looked at,
	// the texture counts twice over (Advent's skins are drawn at double brightness).
	DWORD CurPsHash = 0;       // the game's pixel shader for this draw (0 none), set by the device
	float PbrDebug = 0;        // pbrdebug=N: 1 the light alone, 2 normals, 3 roughness, 4 metalness
	IDirect3DBaseTexture9 *OldPbrTex = nullptr;
	DWORD OldPbrSS[5] = {};
	bool PbrBound = false;
	float PbrLayer = 0;        // pbr=: stage 1 multiplies a second texture on, by this factor
	bool CharBegin(IDirect3DDevice9 *Dev, bool FixedFunction, U2Rule *Pbr = nullptr)
	{
		DWORD Blending = 0, Lighting = 0, DiffSrc = 0, AmbSrc = 0, ColorVertex = 0;
		Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &Blending);
		Dev->GetRenderState(D3DRS_LIGHTING, &Lighting);
		if ((Blending && !(Pbr != nullptr && Pbr->PbrBlend)) || !Lighting || !FixedFunction || Offscreen(Dev))
			return false;
		Dev->GetRenderState(D3DRS_COLORVERTEX, &ColorVertex);
		Dev->GetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, &DiffSrc);
		Dev->GetRenderState(D3DRS_AMBIENTMATERIALSOURCE, &AmbSrc);
		DWORD op[3] = {}, a1[2] = {}, a2[2] = {}, Fvf = 0;
		for (DWORD st = 0; st < 3; st++)
			Dev->GetTextureStageState(st, D3DTSS_COLOROP, &op[st]);
		for (DWORD st = 0; st < 2; st++)
		{
			Dev->GetTextureStageState(st, D3DTSS_COLORARG1, &a1[st]);
			Dev->GetTextureStageState(st, D3DTSS_COLORARG2, &a2[st]);
		}
		Dev->GetFVF(&Fvf);
		const bool VertexColours = ColorVertex && (Fvf & D3DFVF_DIFFUSE) && (DiffSrc != D3DMCS_MATERIAL || AmbSrc != D3DMCS_MATERIAL);
		// stage 0: texture x lit colour, or the lit colour alone
		float Factor = ModulateFactor(op[0]), UseTex0 = 1;
		bool Ok0 = Factor > 0 && (ArgsAre(a1[0], a2[0], D3DTA_TEXTURE, D3DTA_DIFFUSE) || ArgsAre(a1[0], a2[0], D3DTA_TEXTURE, D3DTA_CURRENT));
		if ((op[0] == D3DTOP_SELECTARG2 && (a2[0] == D3DTA_DIFFUSE || a2[0] == D3DTA_CURRENT))
			|| (op[0] == D3DTOP_SELECTARG1 && (a1[0] == D3DTA_DIFFUSE || a1[0] == D3DTA_CURRENT)))
		{
			Ok0 = true; Factor = 1; UseTex0 = 0;
		}
		// stage 1: nothing, pass on, x texture, or + alpha x texture
		float Kind1 = -1, Factor1 = 1;
		if (op[1] == D3DTOP_DISABLE
			|| (op[1] == D3DTOP_SELECTARG1 && a1[1] == D3DTA_CURRENT) || (op[1] == D3DTOP_SELECTARG2 && a2[1] == D3DTA_CURRENT))
			Kind1 = 0;
		else if (ModulateFactor(op[1]) > 0 && ArgsAre(a1[1], a2[1], D3DTA_TEXTURE, D3DTA_CURRENT))
		{
			Kind1 = 1; Factor1 = ModulateFactor(op[1]);
		}
		else if (op[1] == D3DTOP_MODULATEALPHA_ADDCOLOR && a1[1] == D3DTA_CURRENT && a2[1] == D3DTA_TEXTURE)
			Kind1 = 2;
		IDirect3DBaseTexture9 *T1 = nullptr;
		if (Kind1 > 0)
		{
			Dev->GetTexture(1, &T1);
			if (T1 == nullptr || T1->GetType() != D3DRTYPE_TEXTURE)
				Kind1 = -1;                   // no texture, or a cube map (reflections): not handled
			if (T1) T1->Release();
		}
		bool Ok = Ok0 && Kind1 >= 0 && (Kind1 == 0 && op[1] == D3DTOP_DISABLE ? true : op[2] == D3DTOP_DISABLE) && !VertexColours;
		if (Pbr != nullptr)
		{
			if (!Pbr->Refused)
				Message("pbr %08x: drawn with game ps %08x, stages %u %x %x | %u %x %x | %u, vertex colours as material %d",
					(unsigned)Pbr->Hash, (unsigned)CurPsHash, op[0], a1[0], a2[0], op[1], a1[1], a2[1], op[2], (int)VertexColours);
			Pbr->Refused = true;              // (told once)
			Ok = true;
			if (!Ok0) { Factor = 2; UseTex0 = 1; }
			// a second texture multiplied on (AdventMod's blood-stained skins: the skin
			// times a blood texture) is kept; anything else on stage 1 isn't
			PbrLayer = Kind1 == 1 && CurPsHash == 0 ? Factor1 : 0;
			Kind1 = 0;
		}
		char Key[200];
		sprintf_s(Key, "st0 op %u %x %x | st1 op %u %x %x | st2 op %u | vertex colours as material %d",
			op[0], a1[0], a2[0], op[1], a1[1], a2[1], op[2], (int)VertexColours);
		if (Pbr == nullptr && !CharRefused[Key])
			Message(Ok ? "charlight: taken (%s)" : "charlight: setup not supported, drawn as before (%s)", Key);
		if (Pbr == nullptr)
			CharRefused[Key] = true;
		if (!Ok)
			return false;
		IDirect3DPixelShader9 *PS = Compile(Dev, Pbr != nullptr ? *Pbr : CharRule);
		if (PS == nullptr)
			return false;
		if (Pbr != nullptr && !Pbr->MapTried)
		{
			Pbr->MapTried = true;
			Pbr->Map = LoadDDS(Dev, Pbr->MapFile);
			Message("pbr %08x: map %s %s", (unsigned)Pbr->Hash, Pbr->MapFile.c_str(), Pbr->Map ? "loaded" : "missing: flat, half rough, not metal");
		}

		float C[32][4] = {};
		const int MaxLights = 4;
		int LightsOn = 0;
		D3DMATERIAL9 M = {};
		Dev->GetMaterial(&M);
		D3DMATRIX V;
		Dev->GetTransform(D3DTS_VIEW, &V);
		auto Point = [&](const D3DVECTOR &p, float *out) {
			for (int c = 0; c < 3; c++)
				out[c] = p.x * V.m[0][c] + p.y * V.m[1][c] + p.z * V.m[2][c] + V.m[3][c];
		};
		auto Dir = [&](const D3DVECTOR &d, float *out) {
			for (int c = 0; c < 3; c++)
				out[c] = d.x * V.m[0][c] + d.y * V.m[1][c] + d.z * V.m[2][c];
			const float l = sqrtf(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
			if (l > 0) for (int c = 0; c < 3; c++) out[c] /= l;
		};
		DWORD Amb = 0;
		Dev->GetRenderState(D3DRS_AMBIENT, &Amb);
		float AmbR = ((Amb >> 16) & 0xFF) / 255.0f, AmbG = ((Amb >> 8) & 0xFF) / 255.0f, AmbB = (Amb & 0xFF) / 255.0f;
		int Count = 0;
		for (DWORD i = 0; i < 8; i++)
		{
			BOOL On = FALSE;
			D3DLIGHT9 L = {};
			if (FAILED(Dev->GetLightEnable(i, &On)) || !On || FAILED(Dev->GetLight(i, &L)))
				continue;
			AmbR += L.Ambient.r; AmbG += L.Ambient.g; AmbB += L.Ambient.b;   // lights' ambient adds up, as in D3D
			LightsOn++;
			if (Count == MaxLights)
				continue;                  // more than 4: their ambient still counts, their light not
			float *P = C[8 + Count * 4];
			Point(L.Position, P);
			P[3] = (float)L.Type;
			Dir(L.Direction, C[9 + Count * 4]);
			C[9 + Count * 4][3] = cosf(L.Phi * 0.5f);
			C[10 + Count * 4][0] = L.Diffuse.r * M.Diffuse.r;
			C[10 + Count * 4][1] = L.Diffuse.g * M.Diffuse.g;
			C[10 + Count * 4][2] = L.Diffuse.b * M.Diffuse.b;
			C[10 + Count * 4][3] = L.Type == D3DLIGHT_DIRECTIONAL ? 1e30f : L.Range;
			C[11 + Count * 4][0] = L.Attenuation0;
			C[11 + Count * 4][1] = L.Attenuation1;
			C[11 + Count * 4][2] = L.Attenuation2;
			C[11 + Count * 4][3] = cosf(L.Theta * 0.5f);
			Count++;
		}
		C[0][0] = (GetTickCount() % 3600000) / 1000.0f;
		C[0][1] = 1;
		C[2][0] = Factor;
		C[2][1] = (float)Count;
		C[2][2] = Kind1;
		C[2][3] = Factor1;
		C[5][0] = UseTex0;
		C[3][0] = AmbR * M.Ambient.r + M.Emissive.r;
		C[3][1] = AmbG * M.Ambient.g + M.Emissive.g;
		C[3][2] = AmbB * M.Ambient.b + M.Emissive.b;
		const D3DVECTOR Up = { 0, 0, 1 };      // Unreal's up (Z) in the world space the game draws in
		Dir(Up, C[4]);

		static const D3DMATRIX Identity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
		// pbr=: stages 4 and 5 carry them, so the game's own coordinates for stages 1-3 stay
		// (Advent's skin shader reads three more textures with them)
		CharGen = Pbr != nullptr ? 4 : 2;
		for (DWORD s = CharGen; s <= CharGen + 1; s++)
		{
			Dev->GetTextureStageState(s, D3DTSS_TEXCOORDINDEX, &OldCharTCI[s]);
			Dev->GetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, &OldCharTTF[s]);
			Dev->GetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), &OldCharTexMat[s]);
			Dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, (s == CharGen ? D3DTSS_TCI_CAMERASPACENORMAL : D3DTSS_TCI_CAMERASPACEPOSITION) | s);
			Dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
			Dev->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), &Identity);
		}
		if (Pbr != nullptr)
		{
			static const D3DSAMPLERSTATETYPE SS[5] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER };
			static const DWORD Want[5] = { D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_LINEAR };
			Dev->GetTexture(4, &OldPbrTex);
			for (int i = 0; i < 5; i++)
			{
				Dev->GetSamplerState(4, SS[i], &OldPbrSS[i]);
				Dev->SetSamplerState(4, SS[i], Want[i]);
			}
			Dev->SetTexture(4, Pbr->Map);
			C[5][1] = Pbr->Map != nullptr ? 1.0f : 0.0f;
			C[2][2] = Pbr->Levels[0];
			C[2][3] = Pbr->Levels[1];
			C[5][3] = PbrDebug;
			// Advent's skin shader (e55c6e08): a second lit layer (t1, masked by t2 . c1) and
			// unlit parts (t3 . c2), all times c0. Its constants go along in c1, c6, c7.
			if (PbrLayer > 0)
			{
				C[5][2] = 2;
				C[1][3] = PbrLayer;
			}
			if (CurPsHash == 0xe55c6e08)
			{
				float Game[3][4] = {};
				Dev->GetPixelShaderConstantF(0, Game[0], 3);
				memcpy(C[1], Game[0], sizeof(Game[0]));
				memcpy(C[6], Game[1], sizeof(Game[1]));
				memcpy(C[7], Game[2], sizeof(Game[2]));
				C[5][2] = 1;
			}
			PbrBound = true;
		}
		Dev->GetPixelShader(&OldPS);
		Dev->GetPixelShaderConstantF(0, OldCharConst[0], 32);
		Dev->SetPixelShaderConstantF(0, C[0], 32);
		if (Pbr != nullptr && !Pbr->Solid)
		{
			Pbr->Solid = true;                // (told once)
			Message("pbr %08x: %d lights on (%d used), ambient %.2f %.2f %.2f, texture factor %.0f", (unsigned)Pbr->Hash, LightsOn, Count, C[3][0], C[3][1], C[3][2], Factor);
		}
		Dev->SetPixelShader(PS);
		if (Pbr != nullptr && Pbr->PbrBlend)
			SkinMaskBegin(Dev);
		Mode = 6;
		return true;
	}

	void End(IDirect3DDevice9 *Dev)
	{
		if (Mode == 9)
		{
			SoftEnd(Dev);
			return;
		}
		if (Mode == 8)
		{
			Dev->SetPixelShader(OldPS);
			Dev->SetPixelShaderConstantF(0, OldConst[0], 8);
			if (OldPS != nullptr) { OldPS->Release(); OldPS = nullptr; }
			Mode = 0;
			return;
		}
		if (Mode == 7)
		{
			Dev->SetPixelShader(OldPS);
			Dev->SetPixelShaderConstantF(0, OldTerrConst[0], 10);
			if (TerrDetailBound)
			{
				static const D3DSAMPLERSTATETYPE SS[6] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_MAXANISOTROPY };
				Dev->SetTexture(6, OldTerrTex6);
				if (OldTerrTex6) { OldTerrTex6->Release(); OldTerrTex6 = nullptr; }
				for (int i = 0; i < 6; i++)
					Dev->SetSamplerState(6, SS[i], OldTerrSamp6[i]);
				TerrDetailBound = false;
			}
			Dev->SetTextureStageState(5, D3DTSS_TEXCOORDINDEX, OldTerrTCI5);
			Dev->SetTextureStageState(5, D3DTSS_TEXTURETRANSFORMFLAGS, OldTerrTTF5);
			Dev->SetTransform(D3DTS_TEXTURE5, &OldTerrTexMat5);
			if (OldPS != nullptr) { OldPS->Release(); OldPS = nullptr; }
			Mode = 0;
			return;
		}
		if (Mode == 6)
		{
			if (PbrBound)
			{
				static const D3DSAMPLERSTATETYPE SS[5] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER };
				Dev->SetTexture(4, OldPbrTex);
				for (int i = 0; i < 5; i++)
					Dev->SetSamplerState(4, SS[i], OldPbrSS[i]);
				if (OldPbrTex) { OldPbrTex->Release(); OldPbrTex = nullptr; }
				PbrBound = false;
			}
			SkinMaskEnd(Dev);
			Dev->SetPixelShader(OldPS);
			Dev->SetPixelShaderConstantF(0, OldCharConst[0], 32);
			for (DWORD s = CharGen; s <= CharGen + 1; s++)
			{
				Dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, OldCharTCI[s]);
				Dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, OldCharTTF[s]);
				Dev->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), &OldCharTexMat[s]);
			}
			if (OldPS != nullptr) { OldPS->Release(); OldPS = nullptr; }
			Mode = 0;
			return;
		}
		if (Mode == 5)
		{
			Dev->SetPixelShader(OldPS);
			Dev->SetPixelShaderConstantF(0, OldConst[0], 8);
			Dev->SetTextureStageState(2, D3DTSS_TEXCOORDINDEX, OldTCI[2]);
			Dev->SetTextureStageState(2, D3DTSS_TEXTURETRANSFORMFLAGS, OldTTF[2]);
			Dev->SetTransform(D3DTS_TEXTURE2, &OldTexMat[2]);
			if (OldPS != nullptr) { OldPS->Release(); OldPS = nullptr; }
			Mode = 0;
			return;
		}
		if (Mode == 4)
		{
			Dev->SetRenderState(D3DRS_COLORWRITEENABLE, OldCWE);
			Mode = 0;
			return;
		}
		if (Mode == 2 || Mode == 3)
		{
			Dev->SetPixelShader(OldPS);
			Dev->SetPixelShaderConstantF(0, OldConst[0], 8);
			if (RawDebug)
			{
				Dev->SetRenderState(D3DRS_SRCBLEND, OldSrc);
				Dev->SetRenderState(D3DRS_DESTBLEND, OldDst);
				RawDebug = false;
			}
			if (Mode == 3)
			{
				static const D3DSAMPLERSTATETYPE Samp[5] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER };
				Dev->SetTexture(3, OldTex3);
				for (int i = 0; i < 5; i++)
					Dev->SetSamplerState(3, Samp[i], OldSamp3[i]);
				if (OldTex3 != nullptr) { OldTex3->Release(); OldTex3 = nullptr; }
				Dev->SetTextureStageState(3, D3DTSS_TEXCOORDINDEX, OldTCI3);
				Dev->SetTextureStageState(3, D3DTSS_TEXTURETRANSFORMFLAGS, OldTTF3);
				Dev->SetTransform(D3DTS_TEXTURE3, &OldTexMat3);
			}
			if (Mode == 2)
			{
				Dev->SetTextureStageState(2, D3DTSS_TEXCOORDINDEX, OldTCI[2]);
				Dev->SetTextureStageState(2, D3DTSS_TEXTURETRANSFORMFLAGS, OldTTF[2]);
				Dev->SetTransform(D3DTS_TEXTURE2, &OldTexMat[2]);
				Dev->SetRenderState(D3DRS_COLORWRITEENABLE, OldCWE);
				Dev->SetTextureStageState(3, D3DTSS_TEXCOORDINDEX, OldMapTCI3);
				Dev->SetTextureStageState(3, D3DTSS_TEXTURETRANSFORMFLAGS, OldMapTTF3);
			}
			if (OldPS != nullptr) { OldPS->Release(); OldPS = nullptr; }
			Mode = 0;
			ProbeAfter(Dev);
			return;
		}
		Mode = 0;
		static const D3DSAMPLERSTATETYPE Samp[5] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER };
		Dev->SetPixelShader(OldPS);
		Dev->SetRenderState(D3DRS_ALPHABLENDENABLE, OldBlend);
		Dev->SetTexture(1, OldTex1);
		Dev->SetPixelShaderConstantF(0, OldConst[0], 8);
		for (int i = 0; i < 5; i++)
			Dev->SetSamplerState(1, Samp[i], OldSamp1[i]);
		if (WasFixedFunction)
			Dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, OldTTF[0]);
		if (WasFixedFunction)
			for (DWORD s = 1; s <= 2; s++)
			{
				Dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, OldTCI[s]);
				Dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, OldTTF[s]);
				Dev->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + s), &OldTexMat[s]);
			}
		if (OldPS != nullptr) { OldPS->Release(); OldPS = nullptr; }
		if (OldTex1 != nullptr) { OldTex1->Release(); OldTex1 = nullptr; }
	}

	// Draws whose stage 0 texture can't be read (render targets such as the dynamic shadow
	// maps) can't be picked by hash: list their render states instead, to find a signature.
	std::map<std::string, unsigned> DrawKinds;
	void LogUnreadableDraw(IDirect3DDevice9 *Dev, IDirect3DTexture9 *Tex)
	{
		D3DSURFACE_DESC Desc = {};
		Tex->GetLevelDesc(0, &Desc);
		DWORD rs[6] = {}, ts[10] = {};
		Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &rs[0]);
		Dev->GetRenderState(D3DRS_SRCBLEND, &rs[1]);
		Dev->GetRenderState(D3DRS_DESTBLEND, &rs[2]);
		Dev->GetRenderState(D3DRS_ZWRITEENABLE, &rs[3]);
		Dev->GetRenderState(D3DRS_ALPHATESTENABLE, &rs[4]);
		Dev->GetTextureStageState(0, D3DTSS_TEXCOORDINDEX, &ts[0]);
		Dev->GetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, &ts[1]);
		Dev->GetTextureStageState(0, D3DTSS_COLOROP, &ts[2]);
		Dev->GetTextureStageState(1, D3DTSS_COLOROP, &ts[3]);
		Dev->GetTextureStageState(1, D3DTSS_TEXCOORDINDEX, &ts[4]);
		Dev->GetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, &ts[5]);
		Dev->GetTextureStageState(2, D3DTSS_COLOROP, &ts[6]);
		IDirect3DBaseTexture9 *t1 = nullptr;
		Dev->GetTexture(1, &t1);
		D3DSURFACE_DESC D1 = {};
		if (t1 != nullptr && t1->GetType() == D3DRTYPE_TEXTURE)
			static_cast<IDirect3DTexture9 *>(t1)->GetLevelDesc(0, &D1);
		if (t1 != nullptr) t1->Release();
		char key[400];
		sprintf_s(key, "tex0 %ux%u fmt %u usage %x | blend %u src %u dst %u zwrite %u atest %u | "
			"st0 tci %x ttf %x op %u | st1 op %u tci %x ttf %x tex %ux%u | st2 op %u",
			Desc.Width, Desc.Height, (unsigned)Desc.Format, (unsigned)Desc.Usage, rs[0], rs[1], rs[2], rs[3], rs[4],
			ts[0], ts[1], ts[2], ts[3], ts[4], ts[5], D1.Width, D1.Height, ts[6]);
		DrawKinds[key]++;

		// projector draws: the full stage setup, and one copy of each shadow-map size
		if (rs[1] == D3DBLEND_DESTCOLOR && (ts[1] & D3DTTFF_PROJECTED))
		{
			char k2[400];
			DWORD a[3][6] = {};
			for (DWORD st = 0; st < 3; st++)
			{
				Dev->GetTextureStageState(st, D3DTSS_COLORARG1, &a[st][0]);
				Dev->GetTextureStageState(st, D3DTSS_COLORARG2, &a[st][1]);
				Dev->GetTextureStageState(st, D3DTSS_ALPHAOP, &a[st][2]);
				Dev->GetTextureStageState(st, D3DTSS_ALPHAARG1, &a[st][3]);
				Dev->GetTextureStageState(st, D3DTSS_ALPHAARG2, &a[st][4]);
				Dev->GetTextureStageState(st, D3DTSS_COLOROP, &a[st][5]);
			}
			DWORD tf = 0;
			Dev->GetRenderState(D3DRS_TEXTUREFACTOR, &tf);
			sprintf_s(k2, "PROJECTOR stages (cop a1 a2 / aop a1 a2): 0: %u %x %x / %u %x %x | 1: %u %x %x / %u %x %x | 2: %u %x %x / %u %x %x | tfactor %08x",
				a[0][5], a[0][0], a[0][1], a[0][2], a[0][3], a[0][4], a[1][5], a[1][0], a[1][1], a[1][2], a[1][3], a[1][4],
				a[2][5], a[2][0], a[2][1], a[2][2], a[2][3], a[2][4], tf);
			DrawKinds[k2]++;
			static std::map<UINT, bool> dumped;
			if (!dumped[Desc.Width] && Frame > 200)
			{
				dumped[Desc.Width] = true;
				IDirect3DSurface9 *RT = nullptr, *Sys = nullptr;
				if (SUCCEEDED(Tex->GetSurfaceLevel(0, &RT)) &&
					SUCCEEDED(Dev->CreateOffscreenPlainSurface(Desc.Width, Desc.Height, Desc.Format, D3DPOOL_SYSTEMMEM, &Sys, nullptr)) &&
					SUCCEEDED(U2PerfReadback(Dev, RT, Sys)))
				{
					D3DLOCKED_RECT L;
					if (SUCCEEDED(Sys->LockRect(&L, nullptr, D3DLOCK_READONLY)))
					{
						D3DSURFACE_DESC D2 = Desc;
						D2.Format = D3DFMT_A8R8G8B8;
						SaveDDS(0x5400 + Desc.Width, D2, (const BYTE *)L.pBits, L.Pitch * Desc.Height, L.Pitch);
						Sys->UnlockRect();
					}
				}
				if (Sys) Sys->Release();
				if (RT) RT->Release();
			}
		}
	}

	// ---- post-processing (post=1) ----------------------------------------------------------
	// Bloom, sharpening and colour grading on the finished 3D frame, before the HUD is drawn
	// over it: run just before the first 2D draw (pre-transformed vertices or an orthographic
	// projection) into the back buffer of a frame that drew 3D, or at Present if no 2D came.
	// Frames with no 3D (menus) are left alone. Every device state it touches is saved and put
	// back by hand (PostSave), stream 0 and the vertex declaration included: on the real game
	// (dgVoodoo) a state block did not bring those back, so the HUD draw that followed drew our
	// fullscreen quad with the HUD's texture. The quad comes from our own vertex buffer.
	bool Post = false;
	float PostSplit = 0;                                 // postsplit=1: right half untouched
	float PostBloom[4] = { 0.75f, 0.5f, 0, 1 };          // threshold, intensity, colorblind type (0 off), its strength
	float PostGrade[4] = { 1.05f, 1.05f, 1.0f, 0.25f };  // saturation, contrast, exposure, vignette
	float PostBalance[4] = { 1, 1, 1, 0.25f };           // colour balance r g b, sharpen
	float PostFx[4] = { 0, 0, 0, 0 };                    // postfx=a b c d: free per-game knobs (c4 in post_final.hlsl)
	// psreplace=: game pixel shaders drawn with ours (Advent's terrain). Constants for them:
	// c0 seconds, c1-c3 inverse view rows (camera -> world), c4 terrainfx=, c5 terrainfog=,
	// c6 terrainfog2=; TEXCOORD5 = camera-space position (stage 5 texgen)
	std::map<DWORD, U2Rule> PsReplace;
	bool PsLog = false;
	float TerrainFx[4] = { 0, 0, 0, 0 };
	float TerrainFog[4] = { 0, 0, 0, 0 };
	float TerrainFog2[4] = { 0, 0, 0, 0 };
	float TerrainFx2[4] = { 0, 0, 0, 0 };                // c7 terrainfx2=: relief, relief scale, slope rock, strata
	float TerrainTone[4] = { 0, 0, 0, 0 };               // c8 terraintone=: ground lift, desaturate, rock brightness, 0
	// terraindetail=FILE (a DDS in U2Shaders, make_terrain_detail.py) on s6 for the psreplace shaders,
	// c9 terraindetailfx= strength, near repeat, far repeat (world units), fade distance (0 strength = off)
	float TerrainDetail[4] = { 0, 40, 170, 900 };
	std::string TerrainDetailFile;
	IDirect3DTexture9 *TerrainDetailTex = nullptr;
	bool TerrainDetailTried = false, TerrDetailBound = false;
	IDirect3DBaseTexture9 *OldTerrTex6 = nullptr;
	DWORD OldTerrSamp6[6] = {};
	float OldTerrConst[10][4] = {};
	DWORD OldTerrTCI5 = 0, OldTerrTTF5 = 0;
	D3DMATRIX OldTerrTexMat5 = {};

	bool PsLogCode = false;
	void LogGamePS(DWORD Hash, DWORD Tokens, DWORD Version, const void *Code9 = nullptr, size_t Bytes9 = 0)
	{
		if (!Loaded)
			Load();
		static std::map<DWORD, bool> told;
		if (!told[Hash] && (PsLog || PsReplace.count(Hash)))
		{
			told[Hash] = true;
			Message("game ps %08x: %u tokens, version %x%s", (unsigned)Hash, (unsigned)Tokens, (unsigned)Version, PsReplace.count(Hash) ? " (replaced)" : "");
			// pslog=2: what it does (the D3D9 form of the game's D3D8 shader)
			ID3DBlob *Text = nullptr;
			if (PsLogCode && Code9 != nullptr && SUCCEEDED(D3DDisassemble(Code9, Bytes9, 0, nullptr, &Text)) && Text)
			{
				Message("%s", (const char *)Text->GetBufferPointer());
				Text->Release();
			}
		}
	}

	bool PsReplaceBegin(IDirect3DDevice9 *Dev, DWORD Hash)
	{
		if (!Loaded)
			Load();
		auto It = PsReplace.find(Hash);
		if (It == PsReplace.end())
			return false;
		IDirect3DPixelShader9 *PS = Compile(Dev, It->second);
		if (PS == nullptr)
			return false;
		Dev->GetPixelShader(&OldPS);
		Dev->GetPixelShaderConstantF(0, OldTerrConst[0], 10);
		Dev->GetTextureStageState(5, D3DTSS_TEXCOORDINDEX, &OldTerrTCI5);
		Dev->GetTextureStageState(5, D3DTSS_TEXTURETRANSFORMFLAGS, &OldTerrTTF5);
		Dev->GetTransform(D3DTS_TEXTURE5, &OldTerrTexMat5);
		static const D3DMATRIX Identity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
		Dev->SetTextureStageState(5, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION | 5);
		Dev->SetTextureStageState(5, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
		Dev->SetTransform(D3DTS_TEXTURE5, &Identity);
		float c[10][4] = {};
		c[0][0] = (GetTickCount() % 100000) / 1000.0f;
		D3DMATRIX V;
		Dev->GetTransform(D3DTS_VIEW, &V);
		const D3DMATRIX Inv = InvView(V);   // row vectors: world = camera * Inv
		for (int r = 0; r < 3; r++)
		{
			c[1 + r][0] = Inv.m[0][r]; c[1 + r][1] = Inv.m[1][r]; c[1 + r][2] = Inv.m[2][r]; c[1 + r][3] = Inv.m[3][r];
		}
		memcpy(c[4], TerrainFx, sizeof(TerrainFx));
		memcpy(c[5], TerrainFog, sizeof(TerrainFog));
		memcpy(c[6], TerrainFog2, sizeof(TerrainFog2));
		memcpy(c[7], TerrainFx2, sizeof(TerrainFx2));
		memcpy(c[8], TerrainTone, sizeof(TerrainTone));
		// the close-up detail texture on s6
		TerrDetailBound = false;
		if (TerrainDetail[0] > 0 && !TerrainDetailFile.empty())
		{
			if (!TerrainDetailTried)
			{
				TerrainDetailTried = true;
				TerrainDetailTex = LoadDDS(Dev, TerrainDetailFile);
				Message("terrain: detail %s %s", TerrainDetailFile.c_str(), TerrainDetailTex ? "loaded" : "missing: no close-up detail");
			}
			if (TerrainDetailTex != nullptr)
			{
				static const D3DSAMPLERSTATETYPE SS[6] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_MAXANISOTROPY };
				static const DWORD Val[6] = { D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTEXF_LINEAR, D3DTEXF_ANISOTROPIC, D3DTEXF_LINEAR, 8 };
				Dev->GetTexture(6, &OldTerrTex6);
				for (int i = 0; i < 6; i++)
				{
					Dev->GetSamplerState(6, SS[i], &OldTerrSamp6[i]);
					Dev->SetSamplerState(6, SS[i], Val[i]);
				}
				Dev->SetTexture(6, TerrainDetailTex);
				TerrDetailBound = true;
				memcpy(c[9], TerrainDetail, sizeof(TerrainDetail));
			}
		}
		Dev->SetPixelShaderConstantF(0, c[0], 10);
		Dev->SetPixelShader(PS);
		Mode = 7;
		return true;
	}
	// lut=FILE (in U2Shaders\, .dds 32-bit or uncompressed .bmp): a colour-grading LUT on s2 for
	// post_final.hlsl, unwrapped 3D: N slices of NxN side by side (256x16 or 1024x32), slice = blue,
	// x in a slice = red, y = green (row 0 = top = green 0). c5 = (N, 1/width, 1/height, 1 if loaded).
	std::string LutFile;
	IDirect3DTexture9 *LutTex = nullptr;
	bool LutTried = false;
	U2Rule PostBright, PostBlur, PostFinal, PostDown, PostUp;
	// bloomchain (on unless bloomchain=0): the bright parts at half size, shrunk level by level to
	// about 1/64 with post_down.hlsl and built back up with post_up.hlsl (Jimenez, SIGGRAPH 2014),
	// for tight glow and wide haze at once. bloomscatter= mixes in the wider levels (0..1).
	// 16-bit float targets where the card has them. If its shaders or targets are missing, the
	// old quarter-size blur runs instead.
	bool BloomChain = true;
	float BloomScatter = 0.7f;
	static const int ChainMax = 7;
	IDirect3DTexture9 *ChainDown[ChainMax] = {}, *ChainUp[ChainMax] = {};
	UINT ChainW[ChainMax] = {}, ChainH[ChainMax] = {};
	int ChainLevels = 0;

	void ReleaseChain()
	{
		for (int k = 0; k < ChainMax; k++)
		{
			if (ChainDown[k]) { ChainDown[k]->Release(); ChainDown[k] = nullptr; }
			if (ChainUp[k]) { ChainUp[k]->Release(); ChainUp[k] = nullptr; }
		}
		ChainLevels = 0;
	}

	// the chain's targets for this frame size (made once, again when the size changes)
	bool MakeChain(IDirect3DDevice9 *Dev)
	{
		const UINT W = (std::max)(SceneW / 2, 1u), H = (std::max)(SceneH / 2, 1u);
		if (ChainLevels > 0 && ChainW[0] == W && ChainH[0] == H)
			return true;
		ReleaseChain();
		int Levels = 0;
		while (Levels < ChainMax && (std::min)(W >> Levels, H >> Levels) >= 4)
			Levels++;
		if (Levels < 2)
			return false;
		static const D3DFORMAT Formats[2] = { D3DFMT_A16B16G16R16F, D3DFMT_A8R8G8B8 };
		for (D3DFORMAT Fmt : Formats)
		{
			bool Ok = true;
			for (int k = 0; k < Levels && Ok; k++)
			{
				ChainW[k] = W >> k;
				ChainH[k] = H >> k;
				Ok = SUCCEEDED(Dev->CreateTexture(ChainW[k], ChainH[k], 1, D3DUSAGE_RENDERTARGET, Fmt, D3DPOOL_DEFAULT, &ChainDown[k], nullptr));
				if (Ok && k < Levels - 1)
					Ok = SUCCEEDED(Dev->CreateTexture(ChainW[k], ChainH[k], 1, D3DUSAGE_RENDERTARGET, Fmt, D3DPOOL_DEFAULT, &ChainUp[k], nullptr));
			}
			if (Ok)
			{
				ChainLevels = Levels;
				Message("post: bloom chain, %d levels from %ux%u down to %ux%u (%s)", Levels, W, H, ChainW[Levels - 1], ChainH[Levels - 1],
					Fmt == D3DFMT_A16B16G16R16F ? "16-bit float" : "8-bit");
				return true;
			}
			ReleaseChain();
		}
		Message("post: bloom chain targets couldn't be made, using the quarter-size blur");
		return false;
	}

	// the chain: bright parts (post_bright) -> down levels -> back up; returns the half-size result
	IDirect3DTexture9 *RunChain(IDirect3DDevice9 *Dev, IDirect3DPixelShader9 *Bright, const float c[6][4])
	{
		IDirect3DPixelShader9 *Down = Compile(Dev, PostDown), *Up = Compile(Dev, PostUp);
		if (Down == nullptr || Up == nullptr || !MakeChain(Dev))
			return nullptr;
		const int L = ChainLevels;
		Target(Dev, ChainDown[0]);
		Dev->SetTexture(0, SceneTex);
		Dev->SetTexture(1, nullptr);
		Dev->SetPixelShader(Bright);
		Dev->SetPixelShaderConstantF(0, c[0], 4);
		Quad(Dev, ChainW[0], ChainH[0]);
		Dev->SetPixelShader(Down);
		for (int k = 1; k < L; k++)
		{
			const float t[4] = { 1.0f / ChainW[k - 1], 1.0f / ChainH[k - 1], 0, 0 };
			Target(Dev, ChainDown[k]);
			Dev->SetTexture(0, ChainDown[k - 1]);
			Dev->SetPixelShaderConstantF(0, t, 1);
			Quad(Dev, ChainW[k], ChainH[k]);
		}
		Dev->SetPixelShader(Up);
		for (int k = L - 2; k >= 0; k--)
		{
			const float t[2][4] = { { 1.0f / ChainW[k + 1], 1.0f / ChainH[k + 1], 0, 0 }, { BloomScatter, 0, 0, 0 } };
			Target(Dev, ChainUp[k]);
			Dev->SetTexture(0, k == L - 2 ? ChainDown[L - 1] : ChainUp[k + 1]);
			Dev->SetTexture(1, ChainDown[k]);
			Dev->SetPixelShaderConstantF(0, t[0], 2);
			Quad(Dev, ChainW[k], ChainH[k]);
		}
		Dev->SetTexture(1, nullptr);
		return ChainUp[0];
	}
	IDirect3DTexture9 *BloomA = nullptr, *BloomB = nullptr;
	UINT BloomW = 0, BloomH = 0;
	IDirect3DVertexBuffer9 *PostQuadVB = nullptr;
	IDirect3DDevice9 *LastDev = nullptr;
	bool Saw3D = false, PostDone = false;
	bool PostHudZ0 = false;    // posthud=z0: only orthographic draws without depth testing start the HUD (Advent:
	                           // a fullscreen z-tested ortho draw comes right after the sky, before the level)
	int PostTrace = 0;                                   // posttrace=N: log the draw order of N frames
	std::string PostTraceLine, PostTraceLast;
	int PostTraceRun = 0;

	// strings=1 (strings.hpp), before every draw while strings live: a perspective world draw on the
	// screen gives the view (and an opaque one the fog); the first HUD draw after them (2D, no depth
	// test: posthud=z0's rule, Advent has z-tested 2D draws in its scene) draws the strings
	void StringsCheck(IDirect3DDevice9 *Dev)
	{
		DWORD fvf = 0, z = 0;
		Dev->GetFVF(&fvf);
		IDirect3DVertexShader9 *VS = nullptr;
		Dev->GetVertexShader(&VS);
		const bool programmable = VS != nullptr;
		if (VS) VS->Release();
		D3DMATRIX P = {};
		Dev->GetTransform(D3DTS_PROJECTION, &P);
		Dev->GetRenderState(D3DRS_ZENABLE, &z);
		const bool rhw = (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
		const bool ortho = !programmable && !rhw && P._34 == 0.0f && P._44 == 1.0f;
		if (!programmable && !rhw && P._34 == 1.0f && P._44 == 0.0f)
		{
			if (z && !Offscreen(Dev))
			{
				DWORD blend = 0;
				Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
				U2Strings::Capture(Dev, !blend);
			}
			return;
		}
		if ((rhw || ortho) && !z && U2Strings::Saw3D && !Offscreen(Dev))
			U2Strings::Draw(Dev);
	}

	void PostCheck(IDirect3DDevice9 *Dev)
	{
		if (!Post || PostDone)
			return;
		LastDev = Dev;
		if (Offscreen(Dev))
			return;
		DWORD fvf = 0;
		Dev->GetFVF(&fvf);
		IDirect3DVertexShader9 *VS = nullptr;
		Dev->GetVertexShader(&VS);
		const bool programmable = VS != nullptr;
		if (VS) VS->Release();
		D3DMATRIX P = {};
		Dev->GetTransform(D3DTS_PROJECTION, &P);
		const bool rhw = (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
		const bool ortho = !programmable && !rhw && P._34 == 0.0f && P._44 == 1.0f;
		if (NeedDepth() && !rhw && !ortho)
			SegDraws++;                       // the size of this part of the frame (OnClear)
		if (NeedDepth() && DepthDirty)
			DepthLazySwap(Dev);
		if (U2Msaa::Wanted() != 0 && !programmable && !rhw && P._34 == 1.0f && P._44 == 0.0f)
		{
			{
				IDirect3DSurface9 *RT = nullptr, *BB = nullptr;
				Dev->GetRenderTarget(0, &RT);
				Dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &BB);
				if (RT == BB) MsaaCount[0]++;
				else if (MsaaBound >= 0 && RT == MsaaProxies[MsaaBound].Multi) MsaaCount[1]++;
				else MsaaCount[2]++;
				if (RT) RT->Release();
				if (BB) BB->Release();
				if (Frame != MsaaCountFrame)
				{
					MsaaCountFrame = Frame;
					static int Told = 0;
					if ((Frame % 300) == 0 && Told++ < 4)
						Message("msaa probe: perspective draws so far: back buffer %u, stand-in %u, other %u", MsaaCount[0], MsaaCount[1], MsaaCount[2]);
				}
			}
			static int Told = 0;
			if (Told < 6 && (Frame % 120) == 60)
			{
				Told++;
				IDirect3DSurface9 *RT = nullptr, *DS = nullptr, *BB = nullptr;
				D3DSURFACE_DESC R = {}, D = {}, B = {};
				DWORD MS = 0;
				Dev->GetRenderState(D3DRS_MULTISAMPLEANTIALIAS, &MS);
				if (SUCCEEDED(Dev->GetRenderTarget(0, &RT)) && RT) { RT->GetDesc(&R); RT->Release(); }
				if (SUCCEEDED(Dev->GetDepthStencilSurface(&DS)) && DS) { DS->GetDesc(&D); DS->Release(); }
				if (SUCCEEDED(Dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &BB)) && BB) { BB->GetDesc(&B); BB->Release(); }
				Message("msaa probe: world draw into %p %ux%u sampled %u (back buffer %p sampled %u), depth %p sampled %u, MULTISAMPLEANTIALIAS %u",
					(void *)RT, R.Width, R.Height, (unsigned)R.MultiSampleType, (void *)BB, (unsigned)B.MultiSampleType, (void *)DS, (unsigned)D.MultiSampleType, (unsigned)MS);
			}
		}
		if (NeedDepth() && !programmable && !rhw && P._34 == 1.0f && P._44 == 0.0f)
		{
			SceneProj = P;                    // gi.hlsl / ssao.hlsl rebuild positions from depth with it
			Dev->GetTransform(D3DTS_VIEW, &SceneView);
			SceneProjOk = true;
			if (GiSlotDirty)
				GiGatherLights(Dev);
		}
		// the HUD draws without depth testing; orthographic draws that still test depth are part
		// of the scene (in third person one came mid-frame: the post ran too early and covered
		// the rest of the frame with a half-drawn copy)
		DWORD zenable = 0, zwrite = 0, zfunc = 0, blend = 0;
		Dev->GetRenderState(D3DRS_ZENABLE, &zenable);
		Dev->GetRenderState(D3DRS_ZWRITEENABLE, &zwrite);
		Dev->GetRenderState(D3DRS_ZFUNC, &zfunc);
		Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
		if (PostTrace > 0)
		{
			char item[64];
			if (!rhw && !ortho)
				snprintf(item, sizeof(item), zenable ? "3 " : "h(w%lu b%lu p%.2f) ", zwrite, blend, P._43);
			else
			{
				D3DVIEWPORT9 vp = {};
				Dev->GetViewport(&vp);
				snprintf(item, sizeof(item), "%s(z%lu w%lu f%lu b%lu vp%lux%lu) ", rhw ? "R" : "O", zenable, zwrite, zfunc, blend, vp.Width, vp.Height);
			}
			// runs of the same item are written once with a count ("3x120"), so a whole frame fits
			if (item == PostTraceLast) PostTraceRun++;
			else
			{
				if (PostTraceRun > 1 && PostTraceLine.size() < 3500) { char n[16]; snprintf(n, sizeof(n), "x%d ", PostTraceRun); PostTraceLine += n; }
				PostTraceRun = 1;
				PostTraceLast = item;
				if (PostTraceLine.size() < 3500) PostTraceLine += item;
			}
		}
		if (!rhw && !ortho)
		{
			Saw3D = true;
			if (U2Lens::WantsView() && !programmable && P._34 == 1.0f && P._44 == 0.0f)
				U2Lens::SeeView(Dev, P);      // lens=1: a blood spot waits to be judged against the camera
			return;
		}
		if (PostTrace > 0)
			return;                           // tracing: just record the frame
		if (PostHudZ0 && zenable)
			return;                           // posthud=z0: z-tested 2D draws are still the scene
		if (!Saw3D)
			return;
		static int told = 0;
		if (told++ < 6)
		{
			DWORD z = 0, blend = 0;
			Dev->GetRenderState(D3DRS_ZENABLE, &z);
			Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
			Message("post: applied before a 2D draw (fvf %x, pre-transformed %d, orthographic %d, z %u, blend %u)",
				(unsigned)fvf, (int)rhw, (int)ortho, z, blend);
		}
		RunPost(Dev);
	}

	// postdebug=1: the device as post-processing finds and uses it, for the first 3 frames
	bool PostDebug = false;
	void PostLog(IDirect3DDevice9 *Dev, const char *Where)
	{
		IDirect3DSurface9 *RT = nullptr, *DS = nullptr, *BB = nullptr;
		D3DSURFACE_DESC R = {}, D = {};
		Dev->GetRenderTarget(0, &RT);
		Dev->GetDepthStencilSurface(&DS);
		Dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &BB);
		if (RT) RT->GetDesc(&R);
		if (DS) DS->GetDesc(&D);
		DWORD Z = 0, Blend = 0;
		Dev->GetRenderState(D3DRS_ZENABLE, &Z);
		Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &Blend);
		IDirect3DPixelShader9 *PS = nullptr;
		Dev->GetPixelShader(&PS);
		char Tex[200] = {};
		for (DWORD s = 0; s < 4; s++)
		{
			IDirect3DBaseTexture9 *T = nullptr;
			Dev->GetTexture(s, &T);
			char One[48];
			sprintf_s(One, " s%u=%p%s", s, (void *)T, T == SceneTex && T ? "(copy)" : T == BloomA && T ? "(bloom)" : "");
			strcat_s(Tex, One);
			if (T) T->Release();
		}
		Message("postdebug %s: target %p %ux%u format %u%s msaa %u, depth %p %ux%u, z %u blend %u, pixel shader %p,%s",
			Where, (void *)RT, R.Width, R.Height, (unsigned)R.Format, RT == BB ? " (the back buffer)" : " (NOT the back buffer)",
			(unsigned)R.MultiSampleType, (void *)DS, D.Width, D.Height, Z, Blend, (void *)PS, Tex);
		if (RT) RT->Release();
		if (DS) DS->Release();
		if (BB) BB->Release();
		if (PS) PS->Release();
	}

	struct U2QuadVertex { float x, y, z, rhw, u, v; };
	// a W x H fullscreen quad from PostQuadVB (pre-transformed; FVF and stream set by RunPost)
	void Quad(IDirect3DDevice9 *Dev, UINT W, UINT H)
	{
		// half-pixel offset: Direct3D 9 pixel centres sit on integer coordinates
		const float w = W - 0.5f, h = H - 0.5f;
		const U2QuadVertex q[4] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { w, -0.5f, 0, 1, 1, 0 }, { -0.5f, h, 0, 1, 0, 1 }, { w, h, 0, 1, 1, 1 } };
		void *p = nullptr;
		if (FAILED(PostQuadVB->Lock(0, sizeof(q), &p, D3DLOCK_DISCARD)) || p == nullptr)
			return;
		memcpy(p, q, sizeof(q));
		PostQuadVB->Unlock();
		Dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
	}

	// Everything RunPost changes, saved before and put back after, one by one.
	static constexpr D3DRENDERSTATETYPE PostRS[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE,
		D3DRS_FOGENABLE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_LIGHTING, D3DRS_SRGBWRITEENABLE, D3DRS_CLIPPLANEENABLE,
		D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE };
	static constexpr D3DSAMPLERSTATETYPE PostSS[] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER,
		D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE };
	struct PostSave
	{
		IDirect3DSurface9 *RT = nullptr, *DS = nullptr;
		D3DVIEWPORT9 Viewport = {};
		IDirect3DVertexBuffer9 *Stream = nullptr;
		UINT StreamOffset = 0, StreamStride = 0;
		IDirect3DIndexBuffer9 *Indices = nullptr;
		IDirect3DVertexDeclaration9 *Decl = nullptr;
		DWORD Fvf = 0;
		IDirect3DVertexShader9 *VS = nullptr;
		IDirect3DPixelShader9 *PS = nullptr;
		IDirect3DBaseTexture9 *Tex[3] = {};
		DWORD RS[sizeof(PostRS) / sizeof(PostRS[0])] = {};
		DWORD SS[3][sizeof(PostSS) / sizeof(PostSS[0])] = {};
		DWORD TCI[3] = {}, TTF[3] = {};
		float Const[12][4] = {};
		float VConst[4] = {};

		void Save(IDirect3DDevice9 *Dev)
		{
			Dev->GetRenderTarget(0, &RT);
			Dev->GetDepthStencilSurface(&DS);
			Dev->GetViewport(&Viewport);
			Dev->GetStreamSource(0, &Stream, &StreamOffset, &StreamStride);
			Dev->GetIndices(&Indices);
			Dev->GetVertexDeclaration(&Decl);
			Dev->GetFVF(&Fvf);
			Dev->GetVertexShader(&VS);
			Dev->GetPixelShader(&PS);
			for (DWORD s = 0; s < 3; s++)
			{
				Dev->GetTexture(s, &Tex[s]);
				for (size_t i = 0; i < sizeof(PostSS) / sizeof(PostSS[0]); i++)
					Dev->GetSamplerState(s, PostSS[i], &SS[s][i]);
				Dev->GetTextureStageState(s, D3DTSS_TEXCOORDINDEX, &TCI[s]);
				Dev->GetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, &TTF[s]);
			}
			for (size_t i = 0; i < sizeof(PostRS) / sizeof(PostRS[0]); i++)
				Dev->GetRenderState(PostRS[i], &RS[i]);
			Dev->GetPixelShaderConstantF(0, Const[0], 12);
			Dev->GetVertexShaderConstantF(0, VConst, 1);
		}

		void Restore(IDirect3DDevice9 *Dev)
		{
			Dev->SetRenderTarget(0, RT);
			Dev->SetDepthStencilSurface(DS);
			Dev->SetViewport(&Viewport);       // after SetRenderTarget, which resets it
			Dev->SetStreamSource(0, Stream, StreamOffset, StreamStride);
			Dev->SetIndices(Indices);
			if (Decl != nullptr)
				Dev->SetVertexDeclaration(Decl);
			else
				Dev->SetFVF(Fvf);
			Dev->SetVertexShader(VS);
			Dev->SetPixelShader(PS);
			for (DWORD s = 0; s < 3; s++)
			{
				Dev->SetTexture(s, Tex[s]);
				for (size_t i = 0; i < sizeof(PostSS) / sizeof(PostSS[0]); i++)
					Dev->SetSamplerState(s, PostSS[i], SS[s][i]);
				Dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, TCI[s]);
				Dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, TTF[s]);
			}
			for (size_t i = 0; i < sizeof(PostRS) / sizeof(PostRS[0]); i++)
				Dev->SetRenderState(PostRS[i], RS[i]);
			Dev->SetPixelShaderConstantF(0, Const[0], 12);
			Dev->SetVertexShaderConstantF(0, VConst, 1);
		}

		~PostSave()
		{
			IUnknown *All[] = { RT, DS, Stream, Indices, Decl, VS, PS, Tex[0], Tex[1], Tex[2] };
			for (IUnknown *U : All)
				if (U != nullptr)
					U->Release();
		}
	};

	static void Target(IDirect3DDevice9 *Dev, IDirect3DTexture9 *T)
	{
		IDirect3DSurface9 *S = nullptr;
		if (SUCCEEDED(T->GetSurfaceLevel(0, &S)) && S)
		{
			Dev->SetRenderTarget(0, S);
			S->Release();
		}
	}

	// lens=1 (lens.hpp): blood drops on the lens over the finished frame, right after the final
	// pass (the frame copied again, the drops blended over it); the state is RunPost's
	void RunLens(IDirect3DDevice9 *Dev)
	{
		U2Lens::Resolve();
		if (!U2Lens::Active() || U2Lens::Live == 0)
			return;
		if (!CopyScene(Dev, true) || SceneTex == nullptr)
			return;
		if (U2Lens::Begin(Dev, SceneTex, SceneW, SceneH))
		{
			Dev->SetTexture(2, nullptr);
			Quad(Dev, SceneW, SceneH);
			U2Lens::End(Dev);
		}
	}

	void RunPost(IDirect3DDevice9 *Dev)
	{
		U2PerfScope PerfPost(U2PerfC().Post);   // perf: "post passes" (bloom, gi, ssao, sss, smaa, lens)
		PostDone = true;
		IDirect3DPixelShader9 *Bright = Compile(Dev, PostBright), *Blur = Compile(Dev, PostBlur), *Final = Compile(Dev, PostFinal);
		if (Bright == nullptr || Blur == nullptr || Final == nullptr)
		{
			Message("post: a shader failed to compile, post-processing off");
			Post = false;
			return;
		}
		if (PostQuadVB == nullptr && FAILED(Dev->CreateVertexBuffer(4 * sizeof(U2QuadVertex), D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC,
				D3DFVF_XYZRHW | D3DFVF_TEX1, D3DPOOL_DEFAULT, &PostQuadVB, nullptr)))
		{
			PostQuadVB = nullptr;
			Message("post: could not make the quad's vertex buffer, post-processing off");
			Post = false;
			return;
		}
		PostSave Saved;
		Saved.Save(Dev);
		IDirect3DSurface9 *OldRT = Saved.RT;
		static int Debugged = 0;
		const bool Debug = PostDebug && Debugged++ < 3;
		if (Debug)
			PostLog(Dev, "at the hook");

		if (!CopyScene(Dev, true))
		{
			// a failed copy leaves old memory in SceneTex (on real cards often another texture):
			// drawing it would cover the frame, so this frame stays unprocessed
			static int Told = 0;
			if (Told++ < 3)
				Message("post: the scene copy failed (%08lx), frame left unprocessed", (unsigned long)LastCopyHr);
			Saved.Restore(Dev);
			return;
		}
		if (Debug)
			Message("postdebug: scene copy ok (%ux%u format %u), copy texture %p", SceneW, SceneH, (unsigned)SceneFmt, (void *)SceneTex);
		const UINT W = (std::max)(SceneW / 4, 1u), H = (std::max)(SceneH / 4, 1u);
		if (BloomA != nullptr && (BloomW != W || BloomH != H))
		{
			BloomA->Release(); BloomA = nullptr;
			if (BloomB) { BloomB->Release(); BloomB = nullptr; }
		}
		if (BloomA == nullptr)
		{
			BloomW = W; BloomH = H;
			if (FAILED(Dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &BloomA, nullptr)))
				BloomA = nullptr;
			if (FAILED(Dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &BloomB, nullptr)))
				BloomB = nullptr;
		}
		if (SceneTex != nullptr && BloomA != nullptr && BloomB != nullptr && OldRT != nullptr)
		{
			for (D3DRENDERSTATETYPE R : PostRS)
				if (R != D3DRS_CULLMODE && R != D3DRS_COLORWRITEENABLE)
					Dev->SetRenderState(R, FALSE);
			Dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
			Dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
			Dev->SetDepthStencilSurface(nullptr);
			Dev->SetVertexShader(nullptr);
			Dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
			Dev->SetStreamSource(0, PostQuadVB, 0, sizeof(U2QuadVertex));
			for (DWORD s = 0; s < 2; s++)
			{
				Dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
				Dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
				Dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
				Dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
				Dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
				Dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
				Dev->SetTextureStageState(s, D3DTSS_TEXCOORDINDEX, 0);
				Dev->SetTextureStageState(s, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
			}
			Dev->SetTexture(1, nullptr);
			RunGi(Dev, Saved.DS);
			RunSsao(Dev, Saved.DS);
			RunSss(Dev, Saved.DS);
			RunAtmos(Dev, Saved.DS);
			IDirect3DTexture9 *AAFrame = RunSmaa(Dev);

			float c[6][4] = {};
			c[0][0] = 1.0f / SceneW; c[0][1] = 1.0f / SceneH; c[0][2] = PostSplit;
			c[0][3] = (GetTickCount() % 100000) / 1000.0f;     // seconds, wrapping every 100 s (grain)
			memcpy(c[4], PostFx, sizeof(PostFx));
			memcpy(c[1], PostBloom, sizeof(PostBloom));
			memcpy(c[2], PostGrade, sizeof(PostGrade));
			memcpy(c[3], PostBalance, sizeof(PostBalance));
			IDirect3DTexture9 *BloomTex = BloomChain ? RunChain(Dev, Bright, c) : nullptr;
			if (BloomTex == nullptr)
			{
			// 1: the bright parts, quarter size
			Target(Dev, BloomA);
			Dev->SetTexture(0, SceneTex);
			Dev->SetPixelShader(Bright);
			Dev->SetPixelShaderConstantF(0, c[0], 4);
			if (Debug)
				PostLog(Dev, "before the bright pass (stage 0 should be the copy)");
			Quad(Dev, W, H);

			// 2: blurred, across then down, twice
			Dev->SetPixelShader(Blur);
			for (int round = 0; round < 2; round++)
			{
				const float across[4] = { 1.0f / W, 0, 0, 0 }, down[4] = { 0, 1.0f / H, 0, 0 };
				Target(Dev, BloomB);
				Dev->SetTexture(0, BloomA);
				Dev->SetPixelShaderConstantF(0, across, 1);
				Quad(Dev, W, H);
				Target(Dev, BloomA);
				Dev->SetTexture(0, BloomB);
				Dev->SetPixelShaderConstantF(0, down, 1);
				Quad(Dev, W, H);
			}
			BloomTex = BloomA;
			}

			// 3: the finished frame back into the game's own target
			Dev->SetRenderTarget(0, OldRT);
			Dev->SetTexture(0, AAFrame != nullptr ? AAFrame : SceneTex);
			Dev->SetTexture(1, BloomTex);
			IDirect3DTexture9 *Lut = PostLut(Dev);
			if (Lut != nullptr)
			{
				D3DSURFACE_DESC LD = {};
				Lut->GetLevelDesc(0, &LD);
				c[5][0] = (float)LD.Height; c[5][1] = 1.0f / LD.Width; c[5][2] = 1.0f / LD.Height; c[5][3] = 1;
				Dev->SetSamplerState(2, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
				Dev->SetSamplerState(2, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
				Dev->SetSamplerState(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
				Dev->SetSamplerState(2, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
				Dev->SetSamplerState(2, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
				Dev->SetSamplerState(2, D3DSAMP_SRGBTEXTURE, FALSE);
			}
			Dev->SetTexture(2, Lut);
			Dev->SetPixelShader(Final);
			Dev->SetPixelShaderConstantF(0, c[0], 6);
			if (Debug)
				PostLog(Dev, "before the final pass (stage 0 the copy, stage 1 the bloom)");
			Quad(Dev, SceneW, SceneH);
			RunLens(Dev);
		}

		Saved.Restore(Dev);
		static int Checked = 0;
		if (Checked++ < 3)
		{
			// what the next game draw will find: the game's own vertex stream and declaration
			IDirect3DVertexBuffer9 *VB = nullptr;
			IDirect3DVertexDeclaration9 *D = nullptr;
			UINT Off = 0, Stride = 0;
			Dev->GetStreamSource(0, &VB, &Off, &Stride);
			Dev->GetVertexDeclaration(&D);
			Message("post: state put back: stream 0 %s, declaration %s", VB == Saved.Stream ? "ok" : "DIFFERENT", D == Saved.Decl ? "ok" : "DIFFERENT");
			if (VB) VB->Release();
			if (D) D->Release();
		}
	}

	// ---- screen-space global illumination by radiance cascades (gi=1) ----------------------
	// gi.hlsl on the frame copy, before SMAA and the final pass: bounce light gathered from
	// what the screen shows around each pixel, and darkening where surfaces meet. It needs the
	// scene's depth as a texture: the game's depth surface is swapped for an INTZ depth texture
	// of the same size (DepthFor, from the device's SetRenderTarget and lazily for the
	// automatic depth buffer), and the game is handed its own surface back when it asks.
	// gifx=strength corners reach debug (debug 1 light alone, 2 darkening alone, 3 normals,
	// 4 depth bands); gibase=the nearest cascade's stretch in pixels.
	bool Gi = false, GiBroken = false, DepthBroken = false, DepthDirty = true;
	float GiFx[4] = { 0.35f, 0.6f, 300.0f, 0.0f };
	float GiBase = 6.0f;
	static const int GiCascades = 4;
	// A game that clears depth in mid-frame (Unreal II before its first-person weapon, and
	// after its sky box) leaves only the last part's depth by the time the post chain runs.
	// So each readable depth surface is one of a pair: at such a clear (OnClear) the part just
	// drawn is kept, if it is the biggest so far this frame (counted in perspective draws), and
	// the game carries on in the other one. RunGi reads the kept part when it is bigger than
	// the last one, and leaves alone what the last one covers (the weapon).
	struct DepthPair { IDirect3DTexture9 *Tex; IDirect3DSurface9 *Surf; UINT W, H; IDirect3DTexture9 *Tex2; IDirect3DSurface9 *Surf2; bool Alt; };
	int SegDraws = 0, KeptDraws = 0;
	IDirect3DTexture9 *KeptDepth = nullptr;
	D3DMATRIX KeptProj = {}, KeptView = {};
	std::map<IDirect3DSurface9 *, DepthPair> DepthSwap;     // the game's depth surface -> ours
	D3DMATRIX SceneProj = {}, SceneView = {};
	bool SceneProjOk = false;
	// the world cache (gicache=1, gicell=world units): see gi.hlsl
	bool GiCache = true;
	float GiCell = 64.0f;
	IDirect3DTexture9 *GiCacheTex[2] = {};
	int GiCacheNow = 0;
	float GiCorner[3] = {};
	bool GiCacheFresh = true;
	// the cache's second half: per cell the surface's facing (world space) as last seen
	// (same grid, same ping-pong); and the cells' light with the game's own lights added
	// (gilights=strength count): what the cascades read
	IDirect3DTexture9 *GiNormTex[2] = {}, *GiLitTex = nullptr;
	float GiLightGain = 0.4f;
	int GiLightMax = 16;
	// the game's fixed-function lights: set by slot, and gathered from every lit draw of
	// the frame (each light once)
	D3DLIGHT9 GiSlot[8] = {};
	bool GiSlotOn[8] = {}, GiSlotDirty = true;
	std::vector<D3DLIGHT9> GiFrameLights;
	IDirect3DVertexShader9 *GiVS = nullptr;
	IDirect3DPixelShader9 *GiPS[7] = {};
	IDirect3DTexture9 *GiLight = nullptr;
	IDirect3DVertexBuffer9 *GiVB = nullptr;
	IDirect3DTexture9 *GiGBuf = nullptr, *GiCasc[GiCascades] = {}, *GiScene = nullptr;
	UINT GiW = 0, GiH = 0, GiRes = 1, GiDiv = 0;    // gires=1|2: the cascades at full or half size

	// the depth surface to bind in place of the game's: an INTZ texture's, or the game's own
	// when it can't be swapped (multisampled, an unknown format, or the card has no INTZ)
	IDirect3DSurface9 *DepthFor(IDirect3DDevice9 *Dev, IDirect3DSurface9 *Game)
	{
		if (!NeedDepth() || DepthBroken || Game == nullptr || U2Msaa::Wanted() != 0)    // msaa: a multisampled scene's depth can't be read
			return Game;
		auto It = DepthSwap.find(Game);
		if (It != DepthSwap.end())
			return It->second.Alt ? It->second.Surf2 : It->second.Surf;
		for (auto &Other : DepthSwap)
			if (Other.second.Surf == Game || Other.second.Surf2 == Game)
				return Game;                        // already one of ours
		D3DSURFACE_DESC D = {};
		Game->GetDesc(&D);
		if (D.MultiSampleType != D3DMULTISAMPLE_NONE || (D.Format != D3DFMT_D24S8 && D.Format != D3DFMT_D24X8 && D.Format != D3DFMT_D16))
		{
			static int Told = 0;
			if (Told++ < 3)
				Message("gi: a depth surface %ux%u format %u multisample %u can't be read as a texture, left alone",
					D.Width, D.Height, (unsigned)D.Format, (unsigned)D.MultiSampleType);
			return Game;
		}
		DepthPair P = { nullptr, nullptr, D.Width, D.Height, nullptr, nullptr, false };
		if (FAILED(Dev->CreateTexture(D.Width, D.Height, 1, D3DUSAGE_DEPTHSTENCIL, (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'), D3DPOOL_DEFAULT, &P.Tex, nullptr))
			|| P.Tex == nullptr || FAILED(P.Tex->GetSurfaceLevel(0, &P.Surf)) || P.Surf == nullptr)
		{
			if (P.Tex) P.Tex->Release();
			Message("gi: no INTZ depth texture on this card (%ux%u), global illumination off", D.Width, D.Height);
			DepthBroken = true;
			return Game;
		}
		P.Surf->Release();                          // the texture keeps it alive
		DepthSwap[Game] = P;
		Message("gi: depth surface %ux%u (format %u) swapped for a readable one", D.Width, D.Height, (unsigned)D.Format);
		return P.Surf;
	}

	// msaa=N (see msaa.hpp): a plain depth buffer for a plain target drawn while the multisampled
	// one is bound (Direct3D 9 draws nothing when the two differ). One per size and sampling.
	struct MatchPair { IDirect3DSurface9 *Surf; UINT W, H; D3DMULTISAMPLE_TYPE MS; D3DFORMAT Fmt; IDirect3DSurface9 *Game; };
	std::vector<MatchPair> DepthMatches;

	void MatchDepth(IDirect3DDevice9 *Dev)
	{
		if (U2Msaa::Wanted() == 0)
			return;
		IDirect3DSurface9 *RT = nullptr, *DS = nullptr;
		if (FAILED(Dev->GetRenderTarget(0, &RT)) || RT == nullptr)
			return;
		if (FAILED(Dev->GetDepthStencilSurface(&DS)) || DS == nullptr)
		{
			RT->Release();
			return;
		}
		D3DSURFACE_DESC R = {}, D = {};
		RT->GetDesc(&R);
		DS->GetDesc(&D);
		RT->Release();
		DS->Release();
		if (R.MultiSampleType == D.MultiSampleType && D.Width >= R.Width && D.Height >= R.Height)
			return;
		IDirect3DSurface9 *Game = DS;
		for (const MatchPair &M : DepthMatches)
			if (M.Surf == DS)
				Game = M.Game;
		MatchPair *Use = nullptr;
		for (MatchPair &M : DepthMatches)
			if (M.W == R.Width && M.H == R.Height && M.MS == R.MultiSampleType && M.Fmt == D.Format)
				Use = &M;
		if (Use == nullptr)
		{
			MatchPair M = { nullptr, R.Width, R.Height, R.MultiSampleType, D.Format, Game };
			if (FAILED(Dev->CreateDepthStencilSurface(R.Width, R.Height, D.Format, R.MultiSampleType, 0, TRUE, &M.Surf, nullptr)) || M.Surf == nullptr)
			{
				static int Told = 0;
				if (Told++ < 3)
					Message("msaa: no depth buffer %ux%u format %u for a target sampled %u", R.Width, R.Height, (unsigned)D.Format, (unsigned)R.MultiSampleType);
				return;
			}
			DepthMatches.push_back(M);
			Use = &DepthMatches.back();
			Message("msaa: a %ux%u target (sampled %u) drawn with depth sampled %u: it gets its own depth buffer",
				R.Width, R.Height, (unsigned)R.MultiSampleType, (unsigned)D.MultiSampleType);
		}
		Use->Game = Game;
		if (SUCCEEDED(Dev->SetDepthStencilSurface(Use->Surf)))
			Dev->Clear(0, nullptr, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
	}

	// msaa=N: the game draws its world into a screen-sized render-target TEXTURE, which can't be
	// multisampled. While it is bound, a multisampled surface of the same size stands in for it;
	// its samples are resolved into the texture before the game reads it (as a texture, by a copy,
	// or at Present) and whenever another target is bound.
	struct MsaaProxy { IDirect3DSurface9 *Plain, *Multi; IDirect3DBaseTexture9 *Tex; };
	std::vector<MsaaProxy> MsaaProxies;
	int MsaaBound = -1;
	unsigned MsaaCount[3] = {}, MsaaCountFrame = 0;
	bool MsaaDirty = false;

	void MsaaResolve(IDirect3DDevice9 *Dev)
	{
		if (MsaaBound >= 0 && MsaaDirty)
		{
			if (U2Msaa::ShotTest() && Frame > 300)
				MsaaStamp(Dev, MsaaProxies[MsaaBound].Multi);    // test: a triangle on what the game drew
			Dev->StretchRect(MsaaProxies[MsaaBound].Multi, nullptr, MsaaProxies[MsaaBound].Plain, nullptr, D3DTEXF_NONE);
			MsaaDirty = false;
		}
	}

	// the device's SetRenderTarget: the surface to bind in place of the game's
	IDirect3DSurface9 *MsaaTarget(IDirect3DDevice9 *Dev, IDirect3DSurface9 *S)
	{
		MsaaResolve(Dev);
		MsaaBound = -1;
		if (U2Msaa::Wanted() == 0 || S == nullptr)
			return S;
		for (size_t i = 0; i < MsaaProxies.size(); i++)
			if (MsaaProxies[i].Plain == S)
			{
				MsaaBound = (int)i;
				MsaaDirty = true;
				return MsaaProxies[i].Multi;
			}
		D3DSURFACE_DESC D = {}, B = {};
		S->GetDesc(&D);
		if (D.MultiSampleType != D3DMULTISAMPLE_NONE || !(D.Usage & D3DUSAGE_RENDERTARGET))
			return S;
		IDirect3DSurface9 *BB = nullptr;
		if (SUCCEEDED(Dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &BB)) && BB) { BB->GetDesc(&B); BB->Release(); }
		if (B.MultiSampleType == D3DMULTISAMPLE_NONE || D.Width != B.Width || D.Height != B.Height)
			return S;
		IDirect3DTexture9 *Tex = nullptr;
		if (FAILED(S->GetContainer(IID_IDirect3DTexture9, (void **)&Tex)) || Tex == nullptr)
			return S;
		Tex->Release();                             // the game keeps it alive
		MsaaProxy P = { S, nullptr, Tex };
		if (FAILED(Dev->CreateRenderTarget(D.Width, D.Height, D.Format, B.MultiSampleType, 0, FALSE, &P.Multi, nullptr)) || P.Multi == nullptr)
		{
			static int Told = 0;
			if (Told++ < 3)
				Message("msaa: no multisampled stand-in %ux%u format %u", D.Width, D.Height, (unsigned)D.Format);
			return S;
		}
		MsaaProxies.push_back(P);
		MsaaBound = (int)MsaaProxies.size() - 1;
		MsaaDirty = true;
		Message("msaa: the game's %ux%u render texture (format %u) gets a multisampled stand-in", D.Width, D.Height, (unsigned)D.Format);
		return P.Multi;
	}

	// before the game reads a texture or copies a surface: the bound stand-in resolved if it is that one
	void MsaaBeforeRead(IDirect3DDevice9 *Dev, IDirect3DBaseTexture9 *Tex, IDirect3DSurface9 *Surf)
	{
		if (MsaaBound < 0 || !MsaaDirty)
			return;
		const MsaaProxy &P = MsaaProxies[MsaaBound];
		if ((Tex != nullptr && Tex == P.Tex) || (Surf != nullptr && Surf == P.Plain))
			MsaaResolve(Dev);
	}

	// msaa: once, a white triangle drawn into a small multisampled target and resolved: are there
	// in-between pixels on its edge? (no: something - often the driver's own settings - turns the
	// sampling off)
	bool MsaaTested = false;
	void MsaaSelfTest(IDirect3DDevice9 *Dev)
	{
		MsaaTested = true;
		IDirect3DSurface9 *Multi = nullptr, *Plain = nullptr, *Sys = nullptr, *OldRT = nullptr, *OldDS = nullptr;
		IDirect3DStateBlock9 *SB = nullptr;
		const D3DMULTISAMPLE_TYPE MS = (D3DMULTISAMPLE_TYPE)U2Msaa::Wanted();
		if (FAILED(Dev->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, MS, 0, FALSE, &Multi, nullptr))
			|| FAILED(Dev->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &Plain, nullptr))
			|| FAILED(Dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &Sys, nullptr))
			|| FAILED(Dev->CreateStateBlock(D3DSBT_ALL, &SB)))
		{
			Message("msaa self-test: couldn't make its targets");
		}
		else
		{
			Dev->GetRenderTarget(0, &OldRT);
			Dev->GetDepthStencilSurface(&OldDS);
			Dev->SetRenderTarget(0, Multi);
			Dev->SetDepthStencilSurface(nullptr);
			Dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
			struct V { float x, y, z, w; DWORD c; } Tri[3] = {
				{ 2, 2, 0.5f, 1, 0xFFFFFFFF }, { 62, 10, 0.5f, 1, 0xFFFFFFFF }, { 6, 61, 0.5f, 1, 0xFFFFFFFF } };
			Dev->SetVertexShader(nullptr);
			Dev->SetPixelShader(nullptr);
			Dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
			Dev->SetTexture(0, nullptr);
			Dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
			Dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
			Dev->SetRenderState(D3DRS_ZENABLE, FALSE);
			Dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
			Dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
			Dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
			Dev->SetRenderState(D3DRS_LIGHTING, FALSE);
			Dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
			Dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
			Dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
			Dev->SetRenderState(D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF);
			Dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, Tri, sizeof(V));
			Dev->StretchRect(Multi, nullptr, Plain, nullptr, D3DTEXF_NONE);
			int Mid = 0, Lit = 0;
			D3DLOCKED_RECT L = {};
			if (SUCCEEDED(U2PerfReadback(Dev, Plain, Sys)) && SUCCEEDED(Sys->LockRect(&L, nullptr, D3DLOCK_READONLY)))
			{
				for (int y = 0; y < 64; y++)
					for (int x = 0; x < 64; x++)
					{
						const BYTE g = static_cast<const BYTE *>(L.pBits)[y * L.Pitch + x * 4 + 1];
						if (g > 20 && g < 235) Mid++;
						if (g >= 235) Lit++;
					}
				Sys->UnlockRect();
			}
			Message("msaa self-test (%ux): %d lit pixels, %d in-between on the edges -> %s", (unsigned)MS, Lit, Mid,
				Mid > 10 ? "the card multisamples" : "NOT multisampled (the driver's anti-aliasing settings may override the game: set it to 'application-controlled')");
			Dev->SetRenderTarget(0, OldRT);
			Dev->SetDepthStencilSurface(OldDS);
			SB->Apply();
		}
		if (OldRT) OldRT->Release();
		if (OldDS) OldDS->Release();
		if (SB) SB->Release();
		if (Sys) Sys->Release();
		if (Plain) Plain->Release();
		if (Multi) Multi->Release();
	}

	// test (msaastamp=1): a white triangle into a surface (top left), state kept
	void MsaaStamp(IDirect3DDevice9 *Dev, IDirect3DSurface9 *Into)
	{
		IDirect3DSurface9 *OldRT = nullptr, *OldDS = nullptr;
		IDirect3DStateBlock9 *SB = nullptr;
		if (FAILED(Dev->CreateStateBlock(D3DSBT_ALL, &SB)))
			return;
		Dev->GetRenderTarget(0, &OldRT);
		Dev->GetDepthStencilSurface(&OldDS);
		Dev->SetRenderTarget(0, Into);
		Dev->SetDepthStencilSurface(nullptr);
		struct V { float x, y, z, w; DWORD c; } Tri[3] = {
			{ 2, 2, 0.5f, 1, 0xFFFFFFFF }, { 182, 30, 0.5f, 1, 0xFFFFFFFF }, { 18, 183, 0.5f, 1, 0xFFFFFFFF } };
		Dev->SetVertexShader(nullptr);
		Dev->SetPixelShader(nullptr);
		Dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
		Dev->SetTexture(0, nullptr);
		Dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		Dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
		Dev->SetRenderState(D3DRS_ZENABLE, FALSE);
		Dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		Dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
		Dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
		Dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		Dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
		Dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
		Dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, Tri, sizeof(V));
		Dev->SetRenderTarget(0, OldRT);
		Dev->SetDepthStencilSurface(OldDS);
		SB->Apply();
		SB->Release();
		if (OldRT) OldRT->Release();
		if (OldDS) OldDS->Release();
	}

	// the game's own target for a stand-in
	IDirect3DSurface9 *MsaaPlainOf(IDirect3DSurface9 *S)
	{
		for (const MsaaProxy &P : MsaaProxies)
			if (P.Multi == S)
				return P.Plain;
		return S;
	}

	// the game's own surface for one of ours (nullptr: not ours)
	IDirect3DSurface9 *GameDepthOf(IDirect3DSurface9 *Ours)
	{
		for (const MatchPair &M : DepthMatches)
			if (M.Surf == Ours)
				return M.Game;
		for (auto &It : DepthSwap)
			if (It.second.Surf == Ours || (Ours != nullptr && It.second.Surf2 == Ours))
				return It.first;
		return nullptr;
	}

	// the device's Clear, before it happens: a depth clear in mid-frame (see DepthPair)
	void OnClear(IDirect3DDevice9 *Dev, DWORD Count, DWORD Flags)
	{
		if (!NeedDepth() || DepthBroken || Count != 0 || (Flags & D3DCLEAR_ZBUFFER) == 0 || (Flags & D3DCLEAR_TARGET) != 0)
			return;
		const int Draws = SegDraws;
		SegDraws = 0;
		if (Draws == 0 || Draws < KeptDraws || Offscreen(Dev))
			return;
		IDirect3DSurface9 *DS = nullptr;
		if (FAILED(Dev->GetDepthStencilSurface(&DS)) || DS == nullptr)
			return;
		DS->Release();
		for (auto &It : DepthSwap)
		{
			DepthPair &P = It.second;
			if (P.Surf != DS && P.Surf2 != DS)
				continue;
			if (P.Tex2 == nullptr)
			{
				if (FAILED(Dev->CreateTexture(P.W, P.H, 1, D3DUSAGE_DEPTHSTENCIL, (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'), D3DPOOL_DEFAULT, &P.Tex2, nullptr))
					|| P.Tex2 == nullptr || FAILED(P.Tex2->GetSurfaceLevel(0, &P.Surf2)) || P.Surf2 == nullptr)
				{
					if (P.Tex2) { P.Tex2->Release(); P.Tex2 = nullptr; }
					return;
				}
				P.Surf2->Release();
				Message("gi: the game clears depth in mid-frame (after %d draws): that part's depth is kept", Draws);
			}
			if (FAILED(Dev->SetDepthStencilSurface(P.Alt ? P.Surf : P.Surf2)))
				return;
			KeptDepth = P.Alt ? P.Tex2 : P.Tex;
			P.Alt = !P.Alt;
			KeptDraws = Draws;
			KeptProj = SceneProj;
			KeptView = SceneView;
			// the stencil isn't carried over: the new surface starts clean
			if ((Flags & D3DCLEAR_STENCIL) == 0)
				Dev->Clear(0, nullptr, D3DCLEAR_STENCIL, 0, 1.0f, 0);
			return;
		}
	}

	IDirect3DTexture9 *DepthTexOf(IDirect3DSurface9 *Ours)
	{
		for (auto &It : DepthSwap)
		{
			if (It.second.Surf == Ours)
				return It.second.Tex;
			if (Ours != nullptr && It.second.Surf2 == Ours)
				return It.second.Tex2;
		}
		return nullptr;
	}

	// the automatic depth buffer is bound without a SetRenderTarget call: swap it the first
	// time a frame draws with it (that one frame loses the depth drawn so far)
	void DepthLazySwap(IDirect3DDevice9 *Dev)
	{
		DepthDirty = false;
		IDirect3DSurface9 *DS = nullptr;
		if (FAILED(Dev->GetDepthStencilSurface(&DS)) || DS == nullptr)
			return;
		IDirect3DSurface9 *Ours = DepthFor(Dev, DS);
		if (Ours != DS && SUCCEEDED(Dev->SetDepthStencilSurface(Ours)))
			Dev->Clear(0, nullptr, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0);
		DS->Release();
	}

	// the device's SetLight / LightEnable (gi=1)
	void GiLightSet(DWORD Index, const D3DLIGHT9 &L)
	{
		if (Index < 8) { GiSlot[Index] = L; GiSlotDirty = true; }
	}
	void GiLightEnable(DWORD Index, BOOL On)
	{
		if (Index < 8) { GiSlotOn[Index] = On != FALSE; GiSlotDirty = true; }
	}

	// a lit draw of the world: its lights into this frame's list, each once (fixed-function
	// lights are in world space)
	void GiGatherLights(IDirect3DDevice9 *Dev)
	{
		if (!Gi && !Atmos)                              // atmos=1 finds the sun among them
			return;
		GiSlotDirty = false;
		DWORD Lit = 0;
		if (FAILED(Dev->GetRenderState(D3DRS_LIGHTING, &Lit)) || !Lit)
		{
			GiSlotDirty = true;                         // look again at the next draw
			return;
		}
		for (int i = 0; i < 8; i++)
		{
			if (!GiSlotOn[i])
				continue;
			const D3DLIGHT9 &L = GiSlot[i];
			if (L.Type != D3DLIGHT_POINT && L.Type != D3DLIGHT_SPOT && L.Type != D3DLIGHT_DIRECTIONAL)
				continue;
			if (L.Diffuse.r + L.Diffuse.g + L.Diffuse.b < 0.01f)
				continue;
			bool Seen = false;
			for (const D3DLIGHT9 &O : GiFrameLights)
				if (O.Type == L.Type && fabsf(O.Position.x - L.Position.x) < 1 && fabsf(O.Position.y - L.Position.y) < 1 && fabsf(O.Position.z - L.Position.z) < 1
					&& fabsf(O.Direction.x - L.Direction.x) < 0.01f && fabsf(O.Direction.y - L.Direction.y) < 0.01f && fabsf(O.Diffuse.r - L.Diffuse.r) < 0.01f)
				{
					Seen = true;
					break;
				}
			if (!Seen && GiFrameLights.size() < 64)
				GiFrameLights.push_back(L);
		}
	}

	void GiReleaseTargets()
	{
		if (GiGBuf) { GiGBuf->Release(); GiGBuf = nullptr; }
		if (GiScene) { GiScene->Release(); GiScene = nullptr; }
		if (GiLight) { GiLight->Release(); GiLight = nullptr; }
		for (int i = 0; i < GiCascades; i++)
			if (GiCasc[i]) { GiCasc[i]->Release(); GiCasc[i] = nullptr; }
		for (int i = 0; i < 2; i++)
		{
			if (GiCacheTex[i]) { GiCacheTex[i]->Release(); GiCacheTex[i] = nullptr; }
			if (GiNormTex[i]) { GiNormTex[i]->Release(); GiNormTex[i] = nullptr; }
		}
		if (GiLitTex) { GiLitTex->Release(); GiLitTex = nullptr; }
		GiCacheFresh = true;
		GiW = GiH = 0;
	}

	void GiReleaseAll()
	{
		GiReleaseTargets();
		for (auto &It : DepthSwap)
		{
			if (It.second.Tex) It.second.Tex->Release();
			if (It.second.Tex2) It.second.Tex2->Release();
		}
		DepthSwap.clear();
		KeptDepth = nullptr;
		for (MatchPair &M : DepthMatches)
			if (M.Surf) M.Surf->Release();
		DepthMatches.clear();
		for (MsaaProxy &P : MsaaProxies)
			if (P.Multi) P.Multi->Release();
		MsaaProxies.clear();
		MsaaBound = -1;
		DepthDirty = true;
		if (GiVS) { GiVS->Release(); GiVS = nullptr; }
		for (int i = 0; i < 7; i++)
			if (GiPS[i]) { GiPS[i]->Release(); GiPS[i] = nullptr; }
		if (GiVB) { GiVB->Release(); GiVB = nullptr; }
	}

	bool GiBuild(IDirect3DDevice9 *Dev)
	{
		if (GiBroken)
			return false;
		if (GiPS[6] != nullptr && GiVB != nullptr)
			return true;
		const std::string Lib = ReadShaderFile("gi.hlsl");
		if (Lib.empty())
		{
			Message("gi: gi.hlsl missing in U2Shaders, global illumination off");
			GiBroken = true;
			return false;
		}
		static const char *Names[8] = { "GiVS", "GBufPS", "CascadePS", "GatherPS", "CachePS", "ResolvePS", "CacheNormalPS", "LightPS" };
		for (int i = 0; i < 8; i++)
		{
			char Head[96];
			sprintf_s(Head, "#define GI_PASS %d\n#line 1 \"gi.hlsl\"\n", i == 0 ? 1 : i);
			const std::string Src = std::string(Head) + Lib;
			ID3DBlob *Code = nullptr, *Errors = nullptr;
			const HRESULT hr = D3DCompile(Src.data(), Src.size(), "gi", nullptr, nullptr, Names[i], i == 0 ? "vs_3_0" : "ps_3_0",
				D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &Code, &Errors);
			if (FAILED(hr) || Code == nullptr)
			{
				Message("gi: %s failed: %s", Names[i], Errors ? (const char *)Errors->GetBufferPointer() : "?");
				if (Errors) Errors->Release();
				if (Code) Code->Release();
				GiBroken = true;
				return false;
			}
			if (Errors) Errors->Release();
			const HRESULT cr = i == 0 ? Dev->CreateVertexShader((const DWORD *)Code->GetBufferPointer(), &GiVS)
				: Dev->CreatePixelShader((const DWORD *)Code->GetBufferPointer(), &GiPS[i - 1]);
			Code->Release();
			if (FAILED(cr))
			{
				Message("gi: the card refused %s (%08x)", Names[i], (unsigned)cr);
				GiBroken = true;
				return false;
			}
		}
		const SmaaVertex q[4] = { { -1, 1, 0, 0, 0 }, { 1, 1, 0, 1, 0 }, { -1, -1, 0, 0, 1 }, { 1, -1, 0, 1, 1 } };
		void *Mem = nullptr;
		if (FAILED(Dev->CreateVertexBuffer(sizeof(q), D3DUSAGE_WRITEONLY, D3DFVF_XYZ | D3DFVF_TEX1, D3DPOOL_MANAGED, &GiVB, nullptr))
			|| FAILED(GiVB->Lock(0, sizeof(q), &Mem, 0)))
		{
			Message("gi: the quad couldn't be made, global illumination off");
			GiBroken = true;
			return false;
		}
		memcpy(Mem, q, sizeof(q));
		GiVB->Unlock();
		Message("gi: ready (radiance cascades, %d levels)", GiCascades);
		return true;
	}

	// the passes; afterwards SceneTex is the lit frame (the copy and the result trade places).
	// Leaves the post chain's own vertex setup behind it, as RunSmaa does.
	void RunGi(IDirect3DDevice9 *Dev, IDirect3DSurface9 *BoundDepth)
	{
		if (!Gi || GiBroken || SceneTex == nullptr)
			return;
		IDirect3DTexture9 *Depth = DepthTexOf(BoundDepth), *Over = nullptr;
		if (Depth != nullptr && KeptDepth != nullptr && KeptDepth != Depth && KeptDraws > SegDraws)
		{
			// the last part of the frame was the smaller one (a first-person weapon): the world is the kept part
			Over = Depth;
			Depth = KeptDepth;
			SceneProj = KeptProj;
			SceneView = KeptView;
		}
		static int Told = 0;
		if (Depth == nullptr || !SceneProjOk)
		{
			if (Told++ < 3)
				Message("gi: skipped (%s)", Depth == nullptr ? "the scene's depth isn't one of the readable ones yet" : "no perspective projection seen");
			return;
		}
		D3DSURFACE_DESC DD = {};
		Depth->GetLevelDesc(0, &DD);
		if (DD.Width != SceneW || DD.Height != SceneH || !GiBuild(Dev))
			return;
		const UINT W = (std::max)(SceneW / GiRes, 8u), H = (std::max)(SceneH / GiRes, 8u);
		if (GiScene == nullptr || GiW != SceneW || GiH != SceneH || GiDiv != GiRes)
		{
			GiReleaseTargets();
			bool Ok = SUCCEEDED(Dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &GiGBuf, nullptr))
				&& SUCCEEDED(Dev->CreateTexture(SceneW, SceneH, 1, D3DUSAGE_RENDERTARGET, SceneFmt, D3DPOOL_DEFAULT, &GiScene, nullptr))
				&& SUCCEEDED(Dev->CreateTexture(SceneW, SceneH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &GiLight, nullptr));
			for (int i = 0; Ok && i < GiCascades; i++)
				Ok = SUCCEEDED(Dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &GiCasc[i], nullptr));
			for (int i = 0; Ok && i < 2; i++)
				Ok = SUCCEEDED(Dev->CreateTexture(512, 256, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &GiCacheTex[i], nullptr))
					&& SUCCEEDED(Dev->CreateTexture(512, 256, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &GiNormTex[i], nullptr));
			Ok = Ok && SUCCEEDED(Dev->CreateTexture(512, 256, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &GiLitTex, nullptr));
			GiCacheFresh = true;
			if (!Ok)
			{
				Message("gi: targets couldn't be made (%ux%u), global illumination off", SceneW, SceneH);
				GiReleaseTargets();
				GiBroken = true;
				return;
			}
			GiW = SceneW;
			GiH = SceneH;
			GiDiv = GiRes;
		}
		const float half[4] = { 1.0f / W, 1.0f / H, (float)W, (float)H };
		const float full[4] = { 1.0f / SceneW, 1.0f / SceneH, (float)SceneW, (float)SceneH };
		const float proj[4] = { SceneProj._11, SceneProj._22, SceneProj._33, SceneProj._43 };
		Dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
		Dev->SetStreamSource(0, GiVB, 0, sizeof(SmaaVertex));
		Dev->SetVertexShader(GiVS);
		Dev->SetVertexShaderConstantF(0, half, 1);
		Dev->SetPixelShaderConstantF(0, half, 1);
		Dev->SetPixelShaderConstantF(1, proj, 1);
		Dev->SetPixelShaderConstantF(3, full, 1);
		Dev->SetPixelShaderConstantF(4, GiFx, 1);

		// 1: depth and normals, half size
		Target(Dev, GiGBuf);
		Dev->SetPixelShader(GiPS[0]);
		SmaaSampler(Dev, 0, D3DTEXF_POINT);
		SmaaSampler(Dev, 1, D3DTEXF_POINT);
		Dev->SetTexture(0, Depth);
		Dev->SetTexture(1, Over);                   // what was drawn over the world after a depth clear
		Dev->SetTexture(2, nullptr);
		Dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);

		// the world cache: its grid follows the camera in whole cells; every cell is brought
		// up to date from this frame into the other texture
		SmaaSampler(Dev, 1, D3DTEXF_LINEAR);
		Dev->SetTexture(0, GiGBuf);
		Dev->SetTexture(1, SceneTex);
		const D3DMATRIX Inv = InvView(SceneView);
		float grid[4] = { 0, 0, 0, GiCell }, gridOld[4] = { GiCorner[0], GiCorner[1], GiCorner[2], GiCache ? 1.0f : 0.0f };
		static const float GridN[3] = { 64, 64, 32 };
		for (int a = 0; a < 3; a++)
			grid[a] = (floorf(Inv.m[3][a] / GiCell) - GridN[a] * 0.5f) * GiCell;
		if (GiCache)
		{
			const float cachePx[4] = { 1.0f / 512, 1.0f / 256, 512, 256 };
			const float v0[4] = { SceneView._11, SceneView._21, SceneView._31, SceneView._41 };
			const float v1[4] = { SceneView._12, SceneView._22, SceneView._32, SceneView._42 };
			const float v2[4] = { SceneView._13, SceneView._23, SceneView._33, SceneView._43 };
			Dev->SetTexture(2, nullptr);
			Target(Dev, GiCacheTex[1 - GiCacheNow]);
			if (GiCacheFresh)
			{
				// nothing remembered yet: the texture it reads from starts empty
				Target(Dev, GiCacheTex[GiCacheNow]);
				Dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
				Target(Dev, GiNormTex[GiCacheNow]);
				Dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
				Target(Dev, GiCacheTex[1 - GiCacheNow]);
				memcpy(gridOld, grid, 3 * sizeof(float));
				GiCacheFresh = false;
			}
			Dev->SetVertexShaderConstantF(0, cachePx, 1);
			Dev->SetPixelShaderConstantF(0, cachePx, 1);
			Dev->SetPixelShaderConstantF(5, v0, 1);
			Dev->SetPixelShaderConstantF(6, v1, 1);
			Dev->SetPixelShaderConstantF(7, v2, 1);
			Dev->SetPixelShaderConstantF(8, grid, 1);
			Dev->SetPixelShaderConstantF(9, gridOld, 1);
			Dev->SetPixelShader(GiPS[3]);
			SmaaSampler(Dev, 2, D3DTEXF_POINT);
			Dev->SetTexture(2, GiCacheTex[GiCacheNow]);
			Dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			// the surfaces' facing, the same way
			Dev->SetTexture(2, nullptr);
			Target(Dev, GiNormTex[1 - GiCacheNow]);
			Dev->SetPixelShader(GiPS[5]);
			Dev->SetTexture(2, GiNormTex[GiCacheNow]);
			Dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			Dev->SetTexture(2, nullptr);
			GiCacheNow = 1 - GiCacheNow;
			// the game's lights on the cells (c10 gain + count; per light c11+i position + range,
			// c43+i colour + 1 if directional, c75+i direction)
			{
				float head[4] = { GiLightGain, 0, 0, 0 };
				int n = 0;
				for (const D3DLIGHT9 &L : GiFrameLights)
				{
					if (n >= GiLightMax || n >= 32)
						break;
					const bool dir = L.Type == D3DLIGHT_DIRECTIONAL;
					const float a[4] = { L.Position.x, L.Position.y, L.Position.z, dir ? 0.0f : L.Range };
					const float b[4] = { L.Diffuse.r, L.Diffuse.g, L.Diffuse.b, dir ? 1.0f : 0.0f };
					const float c[4] = { L.Direction.x, L.Direction.y, L.Direction.z, 0 };
					Dev->SetPixelShaderConstantF(11 + n, a, 1);
					Dev->SetPixelShaderConstantF(43 + n, b, 1);
					Dev->SetPixelShaderConstantF(75 + n, c, 1);
					n++;
				}
				head[1] = (float)n;
				Dev->SetPixelShaderConstantF(10, head, 1);
				static int Told = 0;
				if (n > 0 && Told++ < 3)
					Message("gi: %d of the game's lights on the world cache (%d this frame, gain %.2f)", n, (int)GiFrameLights.size(), GiLightGain);
				Target(Dev, GiLitTex);
				Dev->SetPixelShader(GiPS[6]);
				Dev->SetTexture(0, GiCacheTex[GiCacheNow]);
				Dev->SetTexture(1, GiNormTex[GiCacheNow]);
				SmaaSampler(Dev, 0, D3DTEXF_POINT);
				SmaaSampler(Dev, 1, D3DTEXF_POINT);
				Dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
				Dev->SetTexture(0, nullptr);
				Dev->SetTexture(1, nullptr);
			}
			memcpy(GiCorner, grid, 3 * sizeof(float));
			Dev->SetVertexShaderConstantF(0, half, 1);
			Dev->SetPixelShaderConstantF(0, half, 1);
		}
		// from here on the matrix is the camera's inverse view: view space to world
		const float i0[4] = { Inv._11, Inv._21, Inv._31, Inv._41 }, i1[4] = { Inv._12, Inv._22, Inv._32, Inv._42 }, i2[4] = { Inv._13, Inv._23, Inv._33, Inv._43 };
		Dev->SetPixelShaderConstantF(5, i0, 1);
		Dev->SetPixelShaderConstantF(6, i1, 1);
		Dev->SetPixelShaderConstantF(7, i2, 1);
		Dev->SetPixelShaderConstantF(8, grid, 1);
		Dev->SetPixelShaderConstantF(9, gridOld, 1);

		// 2: the cascades, the farthest first, each merging the one before
		Dev->SetPixelShader(GiPS[1]);
		SmaaSampler(Dev, 1, D3DTEXF_LINEAR);
		SmaaSampler(Dev, 2, D3DTEXF_LINEAR);
		Dev->SetTexture(0, GiGBuf);
		Dev->SetTexture(1, SceneTex);
		for (int c = GiCascades - 1; c >= 0; c--)
		{
			float start = 0, len = GiBase;
			for (int i = 0; i < c; i++) { start += len; len *= 4; }
			const float casc[4] = { (float)(2 << c), start, len, c < GiCascades - 1 ? 1.0f : 0.0f };
			Dev->SetPixelShaderConstantF(2, casc, 1);
			Dev->SetTexture(2, nullptr);
			Target(Dev, GiCasc[c]);
			Dev->SetTexture(2, c < GiCascades - 1 ? GiCasc[c + 1] : (GiCache ? GiLitTex : nullptr));
			Dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
		}

		// 3: gathered per pixel, full size
		Dev->SetTexture(2, nullptr);
		Target(Dev, GiLight);
		Dev->SetVertexShaderConstantF(0, full, 1);
		Dev->SetPixelShaderConstantF(0, full, 1);
		const float atlas[4] = { 0, 0, (float)W, (float)H };
		Dev->SetPixelShaderConstantF(2, atlas, 1);
		Dev->SetPixelShader(GiPS[2]);
		Dev->SetTexture(2, GiCasc[0]);
		Dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);

		// 4: smoothed along surfaces into the lit frame
		Dev->SetTexture(2, nullptr);
		Target(Dev, GiScene);
		Dev->SetPixelShader(GiPS[4]);
		Dev->SetTexture(2, GiFx[3] > 4.5f && GiCache ? GiLitTex : GiLight);    // debug view 5 shows the cache (with the game's lights)
		Dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
		std::swap(SceneTex, GiScene);
		static int Ran = 0;
		if (Ran++ < 1)
			Message("gi: running (%ux%u, base %.0f px, strength %.2f, corners %.2f, reach %.0f)",
				SceneW, SceneH, GiBase, GiFx[0], GiFx[1], GiFx[2]);

		// back to the post chain's setup
		Dev->SetVertexShader(nullptr);
		Dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		Dev->SetStreamSource(0, PostQuadVB, 0, sizeof(U2QuadVertex));
		for (DWORD t = 0; t < 3; t++)
			SmaaSampler(Dev, t, D3DTEXF_LINEAR);
		Dev->SetTexture(1, nullptr);
		Dev->SetTexture(2, nullptr);
	}

	// ---- screen-space ambient occlusion (ssao=1) -------------------------------------------
	// ssao.hlsl on the frame copy, after gi and before SMAA: corners, wall feet and the ground
	// under things darken, by how much of the space around each point the depth buffer shows
	// filled (Scalable Ambient Obscurance). Much lighter than gi=1 (no cascades, no cache) and
	// independent of it: it uses the same readable depth (DepthFor swaps the game's depth surface
	// for an INTZ texture when gi or ssao is on) and the same mid-frame depth-clear handling
	// (the first-person weapon is left alone). ssaofx=strength radius intensity debug
	// (default 0.8 40 1 0; radius in world units, debug 1 shows the AO alone); ssaores=1|2:
	// the AO at full or half size (default half).
	bool Ssao = false, SsaoBroken = false;
	float SsaoFx[4] = { 0.8f, 40.0f, 1.0f, 0.0f };
	UINT SsaoRes = 2, SsaoW = 0, SsaoH = 0, SsaoDiv = 0;
	IDirect3DVertexShader9 *SsaoVS = nullptr;
	IDirect3DPixelShader9 *SsaoPS[4] = {};
	IDirect3DTexture9 *SsaoGBuf = nullptr, *SsaoA = nullptr, *SsaoB = nullptr, *SsaoOut = nullptr;
	IDirect3DVertexBuffer9 *SsaoVB = nullptr;

	bool NeedDepth() const { return Gi || Ssao || Sss || Atmos; }

	void SsaoReleaseTargets()
	{
		IDirect3DTexture9 **All[] = { &SsaoGBuf, &SsaoA, &SsaoB, &SsaoOut };
		for (IDirect3DTexture9 **T : All)
			if (*T) { (*T)->Release(); *T = nullptr; }
		SsaoW = SsaoH = 0;
	}

	void SsaoReleaseAll()
	{
		SsaoReleaseTargets();
		if (SsaoVS) { SsaoVS->Release(); SsaoVS = nullptr; }
		if (SsaoVB) { SsaoVB->Release(); SsaoVB = nullptr; }
		for (int i = 0; i < 4; i++)
			if (SsaoPS[i]) { SsaoPS[i]->Release(); SsaoPS[i] = nullptr; }
	}

	bool SsaoBuild(IDirect3DDevice9 *Dev)
	{
		if (SsaoBroken)
			return false;
		if (SsaoPS[3] != nullptr)
			return true;
		const std::string Lib = ReadShaderFile("ssao.hlsl");
		if (Lib.empty())
		{
			Message("ssao: ssao.hlsl missing in U2Shaders, ambient occlusion off");
			SsaoBroken = true;
			return false;
		}
		static const char *Names[5] = { "SsaoVS", "GBufPS", "AoPS", "BlurPS", "ApplyPS" };
		for (int i = 0; i < 5; i++)
		{
			char Head[96];
			sprintf_s(Head, "#define SSAO_PASS %d\n#line 1 \"ssao.hlsl\"\n", i == 0 ? 1 : i);
			const std::string Src = std::string(Head) + Lib;
			ID3DBlob *Code = nullptr, *Errors = nullptr;
			const HRESULT hr = D3DCompile(Src.data(), Src.size(), "ssao", nullptr, nullptr, Names[i], i == 0 ? "vs_3_0" : "ps_3_0",
				D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &Code, &Errors);
			if (FAILED(hr) || Code == nullptr)
			{
				Message("ssao: %s failed: %s", Names[i], Errors ? (const char *)Errors->GetBufferPointer() : "?");
				if (Errors) Errors->Release();
				if (Code) Code->Release();
				SsaoBroken = true;
				return false;
			}
			if (Errors) Errors->Release();
			const HRESULT cr = i == 0 ? Dev->CreateVertexShader((const DWORD *)Code->GetBufferPointer(), &SsaoVS)
				: Dev->CreatePixelShader((const DWORD *)Code->GetBufferPointer(), &SsaoPS[i - 1]);
			Code->Release();
			if (FAILED(cr))
			{
				Message("ssao: the card refused %s (%08x)", Names[i], (unsigned)cr);
				SsaoBroken = true;
				return false;
			}
		}
		Message("ssao: ready");
		return true;
	}

	// the passes; afterwards SceneTex is the darkened frame (the copy and the result trade
	// places). Leaves the post chain's own vertex setup behind it, as RunGi does.
	void RunSsao(IDirect3DDevice9 *Dev, IDirect3DSurface9 *BoundDepth)
	{
		if (!Ssao || SsaoBroken || SceneTex == nullptr)
			return;
		IDirect3DTexture9 *Depth = DepthTexOf(BoundDepth), *Over = nullptr;
		if (Depth != nullptr && KeptDepth != nullptr && KeptDepth != Depth && KeptDraws > SegDraws)
		{
			// the last part of the frame was the smaller one (a first-person weapon): the world is the kept part
			Over = Depth;
			Depth = KeptDepth;
			SceneProj = KeptProj;
			SceneView = KeptView;
		}
		static int Told = 0;
		if (Depth == nullptr || !SceneProjOk)
		{
			if (Told++ < 3)
				Message("ssao: skipped (%s)", Depth == nullptr ? "the scene's depth isn't one of the readable ones yet" : "no perspective projection seen");
			return;
		}
		D3DSURFACE_DESC DD = {};
		Depth->GetLevelDesc(0, &DD);
		if (DD.Width != SceneW || DD.Height != SceneH || !SsaoBuild(Dev))
			return;
		if (SsaoVB == nullptr)
		{
			// the clip-space quad (as gi's)
			const SmaaVertex q[4] = { { -1, 1, 0, 0, 0 }, { 1, 1, 0, 1, 0 }, { -1, -1, 0, 0, 1 }, { 1, -1, 0, 1, 1 } };
			void *Mem = nullptr;
			if (FAILED(Dev->CreateVertexBuffer(sizeof(q), D3DUSAGE_WRITEONLY, D3DFVF_XYZ | D3DFVF_TEX1, D3DPOOL_MANAGED, &SsaoVB, nullptr))
				|| FAILED(SsaoVB->Lock(0, sizeof(q), &Mem, 0)))
			{
				if (SsaoVB) { SsaoVB->Release(); SsaoVB = nullptr; }
				Message("ssao: the quad couldn't be made, ambient occlusion off");
				SsaoBroken = true;
				return;
			}
			memcpy(Mem, q, sizeof(q));
			SsaoVB->Unlock();
		}
		const UINT W = (std::max)(SceneW / SsaoRes, 8u), H = (std::max)(SceneH / SsaoRes, 8u);
		if (SsaoOut == nullptr || SsaoW != SceneW || SsaoH != SceneH || SsaoDiv != SsaoRes)
		{
			SsaoReleaseTargets();
			const bool Ok = SUCCEEDED(Dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &SsaoGBuf, nullptr))
				&& SUCCEEDED(Dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &SsaoA, nullptr))
				&& SUCCEEDED(Dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &SsaoB, nullptr))
				&& SUCCEEDED(Dev->CreateTexture(SceneW, SceneH, 1, D3DUSAGE_RENDERTARGET, SceneFmt, D3DPOOL_DEFAULT, &SsaoOut, nullptr));
			if (!Ok)
			{
				Message("ssao: targets couldn't be made (%ux%u), ambient occlusion off", SceneW, SceneH);
				SsaoReleaseTargets();
				SsaoBroken = true;
				return;
			}
			SsaoW = SceneW;
			SsaoH = SceneH;
			SsaoDiv = SsaoRes;
		}
		const float SmallTex[4] = { 1.0f / W, 1.0f / H, (float)W, (float)H };
		const float full[4] = { 1.0f / SceneW, 1.0f / SceneH, (float)SceneW, (float)SceneH };
		const float proj[4] = { SceneProj._11, SceneProj._22, SceneProj._33, SceneProj._43 };
		// stage 3 isn't part of the post chain's saved state: put it back afterwards
		static const D3DSAMPLERSTATETYPE Samp3[6] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE };
		DWORD Old3[6] = {};
		IDirect3DBaseTexture9 *OldTex3 = nullptr;
		Dev->GetTexture(3, &OldTex3);
		for (int i = 0; i < 6; i++)
			Dev->GetSamplerState(3, Samp3[i], &Old3[i]);
		Dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
		Dev->SetStreamSource(0, SsaoVB, 0, sizeof(SmaaVertex));
		Dev->SetVertexShader(SsaoVS);
		Dev->SetVertexShaderConstantF(0, SmallTex, 1);
		Dev->SetPixelShaderConstantF(0, SmallTex, 1);
		Dev->SetPixelShaderConstantF(1, proj, 1);
		Dev->SetPixelShaderConstantF(3, full, 1);
		Dev->SetPixelShaderConstantF(4, SsaoFx, 1);
		for (DWORD t = 0; t < 4; t++)
			SmaaSampler(Dev, t, D3DTEXF_POINT);
		Dev->SetTexture(2, nullptr);
		Dev->SetTexture(3, nullptr);

		// 1: depth and normals
		Target(Dev, SsaoGBuf);
		Dev->SetPixelShader(SsaoPS[0]);
		Dev->SetTexture(0, Depth);
		Dev->SetTexture(1, Over);
		Dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
		Dev->SetTexture(1, nullptr);

		// 2: the occlusion
		Target(Dev, SsaoA);
		Dev->SetPixelShader(SsaoPS[1]);
		Dev->SetTexture(0, SsaoGBuf);
		Dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);

		// 3: blurred across, then down
		Dev->SetPixelShader(SsaoPS[2]);
		const float across[4] = { 1.0f / W, 0, 0, 0 }, down[4] = { 0, 1.0f / H, 0, 0 };
		Target(Dev, SsaoB);
		Dev->SetTexture(0, SsaoA);
		Dev->SetPixelShaderConstantF(2, across, 1);
		Dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
		Dev->SetTexture(0, nullptr);
		Target(Dev, SsaoA);
		Dev->SetTexture(0, SsaoB);
		Dev->SetPixelShaderConstantF(2, down, 1);
		Dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);

		// 4: the frame darkened, full size
		Target(Dev, SsaoOut);
		Dev->SetVertexShaderConstantF(0, full, 1);
		Dev->SetPixelShaderConstantF(0, full, 1);
		Dev->SetPixelShaderConstantF(2, SmallTex, 1);
		Dev->SetPixelShader(SsaoPS[3]);
		Dev->SetTexture(0, Depth);
		Dev->SetTexture(1, SceneTex);
		Dev->SetTexture(2, SsaoA);
		Dev->SetTexture(3, Over);
		Dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
		std::swap(SceneTex, SsaoOut);
		static int Ran = 0;
		if (Ran++ < 1)
			Message("ssao: running (%ux%u, AO %ux%u, strength %.2f, radius %.0f, intensity %.2f)",
				SceneW, SceneH, W, H, SsaoFx[0], SsaoFx[1], SsaoFx[2]);

		// back to the post chain's setup
		Dev->SetVertexShader(nullptr);
		Dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		Dev->SetStreamSource(0, PostQuadVB, 0, sizeof(U2QuadVertex));
		for (DWORD t = 0; t < 3; t++)
			SmaaSampler(Dev, t, D3DTEXF_LINEAR);
		Dev->SetTexture(0, nullptr);
		Dev->SetTexture(1, nullptr);
		Dev->SetTexture(2, nullptr);
		Dev->SetTexture(3, OldTex3);
		if (OldTex3) OldTex3->Release();
		for (int i = 0; i < 6; i++)
			Dev->SetSamplerState(3, Samp3[i], Old3[i]);
	}

	// ---- height fog and light shafts (atmos=1) ---------------------------------------------
	// atmos.hlsl on the frame copy, after ssao/sss and before SMAA, from the same readable depth:
	//   height fog: exponential in the world's height (Inigo Quilez, "better fog"): thick low
	//   down, thin up high, more of the sun's colour toward the sun; the sky gets the fog of
	//   atmosfog2's sky distance, so the horizon hazes and the zenith stays clear;
	//   light shafts: the sky's bright parts near the sun, at half size, blurred along lines
	//   toward the sun three times (Mitchell, GPU Gems 3 ch.13, as a 3-pass radial blur), added
	//   in the sun's colour; they fade out as the sun leaves the screen or goes behind.
	// The sun: the brightest directional light the game set for lit draws this frame (UE2's
	// Sunlight lights characters with one), or atmossun= azimuth elevation (degrees, world) to
	// set it by hand (per map with map= parts); none = no shafts, fog only.
	//   atmos=1                       on (default off)
	//   atmosfog=r g b density        fog colour (0..1) and density per world unit at the base
	//   atmosfog2=falloff height most sky   per-unit height falloff, the camera's height over the
	//                                 densest air (the fog layer moves with the camera: levels sit at
	//                                 any z; what lies below still gets thicker fog), most fog
	//                                 allowed (0..1), how far the sky counts as (world units)
	//   atmosshafts=strength length threshold suntint   shaft brightness, how far toward the
	//                                 sun the blur reaches (0..1 of the way), how bright sky must
	//                                 be to shine, how much the fog takes the sun's colour
	//   atmossun=az el                the sun by hand (0 0 = from the game's lights)
	//   atmosdebug=1|2|3              1 fog amount, 2 shafts alone, 3 the shaft mask
	bool Atmos = false, AtmosBroken = false;
	float AtmosFog[4] = { 0.62f, 0.66f, 0.72f, 0.00004f };
	float AtmosFog2[4] = { 0.0006f, 400.0f, 0.5f, 30000.0f };
	float AtmosShafts[4] = { 0.6f, 0.75f, 0.55f, 0.5f };
	float AtmosSun[2] = { 0, 0 };
	float AtmosDebug = 0;
	UINT AtmosW = 0, AtmosH = 0;
	IDirect3DVertexShader9 *AtmosVS = nullptr;
	IDirect3DPixelShader9 *AtmosPS[3] = {};
	IDirect3DTexture9 *AtmosA = nullptr, *AtmosB = nullptr, *AtmosOut = nullptr;
	bool AtmosToldSun = false;

	void AtmosReleaseTargets()
	{
		IDirect3DTexture9 **All[] = { &AtmosA, &AtmosB, &AtmosOut };
		for (IDirect3DTexture9 **T : All)
			if (*T) { (*T)->Release(); *T = nullptr; }
		AtmosW = AtmosH = 0;
	}

	void AtmosReleaseAll()
	{
		AtmosReleaseTargets();
		if (AtmosVS) { AtmosVS->Release(); AtmosVS = nullptr; }
		for (int i = 0; i < 3; i++)
			if (AtmosPS[i]) { AtmosPS[i]->Release(); AtmosPS[i] = nullptr; }
	}

	bool AtmosBuild(IDirect3DDevice9 *Dev)
	{
		if (AtmosBroken)
			return false;
		if (AtmosPS[2] != nullptr)
			return true;
		const std::string Lib = ReadShaderFile("atmos.hlsl");
		if (Lib.empty())
		{
			Message("atmos: atmos.hlsl missing in U2Shaders, fog and shafts off");
			AtmosBroken = true;
			return false;
		}
		static const char *Names[4] = { "AtmosVS", "MaskPS", "RayPS", "ApplyPS" };
		for (int i = 0; i < 4; i++)
		{
			char Head[96];
			sprintf_s(Head, "#define ATMOS_PASS %d\n#line 1 \"atmos.hlsl\"\n", i == 0 ? 1 : i);
			const std::string Src = std::string(Head) + Lib;
			ID3DBlob *Code = nullptr, *Errors = nullptr;
			const HRESULT hr = D3DCompile(Src.data(), Src.size(), "atmos", nullptr, nullptr, Names[i], i == 0 ? "vs_3_0" : "ps_3_0",
				D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &Code, &Errors);
			if (FAILED(hr) || Code == nullptr)
			{
				Message("atmos: %s failed: %s", Names[i], Errors ? (const char *)Errors->GetBufferPointer() : "?");
				if (Errors) Errors->Release();
				if (Code) Code->Release();
				AtmosBroken = true;
				return false;
			}
			if (Errors) Errors->Release();
			const HRESULT cr = i == 0 ? Dev->CreateVertexShader((const DWORD *)Code->GetBufferPointer(), &AtmosVS)
				: Dev->CreatePixelShader((const DWORD *)Code->GetBufferPointer(), &AtmosPS[i - 1]);
			Code->Release();
			if (FAILED(cr))
			{
				Message("atmos: the card refused %s (%08x)", Names[i], (unsigned)cr);
				AtmosBroken = true;
				return false;
			}
		}
		Message("atmos: ready");
		return true;
	}

	// the sun in world space (unit vector toward it) and its colour; false = none
	bool AtmosFindSun(float Dir[3], float Col[3])
	{
		if (AtmosSun[0] != 0 || AtmosSun[1] != 0)
		{
			const float Az = AtmosSun[0] * 0.0174533f, El = AtmosSun[1] * 0.0174533f;
			Dir[0] = cosf(El) * cosf(Az); Dir[1] = cosf(El) * sinf(Az); Dir[2] = sinf(El);
			Col[0] = 1.0f; Col[1] = 0.93f; Col[2] = 0.8f;
			return true;
		}
		const D3DLIGHT9 *Best = nullptr;
		float BestI = 0.05f;
		for (const D3DLIGHT9 &L : GiFrameLights)
			if (L.Type == D3DLIGHT_DIRECTIONAL)
			{
				const float I = L.Diffuse.r + L.Diffuse.g + L.Diffuse.b;
				if (I > BestI) { BestI = I; Best = &L; }
			}
		if (Best == nullptr)
			return false;
		const float x = -Best->Direction.x, y = -Best->Direction.y, z = -Best->Direction.z;
		const float n = sqrtf(x * x + y * y + z * z);
		if (n < 1e-6f)
			return false;
		Dir[0] = x / n; Dir[1] = y / n; Dir[2] = z / n;
		// the colour, brightest channel at 1 (how bright the shafts are is atmosshafts' job)
		const float m = (std::max)((std::max)(Best->Diffuse.r, Best->Diffuse.g), (std::max)(Best->Diffuse.b, 0.001f));
		Col[0] = Best->Diffuse.r / m; Col[1] = Best->Diffuse.g / m; Col[2] = Best->Diffuse.b / m;
		if (!AtmosToldSun)
		{
			AtmosToldSun = true;
			Message("atmos: the sun from the game's directional light: toward (%.2f %.2f %.2f), colour %.2f %.2f %.2f",
				Dir[0], Dir[1], Dir[2], Best->Diffuse.r, Best->Diffuse.g, Best->Diffuse.b);
		}
		return true;
	}

	// the passes; afterwards SceneTex is the fogged frame with the shafts (copy and result trade
	// places), and the post chain's own vertex setup is back, as after RunSsao
	void RunAtmos(IDirect3DDevice9 *Dev, IDirect3DSurface9 *BoundDepth)
	{
		if (!Atmos || AtmosBroken || SceneTex == nullptr)
			return;
		IDirect3DTexture9 *Depth = DepthTexOf(BoundDepth), *Over = nullptr;
		D3DMATRIX Proj = SceneProj, View = SceneView;
		if (Depth != nullptr && KeptDepth != nullptr && KeptDepth != Depth && KeptDraws > SegDraws)
		{
			Over = Depth;
			Depth = KeptDepth;
			Proj = KeptProj;
			View = KeptView;
		}
		static int Told = 0;
		if (Depth == nullptr || !SceneProjOk)
		{
			if (Told++ < 3)
				Message("atmos: skipped (%s)", Depth == nullptr ? "the scene's depth isn't one of the readable ones yet" : "no perspective projection seen");
			return;
		}
		D3DSURFACE_DESC DD = {};
		Depth->GetLevelDesc(0, &DD);
		if (DD.Width != SceneW || DD.Height != SceneH || !AtmosBuild(Dev))
			return;
		if (SsaoVB == nullptr)
		{
			const SmaaVertex q[4] = { { -1, 1, 0, 0, 0 }, { 1, 1, 0, 1, 0 }, { -1, -1, 0, 0, 1 }, { 1, -1, 0, 1, 1 } };
			void *Mem = nullptr;
			if (FAILED(Dev->CreateVertexBuffer(sizeof(q), D3DUSAGE_WRITEONLY, D3DFVF_XYZ | D3DFVF_TEX1, D3DPOOL_MANAGED, &SsaoVB, nullptr))
				|| FAILED(SsaoVB->Lock(0, sizeof(q), &Mem, 0)))
			{
				if (SsaoVB) { SsaoVB->Release(); SsaoVB = nullptr; }
				Message("atmos: the quad couldn't be made, fog and shafts off");
				AtmosBroken = true;
				return;
			}
			memcpy(Mem, q, sizeof(q));
			SsaoVB->Unlock();
		}
		const UINT W = (std::max)(SceneW / 2, 8u), H = (std::max)(SceneH / 2, 8u);
		if (AtmosOut == nullptr || AtmosW != SceneW || AtmosH != SceneH)
		{
			AtmosReleaseTargets();
			const bool Ok = SUCCEEDED(Dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &AtmosA, nullptr))
				&& SUCCEEDED(Dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &AtmosB, nullptr))
				&& SUCCEEDED(Dev->CreateTexture(SceneW, SceneH, 1, D3DUSAGE_RENDERTARGET, SceneFmt, D3DPOOL_DEFAULT, &AtmosOut, nullptr));
			if (!Ok)
			{
				Message("atmos: targets couldn't be made (%ux%u), fog and shafts off", SceneW, SceneH);
				AtmosReleaseTargets();
				AtmosBroken = true;
				return;
			}
			AtmosW = SceneW;
			AtmosH = SceneH;
		}
		// c1 projection, c3 frame size, c4-c6 settings, c7 sun on screen, c8 its colour,
		// c9 its direction (camera), c10-c12 camera -> world rows (as psreplace's c1-c3), c13 debug
		const D3DMATRIX Inv = InvView(View);
		float C[14][4] = {};
		C[1][0] = Proj._11; C[1][1] = Proj._22; C[1][2] = Proj._33; C[1][3] = Proj._43;
		C[3][0] = 1.0f / SceneW; C[3][1] = 1.0f / SceneH; C[3][2] = (float)SceneW; C[3][3] = (float)SceneH;
		memcpy(C[4], AtmosFog, sizeof(AtmosFog));
		memcpy(C[5], AtmosFog2, sizeof(AtmosFog2));
		memcpy(C[6], AtmosShafts, sizeof(AtmosShafts));
		for (int r = 0; r < 3; r++)
		{
			C[10 + r][0] = Inv.m[0][r]; C[10 + r][1] = Inv.m[1][r]; C[10 + r][2] = Inv.m[2][r]; C[10 + r][3] = Inv.m[3][r];
		}
		C[13][0] = AtmosDebug;
		float SunW[3] = {}, SunCol[3] = { 1, 1, 1 };
		float Vis = 0;
		if (AtmosFindSun(SunW, SunCol))
		{
			// world direction -> camera (the view's rotation)
			const float sx = SunW[0] * View._11 + SunW[1] * View._21 + SunW[2] * View._31;
			const float sy = SunW[0] * View._12 + SunW[1] * View._22 + SunW[2] * View._32;
			const float sz = SunW[0] * View._13 + SunW[1] * View._23 + SunW[2] * View._33;
			C[9][0] = sx; C[9][1] = sy; C[9][2] = sz; C[9][3] = 1;
			memcpy(C[8], SunCol, sizeof(SunCol));
			if (sz > 0.05f)
			{
				const float u = 0.5f + 0.5f * Proj._11 * sx / sz, v = 0.5f - 0.5f * Proj._22 * sy / sz;
				C[7][0] = u; C[7][1] = v;
				// fades as it leaves the screen (gone half a screen out) and as it turns away
				const float ox = (std::max)(0.0f, fabsf(u - 0.5f) - 0.5f), oy = (std::max)(0.0f, fabsf(v - 0.5f) - 0.5f);
				const float Out = (std::min)(1.0f, sqrtf(ox * ox + oy * oy) / 0.5f);
				Vis = (1 - Out) * (std::min)(1.0f, (sz - 0.05f) / 0.3f);
			}
		}
		C[7][2] = Vis;
		const bool Shafts = Vis > 0.001f && AtmosShafts[0] > 0;
		const float Small[4] = { 1.0f / W, 1.0f / H, (float)W, (float)H };
		const float Full[4] = { 1.0f / SceneW, 1.0f / SceneH, (float)SceneW, (float)SceneH };
		// stage 3 isn't part of the post chain's saved state: put it back afterwards
		static const D3DSAMPLERSTATETYPE Samp3[6] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE };
		DWORD Old3[6] = {};
		IDirect3DBaseTexture9 *OldTex3 = nullptr;
		Dev->GetTexture(3, &OldTex3);
		for (int i = 0; i < 6; i++)
			Dev->GetSamplerState(3, Samp3[i], &Old3[i]);
		Dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
		Dev->SetStreamSource(0, SsaoVB, 0, sizeof(SmaaVertex));
		Dev->SetVertexShader(AtmosVS);
		Dev->SetPixelShaderConstantF(1, C[1], 13);
		for (DWORD t = 0; t < 4; t++)
			SmaaSampler(Dev, t, D3DTEXF_POINT);
		Dev->SetTexture(2, nullptr);
		Dev->SetTexture(3, nullptr);
		if (Shafts)
		{
			// 1: the sky's bright parts around the sun, half size
			Dev->SetVertexShaderConstantF(0, Small, 1);
			Dev->SetPixelShaderConstantF(0, Small, 1);
			Target(Dev, AtmosA);
			Dev->SetPixelShader(AtmosPS[0]);
			Dev->SetTexture(0, Depth);
			Dev->SetTexture(1, SceneTex);
			Dev->SetTexture(2, Over);
			SmaaSampler(Dev, 1, D3DTEXF_LINEAR);
			Dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			Dev->SetTexture(1, nullptr);
			Dev->SetTexture(2, nullptr);
			// 2: three radial blurs toward the sun, each reaching further
			Dev->SetPixelShader(AtmosPS[1]);
			SmaaSampler(Dev, 0, D3DTEXF_LINEAR);
			IDirect3DTexture9 *From = AtmosA, *To = AtmosB;
			static const float Reach[3] = { 1.0f / 64, 1.0f / 8, 1.0f };
			for (int k = 0; k < 3; k++)
			{
				const float S[4] = { C[7][0], C[7][1], Vis, Reach[k] };
				Dev->SetPixelShaderConstantF(7, S, 1);
				Target(Dev, To);
				Dev->SetTexture(0, From);
				Dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
				std::swap(From, To);
			}
			Dev->SetTexture(0, nullptr);
			if (From != AtmosA)
				std::swap(AtmosA, AtmosB);          // AtmosA = the shafts
			const float S[4] = { C[7][0], C[7][1], Vis, 0 };
			Dev->SetPixelShaderConstantF(7, S, 1);
		}
		// 3: fog and shafts over the frame, full size
		Target(Dev, AtmosOut);
		Dev->SetVertexShaderConstantF(0, Full, 1);
		Dev->SetPixelShaderConstantF(0, Full, 1);
		Dev->SetPixelShaderConstantF(2, Small, 1);
		Dev->SetPixelShader(AtmosPS[2]);
		Dev->SetTexture(0, Depth);
		Dev->SetTexture(1, SceneTex);
		Dev->SetTexture(2, Shafts ? AtmosA : nullptr);
		Dev->SetTexture(3, Over);
		SmaaSampler(Dev, 2, D3DTEXF_LINEAR);
		Dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
		std::swap(SceneTex, AtmosOut);
		static int Ran = 0;
		if (Ran++ < 1)
			Message("atmos: running (%ux%u, shafts %ux%u, sun %s)", SceneW, SceneH, W, H, Shafts ? "on screen" : "not on screen or none");

		// back to the post chain's setup
		Dev->SetVertexShader(nullptr);
		Dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		Dev->SetStreamSource(0, PostQuadVB, 0, sizeof(U2QuadVertex));
		for (DWORD t = 0; t < 3; t++)
			SmaaSampler(Dev, t, D3DTEXF_LINEAR);
		Dev->SetTexture(0, nullptr);
		Dev->SetTexture(1, nullptr);
		Dev->SetTexture(2, nullptr);
		Dev->SetTexture(3, OldTex3);
		if (OldTex3) OldTex3->Release();
		for (int i = 0; i < 6; i++)
			Dev->SetSamplerState(3, Samp3[i], Old3[i]);
	}

	// ---- soft particles (soft=1) ---------------------------------------------------------
	// Smoke, dust, flames and sparks are flat sprites: where one cuts into the floor or a wall the
	// game draws a hard line. With soft=1 a blended, depth-tested, unlit fixed-function draw that
	// doesn't write depth (sprites, beams, translucent water and glass; not projector decals) is
	// drawn with soft.hlsl instead of the texture stages: it does what stage 0 did (texture and
	// vertex colour combined by the stage's own operation), then fades out where the scene behind
	// is within softparams' distance of the sprite, read from the scene's readable depth (gi,
	// ssao or atmos make it readable; nothing else needs it). The fade goes where the draw's
	// blending reads it: alpha for alpha blending, the colour for additive glow, toward "no
	// change" for multiply. Draws whose stage 0 does anything else (more stages, a texture factor,
	// complemented arguments) are left alone.
	//   soft=1                        on (default off)
	//   softparams=distance debug     the fade's reach in world units (default 40); debug 1 =
	//                                 soft draws in flat green
	bool Soft = false, SoftBroken = false, SoftTold = false;
	float SoftParams[2] = { 40.0f, 0.0f };
	U2Rule SoftRule;
	IDirect3DBaseTexture9 *SoftOldTex7 = nullptr;
	DWORD SoftOldSamp7[5] = {}, SoftOldTCI7 = 0, SoftOldTTF7 = 0;
	D3DMATRIX SoftOldTexMat7 = {};
	unsigned SoftDraws = 0, SoftSkipped = 0;

	// what a stage argument reads: 1 texture, 0 the vertex colour, -1 something we don't redo
	static int SoftArg(DWORD A)
	{
		if (A & ~D3DTA_SELECTMASK)
			return -1;                    // complement / alpha replicate
		if (A == D3DTA_TEXTURE)
			return 1;
		if (A == D3DTA_DIFFUSE || A == D3DTA_CURRENT)
			return 0;                     // stage 0's "current" is the vertex colour
		return -1;
	}
	// a stage operation as (op, arg1, arg2): op 0 arg1, 1 arg2, 2 multiply, 3 multiply x2; false = not ours
	static bool SoftOp(IDirect3DDevice9 *Dev, bool Alpha, float Out[4])
	{
		DWORD Op = 0, A1 = 0, A2 = 0;
		Dev->GetTextureStageState(0, Alpha ? D3DTSS_ALPHAOP : D3DTSS_COLOROP, &Op);
		Dev->GetTextureStageState(0, Alpha ? D3DTSS_ALPHAARG1 : D3DTSS_COLORARG1, &A1);
		Dev->GetTextureStageState(0, Alpha ? D3DTSS_ALPHAARG2 : D3DTSS_COLORARG2, &A2);
		const int a1 = SoftArg(A1), a2 = SoftArg(A2);
		switch (Op)
		{
		case D3DTOP_SELECTARG1: if (a1 < 0) return false; Out[0] = 0; break;
		case D3DTOP_SELECTARG2: if (a2 < 0) return false; Out[0] = 1; break;
		case D3DTOP_MODULATE: if (a1 < 0 || a2 < 0) return false; Out[0] = 2; break;
		case D3DTOP_MODULATE2X: if (a1 < 0 || a2 < 0) return false; Out[0] = 3; break;
		default: return false;
		}
		Out[1] = (float)(a1 > 0 ? 1 : 0);
		Out[2] = (float)(a2 > 0 ? 1 : 0);
		Out[3] = 0;
		return true;
	}

	bool SoftBegin(IDirect3DDevice9 *Dev, bool FixedFunction, bool Textured)
	{
		if (!Soft || SoftBroken || !FixedFunction || !Textured || Offscreen(Dev))
			return false;
		DWORD Blend = 0, Z = 0, ZW = 0, Lit = 0, Fvf = 0, Op1 = 0, Ttf0 = 0;
		Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &Blend);
		Dev->GetRenderState(D3DRS_ZENABLE, &Z);
		Dev->GetRenderState(D3DRS_ZWRITEENABLE, &ZW);
		Dev->GetRenderState(D3DRS_LIGHTING, &Lit);
		if (!Blend || !Z || ZW || Lit)
			return false;
		Dev->GetFVF(&Fvf);
		if ((Fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW)
			return false;
		D3DMATRIX P = {};
		Dev->GetTransform(D3DTS_PROJECTION, &P);
		if (P._34 != 1.0f || P._44 != 0.0f)
			return false;                 // not a perspective 3D draw
		Dev->GetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, &Ttf0);
		Dev->GetTextureStageState(1, D3DTSS_COLOROP, &Op1);
		if ((Ttf0 & D3DTTFF_PROJECTED) || Op1 != D3DTOP_DISABLE)
		{
			SoftSkipped++;
			return false;                 // projector decals, multi-stage draws
		}
		float C[8][4] = {};
		if (!SoftOp(Dev, false, C[0]) || !SoftOp(Dev, true, C[1]))
		{
			SoftSkipped++;
			return false;
		}
		// the scene's depth, readable (the bound depth surface is one of ours)
		IDirect3DSurface9 *DS = nullptr;
		if (FAILED(Dev->GetDepthStencilSurface(&DS)) || DS == nullptr)
			return false;
		IDirect3DTexture9 *Depth = DepthTexOf(DS);
		DS->Release();
		if (Depth == nullptr)
			return false;
		if (SoftRule.File.empty())
			SoftRule.File = "soft.hlsl";
		IDirect3DPixelShader9 *PS = Compile(Dev, SoftRule);
		if (PS == nullptr)
		{
			Message("soft: soft.hlsl missing or broken in U2Shaders, soft particles off");
			SoftBroken = true;
			return false;
		}
		// how the draw blends: where the fade has to go
		DWORD Src = 0, Dst = 0;
		Dev->GetRenderState(D3DRS_SRCBLEND, &Src);
		Dev->GetRenderState(D3DRS_DESTBLEND, &Dst);
		float Kind = 0;                                       // 0 alpha blending: fade the alpha
		if (Dst == D3DBLEND_ONE || (Src == D3DBLEND_ONE && Dst == D3DBLEND_INVSRCALPHA))
			Kind = 1;                                         // added (or premultiplied): fade colour and alpha
		else if ((Src == D3DBLEND_DESTCOLOR && Dst == D3DBLEND_ZERO) || (Src == D3DBLEND_ZERO && Dst == D3DBLEND_SRCCOLOR))
			Kind = 2;                                         // multiply: toward white
		else if ((Src == D3DBLEND_DESTCOLOR && Dst == D3DBLEND_SRCCOLOR) || (Src == D3DBLEND_SRCCOLOR && Dst == D3DBLEND_DESTCOLOR))
			Kind = 3;                                         // multiply x2: toward grey
		C[2][0] = P._11; C[2][1] = P._22; C[2][2] = P._33; C[2][3] = P._43;
		C[3][0] = (std::max)(SoftParams[0], 1.0f); C[3][1] = Kind; C[3][2] = SoftParams[1]; C[3][3] = 0;
		Dev->GetPixelShader(&OldPS);
		Dev->GetPixelShaderConstantF(0, OldConst[0], 8);
		Dev->SetPixelShaderConstantF(0, C[0], 4);
		// stage 7: the depth texture and the camera-space position
		static const D3DSAMPLERSTATETYPE Samp[5] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER };
		static const DWORD Val[5] = { D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE };
		Dev->GetTexture(7, &SoftOldTex7);
		for (int i = 0; i < 5; i++)
		{
			Dev->GetSamplerState(7, Samp[i], &SoftOldSamp7[i]);
			Dev->SetSamplerState(7, Samp[i], Val[i]);
		}
		Dev->SetTexture(7, Depth);
		static const D3DMATRIX Identity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
		Dev->GetTextureStageState(7, D3DTSS_TEXCOORDINDEX, &SoftOldTCI7);
		Dev->GetTextureStageState(7, D3DTSS_TEXTURETRANSFORMFLAGS, &SoftOldTTF7);
		Dev->GetTransform(D3DTS_TEXTURE7, &SoftOldTexMat7);
		Dev->SetTextureStageState(7, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION | 7);
		Dev->SetTextureStageState(7, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
		Dev->SetTransform(D3DTS_TEXTURE7, &Identity);
		Dev->SetPixelShader(PS);
		Mode = 9;
		SoftDraws++;
		if (!SoftTold)
		{
			SoftTold = true;
			Message("soft: first soft draw (blend %u/%u: kind %.0f, colour op %.0f, alpha op %.0f)", Src, Dst, Kind, C[0][0], C[1][0]);
		}
		return true;
	}
	void SoftEnd(IDirect3DDevice9 *Dev)
	{
		static const D3DSAMPLERSTATETYPE Samp[5] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER };
		Dev->SetPixelShader(OldPS);
		Dev->SetPixelShaderConstantF(0, OldConst[0], 8);
		if (OldPS != nullptr) { OldPS->Release(); OldPS = nullptr; }
		Dev->SetTexture(7, SoftOldTex7);
		if (SoftOldTex7) { SoftOldTex7->Release(); SoftOldTex7 = nullptr; }
		for (int i = 0; i < 5; i++)
			Dev->SetSamplerState(7, Samp[i], SoftOldSamp7[i]);
		Dev->SetTextureStageState(7, D3DTSS_TEXCOORDINDEX, SoftOldTCI7);
		Dev->SetTextureStageState(7, D3DTSS_TEXTURETRANSFORMFLAGS, SoftOldTTF7);
		Dev->SetTransform(D3DTS_TEXTURE7, &SoftOldTexMat7);
		Mode = 0;
	}

	// ---- subsurface scattering for skin (sss=1) -------------------------------------------
	// sss.hlsl on the frame copy, after ssao and before SMAA: a separable blur of the lit frame
	// under the skin. The skin is whatever a pbr= rule with its own shader (char_skin.hlsl,
	// PbrBlend) draws: while it draws, SkinMask is bound as render target 1 and the shader
	// writes 1 there (COLOR1), blended like the colour; the mask is cleared after each use.
	// sssfx=width strength falloff debug (default 1.2 0.7 1 0; width in world units).
	bool Sss = false, SssBroken = false;
	float SssFx[4] = { 1.2f, 0.7f, 1.0f, 0.0f };
	IDirect3DVertexShader9 *SssVS = nullptr;
	IDirect3DPixelShader9 *SssPS = nullptr;
	IDirect3DTexture9 *SkinMask = nullptr, *SssTmp = nullptr, *SssOut = nullptr;
	UINT SssW = 0, SssH = 0;
	bool SkinMaskBound = false, SkinMaskUsed = false;
	IDirect3DSurface9 *OldRT1 = nullptr;

	void SssReleaseTargets()
	{
		IDirect3DTexture9 **All[] = { &SkinMask, &SssTmp, &SssOut };
		for (IDirect3DTexture9 **T : All)
			if (*T) { (*T)->Release(); *T = nullptr; }
		SssW = SssH = 0;
	}
	void SssReleaseAll()
	{
		SssReleaseTargets();
		if (SssVS) { SssVS->Release(); SssVS = nullptr; }
		if (SssPS) { SssPS->Release(); SssPS = nullptr; }
	}
	bool SssBuild(IDirect3DDevice9 *Dev)
	{
		if (SssBroken)
			return false;
		if (SssPS != nullptr)
			return true;
		const std::string Src = ReadShaderFile("sss.hlsl");
		if (Src.empty())
		{
			Message("sss: sss.hlsl missing in U2Shaders, subsurface scattering off");
			SssBroken = true;
			return false;
		}
		static const char *Names[2] = { "SssVS", "SssPS" };
		for (int i = 0; i < 2; i++)
		{
			ID3DBlob *Code = nullptr, *Errors = nullptr;
			const HRESULT hr = D3DCompile(Src.data(), Src.size(), "sss", nullptr, nullptr, Names[i], i == 0 ? "vs_3_0" : "ps_3_0",
				D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &Code, &Errors);
			if (FAILED(hr) || Code == nullptr)
			{
				Message("sss: %s failed: %s", Names[i], Errors ? (const char *)Errors->GetBufferPointer() : "?");
				if (Errors) Errors->Release();
				SssBroken = true;
				return false;
			}
			if (Errors) Errors->Release();
			const HRESULT cr = i == 0 ? Dev->CreateVertexShader((const DWORD *)Code->GetBufferPointer(), &SssVS)
				: Dev->CreatePixelShader((const DWORD *)Code->GetBufferPointer(), &SssPS);
			Code->Release();
			if (FAILED(cr))
			{
				Message("sss: the card refused %s (%08x)", Names[i], (unsigned)cr);
				SssBroken = true;
				return false;
			}
		}
		Message("sss: ready");
		return true;
	}

	// the skin mask as render target 1 for a skin draw (CharBegin)
	void SkinMaskBegin(IDirect3DDevice9 *Dev)
	{
		if (!Sss || SssBroken || SkinMask == nullptr || SkinMaskBound)
			return;
		IDirect3DSurface9 *RT0 = nullptr, *M = nullptr;
		D3DSURFACE_DESC D0 = {};
		if (FAILED(Dev->GetRenderTarget(0, &RT0)) || RT0 == nullptr)
			return;
		RT0->GetDesc(&D0);
		RT0->Release();
		if (D0.Width != SssW || D0.Height != SssH || D0.MultiSampleType != D3DMULTISAMPLE_NONE)
			return;
		if (FAILED(SkinMask->GetSurfaceLevel(0, &M)) || M == nullptr)
			return;
		OldRT1 = nullptr;
		Dev->GetRenderTarget(1, &OldRT1);
		if (SUCCEEDED(Dev->SetRenderTarget(1, M)))
		{
			SkinMaskBound = true;
			SkinMaskUsed = true;
		}
		else if (OldRT1) { OldRT1->Release(); OldRT1 = nullptr; }
		M->Release();
	}
	void SkinMaskEnd(IDirect3DDevice9 *Dev)
	{
		if (!SkinMaskBound)
			return;
		Dev->SetRenderTarget(1, OldRT1);
		if (OldRT1) { OldRT1->Release(); OldRT1 = nullptr; }
		SkinMaskBound = false;
	}

	void RunSss(IDirect3DDevice9 *Dev, IDirect3DSurface9 *BoundDepth)
	{
		if (!Sss || SssBroken || SceneTex == nullptr)
			return;
		// the targets first (the mask has to exist before the next frame's skin draws)
		if (SkinMask == nullptr || SssW != SceneW || SssH != SceneH)
		{
			SssReleaseTargets();
			const bool Ok = SUCCEEDED(Dev->CreateTexture(SceneW, SceneH, 1, D3DUSAGE_RENDERTARGET, SceneFmt, D3DPOOL_DEFAULT, &SkinMask, nullptr))
				&& SUCCEEDED(Dev->CreateTexture(SceneW, SceneH, 1, D3DUSAGE_RENDERTARGET, SceneFmt, D3DPOOL_DEFAULT, &SssTmp, nullptr))
				&& SUCCEEDED(Dev->CreateTexture(SceneW, SceneH, 1, D3DUSAGE_RENDERTARGET, SceneFmt, D3DPOOL_DEFAULT, &SssOut, nullptr));
			if (!Ok)
			{
				Message("sss: targets couldn't be made (%ux%u), subsurface scattering off", SceneW, SceneH);
				SssReleaseTargets();
				SssBroken = true;
				return;
			}
			SssW = SceneW;
			SssH = SceneH;
			IDirect3DSurface9 *M = nullptr;
			if (SUCCEEDED(SkinMask->GetSurfaceLevel(0, &M)) && M) { Dev->ColorFill(M, nullptr, 0); M->Release(); }
			return;
		}
		IDirect3DTexture9 *Depth = DepthTexOf(BoundDepth);
		if (Depth != nullptr && KeptDepth != nullptr && KeptDepth != Depth && KeptDraws > SegDraws)
			Depth = KeptDepth;
		if (SsaoVB == nullptr && SkinMaskUsed)
		{
			// the clip-space quad (ssao's, made here when ssao is off)
			const SmaaVertex q[4] = { { -1, 1, 0, 0, 0 }, { 1, 1, 0, 1, 0 }, { -1, -1, 0, 0, 1 }, { 1, -1, 0, 1, 1 } };
			void *Mem = nullptr;
			if (SUCCEEDED(Dev->CreateVertexBuffer(sizeof(q), D3DUSAGE_WRITEONLY, D3DFVF_XYZ | D3DFVF_TEX1, D3DPOOL_MANAGED, &SsaoVB, nullptr))
				&& SUCCEEDED(SsaoVB->Lock(0, sizeof(q), &Mem, 0)))
			{
				memcpy(Mem, q, sizeof(q));
				SsaoVB->Unlock();
			}
			else if (SsaoVB) { SsaoVB->Release(); SsaoVB = nullptr; }
		}
		const bool Go = SkinMaskUsed && Depth != nullptr && SceneProjOk && SsaoVB != nullptr && SssBuild(Dev);
		if (Go)
		{
			const float full[4] = { 1.0f / SceneW, 1.0f / SceneH, (float)SceneW, (float)SceneH };
			const float proj[4] = { SceneProj._11, SceneProj._22, SceneProj._33, SceneProj._43 };
			const float across[4] = { 1, 0, 0, 0 }, down[4] = { 0, 1, 0, 0 };
			Dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
			Dev->SetStreamSource(0, SsaoVB, 0, sizeof(SmaaVertex));
			Dev->SetVertexShader(SssVS);
			Dev->SetVertexShaderConstantF(0, full, 1);
			Dev->SetPixelShaderConstantF(0, full, 1);
			Dev->SetPixelShaderConstantF(1, proj, 1);
			Dev->SetPixelShaderConstantF(3, SssFx, 1);
			Dev->SetPixelShader(SssPS);
			SmaaSampler(Dev, 0, D3DTEXF_LINEAR);
			SmaaSampler(Dev, 1, D3DTEXF_POINT);
			SmaaSampler(Dev, 2, D3DTEXF_POINT);
			Target(Dev, SssTmp);
			Dev->SetTexture(0, SceneTex);
			Dev->SetTexture(1, Depth);
			Dev->SetTexture(2, SkinMask);
			Dev->SetPixelShaderConstantF(2, across, 1);
			Dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			Target(Dev, SssOut);
			Dev->SetTexture(0, SssTmp);
			Dev->SetPixelShaderConstantF(2, down, 1);
			Dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
			std::swap(SceneTex, SssOut);
			static int Ran = 0;
			if (Ran++ < 1)
				Message("sss: running (%ux%u, width %.2f, strength %.2f, falloff %.2f)", SceneW, SceneH, SssFx[0], SssFx[1], SssFx[2]);
			// back to the post chain's setup
			Dev->SetVertexShader(nullptr);
			Dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
			Dev->SetStreamSource(0, PostQuadVB, 0, sizeof(U2QuadVertex));
			for (DWORD t = 0; t < 3; t++)
				SmaaSampler(Dev, t, D3DTEXF_LINEAR);
			Dev->SetTexture(0, nullptr);
			Dev->SetTexture(1, nullptr);
			Dev->SetTexture(2, nullptr);
		}
		else if (SkinMaskUsed)
		{
			static int Told = 0;
			if (Told++ < 3)
				Message("sss: skipped (%s)", Depth == nullptr ? "no readable depth" : SsaoVB == nullptr ? "needs ssao=1 for its quad" : "no projection / shader");
		}
		// the mask starts empty for the next frame
		if (SkinMaskUsed)
		{
			IDirect3DSurface9 *M = nullptr;
			if (SUCCEEDED(SkinMask->GetSurfaceLevel(0, &M)) && M) { Dev->ColorFill(M, nullptr, 0); M->Release(); }
			SkinMaskUsed = false;
		}
	}

	// ---- SMAA 1x (smaa=1 by default, smaa=0 off) -------------------------------------------
	// The reference SMAA.hlsl (Jimenez et al., MIT, github.com/iryoku/smaa) with our
	// smaa_passes.hlsl, on the game's frame copy before the final pass: edges from luma, blending
	// weights from the precomputed AreaTexDX9.dds / SearchTex.dds, then the frame blended along
	// the edges. All in U2Shaders\. Shader model 3 needs vertex shaders, so the passes draw their
	// own clip-space quad. Anything missing: SMAA stays off and the post chain runs as before.
	bool Smaa = true, SmaaBroken = false;
	IDirect3DVertexShader9 *SmaaVS[3] = {};
	IDirect3DPixelShader9 *SmaaPS[3] = {};
	IDirect3DTexture9 *SmaaArea = nullptr, *SmaaSearch = nullptr;              // managed
	IDirect3DTexture9 *SmaaEdges = nullptr, *SmaaBlend = nullptr, *SmaaOut = nullptr;   // targets
	IDirect3DVertexBuffer9 *SmaaVB = nullptr;
	UINT SmaaW = 0, SmaaH = 0;
	struct SmaaVertex { float x, y, z, u, v; };

	std::string ReadShaderFile(const char *Name)
	{
		std::string Data;
		FILE *F = nullptr;
		if (fopen_s(&F, (Dir + "U2Shaders\\" + Name).c_str(), "rb") || F == nullptr)
			return Data;
		char Buf[8192];
		size_t n;
		while ((n = fread(Buf, 1, sizeof(Buf), F)) > 0)
			Data.append(Buf, n);
		fclose(F);
		return Data;
	}

	// SMAA's lookup tables: A8L8 (area) and L8 (search) DDS files, put into A8R8G8B8 textures
	// (red = luminance, alpha = alpha), which SMAA's Direct3D 9 path reads as .ra and .r
	IDirect3DTexture9 *SmaaLookup(IDirect3DDevice9 *Dev, const char *Name, UINT Bytes)
	{
		const std::string D = ReadShaderFile(Name);
		if (D.size() < 128 + 4 || D.compare(0, 4, "DDS ") != 0)
		{
			Message("smaa: %s missing or not a DDS", Name);
			return nullptr;
		}
		const UINT H = *(const UINT *)(D.data() + 12), W = *(const UINT *)(D.data() + 16);
		if ((size_t)W * H * Bytes + 128 > D.size())
		{
			Message("smaa: %s is shorter than %ux%u", Name, W, H);
			return nullptr;
		}
		IDirect3DTexture9 *T = nullptr;
		D3DLOCKED_RECT L = {};
		if (FAILED(Dev->CreateTexture(W, H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &T, nullptr)) || T == nullptr)
			return nullptr;
		if (FAILED(T->LockRect(0, &L, nullptr, 0)))
		{
			T->Release();
			return nullptr;
		}
		const BYTE *In = (const BYTE *)D.data() + 128;
		for (UINT y = 0; y < H; y++)
			for (UINT x = 0; x < W; x++)
			{
				const BYTE *px = In + ((size_t)y * W + x) * Bytes;
				BYTE *o = (BYTE *)L.pBits + (size_t)y * L.Pitch + x * 4;
				o[0] = 0; o[1] = 0; o[2] = px[0];               // B G R: red = luminance
				o[3] = Bytes > 1 ? px[1] : 255;                 // alpha
			}
		T->UnlockRect(0);
		return T;
	}

	bool SmaaBuild(IDirect3DDevice9 *Dev)
	{
		if (SmaaBroken)
			return false;
		if (SmaaPS[2] != nullptr && SmaaArea != nullptr)
			return true;
		const std::string Lib = ReadShaderFile("SMAA.hlsl"), Passes = ReadShaderFile("smaa_passes.hlsl");
		if (Lib.empty() || Passes.empty())
		{
			Message("smaa: SMAA.hlsl or smaa_passes.hlsl missing in U2Shaders, SMAA off");
			SmaaBroken = true;
			return false;
		}
		static const char *VSName[3] = { "EdgeVS", "WeightVS", "BlendVS" }, *PSName[3] = { "EdgePS", "WeightPS", "BlendPS" };
		for (int i = 0; i < 3; i++)
		{
			char Head[256];
			sprintf_s(Head, "#define SMAA_HLSL_3 1\n#define SMAA_PRESET_HIGH 1\n#define SMAA_PASS %d\n"
				"float4 SmaaMetrics : register(c0);\n#define SMAA_RT_METRICS SmaaMetrics\n#line 1 \"SMAA.hlsl\"\n", i + 1);
			const std::string Src = std::string(Head) + Lib + "\n#line 1 \"smaa_passes.hlsl\"\n" + Passes;
			for (int k = 0; k < 2; k++)
			{
				ID3DBlob *Code = nullptr, *Errors = nullptr;
				const HRESULT hr = D3DCompile(Src.data(), Src.size(), "smaa", nullptr, nullptr, k ? PSName[i] : VSName[i],
					k ? "ps_3_0" : "vs_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &Code, &Errors);
				if (FAILED(hr) || Code == nullptr)
				{
					Message("smaa: %s failed: %s", k ? PSName[i] : VSName[i], Errors ? (const char *)Errors->GetBufferPointer() : "?");
					if (Errors) Errors->Release();
					if (Code) Code->Release();
					SmaaBroken = true;
					return false;
				}
				if (Errors) Errors->Release();
				const HRESULT cr = k ? Dev->CreatePixelShader((const DWORD *)Code->GetBufferPointer(), &SmaaPS[i])
					: Dev->CreateVertexShader((const DWORD *)Code->GetBufferPointer(), &SmaaVS[i]);
				Code->Release();
				if (FAILED(cr))
				{
					Message("smaa: the card refused %s (%08x)", k ? PSName[i] : VSName[i], (unsigned)cr);
					SmaaBroken = true;
					return false;
				}
			}
		}
		SmaaArea = SmaaLookup(Dev, "AreaTexDX9.dds", 2);
		SmaaSearch = SmaaLookup(Dev, "SearchTex.dds", 1);
		const SmaaVertex q[4] = { { -1, 1, 0, 0, 0 }, { 1, 1, 0, 1, 0 }, { -1, -1, 0, 0, 1 }, { 1, -1, 0, 1, 1 } };
		void *Mem = nullptr;
		if (SmaaArea == nullptr || SmaaSearch == nullptr
			|| FAILED(Dev->CreateVertexBuffer(sizeof(q), D3DUSAGE_WRITEONLY, D3DFVF_XYZ | D3DFVF_TEX1, D3DPOOL_MANAGED, &SmaaVB, nullptr))
			|| FAILED(SmaaVB->Lock(0, sizeof(q), &Mem, 0)))
		{
			Message("smaa: lookup textures or the quad couldn't be made, SMAA off");
			SmaaBroken = true;
			return false;
		}
		memcpy(Mem, q, sizeof(q));
		SmaaVB->Unlock();
		Message("smaa: ready (SMAA 1x, preset high)");
		return true;
	}

	void SmaaReleaseTargets()
	{
		for (IDirect3DTexture9 **T : { &SmaaEdges, &SmaaBlend, &SmaaOut })
			if (*T) { (*T)->Release(); *T = nullptr; }
		SmaaW = SmaaH = 0;
	}

	void SmaaReleaseAll()
	{
		SmaaReleaseTargets();
		for (int i = 0; i < 3; i++)
		{
			if (SmaaVS[i]) { SmaaVS[i]->Release(); SmaaVS[i] = nullptr; }
			if (SmaaPS[i]) { SmaaPS[i]->Release(); SmaaPS[i] = nullptr; }
		}
		if (SmaaArea) { SmaaArea->Release(); SmaaArea = nullptr; }
		if (SmaaSearch) { SmaaSearch->Release(); SmaaSearch = nullptr; }
		if (SmaaVB) { SmaaVB->Release(); SmaaVB = nullptr; }
	}

	static void SmaaSampler(IDirect3DDevice9 *Dev, DWORD s, D3DTEXTUREFILTERTYPE F)
	{
		Dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		Dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		Dev->SetSamplerState(s, D3DSAMP_MAGFILTER, F);
		Dev->SetSamplerState(s, D3DSAMP_MINFILTER, F);
		Dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		Dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
	}

	// the three passes on the frame copy; returns the anti-aliased frame (or nullptr: use the copy).
	// Leaves the post chain's own vertex setup (pre-transformed quad, no vertex shader) and
	// linear samplers on 0-2 behind it.
	IDirect3DTexture9 *RunSmaa(IDirect3DDevice9 *Dev)
	{
		if (!Smaa || SceneTex == nullptr || !SmaaBuild(Dev))
			return nullptr;
		if (SmaaOut == nullptr || SmaaW != SceneW || SmaaH != SceneH)
		{
			SmaaReleaseTargets();
			if (FAILED(Dev->CreateTexture(SceneW, SceneH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &SmaaEdges, nullptr))
				|| FAILED(Dev->CreateTexture(SceneW, SceneH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &SmaaBlend, nullptr))
				|| FAILED(Dev->CreateTexture(SceneW, SceneH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &SmaaOut, nullptr)))
			{
				Message("smaa: targets couldn't be made (%ux%u), SMAA off", SceneW, SceneH);
				SmaaReleaseTargets();
				SmaaBroken = true;
				return nullptr;
			}
			SmaaW = SceneW;
			SmaaH = SceneH;
		}
		const float m[4] = { 1.0f / SceneW, 1.0f / SceneH, (float)SceneW, (float)SceneH };
		Dev->SetFVF(D3DFVF_XYZ | D3DFVF_TEX1);
		Dev->SetStreamSource(0, SmaaVB, 0, sizeof(SmaaVertex));
		Dev->SetVertexShaderConstantF(0, m, 1);
		Dev->SetPixelShaderConstantF(0, m, 1);

		// 1: edges
		Target(Dev, SmaaEdges);
		Dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
		Dev->SetVertexShader(SmaaVS[0]);
		Dev->SetPixelShader(SmaaPS[0]);
		SmaaSampler(Dev, 0, D3DTEXF_POINT);
		Dev->SetTexture(0, SceneTex);
		Dev->SetTexture(1, nullptr);
		Dev->SetTexture(2, nullptr);
		Dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);

		// 2: blending weights
		Target(Dev, SmaaBlend);
		Dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
		Dev->SetVertexShader(SmaaVS[1]);
		Dev->SetPixelShader(SmaaPS[1]);
		SmaaSampler(Dev, 0, D3DTEXF_LINEAR);
		SmaaSampler(Dev, 1, D3DTEXF_LINEAR);
		SmaaSampler(Dev, 2, D3DTEXF_POINT);
		Dev->SetTexture(0, SmaaEdges);
		Dev->SetTexture(1, SmaaArea);
		Dev->SetTexture(2, SmaaSearch);
		Dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);

		// 3: the frame blended along the edges
		Target(Dev, SmaaOut);
		Dev->SetVertexShader(SmaaVS[2]);
		Dev->SetPixelShader(SmaaPS[2]);
		SmaaSampler(Dev, 0, D3DTEXF_LINEAR);
		SmaaSampler(Dev, 1, D3DTEXF_LINEAR);
		SmaaSampler(Dev, 2, D3DTEXF_LINEAR);
		Dev->SetTexture(0, SceneTex);
		Dev->SetTexture(1, SmaaBlend);
		Dev->SetTexture(2, nullptr);
		Dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);

		// back to the post chain's setup
		Dev->SetVertexShader(nullptr);
		Dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
		Dev->SetStreamSource(0, PostQuadVB, 0, sizeof(U2QuadVertex));
		Dev->SetTexture(1, nullptr);
		return SmaaOut;
	}

	// the lut= texture, loaded once (DDS through LoadDDS, or an uncompressed 24/32-bit BMP)
	IDirect3DTexture9 *PostLut(IDirect3DDevice9 *Dev)
	{
		if (LutTex != nullptr || LutTried || LutFile.empty())
			return LutTex;
		LutTried = true;
		std::string Ext = LutFile.size() > 4 ? LutFile.substr(LutFile.size() - 4) : "";
		if (_stricmp(Ext.c_str(), ".dds") == 0)
			LutTex = LoadDDS(Dev, LutFile);
		else
			LutTex = LoadBMP(Dev, LutFile);
		if (LutTex != nullptr)
		{
			D3DSURFACE_DESC D = {};
			LutTex->GetLevelDesc(0, &D);
			Message("post: lut %s loaded, %ux%u (%s)", LutFile.c_str(), D.Width, D.Height,
				D.Width == D.Height * D.Height ? "N slices of NxN" : "not N*N x N: check the layout");
		}
		return LutTex;
	}

	// an uncompressed 24- or 32-bit BMP from U2Shaders\ as a managed A8R8G8B8 texture
	IDirect3DTexture9 *LoadBMP(IDirect3DDevice9 *Dev, const std::string &File)
	{
		FILE *F = nullptr;
		if (fopen_s(&F, (Dir + "U2Shaders\\" + File).c_str(), "rb") || F == nullptr)
		{
			Message("post: %s not found", File.c_str());
			return nullptr;
		}
		std::string Data;
		char Buf[65536];
		size_t n;
		while ((n = fread(Buf, 1, sizeof(Buf), F)) > 0)
			Data.append(Buf, n);
		fclose(F);
		const BYTE *B = (const BYTE *)Data.data();
		if (Data.size() < 54 || B[0] != 'B' || B[1] != 'M')
		{
			Message("post: %s is not a BMP", File.c_str());
			return nullptr;
		}
		const DWORD Off = *(const DWORD *)(B + 10);
		const LONG W = *(const LONG *)(B + 18), H0 = *(const LONG *)(B + 22);
		const WORD Bpp = *(const WORD *)(B + 28);
		const DWORD Comp = *(const DWORD *)(B + 30);
		const LONG H = H0 < 0 ? -H0 : H0;
		const size_t Pitch = ((size_t)W * (Bpp / 8) + 3) & ~(size_t)3;
		if ((Bpp != 24 && Bpp != 32) || (Comp != 0 && Comp != 3) || W <= 0 || H <= 0 || Off + Pitch * H > Data.size())
		{
			Message("post: %s: only uncompressed 24/32-bit BMPs", File.c_str());
			return nullptr;
		}
		IDirect3DTexture9 *T = nullptr;
		if (FAILED(Dev->CreateTexture(W, H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &T, nullptr)) || T == nullptr)
			return nullptr;
		D3DLOCKED_RECT L;
		if (SUCCEEDED(T->LockRect(0, &L, nullptr, 0)))
		{
			for (LONG y = 0; y < H; y++)
			{
				const BYTE *Src = B + Off + Pitch * (H0 > 0 ? H - 1 - y : y);   // positive height = bottom-up rows
				DWORD *Dst = (DWORD *)((BYTE *)L.pBits + L.Pitch * y);
				for (LONG x = 0; x < W; x++)
				{
					const BYTE *P = Src + x * (Bpp / 8);
					Dst[x] = 0xFF000000u | (P[2] << 16) | (P[1] << 8) | P[0];
				}
			}
			T->UnlockRect(0);
		}
		return T;
	}

	void PostRelease()
	{
		if (BloomA) { BloomA->Release(); BloomA = nullptr; }
		if (BloomB) { BloomB->Release(); BloomB = nullptr; }
		ReleaseChain();
		SmaaReleaseTargets();
		GiReleaseTargets();
		for (auto &It : DepthSwap)
		{
			if (It.second.Tex) It.second.Tex->Release();
			if (It.second.Tex2) It.second.Tex2->Release();
		}
		DepthSwap.clear();
		KeptDepth = nullptr;
		for (MatchPair &M : DepthMatches)
			if (M.Surf) M.Surf->Release();
		DepthMatches.clear();
		for (MsaaProxy &P : MsaaProxies)
			if (P.Multi) P.Multi->Release();
		MsaaProxies.clear();
		MsaaBound = -1;
		DepthDirty = true;
		if (PostQuadVB) { PostQuadVB->Release(); PostQuadVB = nullptr; }
	}

	// ---- character probe (charprobe=1) -----------------------------------------------------
	// Character skins are opaque, so the per-surface rules above never see them. Before any
	// character lighting or self-shadowing work, this records how each opaque on-screen draw is
	// lit (fixed-function lighting, lights, material, ambient, skinning) and whether shadow
	// silhouettes were drawn earlier in the same frame. Written to U2Shaders\dump\chars.txt;
	// each texture is also saved once as <hash>_<w>x<h>.dds so a character's skin can be found.
	bool CharProbe = false;
	bool CharLight = false;

	// ---- lmcapture=1: the lightmapped geometry, for baking elsewhere ---------------------------
	// Every draw with a texture on stage 1 that stage 1 multiplies in (a lightmap) is recorded as
	// world-space triangles with their stage 1 (lightmap) coordinates, worked out the way
	// fixed-function Direct3D does (texture coordinate set or camera-space position, then the
	// texture transform). Triangles are kept once each, however often they are drawn. Every few
	// seconds the lot is written to U2Shaders\capture\scene.obj (one object per lightmap,
	// "o lm_<hash>"; one material per stage 0 texture, "usemtl tex_<hash>"; vt = lightmap
	// coordinates in Direct3D's convention, v down; faces in the order Direct3D shows as their
	// front) and lightmaps.txt ("<hash> <width> <height> <stage 1 op> <triangles>"); each
	// lightmap is saved once in U2Shaders\dump. The buffers' contents come from copies the
	// device keeps while capturing (vertex buffers may not be readable).
	bool Capture = false;
	struct U2LightmapCapture
	{
		UINT W = 0, H = 0;
		DWORD Op = 0;
		std::vector<float> Verts;    // x y z u v, 3 per triangle
		std::vector<DWORD> Tex;      // stage 0 hash, 1 per triangle
	};
	std::map<DWORD, U2LightmapCapture> Captured;
	std::unordered_set<unsigned long long> CapturedTris;
	bool CaptureDirty = false;
	unsigned CaptureWritten = 0;

	struct U2VertexLayout
	{
		int Pos = -1;                // byte offset of the float3 position, -1 = none (or pre-transformed)
		int Tex[8];                  // byte offset of each texture coordinate set, -1 = none
		int TexComps[8];
		U2VertexLayout() { for (int i = 0; i < 8; i++) { Tex[i] = -1; TexComps[i] = 0; } }
	};

	static U2VertexLayout LayoutOf(IDirect3DDevice9 *Dev)
	{
		U2VertexLayout L;
		IDirect3DVertexDeclaration9 *Decl = nullptr;
		if (SUCCEEDED(Dev->GetVertexDeclaration(&Decl)) && Decl != nullptr)
		{
			D3DVERTEXELEMENT9 E[MAXD3DDECLLENGTH + 1];
			UINT N = MAXD3DDECLLENGTH + 1;
			if (SUCCEEDED(Decl->GetDeclaration(E, &N)))
				for (UINT i = 0; i < N && E[i].Stream != 0xFF; i++)
				{
					if (E[i].Stream != 0)
						continue;
					if (E[i].Usage == D3DDECLUSAGE_POSITION && E[i].UsageIndex == 0 && E[i].Type == D3DDECLTYPE_FLOAT3)
						L.Pos = E[i].Offset;
					else if (E[i].Usage == D3DDECLUSAGE_TEXCOORD && E[i].UsageIndex < 8 && E[i].Type <= D3DDECLTYPE_FLOAT4)
					{
						L.Tex[E[i].UsageIndex] = E[i].Offset;
						L.TexComps[E[i].UsageIndex] = E[i].Type + 1;   // FLOAT1..FLOAT4 are 0..3
					}
				}
			Decl->Release();
			return L;
		}
		DWORD Fvf = 0;
		Dev->GetFVF(&Fvf);
		if ((Fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZ)
			return L;                   // pre-transformed or blended: not captured
		int At = 12;
		L.Pos = 0;
		if (Fvf & D3DFVF_NORMAL) At += 12;
		if (Fvf & D3DFVF_PSIZE) At += 4;
		if (Fvf & D3DFVF_DIFFUSE) At += 4;
		if (Fvf & D3DFVF_SPECULAR) At += 4;
		const int Count = (Fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
		for (int i = 0; i < Count && i < 8; i++)
		{
			static const int Comps[4] = { 2, 3, 4, 1 };   // D3DFVF_TEXTUREFORMAT2/3/4/1
			L.Tex[i] = At;
			L.TexComps[i] = Comps[(Fvf >> (16 + i * 2)) & 3];
			At += L.TexComps[i] * 4;
		}
		return L;
	}

	void CaptureDraw(IDirect3DDevice9 *Dev, D3DPRIMITIVETYPE Type, UINT PrimCount, const BYTE *Verts, size_t VertBytes, UINT Stride,
		INT BaseVertex, const BYTE *Indices, size_t IndexBytes, bool Index32, UINT StartIndex,
		IDirect3DTexture9 *Tex0, DWORD *Hash0, IDirect3DTexture9 *Tex1, DWORD &Hash1)
	{
		if (!Capture || Tex1 == nullptr || Stride == 0 || Offscreen(Dev))
			return;
		if (Type != D3DPT_TRIANGLELIST && Type != D3DPT_TRIANGLESTRIP && Type != D3DPT_TRIANGLEFAN)
			return;
		DWORD Op1 = 0, Blend = 0, Cull = 0;
		Dev->GetTextureStageState(1, D3DTSS_COLOROP, &Op1);
		Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &Blend);
		Dev->GetRenderState(D3DRS_CULLMODE, &Cull);
		IDirect3DVertexShader9 *VS = nullptr;
		Dev->GetVertexShader(&VS);
		if (VS != nullptr) { VS->Release(); return; }
		if (ModulateFactor(Op1) == 0 || Blend)
			return;
		const U2VertexLayout L = LayoutOf(Dev);
		if (L.Pos < 0)
			return;
		Known(Tex1, Hash1);
		if (Hash1 == 0xFFFFFFFF)
			return;
		DWORD H0 = 0;
		if (Tex0 != nullptr && Hash0 != nullptr)
		{
			Known(Tex0, *Hash0);
			H0 = *Hash0;
		}

		DWORD Tci = 0, Ttf = 0;
		Dev->GetTextureStageState(1, D3DTSS_TEXCOORDINDEX, &Tci);
		Dev->GetTextureStageState(1, D3DTSS_TEXTURETRANSFORMFLAGS, &Ttf);
		const DWORD Gen = Tci & 0xFFFF0000, Set = Tci & 0xFFFF;
		if ((Gen != D3DTSS_TCI_PASSTHRU && Gen != D3DTSS_TCI_CAMERASPACEPOSITION) || (Gen == D3DTSS_TCI_PASSTHRU && (Set >= 8 || L.Tex[Set] < 0)))
			return;
		D3DMATRIX W, V, T;
		Dev->GetTransform(D3DTS_WORLD, &W);
		Dev->GetTransform(D3DTS_VIEW, &V);
		Dev->GetTransform(D3DTS_TEXTURE1, &T);
		auto Mul = [](const float *In, const D3DMATRIX &M, float *Out) {
			for (int c = 0; c < 4; c++)
				Out[c] = In[0] * M.m[0][c] + In[1] * M.m[1][c] + In[2] * M.m[2][c] + In[3] * M.m[3][c];
		};

		U2LightmapCapture &C = Captured[Hash1];
		if (C.W == 0)
		{
			D3DSURFACE_DESC D = {};
			Tex1->GetLevelDesc(0, &D);
			C.W = D.Width; C.H = D.Height; C.Op = Op1;
			U2TexInfo Dummy;
			this->Hash(Tex1, Dummy, true);       // the lightmap itself, for comparing with the bake
		}

		// one vertex: world position and lightmap coordinates; false if out of the data
		auto Vertex = [&](UINT Index, float *Out) -> bool {
			const long long At = ((long long)BaseVertex + Index) * Stride;
			if (At < 0 || (size_t)At + Stride > VertBytes)
				return false;
			const BYTE *P = Verts + At;
			const float *Pos = reinterpret_cast<const float *>(P + L.Pos);
			const float Obj[4] = { Pos[0], Pos[1], Pos[2], 1 };
			float World[4], In[4] = { 0, 0, 0, 0 }, Uv[4];
			Mul(Obj, W, World);
			if (Gen == D3DTSS_TCI_CAMERASPACEPOSITION)
			{
				Mul(World, V, In);
				In[3] = 1;
			}
			else
			{
				const float *Tc = reinterpret_cast<const float *>(P + L.Tex[Set]);
				const int N = L.TexComps[Set];
				for (int c = 0; c < N; c++)
					In[c] = Tc[c];
				if (N < 4)
					In[N] = 1;                   // Direct3D pads (u, v) to (u, v, 1, 0) for the transform
			}
			const DWORD Count = Ttf & 0xFF;
			if (Count != D3DTTFF_DISABLE)
			{
				Mul(In, T, Uv);
				if ((Ttf & D3DTTFF_PROJECTED) && Count >= 2 && Uv[Count - 1] != 0)
				{
					Uv[0] /= Uv[Count - 1];
					Uv[1] /= Uv[Count - 1];
				}
			}
			else
			{
				Uv[0] = In[0]; Uv[1] = In[1];
			}
			Out[0] = World[0]; Out[1] = World[1]; Out[2] = World[2]; Out[3] = Uv[0]; Out[4] = Uv[1];
			return true;
		};
		auto IndexAt = [&](UINT k, UINT &Out) -> bool {
			if (Indices == nullptr) { Out = k; return true; }
			const size_t At = ((size_t)StartIndex + k) * (Index32 ? 4 : 2);
			if (At + (Index32 ? 4 : 2) > IndexBytes)
				return false;
			Out = Index32 ? *reinterpret_cast<const DWORD *>(Indices + At) : *reinterpret_cast<const WORD *>(Indices + At);
			return true;
		};

		for (UINT t = 0; t < PrimCount; t++)
		{
			UINT k[3];
			if (Type == D3DPT_TRIANGLELIST) { k[0] = t * 3; k[1] = t * 3 + 1; k[2] = t * 3 + 2; }
			else if (Type == D3DPT_TRIANGLESTRIP) { k[0] = t + (t & 1); k[1] = t + 1 - (t & 1); k[2] = t + 2; }
			else { k[0] = 0; k[1] = t + 1; k[2] = t + 2; }
			if (Cull == D3DCULL_CW)
				std::swap(k[1], k[2]);           // front faces are counter-clockwise here
			float Tri[15];
			UINT I;
			bool Ok = true;
			for (int c = 0; c < 3 && Ok; c++)
				Ok = IndexAt(k[c], I) && Vertex(I, Tri + c * 5);
			if (!Ok)
				return;
			unsigned long long Key = 1469598103934665603ull ^ Hash1;
			for (int c = 0; c < 15; c++)
			{
				const long long Q = (long long)floor(Tri[c] * (c % 5 < 3 ? 8.0f : 4096.0f) + 0.5f);
				Key = (Key ^ (unsigned long long)Q) * 1099511628211ull;
			}
			if (!CapturedTris.insert(Key).second)
				continue;
			C.Verts.insert(C.Verts.end(), Tri, Tri + 15);
			C.Tex.push_back(H0);
			CaptureDirty = true;
		}
	}

	void WriteCapture()
	{
		CaptureDirty = false;
		FILE *F = nullptr;
		if (fopen_s(&F, (Dir + "U2Shaders\\capture\\scene.obj").c_str(), "w") || F == nullptr)
			return;
		fprintf(F, "# U2Shaders lmcapture: world-space triangles of lightmapped draws (game units)\n");
		fprintf(F, "# vt = lightmap coordinates, Direct3D convention (v down); faces in Direct3D front-face order\n");
		unsigned N = 0, Tris = 0;
		for (const auto &It : Captured)
		{
			const U2LightmapCapture &C = It.second;
			fprintf(F, "o lm_%08x\n", It.first);
			DWORD Last = 0xFFFFFFFF;
			for (size_t t = 0; t < C.Tex.size(); t++)
			{
				if (C.Tex[t] != Last)
				{
					fprintf(F, "usemtl tex_%08x\n", C.Tex[t]);
					Last = C.Tex[t];
				}
				const float *P = &C.Verts[t * 15];
				for (int c = 0; c < 3; c++)
					fprintf(F, "v %.4f %.4f %.4f\nvt %.6f %.6f\n", P[c * 5], P[c * 5 + 1], P[c * 5 + 2], P[c * 5 + 3], P[c * 5 + 4]);
				fprintf(F, "f %u/%u %u/%u %u/%u\n", N + 1, N + 1, N + 2, N + 2, N + 3, N + 3);
				N += 3;
				Tris++;
			}
		}
		fclose(F);
		if (fopen_s(&F, (Dir + "U2Shaders\\capture\\lightmaps.txt").c_str(), "w") || F == nullptr)
			return;
		fprintf(F, "# hash width height stage1_op triangles   (op: 4 modulate, 5 modulate x2, 6 modulate x4)\n");
		for (const auto &It : Captured)
			fprintf(F, "%08x %u %u %u %u\n", It.first, It.second.W, It.second.H, It.second.Op, (unsigned)It.second.Tex.size());
		fclose(F);
		if (CaptureWritten++ % 20 == 0)
			Message("lmcapture: %u lightmaps, %u triangles written to U2Shaders\\capture", (unsigned)Captured.size(), Tris);
	}

	U2Rule CharRule;
	std::map<std::string, bool> CharRefused;   // stage/material setups charlight= logged and left alone
	float OldCharConst[32][4] = {};
	DWORD OldCharTCI[6] = {}, OldCharTTF[6] = {};
	D3DMATRIX OldCharTexMat[6] = {};
	DWORD CharGen = 2;         // the first of the two stages that carry normal and position
	unsigned MapsThisFrame = 0, ProbeFrames = 0, FramesWithMaps = 0;
	unsigned PassMap = 0, PassBlur = 0, PassProj = 0;   // pcssdebug>=5: shadow passes per 300 frames
	struct U2Probe { unsigned Draws = 0, AfterMaps = 0, FirstFrame = 0; std::string Sample; };
	std::map<std::string, U2Probe> Probes;
	std::map<DWORD, bool> ProbeDumped;

	// Screen-space contact shadows need the scene's depth as a texture, which Direct3D 9 only
	// offers through vendor formats. Asked once, then actually created: a wrapper (dgVoodoo)
	// may claim a format it can't make. Results go to U2Shaders.log.
	bool DepthChecked = false;
	void ProbeDepth(IDirect3DDevice9 *Dev)
	{
		DepthChecked = true;
		IDirect3D9 *D3D = nullptr;
		D3DDEVICE_CREATION_PARAMETERS CP = {};
		D3DDISPLAYMODE DM = {};
		if (FAILED(Dev->GetDirect3D(&D3D)) || D3D == nullptr || FAILED(Dev->GetCreationParameters(&CP)) || FAILED(Dev->GetDisplayMode(0, &DM)))
		{
			Message("depth probe: device queries failed");
			if (D3D) D3D->Release();
			return;
		}
		IDirect3DSurface9 *DS = nullptr;
		D3DSURFACE_DESC DD = {};
		if (SUCCEEDED(Dev->GetDepthStencilSurface(&DS)) && DS) { DS->GetDesc(&DD); DS->Release(); }
		Message("depth probe: scene depth format %u %ux%u multisample %u", (unsigned)DD.Format, DD.Width, DD.Height, (unsigned)DD.MultiSampleType);
		static const struct { const char *Name; D3DFORMAT Fmt; } Formats[] = {
			{ "INTZ", (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z') },
			{ "DF24", (D3DFORMAT)MAKEFOURCC('D', 'F', '2', '4') },
			{ "DF16", (D3DFORMAT)MAKEFOURCC('D', 'F', '1', '6') },
			{ "RAWZ", (D3DFORMAT)MAKEFOURCC('R', 'A', 'W', 'Z') },
		};
		for (const auto &F : Formats)
		{
			HRESULT hr = D3D->CheckDeviceFormat(CP.AdapterOrdinal, CP.DeviceType, DM.Format, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, F.Fmt);
			IDirect3DTexture9 *T = nullptr;
			HRESULT hc = Dev->CreateTexture(64, 64, 1, D3DUSAGE_DEPTHSTENCIL, F.Fmt, D3DPOOL_DEFAULT, &T, nullptr);
			Message("depth probe: %s reported %s, create %s", F.Name, SUCCEEDED(hr) ? "yes" : "no", SUCCEEDED(hc) && T ? "ok" : "failed");
			if (T) T->Release();
		}
		HRESULT hr = D3D->CheckDeviceFormat(CP.AdapterOrdinal, CP.DeviceType, DM.Format, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, (D3DFORMAT)MAKEFOURCC('R', 'E', 'S', 'Z'));
		Message("depth probe: RESZ (copy a multisampled depth) reported %s", SUCCEEDED(hr) ? "yes" : "no");
		D3D->Release();
	}

	void ProbeDraw(IDirect3DDevice9 *Dev, IDirect3DTexture9 *Tex, DWORD TexHash, bool FixedFunction)
	{
		if (!DepthChecked)
			ProbeDepth(Dev);
		DWORD blend = 0;
		Dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
		if (blend || Offscreen(Dev))
			return;
		DWORD fvf = 0, lighting = 0, ambient = 0, vblend = 0, ivb = 0, colorvertex = 0, diffsrc = 0, ambsrc = 0, spec = 0, tf = 0;
		Dev->GetFVF(&fvf);
		Dev->GetRenderState(D3DRS_LIGHTING, &lighting);
		Dev->GetRenderState(D3DRS_AMBIENT, &ambient);
		Dev->GetRenderState(D3DRS_VERTEXBLEND, &vblend);
		Dev->GetRenderState(D3DRS_INDEXEDVERTEXBLENDENABLE, &ivb);
		Dev->GetRenderState(D3DRS_COLORVERTEX, &colorvertex);
		Dev->GetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, &diffsrc);
		Dev->GetRenderState(D3DRS_AMBIENTMATERIALSOURCE, &ambsrc);
		Dev->GetRenderState(D3DRS_SPECULARENABLE, &spec);
		Dev->GetRenderState(D3DRS_TEXTUREFACTOR, &tf);
		DWORD op[3] = {}, a1[3] = {}, a2[3] = {};
		for (DWORD st = 0; st < 3; st++)
		{
			Dev->GetTextureStageState(st, D3DTSS_COLOROP, &op[st]);
			Dev->GetTextureStageState(st, D3DTSS_COLORARG1, &a1[st]);
			Dev->GetTextureStageState(st, D3DTSS_COLORARG2, &a2[st]);
		}
		int lights = 0;
		std::string ls;
		for (DWORD i = 0; i < 8; i++)
		{
			BOOL on = FALSE;
			D3DLIGHT9 L = {};
			if (FAILED(Dev->GetLightEnable(i, &on)) || !on || FAILED(Dev->GetLight(i, &L)))
				continue;
			lights++;
			char b[160];
			sprintf_s(b, " | L%u type %u dif %.2f %.2f %.2f amb %.2f %.2f %.2f range %.0f att %.3f %.5f",
				i, (unsigned)L.Type, L.Diffuse.r, L.Diffuse.g, L.Diffuse.b, L.Ambient.r, L.Ambient.g, L.Ambient.b,
				L.Range, L.Attenuation1, L.Attenuation2);
			ls += b;
		}
		D3DMATERIAL9 M = {};
		Dev->GetMaterial(&M);

		char key[300];
		sprintf_s(key, "%08x %s fvf %x lighting %u vblend %u/%u colorvertex %u src dif %u amb %u spec %u lights %d | st0 %u %x %x | st1 %u %x %x | st2 %u %x %x",
			TexHash, FixedFunction ? "ff" : "vs", fvf, lighting, vblend, ivb, colorvertex, diffsrc, ambsrc, spec, lights,
			op[0], a1[0], a2[0], op[1], a1[1], a2[1], op[2], a1[2], a2[2]);
		char sample[200];
		sprintf_s(sample, "ambient %08x tfactor %08x mat dif %.2f %.2f %.2f amb %.2f %.2f %.2f emi %.2f %.2f %.2f",
			ambient, tf, M.Diffuse.r, M.Diffuse.g, M.Diffuse.b, M.Ambient.r, M.Ambient.g, M.Ambient.b, M.Emissive.r, M.Emissive.g, M.Emissive.b);

		U2Probe &P = Probes[key];
		if (P.Draws++ == 0)
			P.FirstFrame = Frame;
		if (MapsThisFrame > 0)
			P.AfterMaps++;
		P.Sample = std::string(sample) + ls;
		if (!ProbeDumped[TexHash])
		{
			ProbeDumped[TexHash] = true;
			U2TexInfo Dummy;
			this->Hash(Tex, Dummy, true);
		}
	}

	// The in-game options page (U2SoftShadows.PostFXHelper) saves the post settings into
	// U2Shaders.ini while the game runs: every second the file's time is checked and, when it
	// changed, the post keys are read again (any case, any section: UnrealScript writes them
	// under [U2SoftShadows.PostFXHelper]). The rest of the file is only read at start.
	FILETIME IniTime = {};
	// shotp: the frame exactly as presented (after post), System\ShotP#####.bmp. The game's own
	// "shot" reads the back buffer before Present, so it misses post that runs at Present
	// (frames without a 2D draw: cutscenes). Asked for by bumping shotp= in U2Shaders.ini
	// (U2Pilot's "shotp" step); the ini is checked every 10 frames.
	unsigned ShotPSeen = 0xFFFFFFFFu;
	bool ShotPWant = false;
	void ShotP(IDirect3DDevice9 *Dev)
	{
		ShotPWant = false;
		IDirect3DSurface9 *BB = nullptr, *Plain = nullptr, *Sys = nullptr;
		if (FAILED(Dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &BB)) || BB == nullptr)
			return;
		D3DSURFACE_DESC D = {};
		BB->GetDesc(&D);
		IDirect3DSurface9 *Src = BB;
		if (D.MultiSampleType != D3DMULTISAMPLE_NONE)
		{
			// a multisampled back buffer can't be read directly: resolve it into a plain target
			if (SUCCEEDED(Dev->CreateRenderTarget(D.Width, D.Height, D.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &Plain, nullptr))
				&& SUCCEEDED(Dev->StretchRect(BB, nullptr, Plain, nullptr, D3DTEXF_NONE)))
				Src = Plain;
		}
		// msaa test (msaashot=1): the game's render texture, its stand-in resolved, instead
		if (!MsaaProxies.empty() && U2Msaa::ShotTest())
		{
			MsaaProxy &P = MsaaProxies[MsaaBound >= 0 ? MsaaBound : 0];
			{
				// a white triangle in the top left corner of the stand-in, to tell a sampling problem
				// in the game's drawing from one in this capture
				IDirect3DSurface9 *OldRT = nullptr, *OldDS = nullptr;
				IDirect3DStateBlock9 *SB = nullptr;
				if (SUCCEEDED(Dev->CreateStateBlock(D3DSBT_ALL, &SB)))
				{
					Dev->GetRenderTarget(0, &OldRT);
					Dev->GetDepthStencilSurface(&OldDS);
					Dev->SetRenderTarget(0, P.Multi);
					Dev->SetDepthStencilSurface(nullptr);
					struct V { float x, y, z, w; DWORD c; } Tri[3] = {
						{ 2, 2, 0.5f, 1, 0xFFFFFFFF }, { 182, 30, 0.5f, 1, 0xFFFFFFFF }, { 18, 183, 0.5f, 1, 0xFFFFFFFF } };
					Dev->SetVertexShader(nullptr);
					Dev->SetPixelShader(nullptr);
					Dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
					Dev->SetTexture(0, nullptr);
					Dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
					Dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
					Dev->SetRenderState(D3DRS_ZENABLE, FALSE);
					Dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
					Dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
					Dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
					Dev->SetRenderState(D3DRS_LIGHTING, FALSE);
					Dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
					Dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
					Dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, Tri, sizeof(V));
					Dev->SetRenderTarget(0, OldRT);
					Dev->SetDepthStencilSurface(OldDS);
					SB->Apply();
					SB->Release();
					if (OldRT) OldRT->Release();
					if (OldDS) OldDS->Release();
				}
			}
			Dev->StretchRect(P.Multi, nullptr, P.Plain, nullptr, D3DTEXF_NONE);
			Src = P.Plain;
			P.Plain->GetDesc(&D);
		}
		HRESULT hr = Dev->CreateOffscreenPlainSurface(D.Width, D.Height, D.Format, D3DPOOL_SYSTEMMEM, &Sys, nullptr);
		if (SUCCEEDED(hr))
			hr = U2PerfReadback(Dev, Src, Sys);
		D3DLOCKED_RECT L = {};
		if (SUCCEEDED(hr))
			hr = Sys->LockRect(&L, nullptr, D3DLOCK_READONLY);
		if (SUCCEEDED(hr))
		{
			char Path[MAX_PATH];
			static unsigned Next = 0;
			do
				snprintf(Path, sizeof(Path), "%sShotP%05u.bmp", Dir.c_str(), Next++);
			while (GetFileAttributesA(Path) != INVALID_FILE_ATTRIBUTES && Next < 100000);
			LastShotPath = Path;
			FILE *F = nullptr;
			if (!fopen_s(&F, Path, "wb") && F)
			{
				const DWORD Row = (D.Width * 3 + 3) & ~3u, Size = Row * D.Height;
				BITMAPFILEHEADER FH = {};
				BITMAPINFOHEADER IH = {};
				FH.bfType = 0x4D42;
				FH.bfOffBits = sizeof(FH) + sizeof(IH);
				FH.bfSize = FH.bfOffBits + Size;
				IH.biSize = sizeof(IH);
				IH.biWidth = (LONG)D.Width;
				IH.biHeight = (LONG)D.Height;          // bottom-up
				IH.biPlanes = 1;
				IH.biBitCount = 24;
				IH.biSizeImage = Size;
				fwrite(&FH, sizeof(FH), 1, F);
				fwrite(&IH, sizeof(IH), 1, F);
				std::vector<BYTE> Out(Row, 0);
				for (UINT y = D.Height; y-- > 0;)
				{
					const BYTE *In = static_cast<const BYTE *>(L.pBits) + (size_t)y * L.Pitch;
					for (UINT x = 0; x < D.Width; x++)       // X8R8G8B8 / A8R8G8B8: B G R x
					{
						Out[x * 3 + 0] = In[x * 4 + 0];
						Out[x * 3 + 1] = In[x * 4 + 1];
						Out[x * 3 + 2] = In[x * 4 + 2];
					}
					fwrite(Out.data(), Row, 1, F);
				}
				fclose(F);
			}
			Sys->UnlockRect();
		}
		else
			Message("shotp: couldn't read the back buffer (%08x, format %u)", (unsigned)hr, (unsigned)D.Format);
		if (Sys) Sys->Release();
		if (Plain) Plain->Release();
		BB->Release();
	}

	// ---- live look edits: U2Shaders.ini and the shaders reload while the game runs -------------
	// Every 10 frames the ini's time is checked (ReloadPost): when it changes, the post settings
	// are read again and so are the rules (ReloadRules: shader/glass/layer/decal/surface/pbr/
	// gloss/psreplace/replace/texgrade; compiled shaders and made textures are dropped and remade
	// on their next draw). Every 30 frames each shader file a rule uses is checked
	// (WatchShaders): an edited .hlsl is compiled again at its next draw.
	// Per-map settings: a line "map=NAME" starts a part of the ini that applies only while the
	// map's name contains NAME (case ignored); "map=*" ends it. The map's name is read from the
	// game's own log (the last "Browse: " line, as Unreal-engine games log a level load), the
	// log being <exe name>.log next to the exe unless maplog=FILE names another. So a level can
	// have its own grade=, lut=, colour=, bloom=, rules... without changing the others.
	std::string CurMap, MapLogFile;
	long long MapLogSize = 0;
	bool MapSkip(const char *Line, bool &InMap, bool &MapHit)
	{
		const char *L = Line + strspn(Line, " \t");
		if (_strnicmp(L, "map=", 4) == 0)
		{
			char N[128] = "";
			sscanf_s(L + 4, "%127s", N, (unsigned)sizeof(N));
			for (char *c = N; *c; c++)
				*c = (char)tolower((unsigned char)*c);
			InMap = strcmp(N, "*") != 0 && N[0] != 0;
			MapHit = InMap && !CurMap.empty() && CurMap.find(N) != std::string::npos;
			return true;
		}
		if (_strnicmp(L, "maplog=", 7) == 0)
		{
			char N[260] = "";
			sscanf_s(L + 7, "%259s", N, (unsigned)sizeof(N));
			MapLogFile = N;
			return true;
		}
		return InMap && !MapHit;
	}
	// The map's name from the engine itself (read only): the package holding the newest
	// LevelInfo object (the level's own actor; menu and entry levels have one too), through
	// Core.dll's exported object list. Checked every 60 frames; the log is the fallback.
	typedef const wchar_t *(__fastcall *ObjName_t)(void *This, void *Edx);
	typedef void *(__fastcall *ObjPtr_t)(void *This, void *Edx);
	struct ObjList { void **Data; int Num, Max; };
	ObjList *EngObjs = nullptr;
	ObjName_t EngName = nullptr;
	ObjPtr_t EngClass = nullptr, EngOuter = nullptr;
	bool EngTried = false;
	void *EngLevel = nullptr;
	int EngLevelAt = -1;
	static bool Readable(const void *P, SIZE_T N)
	{
		MEMORY_BASIC_INFORMATION Mi;
		if (!P || !VirtualQuery(P, &Mi, sizeof(Mi)) || Mi.State != MEM_COMMIT || (Mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
			return false;
		return (const BYTE *)P + N <= (const BYTE *)Mi.BaseAddress + Mi.RegionSize;
	}
	std::string EngineMap()
	{
		if (!EngTried)
		{
			EngTried = true;
			if (HMODULE Core = GetModuleHandleA("Core.dll"))
			{
				EngObjs = (ObjList *)GetProcAddress(Core, "?GObjObjects@UObject@@1V?$TArray@PAVUObject@@@@A");
				if (EngObjs == nullptr)            // Unreal II: private (0V)
					EngObjs = (ObjList *)GetProcAddress(Core, "?GObjObjects@UObject@@0V?$TArray@PAVUObject@@@@A");
				EngName = (ObjName_t)GetProcAddress(Core, "?GetName@UObject@@QBEPBGXZ");
				EngClass = (ObjPtr_t)GetProcAddress(Core, "?GetClass@UObject@@QBEPAVUClass@@XZ");
				EngOuter = (ObjPtr_t)GetProcAddress(Core, "?GetOuter@UObject@@QBEPAV1@XZ");
			}
			if (!EngObjs || !EngName || !EngClass || !EngOuter)
				EngObjs = nullptr;
		}
		if (EngObjs == nullptr || !Readable(EngObjs, sizeof(ObjList)))
			return "";
		// the one found last time still in its slot: the same map (a level change frees it)
		if (EngLevelAt >= 0 && EngLevelAt < EngObjs->Num && EngObjs->Data[EngLevelAt] == EngLevel)
			return CurMap;
		EngLevel = nullptr;
		EngLevelAt = -1;
		std::string Found;
		for (int i = EngObjs->Num - 1; i >= 0 && Found.empty(); i--)
		{
			void *O = EngObjs->Data[i];
			if (O == nullptr)
				continue;
			void *C = EngClass(O, nullptr);
			const wchar_t *CN = C ? EngName(C, nullptr) : nullptr;
			if (CN == nullptr)
				continue;
			const size_t L = wcslen(CN);
			if (L < 9 || wcscmp(CN + L - 9, L"LevelInfo") != 0)
				continue;
			// the outermost package is the map
			void *Pkg = O;
			for (int d = 0; d < 8; d++)
			{
				void *Up = EngOuter(Pkg, nullptr);
				if (Up == nullptr)
					break;
				Pkg = Up;
			}
			const wchar_t *PN = EngName(Pkg, nullptr);
			if (PN == nullptr || _wcsicmp(PN, L"Entry") == 0)
				continue;
			for (const wchar_t *c = PN; *c; c++)
				Found += (char)towlower(*c);
			EngLevel = O;
			EngLevelAt = i;
		}
		return Found;
	}
	// true when the game has gone to another map since the last look
	bool WatchMap()
	{
		if (Frame % 60 == 0)
		{
			std::string M = EngineMap();
			if (!M.empty())
			{
				if (M == CurMap)
					return false;
				CurMap = M;
				Message("live: map %s (from the engine; its map= parts of U2Shaders.ini apply)", CurMap.c_str());
				return true;
			}
		}
		else if (EngObjs != nullptr)
			return false;
		std::string Path = MapLogFile;
		if (Path.empty())
		{
			char Exe[MAX_PATH] = {};
			GetModuleFileNameA(nullptr, Exe, MAX_PATH);
			Path = Exe;
			Path = Path.substr(0, Path.find_last_of('.')) + ".log";
		}
		else if (Path.find(':') == std::string::npos)
			Path = Dir + Path;
		WIN32_FILE_ATTRIBUTE_DATA A = {};
		if (!GetFileAttributesExA(Path.c_str(), GetFileExInfoStandard, &A))
			return false;
		const long long Size = ((long long)A.nFileSizeHigh << 32) | A.nFileSizeLow;
		if (Size == MapLogSize)
			return false;
		const long long From = Size < MapLogSize ? 0 : MapLogSize;   // a new log: from the start
		MapLogSize = Size;
		HANDLE H = CreateFileA(Path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
		if (H == INVALID_HANDLE_VALUE)
			return false;
		const long long Want = (std::min)(Size - From, 262144LL);
		LARGE_INTEGER At; At.QuadPart = Size - Want;
		SetFilePointerEx(H, At, nullptr, FILE_BEGIN);
		std::string Buf((size_t)Want, '\0');
		DWORD Got = 0;
		ReadFile(H, &Buf[0], (DWORD)Want, &Got, nullptr);
		CloseHandle(H);
		Buf.resize(Got);
		// the log may be UTF-16 (Unreal Engine 2 logs often are): keep the low bytes
		if (Buf.size() > 1 && Buf.find('\0') != std::string::npos)
		{
			std::string N;
			for (char c : Buf)
				if (c != '\0')
					N += c;
			Buf.swap(N);
		}
		const size_t B = Buf.rfind("Browse: ");
		if (B == std::string::npos)
			return false;
		std::string Url = Buf.substr(B + 8, Buf.find_first_of("?\r\n \t", B + 8) - (B + 8));
		const size_t Slash = Url.find_last_of("/\\");
		if (Slash != std::string::npos)
			Url = Url.substr(Slash + 1);
		const size_t Dot = Url.find('.');
		if (Dot != std::string::npos)
			Url = Url.substr(0, Dot);
		for (char &c : Url)
			c = (char)tolower((unsigned char)c);
		if (Url.empty() || Url == CurMap)
			return false;
		CurMap = Url;
		Message("live: map %s (its map= parts of U2Shaders.ini apply)", CurMap.c_str());
		return true;
	}
	static void DropShader(U2Rule &R)
	{
		if (R.PS) { R.PS->Release(); R.PS = nullptr; }
		R.Tried = false;
	}
	void ReloadRules()
	{
		Warmed = false;
		for (U2Rule &R : Rules)
		{
			DropShader(R);
			if (R.Map) { R.Map->Release(); R.Map = nullptr; }
		}
		for (U2Rule &R : GlossRules)
			DropShader(R);
		for (U2Rule &R : SheenRules)
			DropShader(R);
		DropShader(SoftRule);
		for (auto &It : PsReplace)
			DropShader(It.second);
		for (auto &It : Replacements)
			if (It.second.Tex) { It.second.Tex->Release(); It.second.Tex = nullptr; }
		Rules.clear();
		GlossRules.clear();
		SheenRules.clear();
		PsReplace.clear();
		Replacements.clear();
		FILE *F = nullptr;
		if (fopen_s(&F, (Dir + "U2Shaders.ini").c_str(), "r") || F == nullptr)
			return;
		char Line[512];
		bool InMap = false, MapHit = false;
		while (fgets(Line, sizeof(Line), F))
			if (!MapSkip(Line, InMap, MapHit))
				ParseLine(Line, true);
		fclose(F);
		Message("live: U2Shaders.ini reloaded (%u rule(s), %u gloss, %u replacement(s), map %s)", (unsigned)Rules.size(), (unsigned)GlossRules.size(), (unsigned)Replacements.size(), CurMap.empty() ? "?" : CurMap.c_str());
	}
	// an edited shader file: every rule using it compiles it again at its next draw
	std::map<std::string, FILETIME> ShaderTimes;
	void WatchShaders()
	{
		std::vector<U2Rule *> All;
		for (U2Rule &R : Rules) All.push_back(&R);
		for (U2Rule &R : GlossRules) All.push_back(&R);
		for (U2Rule &R : SheenRules) All.push_back(&R);
		All.push_back(&SoftRule);
		for (auto &It : PsReplace) All.push_back(&It.second);
		U2Rule *Fixed[] = { &PostBright, &PostBlur, &PostFinal, &PostDown, &PostUp, &CharRule, &MapRule, &ProjRule };
		for (U2Rule *R : Fixed) All.push_back(R);
		std::set<std::string> Changed;
		for (U2Rule *R : All)
		{
			if (R->File.empty() || Changed.count(R->File))
				continue;
			WIN32_FILE_ATTRIBUTE_DATA A = {};
			if (!GetFileAttributesExA((Dir + "U2Shaders\\" + R->File).c_str(), GetFileExInfoStandard, &A))
				continue;
			auto It = ShaderTimes.find(R->File);
			if (It == ShaderTimes.end())
				ShaderTimes[R->File] = A.ftLastWriteTime;
			else if (CompareFileTime(&It->second, &A.ftLastWriteTime) != 0)
			{
				It->second = A.ftLastWriteTime;
				Changed.insert(R->File);
			}
		}
		if (Changed.empty())
			return;
		for (U2Rule *R : All)
			if (Changed.count(R->File))
				DropShader(*R);
		for (const std::string &F : Changed)
			Message("live: %s changed, compiled again at its next draw", F.c_str());
	}

	// System\U2GMPanel.txt present and empty (Truncate) or just present (created empty when missing)
	void GmPanelFileEmpty(bool Truncate)
	{
		if (Dir.empty())
			return;
		const std::string Path = Dir + "U2GMPanel.txt";
		if (!Truncate && GetFileAttributesA(Path.c_str()) != INVALID_FILE_ATTRIBUTES)
			return;
		FILE *F = nullptr;
		if (!fopen_s(&F, Path.c_str(), "w") && F)
			fclose(F);
	}
	// U2Shaders.ini without its runtime switches (pcss=, shotp=), plus the map its map= parts are read for
	std::string LastRuleText;
	std::string RuleText()
	{
		std::string T = CurMap + "\n";
		FILE *F = nullptr;
		if (fopen_s(&F, (Dir + "U2Shaders.ini").c_str(), "r") || F == nullptr)
			return T;
		char Line[512];
		while (fgets(Line, sizeof(Line), F))
		{
			const char *c = Line;
			while (*c == ' ' || *c == '\t')
				c++;
			if (_strnicmp(c, "pcss=", 5) == 0 || _strnicmp(c, "shotp=", 6) == 0)
				continue;
			T += c;
		}
		fclose(F);
		return T;
	}
	void ReloadPost(bool Force)
	{
		WIN32_FILE_ATTRIBUTE_DATA A = {};
		if (Dir.empty() || !GetFileAttributesExA((Dir + "U2Shaders.ini").c_str(), GetFileExInfoStandard, &A))
		{
			static int Told = 0;
			if (Told++ < 2)
				Message("post: can't watch %sU2Shaders.ini", Dir.c_str());
			return;
		}
		if (!Force && CompareFileTime(&A.ftLastWriteTime, &IniTime) == 0)
			return;
		if (Force)
			Message("post: watching U2Shaders.ini for changes (frame %u)", Frame);
		IniTime = A.ftLastWriteTime;
		// the rules again only when a rule line changed: the mod flips pcss= (indoors/outdoors) and the
		// pilot bumps shotp= while the game runs, and a full reload compiles every shader again (a hitch
		// of a second or more each time)
		std::string Text = RuleText();
		if (!Force && Text != LastRuleText)
			ReloadRules();
		LastRuleText = Text;
		FILE *F = nullptr;
		if (fopen_s(&F, (Dir + "U2Shaders.ini").c_str(), "r") || F == nullptr)
			return;
		char Line[512];
		bool Seen = false, InMap = false, MapHit = false;
		unsigned V = 0;
		while (fgets(Line, sizeof(Line), F))
		{
			if (MapSkip(Line, InMap, MapHit))
				continue;
			for (char *c = Line; *c; c++)
				*c = (char)tolower((unsigned char)*c);
			if (sscanf_s(Line, " shotp=%u", &V) == 1)
			{
				// a pilot "shotp" step bumped the counter: save the next presented frame
				if (ShotPSeen != 0xFFFFFFFFu && V != ShotPSeen)
					ShotPWant = true;
				ShotPSeen = V;
				continue;
			}
			if (sscanf_s(Line, " post=%u", &V) == 1)
			{
				Post = V != 0;
				Seen = true;
				PostBright.File = "post_bright.hlsl";
				PostBlur.File = "post_blur.hlsl";
				PostFinal.File = "post_final.hlsl";
				PostDown.File = "post_down.hlsl";
				PostUp.File = "post_up.hlsl";
			}
			else if (sscanf_s(Line, " smaa=%u", &V) == 1)
				Smaa = V != 0;
			else if (sscanf_s(Line, " pbrdebug=%f", &PbrDebug) == 1)
				;
			else if (sscanf_s(Line, " gi=%u", &V) == 1)
				Gi = V != 0;
			else if (sscanf_s(Line, " gifx=%f %f %f %f", &GiFx[0], &GiFx[1], &GiFx[2], &GiFx[3]) >= 1)
				;
			else if (sscanf_s(Line, " ssao=%u", &V) == 1)
			{
				if ((V != 0) != Ssao)
					DepthDirty = true;
				Ssao = V != 0;
			}
			else if (sscanf_s(Line, " ssaofx=%f %f %f %f", &SsaoFx[0], &SsaoFx[1], &SsaoFx[2], &SsaoFx[3]) >= 1)
				SsaoFx[1] = (std::max)(SsaoFx[1], 4.0f);
			else if (sscanf_s(Line, " ssaores=%u", &V) == 1)
				SsaoRes = V >= 2 ? 2 : 1;
			else if (sscanf_s(Line, " atmos=%u", &V) == 1)
			{
				if ((V != 0) != Atmos)
					DepthDirty = true;
				Atmos = V != 0;
			}
			else if (sscanf_s(Line, " atmosfog=%f %f %f %f", &AtmosFog[0], &AtmosFog[1], &AtmosFog[2], &AtmosFog[3]) >= 1)
				;
			else if (sscanf_s(Line, " atmosfog2=%f %f %f %f", &AtmosFog2[0], &AtmosFog2[1], &AtmosFog2[2], &AtmosFog2[3]) >= 1)
				;
			else if (sscanf_s(Line, " atmosshafts=%f %f %f %f", &AtmosShafts[0], &AtmosShafts[1], &AtmosShafts[2], &AtmosShafts[3]) >= 1)
				;
			else if (sscanf_s(Line, " atmossun=%f %f", &AtmosSun[0], &AtmosSun[1]) >= 1)
				AtmosToldSun = false;
			else if (sscanf_s(Line, " atmosdebug=%f", &AtmosDebug) == 1)
				;
			else if (sscanf_s(Line, " soft=%u", &V) == 1)
				Soft = V != 0;
			else if (sscanf_s(Line, " softparams=%f %f", &SoftParams[0], &SoftParams[1]) >= 1)
				;
			else if (sscanf_s(Line, " sss=%u", &V) == 1)
			{
				if ((V != 0) != Sss)
					DepthDirty = true;
				Sss = V != 0;
			}
			else if (sscanf_s(Line, " sssfx=%f %f %f %f", &SssFx[0], &SssFx[1], &SssFx[2], &SssFx[3]) >= 1)
				;
			else if (sscanf_s(Line, " gires=%u", &V) == 1)
				GiRes = V >= 2 ? 2 : 1;
			else if (sscanf_s(Line, " gilights=%f %d", &GiLightGain, &GiLightMax) >= 1)
				GiLightMax = (std::min)((std::max)(GiLightMax, 0), 32);
			else if (sscanf_s(Line, " gicache=%u", &V) == 1)
				GiCache = V != 0;
			else if (sscanf_s(Line, " gicell=%f", &GiCell) == 1)
			{
				GiCell = (std::min)((std::max)(GiCell, 16.0f), 512.0f);
				GiCacheFresh = true;
			}
			else if (sscanf_s(Line, " gibase=%f", &GiBase) == 1)
				GiBase = (std::min)((std::max)(GiBase, 2.0f), 32.0f);
			else if (sscanf_s(Line, " bloomchain=%u", &V) == 1)
				BloomChain = V != 0;
			else if (sscanf_s(Line, " bloomscatter=%f", &BloomScatter) == 1)
				BloomScatter = (std::min)((std::max)(BloomScatter, 0.0f), 1.0f);
			else if (sscanf_s(Line, " postsplit=%u", &V) == 1)
				PostSplit = V != 0 ? 1.0f : 0.0f;
			else if (sscanf_s(Line, " bloom=%f %f", &PostBloom[0], &PostBloom[1]) == 2)
				;
			else if (sscanf_s(Line, " colorblind=%f %f", &PostBloom[2], &PostBloom[3]) >= 1)
				;  // post_final: c1.z = 1 protanopia, 2 deuteranopia, 3 tritanopia; c1.w = strength
			else if (sscanf_s(Line, " grade=%f %f %f %f", &PostGrade[0], &PostGrade[1], &PostGrade[2], &PostGrade[3]) == 4)
				;
			else if (sscanf_s(Line, " colour=%f %f %f", &PostBalance[0], &PostBalance[1], &PostBalance[2]) == 3)
				;
			else if (sscanf_s(Line, " sharpen=%f", &PostBalance[3]) == 1)
				;
			else if (sscanf_s(Line, " postfx=%f %f %f %f", &PostFx[0], &PostFx[1], &PostFx[2], &PostFx[3]) >= 1)
				;
			else if (sscanf_s(Line, " terrainfx=%f %f %f %f", &TerrainFx[0], &TerrainFx[1], &TerrainFx[2], &TerrainFx[3]) >= 1)
				;
			else if (sscanf_s(Line, " terrainfog=%f %f %f %f", &TerrainFog[0], &TerrainFog[1], &TerrainFog[2], &TerrainFog[3]) >= 1)
				;
			else if (sscanf_s(Line, " terrainfog2=%f %f %f %f", &TerrainFog2[0], &TerrainFog2[1], &TerrainFog2[2], &TerrainFog2[3]) >= 1)
				;
			else if (sscanf_s(Line, " terrainfx2=%f %f %f %f", &TerrainFx2[0], &TerrainFx2[1], &TerrainFx2[2], &TerrainFx2[3]) >= 1)
				;
			else if (sscanf_s(Line, " terraintone=%f %f %f %f", &TerrainTone[0], &TerrainTone[1], &TerrainTone[2], &TerrainTone[3]) >= 1)
				;
			else if (sscanf_s(Line, " terraindetailfx=%f %f %f %f", &TerrainDetail[0], &TerrainDetail[1], &TerrainDetail[2], &TerrainDetail[3]) >= 1)
			{
				TerrainDetail[1] = (std::max)(TerrainDetail[1], 1.0f);
				TerrainDetail[2] = (std::max)(TerrainDetail[2], 1.0f);
				TerrainDetail[3] = (std::max)(TerrainDetail[3], 1.0f);
			}
			else if (strncmp(Line + strspn(Line, " \t"), "terraindetail=", 14) == 0)
			{
				char F[260] = "";
				sscanf_s(Line + strspn(Line, " \t") + 14, "%259s", F, (unsigned)sizeof(F));
				if (TerrainDetailFile != F)
				{
					TerrainDetailFile = F;
					if (TerrainDetailTex) { TerrainDetailTex->Release(); TerrainDetailTex = nullptr; }
					TerrainDetailTried = false;
				}
			}
			else if (strncmp(Line + strspn(Line, " \t"), "lut=", 4) == 0)
			{
				char F[260] = "";
				sscanf_s(Line + strspn(Line, " \t") + 4, "%259s", F, (unsigned)sizeof(F));
				if (LutFile != F)
				{
					LutFile = F;
					if (LutTex) { LutTex->Release(); LutTex = nullptr; }
		if (TerrainDetailTex) { TerrainDetailTex->Release(); TerrainDetailTex = nullptr; }
		TerrainDetailTried = false;
					LutTried = false;
				}
			}
			else if (sscanf_s(Line, " pcss=%u", &V) == 1 && (V != 0) != Pcss)
			{
				// live switch (Advent: contact hardening indoors only). Off: forget the maps
				// and snapshots, as after a lost device, so nothing stale is used when it's back
				Pcss = V != 0;
				MapRule.File = "pcss_map.hlsl";
				ProjRule.File = "pcss_proj.hlsl";
				if (!Pcss)
				{
					MapViews.clear();
					ClearBlurSources();
					MapTarget = nullptr;
					MapDirty = false;
					CopiedFor = nullptr;
				}
				Message("pcss: switched %s", Pcss ? "on" : "off");
			}
		}
		fclose(F);
		if (ShotPSeen == 0xFFFFFFFFu)
			ShotPSeen = 0;            // no shotp= yet: the pilot's first request will be shotp=1
		if (!Force)
			Message("post: settings reloaded (post %d, bloom %.2f %.2f, grade %.2f %.2f %.2f %.2f, sharpen %.2f)", (int)Post,
				PostBloom[0], PostBloom[1], PostGrade[0], PostGrade[1], PostGrade[2], PostGrade[3], PostBalance[3]);
	}

	void OnPresent(IDirect3DDevice9 *Dev)
	{
		U2Crash::Where("the layer's end-of-frame work (post, GI, panel)");
		if (Loaded && Frame > 5 && !Warmed)
			WarmShaders(Dev);
		if (Loaded && Frame % 10 == 0)
		{
			if (WatchMap())
				IniTime = {};                  // a new map: its map= sections apply (post settings and rules)
			ReloadPost(IniTime.dwLowDateTime == 0 && IniTime.dwHighDateTime == 0 && Frame < 20);
		}
		if (Loaded && Frame % 30 == 15)
			WatchShaders();
		if (Loaded)
			LivePoll(Dev);
		if (Loaded && GoreLinkOn)
			GoreLinkPoll();
		if (Loaded && GmTerrainOn && Frame % 10 == 5)
			GmPoll(Dev);
		if (U2Blood::Count > 0)
			U2Blood::Step(Dev);
		if (U2Runs::Count > 0)
			U2Runs::Step(Dev);
		if (U2Streaks::On || U2Streaks::HandsOn)
			U2Streaks::NewFrame();             // (marks itself for crash reports: "blood streaks")
		if (U2Lens::On)
			U2Lens::NewFrame();                // lens drops age and slide ("lens blood")
		if (U2Strings::On)
		{
			// a frame without a HUD draw: the goo strings now (before the post chain's own fallback
			// below); not with msaa, whose scene is resolved by now
			if (U2Strings::Saw3D && !U2Strings::Drawn && U2Strings::Live > 0 && U2Msaa::Wanted() == 0 && Dev != nullptr && !Offscreen(Dev))
			{
				Dev->BeginScene();
				U2Strings::Draw(Dev);
				Dev->EndScene();
			}
			U2Strings::NewFrame();
		}
		DepthDirty = true;
		SceneProjOk = false;
		GlossLights = GiFrameLights;             // gloss=: the game's lights of the frame just shown
		GiFrameLights.clear();
		GiSlotDirty = true;
		SegDraws = KeptDraws = 0;
		KeptDepth = nullptr;
		if (PostTrace > 0)
		{
			char end[96];
			snprintf(end, sizeof(end), "x%d | present: saw3d %d done %d offscreen %d", PostTraceRun, (int)Saw3D, (int)PostDone, LastDev ? (int)Offscreen(LastDev) : -1);
			PostTraceLine += end;
			PostTraceRun = 0;
			PostTraceLast.clear();
		}
		if (Post && Saw3D && !PostDone && LastDev != nullptr && !Offscreen(LastDev))
		{
			// no 2D draw this frame (no HUD): post-process the 3D frame now, in a scene of our own
			LastDev->BeginScene();
			RunPost(LastDev);
			LastDev->EndScene();
		}
		if (ShotPWant && Dev != nullptr)
		{
			if (ShotMask && MaskState == 0 && !MaskBroken)
				MaskState = 1;               // shotmask: the next frame draws the mask too, and is the one saved
			else
			{
				const bool WithMask = MaskState == 1;
				ShotP(Dev);
				if (WithMask)
					MaskSave(Dev);
			}
		}
		if (Loaded && Dev != nullptr)
			GmPanelFrame(Dev);                   // gmpanel=1: after the HUD and the shot, before Present
		if (PostTrace > 0 && !PostTraceLine.empty())
		{
			static int frame = 0;
			if (++frame % 45 == 0)       // every 45th frame
			{
				Message("posttrace: %s", PostTraceLine.c_str());
				PostTrace--;
			}
		}
		PostTraceLine.clear();
		Saw3D = PostDone = false;
		if (LightProbeFile)
			fprintf(LightProbeFile, "--- present, frame %u\n", Frame);
		Frame++;
		if (PcssDebug > 4.5 && Frame % 300 == 0)
		{
			Message("pcss passes, frames %u-%u: map %u blur %u projector %u\n", Frame - 300, Frame, PassMap, PassBlur, PassProj);
			PassMap = PassBlur = PassProj = 0;
		}
		RelightFrame();
		LightProbeCheck();
		if (Capture && CaptureDirty && Frame % 300 == 0)
			WriteCapture();
		if (CharProbe)
		{
			ProbeFrames++;
			if (MapsThisFrame > 0)
				FramesWithMaps++;
			MapsThisFrame = 0;
		}
		if ((Log || CharProbe) && Frame - LogFrame >= 300)
		{
			LogFrame = Frame;
			WriteSeen();
		}
	}

	void WriteSeen()
	{
		FILE *F = nullptr;
		if (CharProbe && !fopen_s(&F, (Dir + "U2Shaders\\dump\\chars.txt").c_str(), "w") && F)
		{
			fprintf(F, "frames %u, with shadow silhouettes drawn %u\n", ProbeFrames, FramesWithMaps);
			fprintf(F, "draws  after-maps  first-frame  texture / state\n                                  last sample\n");
			for (const auto &It : Probes)
				fprintf(F, "%6u  %6u  %8u  %s\n      %s\n", It.second.Draws, It.second.AfterMaps, It.second.FirstFrame,
					It.first.c_str(), It.second.Sample.c_str());
			fclose(F);
			F = nullptr;
		}
		if (fopen_s(&F, (Dir + "U2Shaders\\dump\\seen.txt").c_str(), "w") || F == nullptr)
			return;
		for (const auto &It : Seen)
			if (It.second.Draws)
				fprintf(F, "%08x %ux%u fmt %u draws %u\n", It.first, It.second.W, It.second.H, (unsigned)It.second.Fmt, It.second.Draws);
		fclose(F);
		if (fopen_s(&F, (Dir + "U2Shaders\\dump\\draws.txt").c_str(), "w") || F == nullptr)
			return;
		for (const auto &It : DrawKinds)
			fprintf(F, "%8u  %s\n", It.second, It.first.c_str());
		fclose(F);
	}

	// Before the device is reset or destroyed: default-pool objects must go
	void OnLost()
	{
		if (GmImDev != nullptr)
			ImGui_ImplDX9_InvalidateDeviceObjects();   // gmpanel: its buffers and font are default-pool
		if (SkGrabRT != nullptr)
		{
			SkGrabRT->Release();             // sketch: a copy not read back yet is lost: copied again
			SkGrabRT = nullptr;
		}
		if (SkGrabPending)
		{
			SkGrabPending = false;
			SkGrabWant = true;
			SkGrabAge = 0;
		}
		U2Blood::Release();
		U2Runs::Release();
		MapViews.clear();
		MapTarget = nullptr;
		ClearBlurSources();
		MapDirty = false;
		CopiedFor = nullptr;
		if (SceneTex != nullptr) { SceneTex->Release(); SceneTex = nullptr; }
		PostRelease();
		if (Log)
			WriteSeen();
	}

	void OnDestroy()
	{
		OnLost();
		U2Strings::Release();                    // its pixel shader belongs to this device
		U2Streaks::Release();                    // the same for the body-streak shader
		U2Lens::Release();                       // and the lens drops'
		for (auto &It : PsCache)                 // the compile cache's shaders too (and warm again on the next device)
			if (It.second != nullptr)
				It.second->Release();
		PsCache.clear();
		Warmed = false;
		for (U2Rule &R : Rules)
		{
			if (R.PS != nullptr) { R.PS->Release(); R.PS = nullptr; R.Tried = false; }
			if (R.Map != nullptr) { R.Map->Release(); R.Map = nullptr; }
			R.MapTried = false;
		}
		for (U2Rule *R : { &MapRule, &ProjRule, &PostBright, &PostBlur, &PostFinal, &PostDown, &PostUp })
			if (R->PS != nullptr) { R->PS->Release(); R->PS = nullptr; R->Tried = false; }
		// the decal/surface passes' shaders belong to this device too
		for (U2Rule &R : GlossRules)
			DropShader(R);
		for (U2Rule &R : SheenRules)
			DropShader(R);
		SmaaReleaseAll();
		SmaaBroken = false;
		GiReleaseAll();
		SsaoReleaseAll();
		AtmosReleaseAll();
		AtmosBroken = false;
		DropShader(SoftRule);
		SoftBroken = false;
		SssReleaseAll();
		SssBroken = false;
		GiBroken = false;
		DepthBroken = false;
		// managed textures survive a reset but belong to this device: the game makes a new device
		// on every fullscreen/windowed switch, and a texture from the old one bound to the new one
		// broke it (black screen, then a crash in the game's resource cleanup)
		if (LutTex) { LutTex->Release(); LutTex = nullptr; }
		if (TerrainDetailTex) { TerrainDetailTex->Release(); TerrainDetailTex = nullptr; }
		TerrainDetailTried = false;
		LutTried = false;
		for (auto &It : Replacements)
		{
			if (It.second.Tex) { It.second.Tex->Release(); It.second.Tex = nullptr; }
			It.second.Tried = false;
		}
		if (PostQuadVB) { PostQuadVB->Release(); PostQuadVB = nullptr; }
		if (SkTex) { SkTex->Release(); SkTex = nullptr; }   // sketch: made again from its pixels on the next device
		if (GmImDev != nullptr)
		{
			ImGui_ImplDX9_Shutdown();            // it holds a reference to the device
			GmImDev = nullptr;
		}
		LastDev = nullptr;
	}

private:
	bool WasFixedFunction = false;
};
