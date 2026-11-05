#include "sigscan.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <fstream>
#include <Windows.h>
#include <Psapi.h>

const sigscan &sigscan::get()
{
	static sigscan instance("GGST-Win64-Shipping.exe");
	return instance;
}

sigscan::sigscan(const char *name)
{
	auto module = GetModuleHandleA(name);
	if (module == nullptr) module = GetModuleHandleA(nullptr);  // Game Pass build is GGST-WinGDK-Shipping.exe
	if (module == nullptr)
		throw std::runtime_error("Module not found");

	MODULEINFO info;
	GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info));
	start = (uintptr_t)(info.lpBaseOfDll);
	end = start + info.SizeOfImage;

	// Executable sections from the PE headers, in address order
	const auto nt = (const IMAGE_NT_HEADERS*)(start + ((const IMAGE_DOS_HEADER*)start)->e_lfanew);
	const auto *sec = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
		if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
		const uintptr_t lo = start + sec->VirtualAddress;
		code.emplace_back(lo, (std::min)(lo + sec->Misc.VirtualSize, end));
	}
}

#pragma comment(lib,"user32")
uintptr_t sigscan::scan(const char *sig, const char *mask) const
{
	const size_t len = strlen(mask);
	const auto last_scan = end - len + 1;
	const auto matches = [&](uintptr_t addr) {
		for (size_t i = 0; i < len; i++)
			if (mask[i] != '?' && sig[i] != *(char*)(addr + i))
				return false;
		return true;
	};

	// Fast path: code sections only, jumping between copies of the first fixed byte
	size_t k = 0;
	while (k < len && mask[k] == '?') k++;
	if (k < len) {
		for (const auto &[lo, hi_sec] : code) {
			const uintptr_t hi = (std::min)(hi_sec, last_scan);  // candidate starts are in [lo, hi)
			for (uintptr_t p = lo + k; p < hi + k;) {
				const auto hit = (const char*)memchr((const void*)p, sig[k], hi + k - p);
				if (!hit) break;
				if (matches((uintptr_t)hit - k)) return (uintptr_t)hit - k;
				p = (uintptr_t)hit + 1;
			}
		}
	}

	// Not found in code, so fall back to the old full-image scan. A result can't get worse.
	for (auto addr = start; addr < last_scan; addr++)
		if (matches(addr))
			return addr;
	throw std::runtime_error("Sigscan failed");
}
