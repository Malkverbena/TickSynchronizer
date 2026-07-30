#pragma once

#include "tests/test_macros.h"

#include "src/public/tick_synchronizer_buffer.h"

#include <cstdint>

namespace TestTickSynchronizerIntegerCodec {

TEST_CASE("[Modules][TickSynchronizer][IntegerCodec] Byte alignment writes and consumes zero padding") {
	Ref<TickSynchronizerBuffer> writer;
	writer.instantiate();

	REQUIRE(writer->begin_write() == OK);
	CHECK(writer->is_byte_aligned());
	REQUIRE(writer->write_bits(0b101, 3) == OK);
	CHECK_FALSE(writer->is_byte_aligned());
	REQUIRE(writer->align_write_to_byte() == OK);
	CHECK(writer->is_byte_aligned());
	CHECK(writer->get_bit_position() == 8);
	CHECK(writer->get_bit_size() == 8);

	const PackedByteArray data = writer->get_data();
	REQUIRE(data.size() == 1);
	CHECK(data[0] == 0x05);

	REQUIRE(writer->align_write_to_byte() == OK);
	CHECK(writer->get_bit_position() == 8);

	Ref<TickSynchronizerBuffer> reader;
	reader.instantiate();
	REQUIRE(reader->begin_read(data, 8) == OK);

	uint64_t prefix = 0;
	REQUIRE(reader->read_bits(3, prefix) == OK);
	CHECK(prefix == 0b101);
	CHECK_FALSE(reader->is_byte_aligned());
	REQUIRE(reader->align_read_to_byte() == OK);
	CHECK(reader->is_byte_aligned());
	CHECK(reader->get_remaining_bits() == 0);
}

TEST_CASE("[Modules][TickSynchronizer][IntegerCodec] Reader rejects bad alignment padding atomically") {
	PackedByteArray data;
	REQUIRE(data.resize_initialized(1) == OK);
	data.ptrw()[0] = 0xFD;

	Ref<TickSynchronizerBuffer> reader;
	reader.instantiate();
	REQUIRE(reader->begin_read(data, 8) == OK);

	uint64_t prefix = 0;
	REQUIRE(reader->read_bits(3, prefix) == OK);
	CHECK(prefix == 0b101);
	CHECK(reader->get_bit_position() == 3);

	CHECK(reader->align_read_to_byte() == ERR_INVALID_DATA);
	CHECK(reader->get_bit_position() == 3);
	CHECK(reader->get_last_error() == ERR_INVALID_DATA);

	data.ptrw()[0] = 0x05;
	REQUIRE(reader->begin_read(data, 5) == OK);
	prefix = 0;
	REQUIRE(reader->read_bits(3, prefix) == OK);
	CHECK(reader->align_read_to_byte() == ERR_FILE_EOF);
	CHECK(reader->get_bit_position() == 3);
	CHECK(reader->get_last_error() == ERR_FILE_EOF);
}

TEST_CASE("[Modules][TickSynchronizer][IntegerCodec] Fixed unsigned integers use little endian order") {
	Ref<TickSynchronizerBuffer> writer;
	writer.instantiate();
	REQUIRE(writer->begin_write() == OK);

	REQUIRE(writer->write_u8(0x5A) == OK);
	REQUIRE(writer->write_u16(0x1234) == OK);
	REQUIRE(writer->write_u32(UINT32_C(0x12345678)) == OK);
	REQUIRE(writer->write_u64(UINT64_C(0x0123456789ABCDEF)) == OK);

	const uint8_t expected[] = {
		0x5A,
		0x34, 0x12,
		0x78, 0x56, 0x34, 0x12,
		0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01,
	};
	const PackedByteArray data = writer->get_data();
	REQUIRE(data.size() == 15);
	for (int index = 0; index < 15; index++) {
		CHECK(data[index] == expected[index]);
	}

	Ref<TickSynchronizerBuffer> reader;
	reader.instantiate();
	REQUIRE(reader->begin_read(data) == OK);

	uint8_t u8 = 0;
	uint16_t u16 = 0;
	uint32_t u32 = 0;
	uint64_t u64 = 0;
	REQUIRE(reader->read_u8(u8) == OK);
	REQUIRE(reader->read_u16(u16) == OK);
	REQUIRE(reader->read_u32(u32) == OK);
	REQUIRE(reader->read_u64(u64) == OK);
	CHECK(u8 == 0x5A);
	CHECK(u16 == 0x1234);
	CHECK(u32 == UINT32_C(0x12345678));
	CHECK(u64 == UINT64_C(0x0123456789ABCDEF));
	CHECK(reader->get_remaining_bits() == 0);
}

TEST_CASE("[Modules][TickSynchronizer][IntegerCodec] Fixed unsigned integers round trip while unaligned") {
	Ref<TickSynchronizerBuffer> writer;
	writer.instantiate();
	REQUIRE(writer->begin_write() == OK);
	REQUIRE(writer->write_bits(0b101, 3) == OK);
	REQUIRE(writer->write_u8(0xA5) == OK);
	REQUIRE(writer->write_u16(0xBEEF) == OK);
	REQUIRE(writer->write_u32(UINT32_C(0xDEADBEEF)) == OK);
	REQUIRE(writer->write_u64(UINT64_C(0xFEDCBA9876543210)) == OK);

	Ref<TickSynchronizerBuffer> reader;
	reader.instantiate();
	REQUIRE(reader->begin_read(writer->get_data(), writer->get_bit_size()) == OK);

	uint64_t prefix = 0;
	uint8_t u8 = 0;
	uint16_t u16 = 0;
	uint32_t u32 = 0;
	uint64_t u64 = 0;
	REQUIRE(reader->read_bits(3, prefix) == OK);
	REQUIRE(reader->read_u8(u8) == OK);
	REQUIRE(reader->read_u16(u16) == OK);
	REQUIRE(reader->read_u32(u32) == OK);
	REQUIRE(reader->read_u64(u64) == OK);
	CHECK(prefix == 0b101);
	CHECK(u8 == 0xA5);
	CHECK(u16 == 0xBEEF);
	CHECK(u32 == UINT32_C(0xDEADBEEF));
	CHECK(u64 == UINT64_C(0xFEDCBA9876543210));
	CHECK(reader->get_remaining_bits() == 0);
}

TEST_CASE("[Modules][TickSynchronizer][IntegerCodec] Truncated fixed reads are atomic and sticky") {
	PackedByteArray data;
	REQUIRE(data.resize_initialized(3) == OK);
	data.ptrw()[0] = 0x78;
	data.ptrw()[1] = 0x56;
	data.ptrw()[2] = 0x34;

	Ref<TickSynchronizerBuffer> reader;
	reader.instantiate();
	REQUIRE(reader->begin_read(data) == OK);

	uint32_t value = UINT32_MAX;
	CHECK(reader->read_u32(value) == ERR_FILE_EOF);
	CHECK(value == 0);
	CHECK(reader->get_bit_position() == 0);
	CHECK(reader->get_last_error() == ERR_FILE_EOF);

	uint8_t byte = UINT8_MAX;
	CHECK(reader->read_u8(byte) == ERR_FILE_EOF);
	CHECK(byte == 0);
	CHECK(reader->get_bit_position() == 0);
}

} // namespace TestTickSynchronizerIntegerCodec
