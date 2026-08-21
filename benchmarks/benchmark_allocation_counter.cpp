// Tracks heap allocations during benchmark measurement windows.
// Overrides allocation entry points only for the standalone benchmark executable.

#include "benchmark_allocation_counter.h"

#include <cstdlib>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace tick_synchronizer::benchmarks {
namespace {

thread_local bool allocation_counter_enabled = false;
thread_local AllocationSnapshot allocation_snapshot;

} // namespace

void AllocationCounter::begin() noexcept {
	allocation_snapshot = {};
	allocation_counter_enabled = true;
}


AllocationSnapshot AllocationCounter::end() noexcept {
	allocation_counter_enabled = false;
	return allocation_snapshot;
}


void AllocationCounter::record(std::size_t bytes) noexcept {
	if (allocation_counter_enabled) {
		++allocation_snapshot.allocation_count;
		allocation_snapshot.allocated_bytes += static_cast<std::uint64_t>(bytes);
	}
}

} // namespace tick_synchronizer::benchmarks

namespace {

void *allocate_unaligned(std::size_t size) noexcept {
	const std::size_t effective_size = size == 0 ? 1 : size;
	if (void *memory = std::malloc(effective_size)) {
		tick_synchronizer::benchmarks::AllocationCounter::record(effective_size);
		return memory;
	}
	return nullptr;
}

void *allocate_aligned(std::size_t size, std::size_t alignment) noexcept {
	const std::size_t effective_size = size == 0 ? 1 : size;
	void *memory = nullptr;
#if defined(_WIN32)
	memory = _aligned_malloc(effective_size, alignment);
#else
	if (posix_memalign(&memory, alignment, effective_size) != 0) {
		memory = nullptr;
	}
#endif
	if (memory == nullptr) {
		return nullptr;
	}
	tick_synchronizer::benchmarks::AllocationCounter::record(effective_size);
	return memory;
}


// Terminates after a failed allocation because benchmark exceptions are disabled.
[[noreturn]] void allocation_failure() noexcept {
	std::abort();
}


void deallocate_aligned(void *memory) noexcept {
#if defined(_WIN32)
	_aligned_free(memory);
#else
	std::free(memory);
#endif
}

} // namespace

void *operator new(std::size_t size) {
	void *memory = allocate_unaligned(size);
	if (memory == nullptr) {
		allocation_failure();
	}
	return memory;
}

void *operator new[](std::size_t size) {
	void *memory = allocate_unaligned(size);
	if (memory == nullptr) {
		allocation_failure();
	}
	return memory;
}


void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
	return allocate_unaligned(size);
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
	return allocate_unaligned(size);
}


void *operator new(std::size_t size, std::align_val_t alignment) {
	void *memory = allocate_aligned(size, static_cast<std::size_t>(alignment));
	if (memory == nullptr) {
		allocation_failure();
	}
	return memory;
}

void *operator new[](std::size_t size, std::align_val_t alignment) {
	void *memory = allocate_aligned(size, static_cast<std::size_t>(alignment));
	if (memory == nullptr) {
		allocation_failure();
	}
	return memory;
}


void *operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept {
	return allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void *operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept {
	return allocate_aligned(size, static_cast<std::size_t>(alignment));
}


void operator delete(void *memory) noexcept {
	std::free(memory);
}

void operator delete[](void *memory) noexcept {
	std::free(memory);
}


void operator delete(void *memory, std::size_t) noexcept {
	std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
	std::free(memory);
}


void operator delete(void *memory, std::align_val_t) noexcept {
	deallocate_aligned(memory);
}

void operator delete[](void *memory, std::align_val_t) noexcept {
	deallocate_aligned(memory);
}


void operator delete(void *memory, std::size_t, std::align_val_t) noexcept {
	deallocate_aligned(memory);
}

void operator delete[](void *memory, std::size_t, std::align_val_t) noexcept {
	deallocate_aligned(memory);
}
