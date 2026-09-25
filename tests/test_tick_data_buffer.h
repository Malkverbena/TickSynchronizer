#pragma once

#include "../source/common/tick_bit_array.h"
#include "../source/common/tick_data_buffer.h"

#include "tests/test_macros.h"

namespace TestTickDataBuffer {

TEST_CASE("[Modules][TickSynchronizer][TickBitArray] Store and read bits across byte boundaries") {
	TickBitArray array(128);
	CHECK(array.size_in_bits() == 128);
	CHECK(array.size_in_bytes() == 16);

	CHECK(array.store_bits(3, 0b10110, 5));
	CHECK(array.store_bits(8, 0xABCD, 16));
	CHECK(array.store_bits(29, 0x123456789ULL, 36));

	uint64_t value = 0;
	CHECK(array.read_bits(3, 5, value));
	CHECK(value == 0b10110);
	CHECK(array.read_bits(8, 16, value));
	CHECK(value == 0xABCD);
	CHECK(array.read_bits(29, 36, value));
	CHECK(value == 0x123456789ULL);

	CHECK(array.store_bits(64, UINT64_MAX, 64));
	CHECK(array.read_bits(64, 64, value));
	CHECK(value == UINT64_MAX);
}

TEST_CASE("[Modules][TickSynchronizer][TickBitArray] Extra high bits don't corrupt neighbors") {
	TickBitArray array(16);
	array.zero();
	// Only the low 3 bits of 0xFF are stored; the NetworkSynchronizer version wrote all of them.
	CHECK(array.store_bits(0, 0xFF, 3));
	uint64_t value = 0;
	CHECK(array.read_bits(0, 16, value));
	CHECK(value == 0b111);
}

TEST_CASE("[Modules][TickSynchronizer][TickBitArray] Out of bounds access fails") {
	TickBitArray array(8);
	uint64_t value = 0;
	ERR_PRINT_OFF;
	CHECK_FALSE(array.store_bits(4, 0, 5));
	CHECK_FALSE(array.read_bits(-1, 1, value));
	CHECK_FALSE(array.read_bits(0, 65, value));
	ERR_PRINT_ON;
}

TEST_CASE("[Modules][TickSynchronizer][TickDataBuffer] Bools and integers round trip") {
	TickDataBuffer db;
	db.begin_write();
	db.add_bool(true);
	db.add_bool(false);
	CHECK(db.add_int(-5, TickDataBuffer::COMPRESSION_LEVEL_3) == -5);
	CHECK(db.add_int(1000, TickDataBuffer::COMPRESSION_LEVEL_3) == 127);
	CHECK(db.add_int(-40000, TickDataBuffer::COMPRESSION_LEVEL_2) == INT16_MIN);
	CHECK(db.add_int(INT64_MIN, TickDataBuffer::COMPRESSION_LEVEL_0) == INT64_MIN);
	CHECK(db.add_int(-123456789, TickDataBuffer::COMPRESSION_LEVEL_1) == -123456789);
	CHECK(db.add_uint(300, TickDataBuffer::COMPRESSION_LEVEL_3) == 255);
	CHECK(db.add_uint(UINT64_MAX, TickDataBuffer::COMPRESSION_LEVEL_0) == UINT64_MAX);
	CHECK(db.add_uint(70000, TickDataBuffer::COMPRESSION_LEVEL_1) == 70000);
	CHECK(db.size() == 1 + 1 + 8 + 8 + 16 + 64 + 32 + 8 + 64 + 32);

	db.begin_read();
	CHECK(db.read_bool());
	CHECK_FALSE(db.read_bool());
	CHECK(db.read_int(TickDataBuffer::COMPRESSION_LEVEL_3) == -5);
	CHECK(db.read_int(TickDataBuffer::COMPRESSION_LEVEL_3) == 127);
	CHECK(db.read_int(TickDataBuffer::COMPRESSION_LEVEL_2) == INT16_MIN);
	CHECK(db.read_int(TickDataBuffer::COMPRESSION_LEVEL_0) == INT64_MIN);
	CHECK(db.read_int(TickDataBuffer::COMPRESSION_LEVEL_1) == -123456789);
	CHECK(db.read_uint(TickDataBuffer::COMPRESSION_LEVEL_3) == 255);
	CHECK(db.read_uint(TickDataBuffer::COMPRESSION_LEVEL_0) == UINT64_MAX);
	CHECK(db.read_uint(TickDataBuffer::COMPRESSION_LEVEL_1) == 70000);
	CHECK(db.is_end_of_buffer());
	CHECK_FALSE(db.is_buffer_failed());
}

TEST_CASE("[Modules][TickSynchronizer][TickDataBuffer] Reals round trip within the epsilon") {
	const double values[] = { 0.0, 1.0, -1.0, 3.14159265358979, -512.25, 1234.5678, 1e-3 };
	for (int level = 0; level < 4; level++) {
		const TickDataBuffer::CompressionLevel compression = TickDataBuffer::CompressionLevel(level);
		const double epsilon = TickDataBuffer::get_real_epsilon(TickDataBuffer::DATA_TYPE_REAL, compression);

		TickDataBuffer db;
		db.begin_write();
		double written[7];
		for (int i = 0; i < 7; i++) {
			written[i] = db.add_real(values[i], compression);
			CHECK(Math::abs(written[i] - values[i]) <= MAX(Math::abs(values[i]), 1e-3) * epsilon);
		}
		CHECK(db.size() == 7 * TickDataBuffer::get_bit_taken(TickDataBuffer::DATA_TYPE_REAL, compression));

		db.begin_read();
		for (int i = 0; i < 7; i++) {
			// The writer and the readers get exactly the same value.
			CHECK(db.read_real(compression) == written[i]);
		}
		CHECK_FALSE(db.is_buffer_failed());
	}
}

TEST_CASE("[Modules][TickSynchronizer][TickDataBuffer] Level 0 reals always take 64 bits") {
	// The NetworkSynchronizer version wrote 32 bits for a float with level 0, while declaring 64.
	TickDataBuffer db;
	db.begin_write();
	const real_t value = real_t(1.5);
	db.add_real(value, TickDataBuffer::COMPRESSION_LEVEL_0);
	CHECK(db.size() == 64);
	CHECK(TickDataBuffer::get_bit_taken(TickDataBuffer::DATA_TYPE_REAL, TickDataBuffer::COMPRESSION_LEVEL_0) == 64);
}

TEST_CASE("[Modules][TickSynchronizer][TickDataBuffer] Unit reals") {
	for (int level = 0; level < 4; level++) {
		const TickDataBuffer::CompressionLevel compression = TickDataBuffer::CompressionLevel(level);
		const double epsilon = TickDataBuffer::get_real_epsilon(TickDataBuffer::DATA_TYPE_UNIT_REAL, compression);

		TickDataBuffer db;
		db.begin_write();
		const float positive = db.add_positive_unit_real(0.3f, compression);
		const float clamped = db.add_positive_unit_real(2.0f, compression);
		const float negative = db.add_unit_real(-0.7f, compression);
		const float tiny = db.add_unit_real(-0.00001f, compression);
		CHECK(Math::abs(positive - 0.3f) <= epsilon + 1e-6);
		CHECK(clamped == 1.0f);
		CHECK(Math::abs(negative + 0.7f) <= epsilon + 1e-6);
		// Too small for the precision: zero, without a sign.
		CHECK(tiny == 0.0f);
		uint32_t tiny_bits;
		memcpy(&tiny_bits, &tiny, sizeof(uint32_t));
		CHECK(tiny_bits == 0);

		db.begin_read();
		CHECK(db.read_positive_unit_real(compression) == positive);
		CHECK(db.read_positive_unit_real(compression) == clamped);
		CHECK(db.read_unit_real(compression) == negative);
		CHECK(db.read_unit_real(compression) == tiny);
		CHECK_FALSE(db.is_buffer_failed());
	}
}

TEST_CASE("[Modules][TickSynchronizer][TickDataBuffer] Vectors") {
	for (int level = 0; level < 4; level++) {
		const TickDataBuffer::CompressionLevel compression = TickDataBuffer::CompressionLevel(level);

		TickDataBuffer db;
		db.begin_write();
		const Vector2 v2 = db.add_vector2(Vector2(1.25, -3.5), compression);
		const Vector3 v3 = db.add_vector3(Vector3(-7.0, 0.5, 100.0), compression);
		const Vector2 n2 = db.add_normalized_vector2(Vector2(3.0, 4.0), compression);
		const Vector2 zero2 = db.add_normalized_vector2(Vector2(), compression);
		const Vector3 n3 = db.add_normalized_vector3(Vector3(1.0, -2.0, 2.0), compression);
		const Vector3 zero3 = db.add_normalized_vector3(Vector3(), compression);

		CHECK(v2.is_equal_approx(Vector2(1.25, -3.5)));
		CHECK(v3.is_equal_approx(Vector3(-7.0, 0.5, 100.0)));
		const double n2_epsilon = TickDataBuffer::get_real_epsilon(TickDataBuffer::DATA_TYPE_NORMALIZED_VECTOR2, compression);
		CHECK(Math::abs(n2.x - 0.6) <= n2_epsilon + 1e-5);
		CHECK(Math::abs(n2.y - 0.8) <= n2_epsilon + 1e-5);
		CHECK(zero2 == Vector2());
		const double n3_epsilon = TickDataBuffer::get_real_epsilon(TickDataBuffer::DATA_TYPE_NORMALIZED_VECTOR3, compression);
		CHECK(Math::abs(n3.x - 1.0 / 3.0) <= n3_epsilon + 1e-5);
		CHECK(Math::abs(n3.y + 2.0 / 3.0) <= n3_epsilon + 1e-5);
		CHECK(Math::abs(n3.z - 2.0 / 3.0) <= n3_epsilon + 1e-5);
		CHECK(zero3 == Vector3());

		db.begin_read();
		CHECK(db.read_vector2(compression) == v2);
		CHECK(db.read_vector3(compression) == v3);
		CHECK(db.read_normalized_vector2(compression) == n2);
		CHECK(db.read_normalized_vector2(compression) == zero2);
		CHECK(db.read_normalized_vector3(compression) == n3);
		CHECK(db.read_normalized_vector3(compression) == zero3);
		CHECK(db.is_end_of_buffer());
		CHECK_FALSE(db.is_buffer_failed());
	}
}

TEST_CASE("[Modules][TickSynchronizer][TickDataBuffer] Strings, bits and nested buffers") {
	TickDataBuffer nested;
	nested.begin_write();
	nested.add_uint(42, TickDataBuffer::COMPRESSION_LEVEL_3);
	nested.add_bool(true);

	const uint8_t bits[2] = { 0xA5, 0x03 };

	TickDataBuffer db;
	db.begin_write();
	db.add_bool(true);
	db.add_string(String::utf8("Olá, mundo"));
	db.add_data_buffer(nested);
	db.add_bits(bits, 10);
	db.add_string(String());
	db.add_uint(7, TickDataBuffer::COMPRESSION_LEVEL_3);

	db.begin_read();
	CHECK(db.read_bool());
	CHECK(db.read_string() == String::utf8("Olá, mundo"));
	TickDataBuffer read_nested;
	db.read_data_buffer(read_nested);
	CHECK(read_nested == nested);
	CHECK(read_nested.read_uint(TickDataBuffer::COMPRESSION_LEVEL_3) == 42);
	CHECK(read_nested.read_bool());
	uint8_t read_bits[2] = { 0, 0 };
	db.read_bits(read_bits, 10);
	CHECK(read_bits[0] == 0xA5);
	CHECK(read_bits[1] == 0x03);
	CHECK(db.read_string().is_empty());
	CHECK(db.read_uint(TickDataBuffer::COMPRESSION_LEVEL_3) == 7);
	CHECK_FALSE(db.is_buffer_failed());

	// Skipping gives the same offsets as reading.
	db.begin_read();
	db.skip_bool();
	db.skip_string();
	db.skip_data_buffer();
	db.skip(10);
	db.skip_string();
	CHECK(db.read_uint(TickDataBuffer::COMPRESSION_LEVEL_3) == 7);
	CHECK_FALSE(db.is_buffer_failed());
}

TEST_CASE("[Modules][TickSynchronizer][TickDataBuffer] Equality compares every payload byte") {
	// The NetworkSynchronizer version compared only `sizeof(int)` bytes.
	TickDataBuffer a;
	TickDataBuffer b;
	a.begin_write();
	b.begin_write();
	for (int i = 0; i < 10; i++) {
		a.add_uint(i, TickDataBuffer::COMPRESSION_LEVEL_3);
		b.add_uint(i == 9 ? 99 : i, TickDataBuffer::COMPRESSION_LEVEL_3);
	}
	CHECK(a != b);

	TickDataBuffer c;
	c.copy(a);
	CHECK(a == c);

	// Metadata doesn't take part in the comparison.
	const uint8_t d_metadata = 0x1F;
	TickDataBuffer d;
	d.begin_write(5);
	d.add_bits(&d_metadata, 5);
	for (int i = 0; i < 10; i++) {
		d.add_uint(i, TickDataBuffer::COMPRESSION_LEVEL_3);
	}
	CHECK(a == d);
}

TEST_CASE("[Modules][TickSynchronizer][TickDataBuffer] Metadata and slices") {
	TickDataBuffer db;
	// The first 8 bits written are the metadata.
	db.begin_write(8);
	db.add_uint(0, TickDataBuffer::COMPRESSION_LEVEL_3);
	CHECK(db.size() == 0);
	db.add_uint(0xAB, TickDataBuffer::COMPRESSION_LEVEL_3);
	db.add_uint(0xCD, TickDataBuffer::COMPRESSION_LEVEL_3);
	CHECK(db.get_metadata_size() == 8);
	CHECK(db.size() == 16);
	CHECK(db.total_size() == 24);

	// Rewrite the metadata once the payload is known.
	db.seek(0);
	db.add_uint(2, TickDataBuffer::COMPRESSION_LEVEL_3);
	CHECK(db.size() == 16);

	TickDataBuffer slice;
	slice.begin_write();
	CHECK(db.slice(slice, 12, 8));
	slice.begin_read();
	CHECK(slice.read_uint(TickDataBuffer::COMPRESSION_LEVEL_3) == 0xDA);

	db.begin_read();
	CHECK(db.read_uint(TickDataBuffer::COMPRESSION_LEVEL_3) == 2);
	CHECK(db.read_uint(TickDataBuffer::COMPRESSION_LEVEL_3) == 0xAB);
	CHECK(db.read_uint(TickDataBuffer::COMPRESSION_LEVEL_3) == 0xCD);
	CHECK(db.is_end_of_buffer());
}

TEST_CASE("[Modules][TickSynchronizer][TickDataBuffer] Malformed data fails without reading out of bounds") {
	// A nested buffer that claims more bits than the packet has; the NetworkSynchronizer version read past the end.
	TickDataBuffer db;
	db.begin_write();
	db.add_uint(60000, TickDataBuffer::COMPRESSION_LEVEL_2);
	db.add_uint(1, TickDataBuffer::COMPRESSION_LEVEL_3);

	TickDataBuffer received(db.get_buffer());
	TickDataBuffer nested;
	received.read_data_buffer(nested);
	CHECK(received.is_buffer_failed());
	CHECK(nested.size() == 0);

	// A string that claims more bytes than the packet has.
	received.begin_read();
	CHECK(received.read_string().is_empty());
	CHECK(received.is_buffer_failed());

	// Once failed, the next reads keep failing and return defaults.
	CHECK(received.read_uint(TickDataBuffer::COMPRESSION_LEVEL_3) == 0);
	CHECK(received.is_buffer_failed());

	// Reading past the end.
	TickDataBuffer small;
	small.begin_write();
	small.add_bool(true);
	small.begin_read();
	small.read_bool();
	CHECK(small.read_uint(TickDataBuffer::COMPRESSION_LEVEL_0) == 0);
	CHECK(small.is_buffer_failed());
}

TEST_CASE("[Modules][TickSynchronizer][TickDataBuffer] Writing while reading is an error") {
	TickDataBuffer db;
	db.begin_write();
	db.add_bool(true);
	db.begin_read();
	ERR_PRINT_OFF;
	db.add_bool(true);
	ERR_PRINT_ON;
	CHECK(db.is_buffer_failed());
}

} // namespace TestTickDataBuffer
