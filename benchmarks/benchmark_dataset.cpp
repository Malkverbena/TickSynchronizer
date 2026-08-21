// Builds deterministic semantic message datasets for protocol comparisons.
// Ensures every candidate receives identical seeded workloads.

#include "benchmark_dataset.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace tick_synchronizer::benchmarks {
namespace {

class DeterministicRandom {
	std::uint64_t state;

public:
	// Provides a reproducible pseudo-random stream for dataset generation.
	explicit DeterministicRandom(std::uint64_t seed) :
			state(seed == 0 ? UINT64_C(0x9E3779B97F4A7C15) : seed) {}

	// Advances the fixed xorshift sequence and returns the next 64-bit value.
	std::uint64_t next_u64() noexcept {
		std::uint64_t x = state;
		x ^= x >> 12;
		x ^= x << 25;
		x ^= x >> 27;
		state = x;
		return x * UINT64_C(2685821657736338717);
	}

	// Returns the low 32 bits of the next deterministic random value.
	std::uint32_t next_u32() noexcept {
		return static_cast<std::uint32_t>(next_u64() >> 32);
	}

	// Maps deterministic random bits to the closed interval around zero.
	double next_signed_unit() noexcept {
		const std::uint64_t mantissa = next_u64() >> 11;
		const double unit = static_cast<double>(mantissa) * (1.0 / 9007199254740992.0);
		return unit * 2.0 - 1.0;
	}
};

enum class IntegerProfile : std::uint8_t {
	SPARSE,
	MEDIUM,
	DENSE,
	SEQUENTIAL,
};


// Maps one 64-bit pattern to a signed value without implementation-defined conversion.
std::int64_t signed_from_bits(std::uint64_t value) noexcept {
	if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
		return static_cast<std::int64_t>(value);
	}
	return -1 - static_cast<std::int64_t>(UINT64_MAX - value);
}


// Assigns an explicit gameplay-oriented integer distribution to one entity.
void assign_profiled_integers(
		BenchmarkEntityState &entity,
		DeterministicRandom &random,
		std::uint32_t ordinal,
		IntegerProfile profile) noexcept {
	switch (profile) {
		case IntegerProfile::SPARSE:
			entity.signed_value = static_cast<std::int64_t>(random.next_u32() % 129U) - 64;
			entity.unsigned_value = random.next_u32() % 128U;
			entity.flags = random.next_u32() & UINT32_C(0x3F);
			break;
		case IntegerProfile::MEDIUM:
			entity.signed_value = static_cast<std::int64_t>(random.next_u32() % 16'385U) - 8'192;
			entity.unsigned_value = random.next_u32() % 16'384U;
			entity.flags = random.next_u32() & UINT32_C(0x3FF);
			break;
		case IntegerProfile::DENSE: {
			const std::uint32_t bucket = ordinal % 64U;
			if (bucket < 48U) {
				entity.signed_value = static_cast<std::int64_t>(random.next_u32() % 129U) - 64;
				entity.unsigned_value = random.next_u32() % 128U;
				entity.flags = random.next_u32() & UINT32_C(0xFF);
			} else if (bucket < 60U) {
				entity.signed_value = static_cast<std::int64_t>(random.next_u32() % 16'385U) - 8'192;
				entity.unsigned_value = random.next_u32() % 16'384U;
				entity.flags = random.next_u32() & UINT32_C(0xFFFF);
			} else if (bucket < 63U) {
				entity.signed_value = static_cast<std::int64_t>(random.next_u32()) - INT64_C(2'147'483'648);
				entity.unsigned_value = random.next_u32();
				entity.flags = random.next_u32();
			} else {
				// Maps one 64-bit pattern to a signed value without implementation-defined conversion.
				entity.signed_value = signed_from_bits(random.next_u64());
				entity.unsigned_value = random.next_u64();
				entity.flags = random.next_u32();
			}
			break;
		}
		case IntegerProfile::SEQUENTIAL:
			entity.signed_value = static_cast<std::int64_t>(ordinal + 1U) * 100;
			entity.unsigned_value = static_cast<std::uint64_t>(ordinal + 1U) * 100U;
			entity.flags = ordinal & UINT32_C(0x0F);
			break;
	}
}


BenchmarkEntityState make_entity(
			DeterministicRandom &random,
			std::uint32_t entity_id,
			std::uint32_t changed_fields,
			double scale,
			IntegerProfile integer_profile) {
	BenchmarkEntityState entity;
	entity.entity_id = entity_id;
	entity.change_mask = changed_fields;
	// Assigns an explicit gameplay-oriented integer distribution to one entity.
	assign_profiled_integers(entity, random, entity_id - 1U, integer_profile);
	entity.position_x = random.next_signed_unit() * scale;
	entity.position_y = random.next_signed_unit() * scale;
	entity.position_z = random.next_signed_unit() * scale;
	entity.velocity_x = random.next_signed_unit() * (scale * 0.1);
	entity.velocity_y = random.next_signed_unit() * (scale * 0.1);
	entity.velocity_z = random.next_signed_unit() * (scale * 0.1);
	return entity;
}


// Builds small control messages that expose minimum framing overhead.
BenchmarkDataset make_control_dataset() {
	BenchmarkDataset dataset;
	dataset.name = "control_minimal";
	dataset.description = "Ping, pong, acknowledgement, sequence and tick control messages.";
	dataset.messages.reserve(64);
	for (std::uint32_t i = 0; i < 64; ++i) {
		BenchmarkMessage message;
		message.kind = BenchmarkMessageKind::CONTROL;
		message.subtype = static_cast<std::uint8_t>((i % 4) + 1);
		message.flags = static_cast<std::uint16_t>(i & 0x7U);
		message.sequence = i;
		message.tick = UINT64_C(1'000'000) + i;
		message.reference_tick = message.tick > 3 ? message.tick - 3 : 0;
		message.buttons = UINT64_C(1) << (i % 32U);
		if ((i % 5U) == 0) {
			message.buttons |= UINT64_C(1) << ((i + 7U) % 32U);
		}
		dataset.messages.push_back(std::move(message));
	}
	return dataset;
}


// Builds frequent input messages with buttons and analog axes.
BenchmarkDataset make_player_input_dataset(DeterministicRandom &random) {
	BenchmarkDataset dataset;
	dataset.name = "player_input";
	dataset.description = "Digital buttons, four analog axes, sequence, tick and acknowledgement data.";
	dataset.messages.reserve(256);
	constexpr std::array<std::int16_t, 12> AXIS_VALUES = {
		0, 1, -1, 63, -64, 127, -128, 8'192, -8'192,
		std::numeric_limits<std::int16_t>::max(),
		std::numeric_limits<std::int16_t>::min(),
		2'048,
	};
	for (std::uint32_t i = 0; i < 256; ++i) {
		BenchmarkMessage message;
		message.kind = BenchmarkMessageKind::PLAYER_INPUT;
		message.subtype = 1;
		message.flags = static_cast<std::uint16_t>(i & 0x3U);
		message.sequence = UINT32_C(50'000) + i;
		message.tick = UINT64_C(200'000) + i;
		message.reference_tick = message.tick - (i % 5);
		message.buttons = (UINT64_C(1) << (i % 32U)) |
				((i % 7U) == 0 ? UINT64_C(1) << ((i + 11U) % 32U) : 0);
		for (std::size_t axis = 0; axis < message.axes.size(); ++axis) {
			message.axes[axis] = AXIS_VALUES[(i + axis * 3U) % AXIS_VALUES.size()];
		}
		const std::size_t blob_size = i % 17;
		message.blob.resize(blob_size);
		for (std::uint8_t &byte : message.blob) {
			byte = static_cast<std::uint8_t>(random.next_u32());
		}
		dataset.messages.push_back(std::move(message));
	}
	return dataset;
}


// Builds snapshots with few changed entities and fields.
BenchmarkDataset make_sparse_snapshot_dataset(DeterministicRandom &random) {
	BenchmarkDataset dataset;
	dataset.name = "snapshot_sparse";
	dataset.description = "One to five entities with sparse change masks and small deltas.";
	dataset.messages.reserve(64);
	for (std::uint32_t i = 0; i < 64; ++i) {
		BenchmarkMessage message;
		message.kind = BenchmarkMessageKind::SNAPSHOT;
		message.subtype = 1;
		message.sequence = UINT32_C(100'000) + i;
		message.tick = UINT64_C(300'000) + i;
		message.reference_tick = message.tick - 1;
		const std::uint32_t count = 1U + (random.next_u32() % 5U);
		message.entities.reserve(count);
		for (std::uint32_t entity = 0; entity < count; ++entity) {
			const std::uint32_t bits = 1U + (random.next_u32() % 3U);
			const std::uint32_t mask = (UINT32_C(1) << bits) - 1U;
			message.entities.push_back(make_entity(
					random, entity + 1U, mask, 128.0, IntegerProfile::SPARSE));
		}
		dataset.messages.push_back(std::move(message));
	}
	return dataset;
}


// Builds representative gameplay snapshots with moderate entity counts.
BenchmarkDataset make_medium_snapshot_dataset(DeterministicRandom &random) {
	BenchmarkDataset dataset;
	dataset.name = "snapshot_medium";
	dataset.description = "Thirty-two entities with transforms, velocity, flags and optional payload bytes.";
	dataset.messages.reserve(16);
	for (std::uint32_t i = 0; i < 16; ++i) {
		BenchmarkMessage message;
		message.kind = BenchmarkMessageKind::SNAPSHOT;
		message.subtype = 2;
		message.flags = 1;
		message.sequence = UINT32_C(200'000) + i;
		message.tick = UINT64_C(400'000) + i;
		message.reference_tick = message.tick - 2;
		message.entities.reserve(32);
		for (std::uint32_t entity = 0; entity < 32; ++entity) {
			message.entities.push_back(make_entity(
					random, entity + 1U, UINT32_C(0x1F), 4096.0, IntegerProfile::MEDIUM));
		}
		message.blob.resize(64);
		for (std::uint8_t &byte : message.blob) {
			byte = static_cast<std::uint8_t>(random.next_u32());
		}
		dataset.messages.push_back(std::move(message));
	}
	return dataset;
}


// Builds high-load snapshots to measure throughput scaling.
BenchmarkDataset make_dense_snapshot_dataset(DeterministicRandom &random) {
	BenchmarkDataset dataset;
	dataset.name = "snapshot_dense";
	dataset.description = "Two hundred and fifty-six densely changed entities for throughput and cache pressure.";
	dataset.messages.reserve(4);
	for (std::uint32_t i = 0; i < 4; ++i) {
		BenchmarkMessage message;
		message.kind = BenchmarkMessageKind::SNAPSHOT;
		message.subtype = 3;
		message.flags = UINT16_C(0x00FF);
		message.sequence = UINT32_C(300'000) + i;
		message.tick = UINT64_C(500'000) + i;
		message.reference_tick = message.tick - 1;
		message.entities.reserve(256);
		for (std::uint32_t entity = 0; entity < 256; ++entity) {
			message.entities.push_back(make_entity(
					random, entity + 1U, UINT32_C(0xFFFFFFFF), 1'000'000.0, IntegerProfile::DENSE));
		}
		message.blob.resize(512);
		for (std::uint8_t &byte : message.blob) {
			byte = static_cast<std::uint8_t>(random.next_u32());
		}
		dataset.messages.push_back(std::move(message));
	}
	return dataset;
}


// Builds values near numeric and length boundaries.
BenchmarkDataset make_extreme_dataset() {
	BenchmarkDataset dataset;
	dataset.name = "numeric_extremes";
	dataset.description = "Integer limits, zero, negative values, infinities, NaNs and precision boundaries.";
	dataset.messages.reserve(16);
	const double values[] = {
		0.0,
		-0.0,
		1.0,
		-1.0,
		std::numeric_limits<float>::min(),
		std::numeric_limits<float>::max(),
		std::numeric_limits<double>::min(),
		std::numeric_limits<double>::max(),
		std::numeric_limits<double>::infinity(),
		-std::numeric_limits<double>::infinity(),
		std::numeric_limits<double>::quiet_NaN(),
	};
	for (std::uint32_t i = 0; i < 16; ++i) {
		BenchmarkMessage message;
		message.kind = BenchmarkMessageKind::SNAPSHOT;
		message.subtype = 4;
		message.sequence = UINT32_MAX - i;
		message.tick = UINT64_MAX - i;
		message.reference_tick = i;
		BenchmarkEntityState entity;
		entity.entity_id = i;
		entity.change_mask = UINT32_MAX;
		entity.signed_value = (i & 1U) == 0 ? INT64_MIN : INT64_MAX;
		entity.unsigned_value = (i & 1U) == 0 ? 0 : UINT64_MAX;
		entity.position_x = values[i % (sizeof(values) / sizeof(values[0]))];
		entity.position_y = values[(i + 1U) % (sizeof(values) / sizeof(values[0]))];
		entity.position_z = values[(i + 2U) % (sizeof(values) / sizeof(values[0]))];
		entity.velocity_x = values[(i + 3U) % (sizeof(values) / sizeof(values[0]))];
		entity.velocity_y = values[(i + 4U) % (sizeof(values) / sizeof(values[0]))];
		entity.velocity_z = values[(i + 5U) % (sizeof(values) / sizeof(values[0]))];
		entity.flags = UINT32_MAX - i;
		message.entities.push_back(entity);
		message.blob.assign(i, static_cast<std::uint8_t>(i));
		dataset.messages.push_back(std::move(message));
	}
	return dataset;
}


// Builds exact canonical-varint width transitions for unsigned and signed values.
BenchmarkDataset make_integer_boundary_dataset() {
	BenchmarkDataset dataset;
	dataset.name = "integer_boundaries";
	dataset.description = "Canonical ULEB128 and ZigZag width transitions from zero through 64-bit limits.";
	constexpr std::array<std::uint64_t, 20> UNSIGNED_VALUES = {
		0,
		127, 128,
		16'383, 16'384,
		2'097'151, 2'097'152,
		268'435'455, 268'435'456,
		UINT64_C(34'359'738'367), UINT64_C(34'359'738'368),
		UINT64_C(4'398'046'511'103), UINT64_C(4'398'046'511'104),
		UINT64_C(562'949'953'421'311), UINT64_C(562'949'953'421'312),
		UINT64_C(72'057'594'037'927'935), UINT64_C(72'057'594'037'927'936),
		UINT64_C(9'223'372'036'854'775'807), UINT64_C(9'223'372'036'854'775'808),
		UINT64_MAX,
	};
	constexpr std::array<std::int64_t, 20> SIGNED_VALUES = {
		0, -1, 63, -64, 64, -65, 8'191, -8'192, 8'192, -8'193,
		1'048'575, -1'048'576, 1'048'576, -1'048'577,
		134'217'727, -134'217'728, 134'217'728, -134'217'729,
		INT64_MAX, INT64_MIN,
	};
	constexpr std::array<std::uint32_t, 10> UNSIGNED_32_VALUES = {
		0, 127, 128, 16'383, 16'384, 2'097'151, 2'097'152,
		268'435'455, 268'435'456, UINT32_MAX,
	};
	constexpr std::array<std::uint16_t, 6> UNSIGNED_16_VALUES = {
		0, 127, 128, 16'383, 16'384, UINT16_MAX,
	};
	dataset.messages.reserve(UNSIGNED_VALUES.size());
	for (std::size_t index = 0; index < UNSIGNED_VALUES.size(); ++index) {
		BenchmarkMessage message;
		message.kind = BenchmarkMessageKind::SNAPSHOT;
		message.subtype = 6;
		message.flags = UNSIGNED_16_VALUES[index % UNSIGNED_16_VALUES.size()];
		message.sequence = UNSIGNED_32_VALUES[index % UNSIGNED_32_VALUES.size()];
		message.tick = UNSIGNED_VALUES[index];
		message.reference_tick = UNSIGNED_VALUES[(index + 1U) % UNSIGNED_VALUES.size()];
		BenchmarkEntityState entity;
		entity.entity_id = UNSIGNED_32_VALUES[index % UNSIGNED_32_VALUES.size()];
		entity.change_mask = UNSIGNED_32_VALUES[(index + 3U) % UNSIGNED_32_VALUES.size()];
		entity.signed_value = SIGNED_VALUES[index];
		entity.unsigned_value = UNSIGNED_VALUES[index];
		entity.position_x = static_cast<double>(index);
		entity.position_y = -static_cast<double>(index);
		entity.velocity_x = 0.5;
		entity.velocity_y = -0.5;
		entity.flags = UNSIGNED_32_VALUES[(index + 5U) % UNSIGNED_32_VALUES.size()];
		message.entities.push_back(entity);
		dataset.messages.push_back(std::move(message));
	}
	return dataset;
}


// Builds correlated tick sequences for future stateful candidate analysis.
BenchmarkDataset make_sequential_dataset(DeterministicRandom &random) {
	BenchmarkDataset dataset;
	dataset.name = "sequential_flow";
	dataset.description = "Five hundred and twelve consecutive ticks with small state changes and stable entity IDs.";
	dataset.messages.reserve(512);
	std::array<BenchmarkEntityState, 8> state = {};
	for (std::uint32_t entity = 0; entity < state.size(); ++entity) {
		state[entity] = make_entity(
				random, entity + 1U, UINT32_C(0x3F), 100.0, IntegerProfile::SEQUENTIAL);
	}
	for (std::uint32_t i = 0; i < 512; ++i) {
		BenchmarkMessage message;
		message.kind = BenchmarkMessageKind::SNAPSHOT;
		message.subtype = 5;
		message.sequence = UINT32_C(400'000) + i;
		message.tick = UINT64_C(600'000) + i;
		message.reference_tick = message.tick == UINT64_C(600'000) ? 0 : message.tick - 1;
		message.entities.reserve(state.size());
		for (BenchmarkEntityState &entity : state) {
			entity.position_x += random.next_signed_unit() * 0.01;
			entity.position_y += random.next_signed_unit() * 0.01;
			entity.position_z += random.next_signed_unit() * 0.01;
			entity.signed_value += static_cast<std::int64_t>(random.next_u32() % 7U) - 3;
			entity.unsigned_value += random.next_u32() % 3U;
			entity.change_mask = UINT32_C(0x07);
			message.entities.push_back(entity);
		}
		dataset.messages.push_back(std::move(message));
	}
	return dataset;
}

} // namespace

std::vector<BenchmarkDataset> make_protocol_benchmark_datasets(std::uint64_t seed) {
	DeterministicRandom random(seed);
	std::vector<BenchmarkDataset> datasets;
	datasets.reserve(8);
	datasets.push_back(make_control_dataset());
	datasets.push_back(make_player_input_dataset(random));
	datasets.push_back(make_sparse_snapshot_dataset(random));
	datasets.push_back(make_medium_snapshot_dataset(random));
	datasets.push_back(make_dense_snapshot_dataset(random));
	datasets.push_back(make_extreme_dataset());
	datasets.push_back(make_integer_boundary_dataset());
	datasets.push_back(make_sequential_dataset(random));
	return datasets;
}

const BenchmarkDataset *find_benchmark_dataset(
		const std::vector<BenchmarkDataset> &datasets,
		std::string_view name) noexcept {
	const auto iterator = std::find_if(
			datasets.begin(),
			datasets.end(),
			[name](const BenchmarkDataset &dataset) { return dataset.name == name; });
	return iterator == datasets.end() ? nullptr : &*iterator;
}

} // namespace tick_synchronizer::benchmarks
