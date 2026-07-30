#pragma once

#include "tests/test_macros.h"

#include "src/public/tick_synchronizer_buffer.h"

#include <cstdint>
#include <limits>

namespace TestTickSynchronizerVarint {

TEST_CASE("[Modules][TickSynchronizer][Varint] Canonical varuint boundary vectors") {
	const uint64_t values[] = {
		0,
		1,
		127,
		128,
		255,
		300,
		16383,
		16384,
		UINT32_MAX,
		UINT64_MAX,
	};
	const uint8_t expected[] = {
		0x00,
		0x01,
		0x7F,
		0x80, 0x01,
		0xFF, 0x01,
		0xAC, 0x02,
		0xFF, 0x7F,
		0x80, 0x80, 0x01,
		0xFF, 0xFF, 0xFF, 0xFF, 0x0F,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01,
	};

	Ref<TickSynchronizerBuffer> writer;
	writer.instantiate();
	REQUIRE(writer->begin_write() == OK);
	for (uint64_t value : values) {
		REQUIRE(writer->write_varuint(value) == OK);
	}

	const PackedByteArray data = writer->get_data();
	REQUIRE(data.size() == static_cast<int64_t>(sizeof(expected)));
	for (int64_t index = 0; index < data.size(); index++) {
		CHECK(data[index] == expected[index]);
	}
}

TEST_CASE("[Modules][TickSynchronizer][Varint] Varuint boundary values round trip") {
	const uint64_t values[] = {
		0,
		1,
		127,
		128,
		255,
		300,
		16383,
		16384,
		UINT32_MAX,
		UINT64_MAX,
	};

	Ref<TickSynchronizerBuffer> writer;
	writer.instantiate();
	REQUIRE(writer->begin_write() == OK);
	for (uint64_t value : values) {
		REQUIRE(writer->write_varuint(value) == OK);
	}

	Ref<TickSynchronizerBuffer> reader;
	reader.instantiate();
	REQUIRE(reader->begin_read(writer->get_data(), writer->get_bit_size()) == OK);
	for (uint64_t expected : values) {
		uint64_t decoded = 0;
		REQUIRE(reader->read_varuint(decoded) == OK);
		CHECK(decoded == expected);
	}
	CHECK(reader->get_remaining_bits() == 0);
}

TEST_CASE("[Modules][TickSynchronizer][Varint] Varuint requires byte alignment") {
	Ref<TickSynchronizerBuffer> writer;
	writer.instantiate();
	REQUIRE(writer->begin_write() == OK);
	REQUIRE(writer->write_bits(1, 1) == OK);
	CHECK(writer->write_varuint(1) == ERR_INVALID_DATA);
	CHECK(writer->get_bit_position() == 1);
	CHECK(writer->get_last_error() == ERR_INVALID_DATA);

	PackedByteArray data;
	REQUIRE(data.resize_initialized(2) == OK);
	data.ptrw()[0] = 0x02;
	data.ptrw()[1] = 0x00;

	Ref<TickSynchronizerBuffer> reader;
	reader.instantiate();
	REQUIRE(reader->begin_read(data) == OK);
	uint64_t prefix = 0;
	REQUIRE(reader->read_bits(1, prefix) == OK);
	uint64_t decoded = UINT64_MAX;
	CHECK(reader->read_varuint(decoded) == ERR_INVALID_DATA);
	CHECK(decoded == 0);
	CHECK(reader->get_bit_position() == 1);
}

TEST_CASE("[Modules][TickSynchronizer][Varint] Truncated varuint read is atomic") {
	PackedByteArray data;
	REQUIRE(data.resize_initialized(1) == OK);
	data.ptrw()[0] = 0x80;

	Ref<TickSynchronizerBuffer> reader;
	reader.instantiate();
	REQUIRE(reader->begin_read(data) == OK);

	uint64_t decoded = UINT64_MAX;
	CHECK(reader->read_varuint(decoded) == ERR_FILE_EOF);
	CHECK(decoded == 0);
	CHECK(reader->get_bit_position() == 0);
	CHECK(reader->get_last_error() == ERR_FILE_EOF);
}

TEST_CASE("[Modules][TickSynchronizer][Varint] Decoder rejects tenth byte overflow and continuation") {
	PackedByteArray overflow;
	REQUIRE(overflow.resize_initialized(10) == OK);
	for (int index = 0; index < 9; index++) {
		overflow.ptrw()[index] = 0xFF;
	}
	overflow.ptrw()[9] = 0x02;

	Ref<TickSynchronizerBuffer> overflow_reader;
	overflow_reader.instantiate();
	REQUIRE(overflow_reader->begin_read(overflow) == OK);
	uint64_t decoded = UINT64_MAX;
	CHECK(overflow_reader->read_varuint(decoded) == ERR_INVALID_DATA);
	CHECK(decoded == 0);
	CHECK(overflow_reader->get_bit_position() == 0);

	PackedByteArray continuation;
	REQUIRE(continuation.resize_initialized(10) == OK);
	for (int index = 0; index < 10; index++) {
		continuation.ptrw()[index] = 0x80;
	}

	Ref<TickSynchronizerBuffer> continuation_reader;
	continuation_reader.instantiate();
	REQUIRE(continuation_reader->begin_read(continuation) == OK);
	decoded = UINT64_MAX;
	CHECK(continuation_reader->read_varuint(decoded) == ERR_INVALID_DATA);
	CHECK(decoded == 0);
	CHECK(continuation_reader->get_bit_position() == 0);
}

TEST_CASE("[Modules][TickSynchronizer][Varint] Decoder rejects noncanonical overlong forms") {
	const uint8_t overlong_forms[][3] = {
		{ 0x80, 0x00, 0x00 },
		{ 0x81, 0x00, 0x00 },
		{ 0x80, 0x80, 0x00 },
	};
	const int sizes[] = { 2, 2, 3 };

	for (int form_index = 0; form_index < 3; form_index++) {
		PackedByteArray data;
		REQUIRE(data.resize_initialized(sizes[form_index]) == OK);
		for (int byte_index = 0; byte_index < sizes[form_index]; byte_index++) {
			data.ptrw()[byte_index] = overlong_forms[form_index][byte_index];
		}

		Ref<TickSynchronizerBuffer> reader;
		reader.instantiate();
		REQUIRE(reader->begin_read(data) == OK);
		uint64_t decoded = UINT64_MAX;
		CHECK(reader->read_varuint(decoded) == ERR_INVALID_DATA);
		CHECK(decoded == 0);
		CHECK(reader->get_bit_position() == 0);
	}
}

TEST_CASE("[Modules][TickSynchronizer][Varint] ZigZag canonical vectors") {
	const int64_t values[] = {
		0,
		-1,
		1,
		-2,
		2,
		-1000,
		1000,
		std::numeric_limits<int64_t>::min(),
		std::numeric_limits<int64_t>::max(),
	};
	const uint8_t expected[] = {
		0x00,
		0x01,
		0x02,
		0x03,
		0x04,
		0xCF, 0x0F,
		0xD0, 0x0F,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01,
		0xFE, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01,
	};

	Ref<TickSynchronizerBuffer> writer;
	writer.instantiate();
	REQUIRE(writer->begin_write() == OK);
	for (int64_t value : values) {
		REQUIRE(writer->write_varint(value) == OK);
	}

	const PackedByteArray data = writer->get_data();
	REQUIRE(data.size() == static_cast<int64_t>(sizeof(expected)));
	for (int64_t index = 0; index < data.size(); index++) {
		CHECK(data[index] == expected[index]);
	}
}

TEST_CASE("[Modules][TickSynchronizer][Varint] ZigZag signed extremes round trip") {
	const int64_t values[] = {
		std::numeric_limits<int64_t>::min(),
		-1000000,
		-1000,
		-2,
		-1,
		0,
		1,
		2,
		1000,
		1000000,
		std::numeric_limits<int64_t>::max(),
	};

	Ref<TickSynchronizerBuffer> writer;
	writer.instantiate();
	REQUIRE(writer->begin_write() == OK);
	for (int64_t value : values) {
		REQUIRE(writer->write_varint(value) == OK);
	}

	Ref<TickSynchronizerBuffer> reader;
	reader.instantiate();
	REQUIRE(reader->begin_read(writer->get_data(), writer->get_bit_size()) == OK);
	for (int64_t expected : values) {
		int64_t decoded = 0;
		REQUIRE(reader->read_varint(decoded) == OK);
		CHECK(decoded == expected);
	}
	CHECK(reader->get_remaining_bits() == 0);
}

TEST_CASE("[Modules][TickSynchronizer][Varint] Failed signed read clears output and preserves position") {
	PackedByteArray data;
	REQUIRE(data.resize_initialized(1) == OK);
	data.ptrw()[0] = 0x80;

	Ref<TickSynchronizerBuffer> reader;
	reader.instantiate();
	REQUIRE(reader->begin_read(data) == OK);

	int64_t decoded = 123;
	CHECK(reader->read_varint(decoded) == ERR_FILE_EOF);
	CHECK(decoded == 0);
	CHECK(reader->get_bit_position() == 0);
	CHECK(reader->get_last_error() == ERR_FILE_EOF);
}

} // namespace TestTickSynchronizerVarint
