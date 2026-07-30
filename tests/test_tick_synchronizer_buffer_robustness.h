#pragma once

#include "tests/test_macros.h"

#include "src/public/tick_synchronizer_buffer.h"

#include <cstdint>

namespace TestTickSynchronizerBufferRobustness {

static uint64_t next_xorshift64(uint64_t &r_state) {
	r_state ^= r_state << 13;
	r_state ^= r_state >> 7;
	r_state ^= r_state << 17;
	return r_state;
}

TEST_CASE("[Modules][TickSynchronizer][BufferRobustness] Default limit is defensive and survives clear") {
	Ref<TickSynchronizerBuffer> buffer;
	buffer.instantiate();

	CHECK(buffer->get_max_size_bytes() == TickSynchronizerBuffer::DEFAULT_MAX_SIZE_BYTES);
	CHECK(buffer->get_max_size_bits() == TickSynchronizerBuffer::DEFAULT_MAX_SIZE_BYTES * 8);
	REQUIRE(buffer->set_max_size_bytes(4096) == OK);
	CHECK(buffer->get_max_size_bytes() == 4096);
	buffer->clear();
	CHECK(buffer->get_max_size_bytes() == 4096);
}

TEST_CASE("[Modules][TickSynchronizer][BufferRobustness] Invalid limit changes are non-destructive") {
	Ref<TickSynchronizerBuffer> buffer;
	buffer.instantiate();

	const int64_t original_limit = buffer->get_max_size_bytes();
	CHECK(buffer->set_max_size_bytes(0) == ERR_INVALID_PARAMETER);
	CHECK(buffer->set_max_size_bytes(-1) == ERR_INVALID_PARAMETER);
	CHECK(buffer->set_max_size_bytes(TickSynchronizerBuffer::MAX_CONFIGURABLE_SIZE_BYTES + 1) == ERR_INVALID_PARAMETER);
	CHECK(buffer->get_max_size_bytes() == original_limit);
	CHECK_FALSE(buffer->has_error());
}

TEST_CASE("[Modules][TickSynchronizer][BufferRobustness] Limit cannot shrink below allocated storage") {
	Ref<TickSynchronizerBuffer> buffer;
	buffer.instantiate();

	REQUIRE(buffer->set_max_size_bytes(64) == OK);
	REQUIRE(buffer->begin_write(32) == OK);
	CHECK(buffer->set_max_size_bytes(16) == ERR_PARAMETER_RANGE_ERROR);
	CHECK(buffer->get_max_size_bytes() == 64);
	CHECK_FALSE(buffer->has_error());

	buffer->clear();
	CHECK(buffer->set_max_size_bytes(16) == OK);
}

TEST_CASE("[Modules][TickSynchronizer][BufferRobustness] Initial capacity cannot exceed configured limit") {
	Ref<TickSynchronizerBuffer> buffer;
	buffer.instantiate();

	REQUIRE(buffer->set_max_size_bytes(8) == OK);
	CHECK(buffer->begin_write(9) == ERR_PARAMETER_RANGE_ERROR);
	CHECK(buffer->get_mode() == TickSynchronizerBuffer::MODE_NONE);
	CHECK(buffer->get_byte_size() == 0);
	CHECK(buffer->get_last_error() == ERR_PARAMETER_RANGE_ERROR);
}

TEST_CASE("[Modules][TickSynchronizer][BufferRobustness] Exact write limit succeeds and overflow is atomic") {
	Ref<TickSynchronizerBuffer> buffer;
	buffer.instantiate();

	REQUIRE(buffer->set_max_size_bytes(2) == OK);
	REQUIRE(buffer->begin_write() == OK);
	CHECK(buffer->get_remaining_write_bits() == 16);
	CHECK(buffer->can_write_bits(16));
	REQUIRE(buffer->write_u16(0xBEEF) == OK);
	CHECK(buffer->get_remaining_write_bits() == 0);
	CHECK_FALSE(buffer->can_write_bits(1));

	const PackedByteArray before_failure = buffer->get_data();
	CHECK(buffer->write_bits(1, 1) == ERR_PARAMETER_RANGE_ERROR);
	CHECK(buffer->get_bit_position() == 16);
	CHECK(buffer->get_bit_size() == 16);
	CHECK(buffer->get_data() == before_failure);
	CHECK(buffer->get_data().capacity() <= 2);

	buffer->clear();
	REQUIRE(buffer->begin_write() == OK);
	REQUIRE(buffer->write_u8(0x7F) == OK);
	const PackedByteArray before_varuint_failure = buffer->get_data();
	CHECK(buffer->write_varuint(128) == ERR_PARAMETER_RANGE_ERROR);
	CHECK(buffer->get_bit_position() == 8);
	CHECK(buffer->get_bit_size() == 8);
	CHECK(buffer->get_data() == before_varuint_failure);
}

TEST_CASE("[Modules][TickSynchronizer][BufferRobustness] Oversized read input is rejected before copying") {
	PackedByteArray input;
	REQUIRE(input.resize_initialized(5) == OK);

	Ref<TickSynchronizerBuffer> buffer;
	buffer.instantiate();
	REQUIRE(buffer->set_max_size_bytes(4) == OK);
	CHECK(buffer->begin_read(input) == ERR_PARAMETER_RANGE_ERROR);
	CHECK(buffer->get_mode() == TickSynchronizerBuffer::MODE_NONE);
	CHECK(buffer->get_byte_size() == 0);
}

TEST_CASE("[Modules][TickSynchronizer][BufferRobustness] Read input is canonicalized to logical bytes and bits") {
	PackedByteArray input;
	REQUIRE(input.resize_initialized(3) == OK);
	input.ptrw()[0] = 0xAB;
	input.ptrw()[1] = 0xFF;
	input.ptrw()[2] = 0xCC;

	Ref<TickSynchronizerBuffer> buffer;
	buffer.instantiate();
	REQUIRE(buffer->begin_read(input, 12) == OK);

	const PackedByteArray canonical = buffer->get_data();
	REQUIRE(canonical.size() == 2);
	CHECK(canonical[0] == 0xAB);
	CHECK(canonical[1] == 0x0F);
	CHECK(canonical.capacity() == 2);
	CHECK(buffer->get_bit_size() == 12);
}

TEST_CASE("[Modules][TickSynchronizer][BufferRobustness] Logical equality ignores cursor mode error and limit") {
	Ref<TickSynchronizerBuffer> writer;
	writer.instantiate();
	REQUIRE(writer->set_max_size_bytes(64) == OK);
	REQUIRE(writer->begin_write() == OK);
	REQUIRE(writer->write_bits(0b101, 3) == OK);
	REQUIRE(writer->write_u16(0x1234) == OK);

	Ref<TickSynchronizerBuffer> reader;
	reader.instantiate();
	REQUIRE(reader->set_max_size_bytes(128) == OK);
	REQUIRE(reader->begin_read(writer->get_data(), writer->get_bit_size()) == OK);
	uint64_t prefix = 0;
	REQUIRE(reader->read_bits(3, prefix) == OK);
	CHECK(prefix == 0b101);
	uint64_t ignored = 0;
	CHECK(reader->read_bits(static_cast<uint32_t>(reader->get_remaining_bits() + 1), ignored) == ERR_FILE_EOF);
	CHECK(reader->has_error());

	CHECK(writer->is_equal_to(reader));
	CHECK(reader->is_equal_to(writer));
	CHECK(writer->get_content_hash() == reader->get_content_hash());
}

TEST_CASE("[Modules][TickSynchronizer][BufferRobustness] Logical size and payload participate in equality and hashing") {
	PackedByteArray bytes;
	REQUIRE(bytes.resize_initialized(1) == OK);
	bytes.ptrw()[0] = 0x01;

	Ref<TickSynchronizerBuffer> one_bit;
	one_bit.instantiate();
	REQUIRE(one_bit->begin_read(bytes, 1) == OK);

	Ref<TickSynchronizerBuffer> eight_bits;
	eight_bits.instantiate();
	REQUIRE(eight_bits->begin_read(bytes, 8) == OK);

	PackedByteArray different_bytes;
	REQUIRE(different_bytes.resize_initialized(1) == OK);
	different_bytes.ptrw()[0] = 0x00;
	Ref<TickSynchronizerBuffer> different_payload;
	different_payload.instantiate();
	REQUIRE(different_payload->begin_read(different_bytes, 1) == OK);

	CHECK_FALSE(one_bit->is_equal_to(eight_bits));
	CHECK_FALSE(one_bit->is_equal_to(different_payload));
	CHECK(one_bit->get_content_hash() != eight_bits->get_content_hash());
	CHECK(one_bit->get_content_hash() != different_payload->get_content_hash());
}

TEST_CASE("[Modules][TickSynchronizer][BufferRobustness] Deterministic stress stream round trips within its limit") {
	constexpr uint32_t operation_count = 4096;
	constexpr int64_t max_size_bytes = 64 * 1024;
	constexpr uint64_t seed = UINT64_C(0xA17E5EED12345678);

	Ref<TickSynchronizerBuffer> writer;
	writer.instantiate();
	REQUIRE(writer->set_max_size_bytes(max_size_bytes) == OK);
	REQUIRE(writer->begin_write(4096) == OK);

	uint64_t write_state = seed;
	for (uint32_t index = 0; index < operation_count; index++) {
		const uint32_t bit_count = static_cast<uint32_t>((next_xorshift64(write_state) % 64) + 1);
		uint64_t value = next_xorshift64(write_state);
		if (bit_count < 64) {
			value &= (UINT64_C(1) << bit_count) - 1;
		}
		REQUIRE(writer->can_write_bits(bit_count));
		REQUIRE(writer->write_bits(value, bit_count) == OK);
	}
	CHECK(writer->get_byte_size() <= max_size_bytes);

	Ref<TickSynchronizerBuffer> reader;
	reader.instantiate();
	REQUIRE(reader->set_max_size_bytes(max_size_bytes) == OK);
	REQUIRE(reader->begin_read(writer->get_data(), writer->get_bit_size()) == OK);

	uint64_t read_state = seed;
	for (uint32_t index = 0; index < operation_count; index++) {
		const uint32_t bit_count = static_cast<uint32_t>((next_xorshift64(read_state) % 64) + 1);
		uint64_t expected = next_xorshift64(read_state);
		if (bit_count < 64) {
			expected &= (UINT64_C(1) << bit_count) - 1;
		}

		uint64_t actual = 0;
		REQUIRE(reader->read_bits(bit_count, actual) == OK);
		CHECK(actual == expected);
	}
	CHECK(reader->get_remaining_bits() == 0);
	CHECK(writer->is_equal_to(reader));
	CHECK(writer->get_content_hash() == reader->get_content_hash());
}

} // namespace TestTickSynchronizerBufferRobustness
