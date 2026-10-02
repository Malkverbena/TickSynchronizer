// Tests of `TickBitArray` and `TickDataBuffer`: bits stored across byte boundaries, every type the buffer writes (and
// what the writer gets back), strings, nested buffers and slices, and what malformed or truncated data does: the buffer
// fails, and nothing is read out of bounds. Several cases are regressions of defects of the NetworkSynchronizer
// `DataBuffer` this one was ported from.

#pragma once

#include "../source/common/tick_bit_array.h"
#include "../source/common/tick_data_buffer.h"

#include "tests/test_macros.h"

#include <cfloat>

namespace TestTickDataBuffer {

// Bits of any length are stored and read back at any offset, across byte boundaries, up to 64 at once.
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


// The bits of a value above the count asked for aren't written over the neighboring bits.
TEST_CASE("[Modules][TickSynchronizer][TickBitArray] Extra high bits don't corrupt neighbors") {
	TickBitArray array(16);
	array.zero();
	// Only the low 3 bits of 0xFF are stored; the NetworkSynchronizer version wrote all of them.
	CHECK(array.store_bits(0, 0xFF, 3));
	uint64_t value = 0;
	CHECK(array.read_bits(0, 16, value));
	CHECK(value == 0b111);
}


// Storing or reading outside the array, or more than 64 bits, fails.
TEST_CASE("[Modules][TickSynchronizer][TickBitArray] Out of bounds access fails") {
	TickBitArray array(8);
	uint64_t value = 0;
	ERR_PRINT_OFF;
	CHECK_FALSE(array.store_bits(4, 0, 5));
	CHECK_FALSE(array.read_bits(-1, 1, value));
	CHECK_FALSE(array.read_bits(0, 65, value));
	ERR_PRINT_ON;
}


// Booleans and integers of every compression level come back as written, clamped to the range of their size.
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


// Reals come back within the epsilon of their compression level, and the writer gets exactly what the readers get.
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


// A real at level 0 always takes 64 bits, also in a single precision build.
TEST_CASE("[Modules][TickSynchronizer][TickDataBuffer] Level 0 reals always take 64 bits") {
	// The NetworkSynchronizer version wrote 32 bits for a float with level 0, while declaring 64.
	TickDataBuffer db;
	db.begin_write();
	const real_t value = real_t(1.5);
	db.add_real(value, TickDataBuffer::COMPRESSION_LEVEL_0);
	CHECK(db.size() == 64);
	CHECK(TickDataBuffer::get_bit_taken(TickDataBuffer::DATA_TYPE_REAL, TickDataBuffer::COMPRESSION_LEVEL_0) == 64);
}


// Unit reals are clamped to their range, quantized within their epsilon, and a value too small for the precision is a
// zero without sign.
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


// Vectors and directions come back as the writer got them, at every compression level; a zero direction stays zero.
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


// Strings, raw bits and nested buffers come back in order, and skipping them moves the offset as reading them does.
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


// NaN and infinities are written as 0, values beyond the encoding as its largest one; a NaN or an infinity found by a
// reader fails the buffer.
TEST_CASE("[Modules][TickSynchronizer][TickDataBuffer] Reals that aren't finite are never sent, nor accepted") {
	TickDataBuffer db;
	db.begin_write();
	ERR_PRINT_OFF;
	CHECK(db.add_real(Math::NaN, TickDataBuffer::COMPRESSION_LEVEL_0) == 0.0);
	CHECK(db.add_real(-Math::INF, TickDataBuffer::COMPRESSION_LEVEL_1) == 0.0);
	ERR_PRINT_ON;
	// Beyond the range of the encoding: its largest value, not an infinity.
	CHECK(db.add_real(1e300, TickDataBuffer::COMPRESSION_LEVEL_1) == double(FLT_MAX));
	CHECK(db.add_real(-100000.0, TickDataBuffer::COMPRESSION_LEVEL_2) == -65504.0);
	db.begin_read();
	CHECK(db.read_real(TickDataBuffer::COMPRESSION_LEVEL_0) == 0.0);
	CHECK(db.read_real(TickDataBuffer::COMPRESSION_LEVEL_1) == 0.0);
	CHECK(db.read_real(TickDataBuffer::COMPRESSION_LEVEL_1) == double(FLT_MAX));
	CHECK(db.read_real(TickDataBuffer::COMPRESSION_LEVEL_2) == -65504.0);
	CHECK_FALSE(db.is_buffer_failed());

	// A NaN written by a modified peer fails the reader.
	db.begin_write();
	db.add_uint_bits(0x7FF8000000000000ULL, 64);
	db.begin_read();
	CHECK(db.read_real(TickDataBuffer::COMPRESSION_LEVEL_0) == 0.0);
	CHECK(db.is_buffer_failed());
	// An infinity in a half float too (0x7C00).
	db.begin_write();
	db.add_uint_bits(0x7C00, 16);
	db.add_uint_bits(0, 16);
	db.add_uint_bits(0, 16);
	db.begin_read();
	CHECK(db.read_vector3(TickDataBuffer::COMPRESSION_LEVEL_2) == Vector3());
	CHECK(db.is_buffer_failed());
}


// `is_valid_utf8()` takes the well-formed sequences at the edges of each length, and refuses overlong ones, surrogates,
// truncated ones and NUL.
TEST_CASE("[Modules][TickSynchronizer][TickDataBuffer] UTF-8 validation") {
	CHECK(TickDataBuffer::is_valid_utf8(nullptr, 0));
	// Well-formed: ASCII, and the edges of each sequence length (U+0080, U+07FF, U+0800, U+D7FF, U+E000, U+FFFF,
	// U+10000, U+10FFFF).
	const char *valid[] = { "hello", "\xC2\x80", "\xDF\xBF", "\xE0\xA0\x80", "\xED\x9F\xBF", "\xEE\x80\x80", "\xEF\xBF\xBF", "\xF0\x90\x80\x80", "\xF4\x8F\xBF\xBF" };
	for (const char *text : valid) {
		CHECK_MESSAGE(TickDataBuffer::is_valid_utf8(reinterpret_cast<const uint8_t *>(text), int(strlen(text))), text);
	}
	// Malformed: a lone continuation byte, overlong encodings, a surrogate, above U+10FFFF, leads that are never
	// valid, and truncated sequences.
	const char *invalid[] = { "\x80", "\xC0\x80", "\xC1\xBF", "\xE0\x9F\xBF", "\xED\xA0\x80", "\xF0\x8F\xBF\xBF", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\xFF", "\xE2\x82", "\xF0\x90\x80", "a\xC3" };
	for (const char *text : invalid) {
		CHECK_FALSE(TickDataBuffer::is_valid_utf8(reinterpret_cast<const uint8_t *>(text), int(strlen(text))));
	}
	// NUL characters too.
	const uint8_t with_nul[3] = { 'a', 0, 'b' };
	CHECK_FALSE(TickDataBuffer::is_valid_utf8(with_nul, 3));
}


// A string longer than the reader accepts, or whose bytes aren't UTF-8, fails the buffer instead of being decoded.
TEST_CASE("[Modules][TickSynchronizer][TickDataBuffer] A string that isn't UTF-8, or is too long, fails the buffer") {
	TickDataBuffer db;
	db.begin_write();
	db.add_string(String::utf8("ação, 日本, 🎮"));
	db.add_string("abcdef");
	db.begin_read();
	CHECK(db.read_string() == String::utf8("ação, 日本, 🎮"));
	CHECK(db.read_string(6) == "abcdef");
	CHECK_FALSE(db.is_buffer_failed());

	// Longer than the reader accepts.
	db.begin_read();
	db.skip_string();
	CHECK(db.read_string(5).is_empty());
	CHECK(db.is_buffer_failed());

	// Bytes that aren't UTF-8 aren't decoded (the engine's decoder would print an error for each of them).
	const uint8_t invalid[5] = { 'o', 'k', 0xFF, 0xC0, 0x80 };
	db.begin_write();
	db.add_uint(5, TickDataBuffer::COMPRESSION_LEVEL_2);
	db.add_bits(invalid, 40);
	db.add_bool(true);
	db.begin_read();
	CHECK(db.read_string().is_empty());
	CHECK(db.is_buffer_failed());
}


// Two buffers are equal when every payload bit is; the metadata doesn't take part.
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


// The metadata is written first and doesn't count as payload; it can be rewritten with `seek()`; a slice copies bits at
// any offset.
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


// A nested buffer or a string that claims more than the packet has fails the buffer; once failed, every read returns a
// default.
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


// Writing to a buffer that is being read fails it.
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
