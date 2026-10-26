// Tests that count allocations replace `operator new(size_t)` / `operator delete(void*)` with malloc/free. The library
// also uses the nothrow form (`std::stable_sort`'s temporary buffer): without this pairing, AddressSanitizer reports an
// alloc-dealloc mismatch. Include once per test executable.
#pragma once
#include <cstdlib>
#include <new>

void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return std::malloc(n ? n : 1); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
