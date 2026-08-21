// Enforces the standalone benchmark compiler-feature contract.
// Rejects builds that enable C++ exceptions or runtime type information.

#pragma once

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#error "TickSynchronizer standalone benchmarks require C++ exceptions to be disabled."
#endif

#if defined(__GXX_RTTI) || defined(_CPPRTTI)
#error "TickSynchronizer standalone benchmarks require RTTI to be disabled."
#endif
