#pragma once

#include "../codec/tick_codec.h"
#include "../common/tick_data_buffer.h"

#include "core/object/class_db.h"
#include "core/object/ref_counted.h"

// Scripting access to a `TickDataBuffer`: the input of a `TickObject` is written and read with it.
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
	static void _bind_methods();

public:
	// Makes this object operate on `p_buffer` (owned by the caller) instead of its own buffer.
	void wrap(TickDataBuffer *p_buffer);
	TickDataBuffer &get_buffer() { return *target; }

	void begin_write();
	void begin_read();
	int get_size() const;
	int get_bit_offset() const;
	bool is_reading() const;
	bool is_failed() const;
	bool is_end_of_buffer() const;

	bool add_bool(bool p_value);
	bool read_bool();
	int64_t add_int(int64_t p_value, int p_bits);
	int64_t read_int(int p_bits);
	int64_t add_uint(int64_t p_value, int p_bits);
	int64_t read_uint(int p_bits);
	double add_real(double p_value, CompressionLevel p_compression_level);
	double read_real(CompressionLevel p_compression_level);
	double add_positive_unit_real(double p_value, CompressionLevel p_compression_level);
	double read_positive_unit_real(CompressionLevel p_compression_level);
	double add_unit_real(double p_value, CompressionLevel p_compression_level);
	double read_unit_real(CompressionLevel p_compression_level);
	Vector2 add_vector2(const Vector2 &p_value, CompressionLevel p_compression_level);
	Vector2 read_vector2(CompressionLevel p_compression_level);
	Vector2 add_normalized_vector2(const Vector2 &p_value, CompressionLevel p_compression_level);
	Vector2 read_normalized_vector2(CompressionLevel p_compression_level);
	Vector3 add_vector3(const Vector3 &p_value, CompressionLevel p_compression_level);
	Vector3 read_vector3(CompressionLevel p_compression_level);
	Vector3 add_normalized_vector3(const Vector3 &p_value, CompressionLevel p_compression_level);
	Vector3 read_normalized_vector3(CompressionLevel p_compression_level);
	void add_string(const String &p_value);
	String read_string();
	Variant add_value(const Variant &p_value, const Ref<TickCodec> &p_codec);
	Variant read_value(const Ref<TickCodec> &p_codec);
};

VARIANT_ENUM_CAST(DataBuffer::CompressionLevel);
