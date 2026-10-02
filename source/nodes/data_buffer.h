// The buffer of bits as scripts see it: `DataBuffer`.
//
// It gives scripts access to a `TickDataBuffer`: the input of a `TickObject` is written and read with it. It operates
// on a buffer of its own, or on the engine's buffer handed to it with `wrap()`, so the input of a tick is written
// straight where the engine reads it. Every `add_*()` returns the value as the other side reads it, after clamping or
// compression.

#pragma once

#include "../codec/tick_codec.h"
#include "../common/tick_data_buffer.h"

#include "core/object/class_db.h"
#include "core/object/ref_counted.h"

class DataBuffer : public RefCounted {
	GDCLASS(DataBuffer, RefCounted);

public:
	enum CompressionLevel {
		COMPRESSION_LEVEL_0 = TickDataBuffer::COMPRESSION_LEVEL_0,
		COMPRESSION_LEVEL_1 = TickDataBuffer::COMPRESSION_LEVEL_1,
		COMPRESSION_LEVEL_2 = TickDataBuffer::COMPRESSION_LEVEL_2,
		COMPRESSION_LEVEL_3 = TickDataBuffer::COMPRESSION_LEVEL_3,
	};

private:
	TickDataBuffer buffer;
	TickDataBuffer *target = &buffer;

protected:
	// Exposes the class to scripts.
	static void _bind_methods();


public:
	// Makes this object operate on `p_buffer` (owned by the caller) instead of its own buffer.
	void wrap(TickDataBuffer *p_buffer);


	// The buffer this object operates on: its own, or the one given to `wrap()`.
	TickDataBuffer &get_buffer() { return *target; }


	// Starts writing from the beginning, discarding what the buffer had.
	void begin_write();


	// Starts reading from the beginning.
	void begin_read();


	// The size of what was written, in bits.
	int get_size() const;


	// The bit the next read or write happens at.
	int get_bit_offset() const;


	// Whether the buffer is being read rather than written.
	bool is_reading() const;


	// Whether a read or a write failed: what was read after that can't be trusted.
	bool is_failed() const;


	// Whether everything that was written has been read.
	bool is_end_of_buffer() const;


	// Writes a boolean, in 1 bit.
	bool add_bool(bool p_value);


	// Reads a boolean.
	bool read_bool();


	// Writes a signed integer in `p_bits` bits (1 to 64), clamped to the range of that size.
	int64_t add_int(int64_t p_value, int p_bits);


	// Reads a signed integer of `p_bits` bits.
	int64_t read_int(int p_bits);


	// Writes an unsigned integer in `p_bits` bits, clamped to the range of that size; a negative value is written as 0.
	// At most 63 bits: a script's integer is signed.
	int64_t add_uint(int64_t p_value, int p_bits);


	// Reads an unsigned integer of `p_bits` bits (1 to 63).
	int64_t read_uint(int p_bits);


	// Writes a real as binary64, binary32 or binary16, by the compression level.
	double add_real(double p_value, CompressionLevel p_compression_level);


	// Reads a real written at the same compression level.
	double read_real(CompressionLevel p_compression_level);


	// Writes a value in the [0, 1] range; values outside it are clamped.
	double add_positive_unit_real(double p_value, CompressionLevel p_compression_level);


	// Reads a value in the [0, 1] range.
	double read_positive_unit_real(CompressionLevel p_compression_level);


	// Writes a value in the [-1, 1] range; values outside it are clamped.
	double add_unit_real(double p_value, CompressionLevel p_compression_level);


	// Reads a value in the [-1, 1] range.
	double read_unit_real(CompressionLevel p_compression_level);


	// Writes the two components of a vector as reals.
	Vector2 add_vector2(const Vector2 &p_value, CompressionLevel p_compression_level);


	// Reads a vector written with `add_vector2()`.
	Vector2 read_vector2(CompressionLevel p_compression_level);


	// Writes a 2D direction (or zero); the input is normalized when it isn't zero.
	Vector2 add_normalized_vector2(const Vector2 &p_value, CompressionLevel p_compression_level);


	// Reads a 2D direction (or zero).
	Vector2 read_normalized_vector2(CompressionLevel p_compression_level);


	// Writes the three components of a vector as reals.
	Vector3 add_vector3(const Vector3 &p_value, CompressionLevel p_compression_level);


	// Reads a vector written with `add_vector3()`.
	Vector3 read_vector3(CompressionLevel p_compression_level);


	// Writes a 3D direction (or zero); the input is normalized when it isn't zero.
	Vector3 add_normalized_vector3(const Vector3 &p_value, CompressionLevel p_compression_level);


	// Reads a 3D direction (or zero).
	Vector3 read_normalized_vector3(CompressionLevel p_compression_level);


	// Writes a string as UTF-8, of at most 65535 bytes.
	void add_string(const String &p_value);


	// Reads a string; one that isn't valid UTF-8 fails the buffer.
	String read_string();


	// Writes any value with a codec, the same way a synchronized variable is written. Returns the value as the other
	// side decodes it.
	Variant add_value(const Variant &p_value, const Ref<TickCodec> &p_codec);


	// Reads a value written with `add_value()` and the same codec.
	Variant read_value(const Ref<TickCodec> &p_codec);
};

VARIANT_ENUM_CAST(DataBuffer::CompressionLevel);
