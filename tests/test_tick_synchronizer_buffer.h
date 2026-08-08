// Tests deterministic bitstream writing, reading, alignment, and state.
// Verifies public buffer behavior through normal and boundary cases.

#pragma once

#include "tests/test_macros.h"

#include "src/public/tick_synchronizer_buffer.h"

#include <cstdint>

namespace TestTickSynchronizerBuffer {

TEST_CASE("[Modules][TickSynchronizer][Buffer] Default state and mode transitions") {
	Ref<TickSynchronizerBuffer> buffer;
	buffer.instantiate();

	CHECK(buffer->get_mode() == TickSynchronizerBuffer::MODE_NONE);
	CHECK_FALSE(buffer->is_reading());
	CHECK_FALSE(buffer->is_writing());
	CHECK_FALSE(buffer->has_error());
	CHECK(buffer->get_last_error() == OK);
	CHECK(buffer->get_byte_size() == 0);
	CHECK(buffer->get_bit_size() == 0);
	CHECK(buffer->get_bit_position() == 0);

	CHECK(buffer->begin_write(64) == OK);
	CHECK(buffer->get_mode() == TickSynchronizerBuffer::MODE_WRITE);
	CHECK(buffer->is_writing());
	CHECK_FALSE(buffer->is_reading());
	CHECK(buffer->get_byte_size() == 0);

	// Resets data and operation state while preserving the configured size limit.
	buffer->clear();
	CHECK(buffer->get_mode() == TickSynchronizerBuffer::MODE_NONE);
	CHECK(buffer->get_last_error() == OK);

	CHECK(buffer->begin_read(PackedByteArray()) == OK);
	CHECK(buffer->get_mode() == TickSynchronizerBuffer::MODE_READ);
	CHECK(buffer->is_reading());
	CHECK(buffer->get_remaining_bits() == 0);
}

TEST_CASE("[Modules][TickSynchronizer][Buffer] Golden vector packs 3 and 5 bits LSB first") {
	Ref<TickSynchronizerBuffer> buffer;
	buffer.instantiate();

	REQUIRE(buffer->begin_write() == OK);
	REQUIRE(buffer->write_bits(0b101, 3) == OK);
	REQUIRE(buffer->write_bits(0b11011, 5) == OK);

	const PackedByteArray data = buffer->get_data();
	REQUIRE(data.size() == 1);
	CHECK(data[0] == 0xDD);
	CHECK(buffer->get_bit_size() == 8);
	CHECK(buffer->get_bit_position() == 8);
}

TEST_CASE("[Modules][TickSynchronizer][Buffer] Golden vector crosses a byte boundary") {
	Ref<TickSynchronizerBuffer> buffer;
	buffer.instantiate();

	REQUIRE(buffer->begin_write() == OK);
	REQUIRE(buffer->write_bits(1, 1) == OK);
	REQUIRE(buffer->write_bits(0xA5, 8) == OK);

	const PackedByteArray data = buffer->get_data();
	REQUIRE(data.size() == 2);
	CHECK(data[0] == 0x4B);
	CHECK(data[1] == 0x01);
	CHECK(buffer->get_bit_size() == 9);
}

TEST_CASE("[Modules][TickSynchronizer][Buffer] Aligned uint64 follows little endian byte order") {
	Ref<TickSynchronizerBuffer> buffer;
	buffer.instantiate();

	REQUIRE(buffer->begin_write() == OK);
	REQUIRE(buffer->write_bits(UINT64_C(0x0123456789ABCDEF), 64) == OK);

	const PackedByteArray data = buffer->get_data();
	const uint8_t expected[] = { 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01 };
	REQUIRE(data.size() == 8);
	for (int index = 0; index < 8; index++) {
		CHECK(data[index] == expected[index]);
	}
}

TEST_CASE("[Modules][TickSynchronizer][Buffer] Mixed field widths round trip") {
	const uint32_t bit_counts[] = { 1, 2, 7, 8, 9, 16, 31, 32, 63, 64 };
	const uint64_t values[] = {
		UINT64_C(0x1),
		UINT64_C(0x2),
		UINT64_C(0x55),
		UINT64_C(0xA5),
		UINT64_C(0x101),
		UINT64_C(0xBEEF),
		UINT64_C(0x5ABCDEFA),
		UINT64_C(0xDEADBEEF),
		UINT64_C(0x7123456789ABCDEF),
		UINT64_C(0xFEDCBA9876543210),
	};

	Ref<TickSynchronizerBuffer> writer;
	writer.instantiate();
	REQUIRE(writer->begin_write(64) == OK);

	int64_t expected_bit_size = 0;
	for (uint32_t index = 0; index < 10; index++) {
		REQUIRE(writer->write_bits(values[index], bit_counts[index]) == OK);
		expected_bit_size += bit_counts[index];
	}
	CHECK(writer->get_bit_size() == expected_bit_size);

	Ref<TickSynchronizerBuffer> reader;
	reader.instantiate();
	REQUIRE(reader->begin_read(writer->get_data(), writer->get_bit_size()) == OK);

	for (uint32_t index = 0; index < 10; index++) {
		uint64_t decoded_value = 0;
		REQUIRE(reader->read_bits(bit_counts[index], decoded_value) == OK);
		CHECK(decoded_value == values[index]);
	}
	CHECK(reader->get_remaining_bits() == 0);
	CHECK(reader->get_bit_position() == expected_bit_size);
}

TEST_CASE("[Modules][TickSynchronizer][Buffer] Partial final byte keeps padding zeroed") {
	Ref<TickSynchronizerBuffer> buffer;
	buffer.instantiate();

	REQUIRE(buffer->begin_write() == OK);
	REQUIRE(buffer->write_bits(1, 1) == OK);

	const PackedByteArray data = buffer->get_data();
	REQUIRE(data.size() == 1);
	CHECK(data[0] == 0x01);
	CHECK(buffer->get_bit_size() == 1);
}

TEST_CASE("[Modules][TickSynchronizer][Buffer] Reading past logical bit size is atomic and sticky") {
	PackedByteArray data;
	REQUIRE(data.resize_initialized(1) == OK);
	data.ptrw()[0] = 0xFF;

	Ref<TickSynchronizerBuffer> buffer;
	buffer.instantiate();
	REQUIRE(buffer->begin_read(data, 3) == OK);

	uint64_t value = 0;
	REQUIRE(buffer->read_bits(3, value) == OK);
	CHECK(value == 0b111);
	CHECK(buffer->get_bit_position() == 3);

	value = UINT64_MAX;
	CHECK(buffer->read_bits(1, value) == ERR_FILE_EOF);
	CHECK(value == 0);
	CHECK(buffer->get_bit_position() == 3);
	CHECK(buffer->has_error());
	CHECK(buffer->get_last_error() == ERR_FILE_EOF);

	value = UINT64_MAX;
	CHECK(buffer->read_bits(1, value) == ERR_FILE_EOF);
	CHECK(value == 0);
	CHECK(buffer->get_bit_position() == 3);
}

TEST_CASE("[Modules][TickSynchronizer][Buffer] Invalid operations set sticky errors until reset") {
	Ref<TickSynchronizerBuffer> buffer;
	buffer.instantiate();

	CHECK(buffer->write_bits(1, 1) == ERR_UNCONFIGURED);
	CHECK(buffer->get_last_error() == ERR_UNCONFIGURED);
	CHECK(buffer->begin_write() == OK);
	CHECK_FALSE(buffer->has_error());

	CHECK(buffer->write_bits(0, 0) == ERR_INVALID_PARAMETER);
	CHECK(buffer->get_bit_position() == 0);
	CHECK(buffer->get_byte_size() == 0);
	CHECK(buffer->write_bits(1, 1) == ERR_INVALID_PARAMETER);
	CHECK(buffer->get_bit_position() == 0);

	CHECK(buffer->begin_write() == OK);
	CHECK(buffer->write_bits(1, 65) == ERR_INVALID_PARAMETER);
	CHECK(buffer->get_bit_position() == 0);

	CHECK(buffer->begin_write(-1) == ERR_INVALID_PARAMETER);
	CHECK(buffer->get_mode() == TickSynchronizerBuffer::MODE_NONE);

	PackedByteArray one_byte;
	REQUIRE(one_byte.resize_initialized(1) == OK);
	CHECK(buffer->begin_read(one_byte, 9) == ERR_INVALID_PARAMETER);
	CHECK(buffer->get_mode() == TickSynchronizerBuffer::MODE_NONE);
	CHECK(buffer->begin_read(one_byte, -2) == ERR_INVALID_PARAMETER);
	CHECK(buffer->get_mode() == TickSynchronizerBuffer::MODE_NONE);
}

TEST_CASE("[Modules][TickSynchronizer][Buffer] Failed reads do not expose partial values") {
	PackedByteArray data;
	REQUIRE(data.resize_initialized(1) == OK);
	data.ptrw()[0] = 0xA5;

	Ref<TickSynchronizerBuffer> buffer;
	buffer.instantiate();
	REQUIRE(buffer->begin_read(data, 7) == OK);

	uint64_t value = UINT64_MAX;
	CHECK(buffer->read_bits(8, value) == ERR_FILE_EOF);
	CHECK(value == 0);
	CHECK(buffer->get_bit_position() == 0);
}

} // namespace TestTickSynchronizerBuffer
