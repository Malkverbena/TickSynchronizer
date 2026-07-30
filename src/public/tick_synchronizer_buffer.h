#pragma once

#include "core/error/error_list.h"
#include "core/object/ref_counted.h"
#include "core/variant/type_info.h"
#include "core/variant/variant.h"

#include <cstdint>
#include <limits>

class TickSynchronizerBuffer : public RefCounted {
	GDCLASS(TickSynchronizerBuffer, RefCounted);

public:
	enum Mode {
		MODE_NONE,
		MODE_READ,
		MODE_WRITE,
	};

	static constexpr int64_t DEFAULT_MAX_SIZE_BYTES = 1024 * 1024;
	static constexpr int64_t MAX_CONFIGURABLE_SIZE_BYTES = std::numeric_limits<int64_t>::max() / 8;

private:
	PackedByteArray data;
	int64_t bit_size = 0;
	int64_t bit_position = 0;
	int64_t max_size_bytes = DEFAULT_MAX_SIZE_BYTES;
	Mode mode = MODE_NONE;
	Error last_error = OK;

	void _reset_state();
	Error _set_error(Error p_error);
	Error _ensure_write_capacity(int64_t p_required_bit_size);
	void _write_bits_unchecked(uint64_t p_value, uint32_t p_bit_count);
	uint64_t _read_bits_unchecked(uint32_t p_bit_count);

	int64_t _read_bits_value(int32_t p_bit_count);
	Error _write_u8_value(int64_t p_value);
	Error _write_u16_value(int64_t p_value);
	Error _write_u32_value(int64_t p_value);
	Error _write_u64_value(int64_t p_value);
	int64_t _read_u8_value();
	int64_t _read_u16_value();
	int64_t _read_u32_value();
	int64_t _read_u64_value();
	Error _write_varuint_value(int64_t p_value);
	int64_t _read_varuint_value();
	int64_t _read_varint_value();
	Error _write_float32_value(double p_value);
	double _read_float32_value();
	double _read_float64_value();

protected:
	static void _bind_methods();

public:
	void clear();

	Error set_max_size_bytes(int64_t p_max_size_bytes);
	int64_t get_max_size_bytes() const;
	int64_t get_max_size_bits() const;
	int64_t get_remaining_write_bits() const;
	bool can_write_bits(int64_t p_bit_count) const;

	Error begin_write(int64_t p_initial_capacity_bytes = 0);
	Error begin_read(const PackedByteArray &p_data, int64_t p_bit_size = -1);

	Error write_bits(uint64_t p_value, uint32_t p_bit_count);
	Error read_bits(uint32_t p_bit_count, uint64_t &r_value);

	bool is_byte_aligned() const;
	Error align_write_to_byte();
	Error align_read_to_byte();

	Error write_u8(uint8_t p_value);
	Error write_u16(uint16_t p_value);
	Error write_u32(uint32_t p_value);
	Error write_u64(uint64_t p_value);
	Error read_u8(uint8_t &r_value);
	Error read_u16(uint16_t &r_value);
	Error read_u32(uint32_t &r_value);
	Error read_u64(uint64_t &r_value);

	Error write_varuint(uint64_t p_value);
	Error read_varuint(uint64_t &r_value);
	Error write_varint(int64_t p_value);
	Error read_varint(int64_t &r_value);

	Error write_float32(float p_value);
	Error read_float32(float &r_value);
	Error write_float64(double p_value);
	Error read_float64(double &r_value);

	PackedByteArray get_data() const;
	int64_t get_byte_size() const;
	int64_t get_bit_size() const;
	int64_t get_bit_position() const;
	int64_t get_remaining_bits() const;
	Mode get_mode() const;
	bool is_reading() const;
	bool is_writing() const;
	bool has_error() const;
	Error get_last_error() const;

	bool is_equal_to(const Ref<TickSynchronizerBuffer> &p_other) const;
	uint32_t get_content_hash() const;
};

VARIANT_ENUM_CAST(TickSynchronizerBuffer::Mode);
