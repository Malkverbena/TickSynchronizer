// Builds deterministic semantic message datasets for protocol comparisons.
// Ensures every candidate receives identical seeded workloads.

#include "benchmark_dataset.h"

#include <algorithm>
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

BenchmarkEntityState make_entity(
		DeterministicRandom &random,
		std::uint32_t entity_id,
		std::uint32_t changed_fields,
		double scale) {
	BenchmarkEntityState entity;
	entity.entity_id = entity_id;
	entity.change_mask = changed_fields;
	entity.signed_value = static_cast<std::int64_t>(random.next_u64());
	entity.unsigned_value = random.next_u64();
	entity.position_x = random.next_signed_unit() * scale;
	entity.position_y = random.next_signed_unit() * scale;
	entity.position_z = random.next_signed_unit() * scale;
	entity.velocity_x = random.next_signed_unit() * (scale * 0.1);
	entity.velocity_y = random.next_signed_unit() * (scale * 0.1);
	entity.velocity_z = random.next_signed_unit() * (scale * 0.1);
	entity.flags = random.next_u32();
	return entity;
}


// Builds small control messages that expose minimum framing overhead.
BenchmarkDataset make_control_dataset(DeterministicRandom &random) {
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
		message.buttons = random.next_u64();
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
	for (std::uint32_t i = 0; i < 256; ++i) {
		BenchmarkMessage message;
		message.kind = BenchmarkMessageKind::PLAYER_INPUT;
		message.subtype = 1;
		message.flags = static_cast<std::uint16_t>(i & 0x3U);
		message.sequence = UINT32_C(50'000) + i;
		message.tick = UINT64_C(200'000) + i;
		message.reference_tick = message.tick - (i % 5);
		message.buttons = random.next_u64() & UINT64_C(0x00000000FFFFFFFF);
		for (std::size_t axis = 0; axis < message.axes.size(); ++axis) {
			message.axes[axis] = static_cast<std::int16_t>(random.next_u32() & 0xFFFFU);
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
			message.entities.push_back(make_entity(random, entity + 1U, mask, 128.0));
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
			message.entities.push_back(make_entity(random, entity + 1U, UINT32_C(0x1F), 4096.0));
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
			message.entities.push_back(make_entity(random, entity + 1U, UINT32_C(0xFFFFFFFF), 1'000'000.0));
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


// Builds correlated tick sequences for future stateful candidate analysis.
BenchmarkDataset make_sequential_dataset(DeterministicRandom &random) {
	BenchmarkDataset dataset;
	dataset.name = "sequential_flow";
	dataset.description = "Five hundred and twelve consecutive ticks with small state changes and stable entity IDs.";
	dataset.messages.reserve(512);
	std::array<BenchmarkEntityState, 8> state = {};
	for (std::uint32_t entity = 0; entity < state.size(); ++entity) {
		state[entity] = make_entity(random, entity + 1U, UINT32_C(0x3F), 100.0);
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
	datasets.reserve(7);
	datasets.push_back(make_control_dataset(random));
	datasets.push_back(make_player_input_dataset(random));
	datasets.push_back(make_sparse_snapshot_dataset(random));
	datasets.push_back(make_medium_snapshot_dataset(random));
	datasets.push_back(make_dense_snapshot_dataset(random));
	datasets.push_back(make_extreme_dataset());
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
