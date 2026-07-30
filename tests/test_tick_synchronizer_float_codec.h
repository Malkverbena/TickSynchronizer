#pragma once

#include "tests/test_macros.h"

#include "core/io/marshalls.h"
#include "src/public/tick_synchronizer_buffer.h"

#include <cstdint>
#include <limits>

namespace TestTickSynchronizerFloatCodec {

static float float32_from_bits(uint32_t p_bits) {
	MarshallFloat value;
	value.i = p_bits;
	return value.f;
}

static uint32_t float32_to_bits(float p_value) {
	MarshallFloat value;
	value.f = p_value;
	return value.i;
}

static double float64_from_bits(uint64_t p_bits) {
	MarshallDouble value;
	value.l = p_bits;
	return value.d;
}

static uint64_t float64_to_bits(double p_value) {
	MarshallDouble value;
	value.d = p_value;
	return value.l;
}

TEST_CASE("[Modules][TickSynchronizer][FloatCodec] Platform exposes IEEE 754 binary32 and binary64") {
	CHECK(sizeof(float) == sizeof(uint32_t));
	CHECK(sizeof(double) == sizeof(uint64_t));
	CHECK(std::numeric_limits<float>::is_iec559);
	CHECK(std::numeric_limits<double>::is_iec559);
}

TEST_CASE("[Modules][TickSynchronizer][FloatCodec] Float32 golden vectors preserve IEEE bit patterns") {
	const uint32_t patterns[] = {
		UINT32_C(0x00000000),
		UINT32_C(0x80000000),
		UINT32_C(0x3F800000),
		UINT32_C(0xC0200000),
		UINT32_C(0x7F800000),
		UINT32_C(0xFF800000),
		UINT32_C(0x7FC00001),
		UINT32_C(0x00000001),
	};
	const uint8_t expected[] = {
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x80,
		0x00, 0x00, 0x80, 0x3F,
		0x00, 0x00, 0x20, 0xC0,
		0x00, 0x00, 0x80, 0x7F,
		0x00, 0x00, 0x80, 0xFF,
		0x01, 0x00, 0xC0, 0x7F,
		0x01, 0x00, 0x00, 0x00,
	};

	Ref<TickSynchronizerBuffer> writer;
	writer.instantiate();
	REQUIRE(writer->begin_write() == OK);
	for (uint32_t pattern : patterns) {
		REQUIRE(writer->write_float32(float32_from_bits(pattern)) == OK);
	}

	const PackedByteArray data = writer->get_data();
	REQUIRE(data.size() == static_cast<int64_t>(sizeof(expected)));
	for (int64_t index = 0; index < data.size(); index++) {
		CHECK(data[index] == expected[index]);
	}

	Ref<TickSynchronizerBuffer> reader;
	reader.instantiate();
	REQUIRE(reader->begin_read(data) == OK);
	for (uint32_t expected_pattern : patterns) {
		float decoded = 0.0f;
		REQUIRE(reader->read_float32(decoded) == OK);
		CHECK(float32_to_bits(decoded) == expected_pattern);
	}
	CHECK(reader->get_remaining_bits() == 0);
}

TEST_CASE("[Modules][TickSynchronizer][FloatCodec] Float64 golden vectors preserve IEEE bit patterns") {
	const uint64_t patterns[] = {
		UINT64_C(0x0000000000000000),
		UINT64_C(0x8000000000000000),
		UINT64_C(0x3FF0000000000000),
		UINT64_C(0xC004000000000000),
		UINT64_C(0x7FF0000000000000),
		UINT64_C(0xFFF0000000000000),
		UINT64_C(0x7FF8000000000001),
		UINT64_C(0x0000000000000001),
	};
	const uint8_t expected[] = {
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0xC0,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x7F,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0xFF,
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x7F,
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};

	Ref<TickSynchronizerBuffer> writer;
	writer.instantiate();
	REQUIRE(writer->begin_write() == OK);
	for (uint64_t pattern : patterns) {
		REQUIRE(writer->write_float64(float64_from_bits(pattern)) == OK);
	}

	const PackedByteArray data = writer->get_data();
	REQUIRE(data.size() == static_cast<int64_t>(sizeof(expected)));
	for (int64_t index = 0; index < data.size(); index++) {
		CHECK(data[index] == expected[index]);
	}

	Ref<TickSynchronizerBuffer> reader;
	reader.instantiate();
	REQUIRE(reader->begin_read(data) == OK);
	for (uint64_t expected_pattern : patterns) {
		double decoded = 0.0;
		REQUIRE(reader->read_float64(decoded) == OK);
		CHECK(float64_to_bits(decoded) == expected_pattern);
	}
	CHECK(reader->get_remaining_bits() == 0);
}

TEST_CASE("[Modules][TickSynchronizer][FloatCodec] Explicit widths are independent of real_t") {
	Ref<TickSynchronizerBuffer> writer;
	writer.instantiate();
	REQUIRE(writer->begin_write() == OK);
	REQUIRE(writer->write_float32(1.25f) == OK);
	REQUIRE(writer->write_float64(-12345.5) == OK);
	CHECK(writer->get_bit_size() == 96);
	CHECK(writer->get_byte_size() == 12);

	Ref<TickSynchronizerBuffer> reader;
	reader.instantiate();
	REQUIRE(reader->begin_read(writer->get_data()) == OK);
	float decoded32 = 0.0f;
	double decoded64 = 0.0;
	REQUIRE(reader->read_float32(decoded32) == OK);
	REQUIRE(reader->read_float64(decoded64) == OK);
	CHECK(decoded32 == 1.25f);
	CHECK(decoded64 == -12345.5);
	CHECK(reader->get_remaining_bits() == 0);
}

TEST_CASE("[Modules][TickSynchronizer][FloatCodec] Float codecs round trip while unaligned") {
	const uint32_t expected32 = UINT32_C(0x3F800001);
	const uint64_t expected64 = UINT64_C(0x400921FB54442D18);

	Ref<TickSynchronizerBuffer> writer;
	writer.instantiate();
	REQUIRE(writer->begin_write() == OK);
	REQUIRE(writer->write_bits(0b101, 3) == OK);
	REQUIRE(writer->write_float32(float32_from_bits(expected32)) == OK);
	REQUIRE(writer->write_float64(float64_from_bits(expected64)) == OK);

	Ref<TickSynchronizerBuffer> reader;
	reader.instantiate();
	REQUIRE(reader->begin_read(writer->get_data(), writer->get_bit_size()) == OK);
	uint64_t prefix = 0;
	float decoded32 = 0.0f;
	double decoded64 = 0.0;
	REQUIRE(reader->read_bits(3, prefix) == OK);
	REQUIRE(reader->read_float32(decoded32) == OK);
	REQUIRE(reader->read_float64(decoded64) == OK);
	CHECK(prefix == 0b101);
	CHECK(float32_to_bits(decoded32) == expected32);
	CHECK(float64_to_bits(decoded64) == expected64);
	CHECK(reader->get_remaining_bits() == 0);
}

TEST_CASE("[Modules][TickSynchronizer][FloatCodec] Truncated float32 read is atomic and sticky") {
	PackedByteArray data;
	REQUIRE(data.resize_initialized(3) == OK);
	data.ptrw()[0] = 0x00;
	data.ptrw()[1] = 0x00;
	data.ptrw()[2] = 0x80;

	Ref<TickSynchronizerBuffer> reader;
	reader.instantiate();
	REQUIRE(reader->begin_read(data) == OK);
	float value = 123.0f;
	CHECK(reader->read_float32(value) == ERR_FILE_EOF);
	CHECK(float32_to_bits(value) == UINT32_C(0x00000000));
	CHECK(reader->get_bit_position() == 0);
	CHECK(reader->get_last_error() == ERR_FILE_EOF);
}

TEST_CASE("[Modules][TickSynchronizer][FloatCodec] Truncated float64 read is atomic and sticky") {
	PackedByteArray data;
	REQUIRE(data.resize_initialized(7) == OK);
	for (int index = 0; index < 7; index++) {
		data.ptrw()[index] = 0xFF;
	}

	Ref<TickSynchronizerBuffer> reader;
	reader.instantiate();
	REQUIRE(reader->begin_read(data) == OK);
	double value = 123.0;
	CHECK(reader->read_float64(value) == ERR_FILE_EOF);
	CHECK(float64_to_bits(value) == UINT64_C(0x0000000000000000));
	CHECK(reader->get_bit_position() == 0);
	CHECK(reader->get_last_error() == ERR_FILE_EOF);
}

} // namespace TestTickSynchronizerFloatCodec
