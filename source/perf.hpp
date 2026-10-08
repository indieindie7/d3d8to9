#pragma once
/**
 * U2Perf - frame times, measured at Present: every 30 s one line in U2Shaders.log with the average
 * frame rate, the 1% low, the worst frame and how many frames went past 50 ms (a hitch you feel),
 * plus a line for each frame over 80 ms (a hitch), with the tick count (GetTickCount, the same
 * clock as AdventNative.log's line stamps), at most 20 of those per minute.
 *
 *   perf: 30 s, 1650 frames: avg 55.0 fps, 1% low 31.2 fps, worst 92 ms, 3 over 50 ms
 *   perf: frame 18234 took 1310 ms (hitch)
 *
 * Always on (one line per 30 s).
 */
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <vector>

struct U2Perf
{
	LARGE_INTEGER Freq = {}, Last = {}, WindowStart = {}, MinuteStart = {};
	std::vector<float> Ms;
	unsigned Frame = 0, FreezesThisMinute = 0;

	template <class Log> void OnPresent(Log &&Message)
	{
		LARGE_INTEGER Now;
		QueryPerformanceCounter(&Now);
		if (Freq.QuadPart == 0)
		{
			QueryPerformanceFrequency(&Freq);
			Last = WindowStart = MinuteStart = Now;
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
		char B[160];
		sprintf_s(B, "perf: %.0f s, %u frames: avg %.1f fps, 1%% low %.1f fps, worst %.0f ms, %u over 50 ms",
			Window, (unsigned)S.size(), 1000.0 * S.size() / Sum, 1000.0 / P99, S.back(), Over);
		Message(B);
		Ms.clear();
		WindowStart = Now;
	}
};
