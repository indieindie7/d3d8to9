#pragma once
/**
 * U2Perf - frame times, measured at Present: every 30 s one line in U2Shaders.log with the average
 * frame rate, the 1% low, the worst frame and how many frames went past 50 ms (a hitch you feel),
 * plus a line for each frame over 80 ms (a hitch), with the tick count (GetTickCount, the same
 * clock as AdventNative.log's line stamps), at most 20 of those per minute.
 *
 *   perf: 30 s, 1650 frames: avg 55.0 fps, 1% low 31.2 fps, worst 92 ms, 3 over 50 ms
 *   perf: layer 1.84 ms/frame (eof 0.21, post 1.02, draws 0.61), readback 0.00 ms/frame (0 reads), game 0.00 (0)
 *   perf: state calls/frame rs 812 (41% repeat), tss 655 (37%), samp 240 (52%), tex 390 (28%), 0.29 ms/frame
 *   perf: frame 18234 took 1310 ms (hitch)
 *
 * The second and third lines (OPTIMIZE.md) say where the layer's own CPU time goes and how much
 * of the game's state traffic repeats a value it set already:
 *   - layer: CPU time inside our own code. eof (end of frame) = the work at Present before the
 *     driver's Present (msaa resolve, texedit, U2Shaders::OnPresent minus a post chain run from
 *     there); post = RunPost (bloom, gi, ssao, sss, smaa, lens), wherever it runs; draws = the
 *     per-draw hook work around the game's draws (rule lookups, replace=, relight, pcss, the extra
 *     gloss/streak/mask/pick draws), sampled on 1 draw in 8 and scaled. Each part counts only
 *     its own time (a nested part is taken out of its parent), so the parts add up.
 *   - readback: time blocked in GPU->CPU reads (GetRenderTargetData) made by the layer: only
 *     screenshots, sketch grabs, texedit picks and debug dumps; 0 in normal play.
 *     game: the game's own reads through us (GetFrontBuffer, CopyRects from a video
 *     memory surface into system memory, LockRect on a render target or depth surface).
 *   - state calls (the game's): SetRenderState, SetTextureStageState (the sampler kinds - address,
 *     filter, mip, anisotropy, border - as "samp"), SetTexture, per frame, and the share that set
 *     the value the game itself had set last in that slot (a state block Apply or a Reset forgets
 *     them). The ms/frame is the CPU time inside those calls (ours + the runtime's +
 *     the driver's), sampled on 1 call in 8 and scaled: the most a redundancy filter could save.
 *
 * Always on (cheap: a counter and a compare per state call, a time stamp on 1 call or draw in 8).
 */
#include <windows.h>
#include <d3d9.h>
#include <intrin.h>
#include <algorithm>
#include <cstdio>
#include <vector>

// one game state slot: the value last set and the generation it was set in (Gen moves on when
// the slots are forgotten: state block Apply, Reset)
struct U2PerfSlot
{
	ULONG_PTR Value;
	unsigned Gen;
};

struct U2PerfCounters
{
	enum { Rs = 0, Tss = 1, Samp = 2, Tex = 3 };
	// time stamp counter ticks, converted to ms per perf window against QueryPerformanceCounter
	unsigned long long EndFrame = 0, Post = 0, Draw = 0, Read = 0, GameRead = 0, State = 0;
	unsigned long long Accounted = 0;     // every scope's whole time so far (a parent takes its children out)
	unsigned Reads = 0, GameReads = 0;
	char GameReadWhat[120] = {};           // what the game's last readback was (logged with the perf line)
	unsigned Calls[4] = {}, Repeats[4] = {};
	unsigned Gen = 1, Sample = 0, DrawSample = 0;
	bool Recording = false;               // the game is recording a state block: its Sets don't reach the device
	DWORD Behavior = 0;                   // the game's CreateDevice behaviour flags (pure device?)
	bool HaveBehavior = false;
	U2PerfSlot RsSlot[256] = {}, TssSlot[8][33] = {}, TexSlot[8] = {};

	void Forget() { Gen++; }
	// a game state call: counted, and whether it repeats the game's own last value in that slot
	void Note(int Kind, U2PerfSlot *S, ULONG_PTR Value)
	{
		Calls[Kind]++;
		if (S == nullptr || Recording)
			return;
		if (S->Gen == Gen && S->Value == Value)
			Repeats[Kind]++;
		S->Value = Value;
		S->Gen = Gen;
	}
};
// one instance for every file that includes this (C++14: no inline variables)
// lagfix=1 (default on): Advent's ReduceMouseLag locks one back-buffer pixel read-only every frame, which
// waits for the GPU to finish the whole frame (~3.6 ms/frame at 1080p). The lock is answered from a dummy
// pixel instead, and Present keeps the CPU at most one frame ahead with an event query: the same low mouse
// lag without stalling on the frame just submitted. lagfix=0: the game's own lock.
inline int &U2LagFix()
{
	static int On = 1;
	return On;
}

inline U2PerfCounters &U2PerfC()
{
	static U2PerfCounters C;
	return C;
}

// times its own part of the layer's work: elapsed minus the nested scopes' time, times Scale
// (Scale 0: not sampled this time, nothing measured; nested scopes still count for themselves)
class U2PerfScope
{
public:
	explicit U2PerfScope(unsigned long long &Into, unsigned Scale = 1) : Into(Into), Scale(Scale)
	{
		if (Scale == 0)
			return;
		Acc0 = U2PerfC().Accounted;
		T0 = __rdtsc();
	}
	~U2PerfScope()
	{
		if (Scale == 0)
			return;
		U2PerfCounters &C = U2PerfC();
		const unsigned long long Elapsed = __rdtsc() - T0;
		const unsigned long long Children = C.Accounted - Acc0;
		Into += (Elapsed > Children ? Elapsed - Children : 0) * Scale;
		C.Accounted = Acc0 + Elapsed;
	}
	U2PerfScope(const U2PerfScope &) = delete;
	U2PerfScope &operator=(const U2PerfScope &) = delete;
private:
	unsigned long long &Into;
	unsigned Scale;
	unsigned long long T0 = 0, Acc0 = 0;
};

// a draw's hook work: measured on 1 draw in 8 (scaled by 8)
inline unsigned U2PerfDrawScale()
{
	return (++U2PerfC().DrawSample & 7) == 0 ? 8u : 0u;
}
// a game state call: timed on 1 call in 8
inline unsigned U2PerfStateScale()
{
	return (++U2PerfC().Sample & 7) == 0 ? 8u : 0u;
}

// the layer's GPU->CPU read: GetRenderTargetData waits for the GPU to finish the source
inline HRESULT U2PerfReadback(IDirect3DDevice9 *Dev, IDirect3DSurface9 *Src, IDirect3DSurface9 *Dst)
{
	U2PerfC().Reads++;
	U2PerfScope S(U2PerfC().Read);
	return Dev->GetRenderTargetData(Src, Dst);
}

struct U2Perf
{
	LARGE_INTEGER Freq = {}, Last = {}, WindowStart = {}, MinuteStart = {};
	unsigned long long TscStart = 0;
	std::vector<float> Ms;
	unsigned Frame = 0, FreezesThisMinute = 0;
	bool ToldBehavior = false;

	template <class Log> void OnPresent(Log &&Message)
	{
		LARGE_INTEGER Now;
		QueryPerformanceCounter(&Now);
		if (Freq.QuadPart == 0)
		{
			QueryPerformanceFrequency(&Freq);
			Last = WindowStart = MinuteStart = Now;
			TscStart = __rdtsc();
			Reset();
			return;
		}
		Frame++;
		const float Dt = (float)((Now.QuadPart - Last.QuadPart) * 1000.0 / Freq.QuadPart);
		Last = Now;
		if (Dt > 0 && Dt < 60000)
			Ms.push_back(Dt);
		if ((Now.QuadPart - MinuteStart.QuadPart) / Freq.QuadPart >= 60)
		{
			MinuteStart = Now;
			FreezesThisMinute = 0;
		}
		if (Dt > 80 && Frame > 30 && FreezesThisMinute < 20)
		{
			FreezesThisMinute++;
			char B[96];
			sprintf_s(B, "perf: [%lu] frame %u took %.0f ms (hitch)", (unsigned long)GetTickCount(), Frame, Dt);
			Message(B);
		}
		const double Window = (double)(Now.QuadPart - WindowStart.QuadPart) / Freq.QuadPart;
		if (Window < 30 || Ms.size() < 10)
			return;
		std::vector<float> S = Ms;
		std::sort(S.begin(), S.end());
		double Sum = 0;
		unsigned Over = 0;
		for (float M : S)
		{
			Sum += M;
			if (M > 50)
				Over++;
		}
		const float P99 = S[(size_t)(S.size() * 0.99)];
		char B[400];
		sprintf_s(B, "perf: %.0f s, %u frames: avg %.1f fps, 1%% low %.1f fps, worst %.0f ms, %u over 50 ms",
			Window, (unsigned)S.size(), 1000.0 * S.size() / Sum, 1000.0 / P99, S.back(), Over);
		Message(B);

		// the layer's own time and the game's state traffic over the same window
		U2PerfCounters &C = U2PerfC();
		const unsigned long long TscNow = __rdtsc();
		const double TicksPerMs = (double)(TscNow - TscStart) / (Window * 1000.0);
		const double N = (double)S.size();
		auto PerFrame = [&](unsigned long long T) { return TicksPerMs > 0 ? T / TicksPerMs / N : 0.0; };
		const double EndFrame = PerFrame(C.EndFrame), Post = PerFrame(C.Post), Draw = PerFrame(C.Draw), Read = PerFrame(C.Read);
		sprintf_s(B, "perf: layer %.2f ms/frame (eof %.2f, post %.2f, draws %.2f), readback %.2f ms/frame (%u reads), game %.2f (%u)",
			EndFrame + Post + Draw + Read, EndFrame, Post, Draw, Read, C.Reads, PerFrame(C.GameRead), C.GameReads);
		if (C.GameReads > 0 && C.GameReadWhat[0])
		{
			char W[200];
			sprintf_s(W, "perf: the game's readback: %s", C.GameReadWhat);
			Message(W);
		}
		Message(B);
		auto Pct = [&](int k) { return C.Calls[k] ? 100.0 * C.Repeats[k] / C.Calls[k] : 0.0; };
		sprintf_s(B, "perf: state calls/frame rs %.0f (%.0f%% repeat), tss %.0f (%.0f%%), samp %.0f (%.0f%%), tex %.0f (%.0f%%), %.2f ms/frame",
			C.Calls[0] / N, Pct(0), C.Calls[1] / N, Pct(1), C.Calls[2] / N, Pct(2), C.Calls[3] / N, Pct(3), PerFrame(C.State));
		Message(B);
		if (!ToldBehavior && C.HaveBehavior)
		{
			ToldBehavior = true;
			sprintf_s(B, "perf: device behaviour flags %08lx: %s", (unsigned long)C.Behavior, (C.Behavior & D3DCREATE_PUREDEVICE)
				? "pure device (the runtime passes repeated states on to the driver)"
				: "not a pure device (the Direct3D 9 runtime drops repeated states itself)");
			Message(B);
		}
		Ms.clear();
		WindowStart = Now;
		TscStart = TscNow;
		Reset();
	}

	static void Reset()
	{
		U2PerfCounters &C = U2PerfC();
		C.EndFrame = C.Post = C.Draw = C.Read = C.GameRead = C.State = 0;
		C.Reads = C.GameReads = 0;
		for (int k = 0; k < 4; k++)
			C.Calls[k] = C.Repeats[k] = 0;
	}
};
