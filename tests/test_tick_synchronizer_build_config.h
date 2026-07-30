#pragma once

#include "src/internal/tick_synchronizer_build_config.h"
#include "tests/test_macros.h"

namespace TestTickSynchronizerBuildConfig {

TEST_CASE("[Modules][TickSynchronizer][Build] C++17 language baseline") {
	CHECK(TICK_SYNCHRONIZER_CXX_STANDARD == 201703L);

#if defined(_MSC_VER)
	CHECK(_MSVC_LANG >= TICK_SYNCHRONIZER_CXX_STANDARD);
#else
	CHECK(__cplusplus >= TICK_SYNCHRONIZER_CXX_STANDARD);
#endif
}

} // namespace TestTickSynchronizerBuildConfig
