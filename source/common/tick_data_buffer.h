#pragma once

#include "tick_bit_array.h"

#include "core/math/vector2.h"
#include "core/math/vector3.h"
#include "core/string/ustring.h"

// Bit-packed serialization buffer, ported from NetworkSynchronizer's `DataBuffer`.
//
// Write with `begin_write()` followed by `add_*()`; read with `begin_read()` followed by `read_*()` in the same
// order. Reads never go past the written data: a read that doesn't fit, or that is malformed, marks the buffer
// as failed and returns a default value. Reading untrusted data doesn't print errors, so a malicious peer can't
// flood the log; check `is_buffer_failed()` after parsing.
//
// The encoding doesn't depend on the engine precision: a `real_t` is always encoded as selected by the
// compression level, so single and double builds read each other's data.
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
	TickDataBuffer() {}
	TickDataBuffer(const TickBitArray &p_buffer);

	// Compares the payload bits (metadata excluded).
	bool operator==(const TickDataBuffer &p_other) const;
	bool operator!=(const TickDataBuffer &p_other) const;

	void copy(const TickDataBuffer &p_other);
	void copy(const TickBitArray &p_buffer);

	// Appends `p_count_in_bits` bits of this buffer, starting at `p_offset_in_bits`, to `r_destination`, which must be
	// writing.
	bool slice(TickDataBuffer &r_destination, int p_offset_in_bits, int p_count_in_bits) const;

	const TickBitArray &get_buffer() const { return buffer; }
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

	int get_metadata_size() const { return metadata_size; }
	// Payload size, in bits.
	int size() const { return bit_size; }
	// Metadata plus payload size, in bits.
	int total_size() const { return metadata_size + bit_size; }
	int get_bit_offset() const { return bit_offset; }
	bool get_is_reading() const { return is_reading; }

	// Skips `p_bits` bits while reading.
	void skip(int p_bits);

	// Returns `true` while reading, once all the data was read.
	bool is_end_of_buffer() const { return is_reading && bit_offset >= total_size(); }
	bool is_buffer_failed() const { return buffer_failed; }

	// Writes 0 on all the bytes.
	void zero();

	// ------------------------------------------------------------------------------------------ Serialization

	// Each `add_*()` returns the value as it's read back on the other side (after clamping or compression),
	// so the writer can use the same value as the readers.

	bool add_bool(bool p_input);
	bool read_bool();

	int64_t add_int(int64_t p_input, CompressionLevel p_compression_level);
	int64_t read_int(CompressionLevel p_compression_level);

	uint64_t add_uint(uint64_t p_input, CompressionLevel p_compression_level);
	uint64_t read_uint(CompressionLevel p_compression_level);

	// Integers of any size from 1 to 64 bits, clamped to the range of that size.
	uint64_t add_uint_bits(uint64_t p_input, int p_bits);
	uint64_t read_uint_bits(int p_bits);
	int64_t add_int_bits(int64_t p_input, int p_bits);
	int64_t read_int_bits(int p_bits);

	double add_real(double p_input, CompressionLevel p_compression_level);
	double read_real(CompressionLevel p_compression_level);

	// Stores a value in the [0, 1] range; values outside it are clamped.
	float add_positive_unit_real(float p_input, CompressionLevel p_compression_level);
	float read_positive_unit_real(CompressionLevel p_compression_level);

	// Stores a value in the [-1, 1] range; values outside it are clamped.
	float add_unit_real(float p_input, CompressionLevel p_compression_level);
	float read_unit_real(CompressionLevel p_compression_level);

	Vector2 add_vector2(const Vector2 &p_input, CompressionLevel p_compression_level);
	Vector2 read_vector2(CompressionLevel p_compression_level);

	// Stores a direction (or zero). The input is normalized when it isn't zero.
	Vector2 add_normalized_vector2(const Vector2 &p_input, CompressionLevel p_compression_level);
	Vector2 read_normalized_vector2(CompressionLevel p_compression_level);

	Vector3 add_vector3(const Vector3 &p_input, CompressionLevel p_compression_level);
	Vector3 read_vector3(CompressionLevel p_compression_level);

	// Stores a direction (or zero). The input is normalized when it isn't zero.
	Vector3 add_normalized_vector3(const Vector3 &p_input, CompressionLevel p_compression_level);
	Vector3 read_normalized_vector3(CompressionLevel p_compression_level);

	// Stores a UTF-8 string of at most `MAX_STRING_BYTES` bytes.
	void add_string(const String &p_input);
	String read_string();

	// Nests the metadata and payload of another buffer, of at most `MAX_NESTED_BUFFER_BITS` bits.
	void add_data_buffer(const TickDataBuffer &p_input);
	// `r_output` receives the nested data and is left ready for reading.
	void read_data_buffer(TickDataBuffer &r_output);

	// Stores `p_bit_count` bits taken from `p_data`, least significant bit of the first byte first.
	void add_bits(const uint8_t *p_data, int p_bit_count);
	void read_bits(uint8_t *r_data, int p_bit_count);

	// ------------------------------------------------------------------------------------------ Sizes

	void skip_bool();
	void skip_int(CompressionLevel p_compression_level);
	void skip_uint(CompressionLevel p_compression_level);
	void skip_real(CompressionLevel p_compression_level);
	void skip_positive_unit_real(CompressionLevel p_compression_level);
	void skip_unit_real(CompressionLevel p_compression_level);
	void skip_vector2(CompressionLevel p_compression_level);
	void skip_normalized_vector2(CompressionLevel p_compression_level);
	void skip_vector3(CompressionLevel p_compression_level);
	void skip_normalized_vector3(CompressionLevel p_compression_level);
	void skip_string();
	void skip_data_buffer();

	// Size in bits of a fixed size data type; 0 for `DATA_TYPE_BITS` and `DATA_TYPE_DATABUFFER`.
	static int get_bit_taken(DataType p_data_type, CompressionLevel p_compression_level);

	// Maximum absolute error introduced by the compression of a real type (for reals and vectors, relative to
	// the magnitude of the value).
	static double get_real_epsilon(DataType p_data_type, CompressionLevel p_compression_level);

	static uint64_t compress_unit_float(double p_value, double p_scale_factor);
	static double decompress_unit_float(uint64_t p_value, double p_scale_factor);

private:
	bool check_writing();
	bool check_reading(int p_bits);
	void write_bits(uint64_t p_value, int p_bits);
	uint64_t fetch_bits(int p_bits);
	void make_room_in_bits(int p_bits);
	void make_room_pad_to_next_byte();
	bool pad_to_next_byte();
	static bool is_valid_compression_level(CompressionLevel p_compression_level);
};
