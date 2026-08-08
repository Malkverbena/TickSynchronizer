// Defines semantic messages, datasets, and byte views for protocol benchmarks.
// Keeps candidate inputs independent of any specific wire representation.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tick_synchronizer::benchmarks {

enum class BenchmarkMessageKind : std::uint8_t {
	CONTROL = 1,
	PLAYER_INPUT = 2,
	SNAPSHOT = 3,
};

struct BenchmarkEntityState {
	std::uint32_t entity_id = 0;
	std::uint32_t change_mask = 0;
	std::int64_t signed_value = 0;
	std::uint64_t unsigned_value = 0;
	double position_x = 0.0;
	double position_y = 0.0;
	double position_z = 0.0;
	double velocity_x = 0.0;
	double velocity_y = 0.0;
	double velocity_z = 0.0;
	std::uint32_t flags = 0;
};

struct BenchmarkMessage {
	BenchmarkMessageKind kind = BenchmarkMessageKind::CONTROL;
	std::uint8_t subtype = 0;
	std::uint16_t flags = 0;
	std::uint32_t sequence = 0;
	std::uint64_t tick = 0;
	std::uint64_t reference_tick = 0;
	std::uint64_t buttons = 0;
	std::array<std::int16_t, 4> axes = {};
	std::vector<BenchmarkEntityState> entities;
	std::vector<std::uint8_t> blob;
};

struct BenchmarkDataset {
	std::string name;
	std::string description;
	std::vector<BenchmarkMessage> messages;
};

struct ByteView {
	const std::uint8_t *data = nullptr;
	std::size_t size = 0;
};

// Creates a non-owning view over a vector for candidate decoding.
inline ByteView make_byte_view(const std::vector<std::uint8_t> &bytes) noexcept {
	return ByteView{ bytes.data(), bytes.size() };
}

} // namespace tick_synchronizer::benchmarks
