// texedit_compiletest.cpp - compiles texedit.hpp on its own (not part of the project; see texedit_hooks.md)
#include "texedit.hpp"
#include <cstdarg>

struct StubTex8
{
	IDirect3DTexture9 *P = nullptr;
	DWORD U2Hash = 0;
	IDirect3DTexture9 *GetProxyInterface() { return P; }
};
struct StubHost
{
	std::string Dir = "C:\\Game\\System\\", CurMap = "tuta";
	void Known(IDirect3DTexture9 *, DWORD &H) { if (H == 0) H = 0x1234; }
	long GmSend(const char *Fmt, ...) { char B[512]; va_list A; va_start(A, Fmt); vsnprintf(B, sizeof(B), Fmt, A); va_end(A); return 1; }
};

void TexEditCompileTest(IDirect3DDevice9 *Dev)
{
	StubHost H;
	StubTex8 *Stages[4] = {};
	U2TexEdit &T = TexEd();
	if (T.PickBegin(H, Dev, Stages))
		T.PickEnd(Dev);
	T.OnPresent(H, Dev);
	T.Adjusted(Dev, nullptr, 1, nullptr);
	if (!T.WorldClick())
		T.PanelUi(H);
	T.OnLost();
	(void)T.Active();
}
