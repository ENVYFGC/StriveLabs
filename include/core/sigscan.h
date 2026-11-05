#pragma once

#include <cstdint>
#include <utility>
#include <vector>

class sigscan
{
	uintptr_t start, end;
	std::vector<std::pair<uintptr_t, uintptr_t>> code;  // (executable sections, scanned first)

public:
	sigscan(const char *name);

	// Needs a singleton so it can be used in global initializers
	static const sigscan &get();

	uintptr_t scan(const char *sig, const char *mask) const;
};

inline void *get_rip_relative(uintptr_t offset)
{
	return (void*)(offset + 4 + *(int32_t*)offset);
}
