#pragma once
/**
 * U2Crash - when the game dies through the C runtime ("Runtime Error! This application has requested
 * the Runtime to terminate it in an unusual way": abort(), an uncaught C++ exception, a pure virtual
 * call), write what the d3d8 layer was doing, the process's free address space and a minidump before
 * the runtime's dialog. Unreal's own crashes (its General Protection Fault box) don't come through
 * here; those are in the game's log.
 *
 *   U2Shaders.log: "crash: <kind> while <where>, free address space N MB, largest block N MB"
 *   U2Shaders\crash-<time>.dmp: a minidump (stacks, modules, thread info) for a debugger
 *
 * U2Crash::Where("...") marks what the layer is doing (a string literal, kept as a pointer).
 */
#include <windows.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

namespace U2Crash
{
	inline const char *volatile &WhereSlot() { static const char *volatile W = "start"; return W; }
	inline std::string &DirSlot() { static std::string D; return D; }
	inline void Where(const char *W) { WhereSlot() = W; }

	// the free address space of this 32-bit process: total, and the largest block (fragmentation)
	inline void FreeSpace(unsigned &TotalMB, unsigned &LargestMB)
	{
		TotalMB = LargestMB = 0;
		MEMORY_BASIC_INFORMATION M;
		unsigned long long Total = 0, Largest = 0;
		for (BYTE *P = nullptr; VirtualQuery(P, &M, sizeof(M)) == sizeof(M); P = (BYTE *)M.BaseAddress + M.RegionSize)
		{
			if (M.State == MEM_FREE)
			{
				Total += M.RegionSize;
				if (M.RegionSize > Largest)
					Largest = M.RegionSize;
			}
			if ((BYTE *)M.BaseAddress + M.RegionSize <= P)
				break;
		}
		TotalMB = (unsigned)(Total >> 20);
		LargestMB = (unsigned)(Largest >> 20);
	}

	typedef BOOL (WINAPI *MiniDump_t)(HANDLE, DWORD, HANDLE, int, void *, void *, void *);

	inline void Report(const char *Kind)
	{
		static volatile LONG Once = 0;
		if (InterlockedExchange(&Once, 1))
			return;
		unsigned Total, Largest;
		FreeSpace(Total, Largest);
		SYSTEMTIME T;
		GetLocalTime(&T);
		char Stamp[32];
		sprintf_s(Stamp, "%04u%02u%02u-%02u%02u%02u", T.wYear, T.wMonth, T.wDay, T.wHour, T.wMinute, T.wSecond);
		const std::string &Dir = DirSlot();
		FILE *F = nullptr;
		if (!fopen_s(&F, (Dir + "U2Shaders.log").c_str(), "a") && F)
		{
			fprintf(F, "crash: %s while %s, free address space %u MB, largest block %u MB, dump U2Shaders\\crash-%s.dmp\n",
				Kind, WhereSlot(), Total, Largest, Stamp);
			fclose(F);
		}
		if (HMODULE Dbg = LoadLibraryA("dbghelp.dll"))
			if (MiniDump_t Write = (MiniDump_t)GetProcAddress(Dbg, "MiniDumpWriteDump"))
			{
				const std::string Path = Dir + "U2Shaders\\crash-" + Stamp + ".dmp";
				HANDLE H = CreateFileA(Path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (H != INVALID_HANDLE_VALUE)
				{
					// MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs
					Write(GetCurrentProcess(), GetCurrentProcessId(), H, 0x1000 | 0x40 | 0x1, nullptr, nullptr, nullptr);
					CloseHandle(H);
				}
			}
	}

	inline void OnAbort(int) { Report("abort()"); }
	inline void OnTerminate()
	{
		const char *Kind = "an uncaught C++ exception (std::terminate)";
		try
		{
			if (std::exception_ptr E = std::current_exception())
				std::rethrow_exception(E);
		}
		catch (const std::bad_alloc &) { Kind = "out of memory (std::bad_alloc, uncaught)"; }
		catch (const std::exception &) { Kind = "an uncaught std::exception"; }
		catch (...) {}
		Report(Kind);
		abort();
	}
	inline void OnPureCall() { Report("a pure virtual function call"); abort(); }

	inline void Install(const std::string &Dir)
	{
		DirSlot() = Dir;
		signal(SIGABRT, OnAbort);
		std::set_terminate(OnTerminate);
		_set_purecall_handler(OnPureCall);
	}
}
