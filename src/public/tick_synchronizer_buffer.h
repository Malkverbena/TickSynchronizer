// Declares the public deterministic binary buffer API.
// Defines bitstream state, scalar codecs, limits, and logical content identity.

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

	// Clears operation state while preserving the configured resource limit.
	void _reset_state();

	// Records the first persistent error and returns it to the caller.
	Error _set_error(Error p_error);

	// Grows storage only after validating the logical size and configured limit.
	Error _ensure_write_capacity(int64_t p_required_bit_size);

	// Writes validated bits after capacity and mode checks have completed.
	void _write_bits_unchecked(uint64_t p_value, uint32_t p_bit_count);

	// Reads validated bits after bounds and mode checks have completed.
	uint64_t _read_bits_unchecked(uint32_t p_bit_count);

	// Adapts the C++ bit reader to the signed GDScript integer surface.
	int64_t _read_bits_value(int32_t p_bit_count);

	// Validates a GDScript integer before writing an unsigned 8-bit value.
	Error _write_u8_value(int64_t p_value);

	// Validates a GDScript integer before writing an unsigned 16-bit value.
	Error _write_u16_value(int64_t p_value);

	// Validates a GDScript integer before writing an unsigned 32-bit value.
	Error _write_u32_value(int64_t p_value);

	// Writes the 64-bit pattern represented by a GDScript integer.
	Error _write_u64_value(int64_t p_value);

	// Returns an unsigned 8-bit value through the GDScript integer surface.
	int64_t _read_u8_value();

	// Returns an unsigned 16-bit value through the GDScript integer surface.
	int64_t _read_u16_value();

	// Returns an unsigned 32-bit value through the GDScript integer surface.
	int64_t _read_u32_value();

	// Returns a 64-bit pattern through the signed GDScript integer surface.
	int64_t _read_u64_value();

	// Rejects negative GDScript values before canonical varuint encoding.
	Error _write_varuint_value(int64_t p_value);

	// Returns canonical varuint data when it fits the GDScript integer range.
	int64_t _read_varuint_value();

	// Returns a canonical ZigZag value through the GDScript integer surface.
	int64_t _read_varint_value();

	// Converts a GDScript float to binary32 while rejecting finite overflow.
	Error _write_float32_value(double p_value);

	// Promotes a decoded binary32 value to the GDScript float surface.
	double _read_float32_value();

	// Returns a decoded binary64 value through the GDScript float surface.
	double _read_float64_value();

protected:
	// Binds the class API and constants to Godot ClassDB.
	static void _bind_methods();

public:
	// Resets data and operation state while preserving the configured size limit.
	void clear();

	// Changes the per-buffer storage ceiling without setting a codec error on failure.
	Error set_max_size_bytes(int64_t p_max_size_bytes);

	// Returns the configured storage ceiling in physical bytes.
	int64_t get_max_size_bytes() const;

	// Returns the configured storage ceiling converted safely to bits.
	int64_t get_max_size_bits() const;

	// Returns writable capacity remaining under the current resource limit.
	int64_t get_remaining_write_bits() const;

	// Checks whether a positive bit count can be written without changing state.
	bool can_write_bits(int64_t p_bit_count) const;

	// Starts a fresh write operation and optionally reserves bounded capacity.
	Error begin_write(int64_t p_initial_capacity_bytes = 0);

	// Copies input into canonical read storage and sets its logical bit length.
	Error begin_read(const PackedByteArray &p_data, int64_t p_bit_size = -1);

	// Writes the requested low-order bits in LSB-first order atomically.
	Error write_bits(uint64_t p_value, uint32_t p_bit_count);

	// Reads up to 64 bits atomically without exposing partial values on failure.
	Error read_bits(uint32_t p_bit_count, uint64_t &r_value);

	// Reports whether the active cursor is positioned on a byte boundary.
	bool is_byte_aligned() const;

	// Writes zero padding until the write cursor reaches a byte boundary.
	Error align_write_to_byte();

	// Consumes only zero padding when advancing to a byte boundary.
	Error align_read_to_byte();

	// Writes an explicit unsigned 8-bit field at the current bit position.
	Error write_u8(uint8_t p_value);

	// Writes an explicit unsigned 16-bit field at the current bit position.
	Error write_u16(uint16_t p_value);

	// Writes an explicit unsigned 32-bit field at the current bit position.
	Error write_u32(uint32_t p_value);

	// Writes an explicit unsigned 64-bit field at the current bit position.
	Error write_u64(uint64_t p_value);

	// Reads an explicit unsigned 8-bit field atomically.
	Error read_u8(uint8_t &r_value);

	// Reads an explicit unsigned 16-bit field atomically.
	Error read_u16(uint16_t &r_value);

	// Reads an explicit unsigned 32-bit field atomically.
	Error read_u32(uint32_t &r_value);

	// Reads an explicit unsigned 64-bit field atomically.
	Error read_u64(uint64_t &r_value);

	// Writes a byte-aligned minimal ULEB128 representation.
	Error write_varuint(uint64_t p_value);

	// Reads only byte-aligned canonical ULEB128 representations.
	Error read_varuint(uint64_t &r_value);

	// Writes a signed value using ZigZag followed by canonical ULEB128.
	Error write_varint(int64_t p_value);

	// Reads canonical ZigZag plus ULEB128 data atomically.
	Error read_varint(int64_t &r_value);

	// Writes the exact little-endian IEEE 754 binary32 pattern.
	Error write_float32(float p_value);

	// Reads the exact IEEE 754 binary32 pattern at the current position.
	Error read_float32(float &r_value);

	// Writes the exact little-endian IEEE 754 binary64 pattern.
	Error write_float64(double p_value);

	// Reads the exact IEEE 754 binary64 pattern at the current position.
	Error read_float64(double &r_value);

	// Returns canonical storage; callers must retain the separate logical bit size.
	PackedByteArray get_data() const;

	// Returns the number of canonical physical bytes currently stored.
	int64_t get_byte_size() const;

	// Returns the logical number of valid payload bits.
	int64_t get_bit_size() const;

	// Returns the active read or write cursor in bits.
	int64_t get_bit_position() const;

	// Returns unread logical bits while the buffer is in read mode.
	int64_t get_remaining_bits() const;

	// Returns whether the buffer is idle, reading, or writing.
	Mode get_mode() const;

	// Reports whether a read operation is active.
	bool is_reading() const;

	// Reports whether a write operation is active.
	bool is_writing() const;

	// Reports whether a sticky operation error has been recorded.
	bool has_error() const;

	// Returns the first sticky operation error or OK.
	Error get_last_error() const;

	// Compares canonical bytes and logical bit size, ignoring runtime cursor state.
	bool is_equal_to(const Ref<TickSynchronizerBuffer> &p_other) const;

	// Returns a non-cryptographic hash of canonical logical content.
	uint32_t get_content_hash() const;
};

VARIANT_ENUM_CAST(TickSynchronizerBuffer::Mode);
