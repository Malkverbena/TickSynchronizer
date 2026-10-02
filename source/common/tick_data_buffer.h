// The bit-packed buffer of the messages: `TickDataBuffer`.
//
// A serialization buffer of bits, ported from NetworkSynchronizer's `DataBuffer`. Write with `begin_write()` followed
// by `add_*()`; read with `begin_read()` followed by `read_*()` in the same order. Reads never go past the written
// data: a read that doesn't fit, or that is malformed, marks the buffer as failed and returns a default value. Reading
// untrusted data doesn't print errors, so a malicious peer can't flood the log; check `is_buffer_failed()` after
// parsing.
//
// The encoding doesn't depend on the engine precision: a `real_t` is always encoded as selected by the compression
// level, so single and double builds read each other's data.

#pragma once

#include "tick_bit_array.h"

#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/string/ustring.h"

class TickDataBuffer {
public:
	enum DataType {
		DATA_TYPE_BOOL,
		DATA_TYPE_INT,
		DATA_TYPE_UINT,
		DATA_TYPE_REAL,
		DATA_TYPE_POSITIVE_UNIT_REAL,
		DATA_TYPE_UNIT_REAL,
		DATA_TYPE_VECTOR2,
		DATA_TYPE_NORMALIZED_VECTOR2,
		DATA_TYPE_VECTOR3,
		DATA_TYPE_NORMALIZED_VECTOR3,
		DATA_TYPE_BITS,
		// The only dynamically sized value.
		DATA_TYPE_DATABUFFER,
	};

	// Bits used by each data type:
	//
	// | Type                 | Level 0      | Level 1      | Level 2      | Level 3      |
	// |----------------------|--------------|--------------|--------------|--------------|
	// | bool                 | 1            | 1            | 1            | 1            |
	// | int / uint           | 64           | 32           | 16           | 8            |
	// | real                 | 64 (binary64)| 32 (binary32)| 16 (binary16)| 16 (binary16)|
	// | positive unit real   | 10           | 8            | 6            | 4            |
	// | unit real            | 11           | 9            | 7            | 5            |
	// | vector2 / vector3    | 2x / 3x real | 2x / 3x real | 2x / 3x real | 2x / 3x real |
	// | normalized vector2   | 12           | 11           | 10           | 9            |
	// | normalized vector3   | 3x unit real | 3x unit real | 3x unit real | 3x unit real |
	//
	// Integers are clamped to the range of the selected size. Use `get_real_epsilon()` to get the precision
	// loss of a real type.
	enum CompressionLevel {
		COMPRESSION_LEVEL_0 = 0,
		COMPRESSION_LEVEL_1 = 1,
		COMPRESSION_LEVEL_2 = 2,
		COMPRESSION_LEVEL_3 = 3,
	};

	// Maximum size, in bits, of a `TickDataBuffer` nested with `add_data_buffer()`.
	static constexpr int MAX_NESTED_BUFFER_BITS = UINT16_MAX;

	// Maximum size, in bytes, of a UTF-8 string stored with `add_string()`.
	static constexpr int MAX_STRING_BYTES = UINT16_MAX;

private:
	int metadata_size = 0;
	int bit_offset = 0;
	int bit_size = 0;
	bool is_reading = false;
	bool buffer_failed = false;
	TickBitArray buffer;

public:
	// An empty buffer, ready to be written.
	TickDataBuffer() {}


	// A buffer with a copy of `p_buffer` as its payload, ready to be read.
	TickDataBuffer(const TickBitArray &p_buffer);


	// Compares the payload bits (metadata excluded).
	bool operator==(const TickDataBuffer &p_other) const;


	// The opposite of `operator==`.
	bool operator!=(const TickDataBuffer &p_other) const;


	// Becomes a copy of `p_other`: its bits, its sizes, where it was reading or writing, and whether it failed.
	void copy(const TickDataBuffer &p_other);


	// Takes a copy of `p_buffer` as the payload, with no metadata, and is left ready for reading.
	void copy(const TickBitArray &p_buffer);


	// Appends `p_count_in_bits` bits of this buffer, starting at `p_offset_in_bits`, to `r_destination`, which must be
	// writing.
	bool slice(TickDataBuffer &r_destination, int p_offset_in_bits, int p_count_in_bits) const;


	// The bits of the buffer: the metadata followed by the payload.
	const TickBitArray &get_buffer() const { return buffer; }


	// The bits of the buffer, to be changed in place.
	TickBitArray &get_buffer_mut() { return buffer; }


	// Starts writing from the beginning. The first `p_metadata_size` bits written are metadata (for example a
	// header), and the rest is the payload measured by `size()`.
	void begin_write(int p_metadata_size = 0);


	// Starts reading from the beginning.
	void begin_read();


	// Shrinks the storage to the used size.
	void dry();


	// Moves the offset to a specific bit; seeking past the end isn't allowed.
	void seek(int p_bits);


	// Sets the metadata and payload size, in bits.
	void shrink_to(int p_metadata_bit_size, int p_bit_size);


	// Metadata size, in bits.
	int get_metadata_size() const { return metadata_size; }


	// Payload size, in bits.
	int size() const { return bit_size; }


	// Metadata plus payload size, in bits.
	int total_size() const { return metadata_size + bit_size; }


	// The bit the next read or write happens at.
	int get_bit_offset() const { return bit_offset; }


	// Whether the buffer is being read (after `begin_read()`) rather than written.
	bool get_is_reading() const { return is_reading; }


	// Skips `p_bits` bits while reading.
	void skip(int p_bits);


	// Returns `true` while reading, once all the data was read.
	bool is_end_of_buffer() const { return is_reading && bit_offset >= total_size(); }


	// Whether a read or a write failed since the last `begin_read()` or `begin_write()`: what was read after that can't
	// be trusted.
	bool is_buffer_failed() const { return buffer_failed; }


	// Fails the buffer for a value that fit but was malformed (see `TickCodec::decode()`).
	void mark_failed() { buffer_failed = true; }


	// Writes 0 on all the bytes.
	void zero();


	// ------------------------------------------------------------------------------------------ Serialization

	// Each `add_*()` returns the value as it's read back on the other side (after clamping or compression),
	// so the writer can use the same value as the readers.

	// Writes a boolean, in 1 bit.
	bool add_bool(bool p_input);


	// Reads a boolean; `false` if the buffer fails.
	bool read_bool();


	// Writes a signed integer with the size of the compression level, clamped to its range.
	int64_t add_int(int64_t p_input, CompressionLevel p_compression_level);


	// Reads a signed integer written with `add_int()` at the same compression level.
	int64_t read_int(CompressionLevel p_compression_level);


	// Writes an unsigned integer with the size of the compression level, clamped to its range.
	uint64_t add_uint(uint64_t p_input, CompressionLevel p_compression_level);


	// Reads an unsigned integer written with `add_uint()` at the same compression level.
	uint64_t read_uint(CompressionLevel p_compression_level);


	// Writes an unsigned integer in `p_bits` bits (1 to 64), clamped to the range of that size.
	uint64_t add_uint_bits(uint64_t p_input, int p_bits);


	// Reads an unsigned integer of `p_bits` bits.
	uint64_t read_uint_bits(int p_bits);


	// Writes a signed integer in `p_bits` bits (1 to 64), in two's complement, clamped to the range of that size.
	int64_t add_int_bits(int64_t p_input, int p_bits);


	// Reads a signed integer of `p_bits` bits, extending its sign.
	int64_t read_int_bits(int p_bits);


	// Writes a real as binary64, binary32 or binary16, by the compression level. NaN and infinities are sent as 0 (with
	// an error), and values beyond the range of the encoding as its largest value.
	double add_real(double p_input, CompressionLevel p_compression_level);


	// Reads a real written with `add_real()`; one that isn't finite fails the buffer.
	double read_real(CompressionLevel p_compression_level);


	// Stores a value in the [0, 1] range; values outside it are clamped.
	float add_positive_unit_real(float p_input, CompressionLevel p_compression_level);


	// Reads a value in the [0, 1] range written with `add_positive_unit_real()`.
	float read_positive_unit_real(CompressionLevel p_compression_level);


	// Stores a value in the [-1, 1] range; values outside it are clamped.
	float add_unit_real(float p_input, CompressionLevel p_compression_level);


	// Reads a value in the [-1, 1] range written with `add_unit_real()`.
	float read_unit_real(CompressionLevel p_compression_level);


	// Writes the two components of a vector as reals (see `add_real()`).
	Vector2 add_vector2(const Vector2 &p_input, CompressionLevel p_compression_level);


	// Reads a vector written with `add_vector2()`.
	Vector2 read_vector2(CompressionLevel p_compression_level);


	// Stores a direction (or zero). The input is normalized when it isn't zero.
	Vector2 add_normalized_vector2(const Vector2 &p_input, CompressionLevel p_compression_level);


	// Reads a direction (or zero) written with `add_normalized_vector2()`.
	Vector2 read_normalized_vector2(CompressionLevel p_compression_level);


	// Writes the three components of a vector as reals (see `add_real()`).
	Vector3 add_vector3(const Vector3 &p_input, CompressionLevel p_compression_level);


	// Reads a vector written with `add_vector3()`.
	Vector3 read_vector3(CompressionLevel p_compression_level);


	// Stores a direction (or zero). The input is normalized when it isn't zero.
	Vector3 add_normalized_vector3(const Vector3 &p_input, CompressionLevel p_compression_level);


	// Reads a direction (or zero) written with `add_normalized_vector3()`.
	Vector3 read_normalized_vector3(CompressionLevel p_compression_level);


	// Stores a string as UTF-8, of at most `MAX_STRING_BYTES` bytes. A longer one, or one the readers would refuse (a
	// NUL character, an unpaired surrogate), fails the buffer.
	void add_string(const String &p_input);


	// Reads a string written with `add_string()`. One longer than `p_max_bytes`, or that isn't valid UTF-8, fails the
	// buffer.
	String read_string(int p_max_bytes = MAX_STRING_BYTES);


	// Nests the metadata and payload of another buffer, of at most `MAX_NESTED_BUFFER_BITS` bits.
	void add_data_buffer(const TickDataBuffer &p_input);


	// Reads a buffer nested with `add_data_buffer()`: `r_output` receives the nested data and is left ready for
	// reading.
	void read_data_buffer(TickDataBuffer &r_output);


	// Stores `p_bit_count` bits taken from `p_data`, least significant bit of the first byte first.
	void add_bits(const uint8_t *p_data, int p_bit_count);


	// Reads `p_bit_count` bits into `r_data`, least significant bit of the first byte first.
	void read_bits(uint8_t *r_data, int p_bit_count);


	// ------------------------------------------------------------------------------------------ Sizes

	// Skips a boolean while reading.
	void skip_bool();


	// Skips a signed integer of the given compression level.
	void skip_int(CompressionLevel p_compression_level);


	// Skips an unsigned integer of the given compression level.
	void skip_uint(CompressionLevel p_compression_level);


	// Skips a real of the given compression level.
	void skip_real(CompressionLevel p_compression_level);


	// Skips a value in the [0, 1] range of the given compression level.
	void skip_positive_unit_real(CompressionLevel p_compression_level);


	// Skips a value in the [-1, 1] range of the given compression level.
	void skip_unit_real(CompressionLevel p_compression_level);


	// Skips a 2D vector of the given compression level.
	void skip_vector2(CompressionLevel p_compression_level);


	// Skips a 2D direction of the given compression level.
	void skip_normalized_vector2(CompressionLevel p_compression_level);


	// Skips a 3D vector of the given compression level.
	void skip_vector3(CompressionLevel p_compression_level);


	// Skips a 3D direction of the given compression level.
	void skip_normalized_vector3(CompressionLevel p_compression_level);


	// Skips a string: reads its length and jumps over its bytes.
	void skip_string();


	// Skips a nested buffer: reads its size and jumps over its bits.
	void skip_data_buffer();


	// Size in bits of a fixed size data type; 0 for `DATA_TYPE_BITS` and `DATA_TYPE_DATABUFFER`.
	static int get_bit_taken(DataType p_data_type, CompressionLevel p_compression_level);


	// Maximum absolute error introduced by the compression of a real type (for reals and vectors, relative to
	// the magnitude of the value).
	static double get_real_epsilon(DataType p_data_type, CompressionLevel p_compression_level);


	// Maps a value in the [0, 1] range to an integer from 0 to `p_scale_factor`. Values outside the range are clamped,
	// and NaN gives 0.
	static uint64_t compress_unit_float(double p_value, double p_scale_factor);


	// Maps an integer from 0 to `p_scale_factor` back to the [0, 1] range.
	static double decompress_unit_float(uint64_t p_value, double p_scale_factor);


	// Whether the bytes are well-formed UTF-8 without NUL characters. Checked without printing anything, unlike the
	// engine's decoder, which prints an error for every invalid byte: check untrusted data before decoding it.
	static bool is_valid_utf8(const uint8_t *p_bytes, int p_length);


private:
	// Whether the buffer is being written; if it's being read, fails it and prints an error.
	bool check_writing();


	// Whether `p_bits` more bits can be read. If not, fails the buffer: silently when the data is short (it may come
	// from an untrusted peer), with an error when the buffer is being written.
	bool check_reading(int p_bits);


	// Writes the `p_bits` low bits of `p_value` at the offset, growing the storage as needed, and moves past them.
	void write_bits(uint64_t p_value, int p_bits);


	// Reads `p_bits` bits at the offset and moves past them; the caller checked that they are there.
	uint64_t fetch_bits(int p_bits);


	// Makes sure `p_bits` more bits fit after the offset, growing the storage and the payload size.
	void make_room_in_bits(int p_bits);


	// While writing: moves the offset to the next whole byte, making room for the padding.
	void make_room_pad_to_next_byte();


	// While reading: moves the offset to the next whole byte; `false` if that is past the data.
	bool pad_to_next_byte();


	// Whether the value is one of the four compression levels.
	static bool is_valid_compression_level(CompressionLevel p_compression_level);
};
