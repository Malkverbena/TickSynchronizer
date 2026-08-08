// Implements canonical bit storage and explicit numeric codecs.
// Provides atomic errors, limits, equality, and hashing for protocol primitives.

#include "tick_synchronizer_buffer.h"

#include "core/io/marshalls.h"
#include "core/object/class_db.h"
#include "core/templates/hashfuncs.h"

#include <cmath>
#include <limits>

static_assert(sizeof(float) == sizeof(uint32_t), "TickSynchronizer requires 32-bit float.");
static_assert(sizeof(double) == sizeof(uint64_t), "TickSynchronizer requires 64-bit double.");
static_assert(std::numeric_limits<float>::is_iec559, "TickSynchronizer requires IEEE 754 float.");
static_assert(std::numeric_limits<double>::is_iec559, "TickSynchronizer requires IEEE 754 double.");

namespace {

static inline int64_t bytes_for_bits(int64_t p_bit_count) {
	return (p_bit_count / 8) + ((p_bit_count % 8) != 0 ? 1 : 0);
}


static inline uint64_t zigzag_encode(int64_t p_value) {
	if (p_value >= 0) {
		return static_cast<uint64_t>(p_value) << 1;
	}
	return (static_cast<uint64_t>(-(p_value + 1)) << 1) | UINT64_C(1);
}


static inline int64_t zigzag_decode(uint64_t p_value) {
	const int64_t magnitude = static_cast<int64_t>(p_value >> 1);
	return (p_value & UINT64_C(1)) != 0 ? -magnitude - 1 : magnitude;
}

} // namespace

void TickSynchronizerBuffer::_reset_state() {
	data.clear();
	bit_size = 0;
	bit_position = 0;
	mode = MODE_NONE;
	last_error = OK;
}


Error TickSynchronizerBuffer::_set_error(Error p_error) {
	if (last_error == OK) {
		last_error = p_error;
	}
	return last_error;
}


Error TickSynchronizerBuffer::_ensure_write_capacity(int64_t p_required_bit_size) {
	if (p_required_bit_size < 0) {
		return _set_error(ERR_OUT_OF_MEMORY);
	}
	if (p_required_bit_size > get_max_size_bits()) {
		return _set_error(ERR_PARAMETER_RANGE_ERROR);
	}

	const int64_t required_byte_size = bytes_for_bits(p_required_bit_size);
	if (required_byte_size <= data.size()) {
		return OK;
	}

	if (required_byte_size > static_cast<int64_t>(data.capacity())) {
		const Error reserve_error = data.reserve_exact(required_byte_size);
		if (reserve_error != OK) {
			return _set_error(reserve_error);
		}
	}

	const Error resize_error = data.resize_initialized(required_byte_size);
	if (resize_error != OK) {
		return _set_error(resize_error);
	}

	return OK;
}


void TickSynchronizerBuffer::_write_bits_unchecked(uint64_t p_value, uint32_t p_bit_count) {
	uint8_t *write_ptr = data.ptrw();
	uint32_t remaining_bits = p_bit_count;

	while (remaining_bits > 0) {
		const int64_t byte_index = bit_position / 8;
		const uint32_t bit_offset = static_cast<uint32_t>(bit_position % 8);
		const uint32_t available_bits = 8 - bit_offset;
		const uint32_t chunk_bits = remaining_bits < available_bits ? remaining_bits : available_bits;
		const uint32_t chunk_mask = (UINT32_C(1) << chunk_bits) - 1;
		const uint8_t shifted_mask = static_cast<uint8_t>(chunk_mask << bit_offset);
		const uint8_t shifted_value = static_cast<uint8_t>((p_value & chunk_mask) << bit_offset);

		write_ptr[byte_index] = static_cast<uint8_t>((write_ptr[byte_index] & ~shifted_mask) | shifted_value);
		p_value >>= chunk_bits;
		bit_position += chunk_bits;
		remaining_bits -= chunk_bits;
	}

	if (bit_position > bit_size) {
		bit_size = bit_position;
	}
}


uint64_t TickSynchronizerBuffer::_read_bits_unchecked(uint32_t p_bit_count) {
	const uint8_t *read_ptr = data.ptr();
	uint64_t value = 0;
	uint32_t remaining_bits = p_bit_count;
	uint32_t output_shift = 0;

	while (remaining_bits > 0) {
		const int64_t byte_index = bit_position / 8;
		const uint32_t bit_offset = static_cast<uint32_t>(bit_position % 8);
		const uint32_t available_bits = 8 - bit_offset;
		const uint32_t chunk_bits = remaining_bits < available_bits ? remaining_bits : available_bits;
		const uint32_t chunk_mask = (UINT32_C(1) << chunk_bits) - 1;
		const uint64_t chunk_value = static_cast<uint64_t>((read_ptr[byte_index] >> bit_offset) & chunk_mask);

		value |= chunk_value << output_shift;
		bit_position += chunk_bits;
		output_shift += chunk_bits;
		remaining_bits -= chunk_bits;
	}

	return value;
}


int64_t TickSynchronizerBuffer::_read_bits_value(int32_t p_bit_count) {
	uint64_t value = 0;
	if (p_bit_count < 0 || read_bits(static_cast<uint32_t>(p_bit_count), value) != OK) {
		return 0;
	}
	return static_cast<int64_t>(value);
}


Error TickSynchronizerBuffer::_write_u8_value(int64_t p_value) {
	if (p_value < 0 || p_value > UINT8_MAX) {
		return _set_error(ERR_INVALID_PARAMETER);
	}
	return write_u8(static_cast<uint8_t>(p_value));
}


Error TickSynchronizerBuffer::_write_u16_value(int64_t p_value) {
	if (p_value < 0 || p_value > UINT16_MAX) {
		return _set_error(ERR_INVALID_PARAMETER);
	}
	return write_u16(static_cast<uint16_t>(p_value));
}


Error TickSynchronizerBuffer::_write_u32_value(int64_t p_value) {
	if (p_value < 0 || static_cast<uint64_t>(p_value) > UINT32_MAX) {
		return _set_error(ERR_INVALID_PARAMETER);
	}
	return write_u32(static_cast<uint32_t>(p_value));
}


Error TickSynchronizerBuffer::_write_u64_value(int64_t p_value) {
	return write_u64(static_cast<uint64_t>(p_value));
}


int64_t TickSynchronizerBuffer::_read_u8_value() {
	uint8_t value = 0;
	read_u8(value);
	return value;
}


int64_t TickSynchronizerBuffer::_read_u16_value() {
	uint16_t value = 0;
	read_u16(value);
	return value;
}


int64_t TickSynchronizerBuffer::_read_u32_value() {
	uint32_t value = 0;
	read_u32(value);
	return value;
}


int64_t TickSynchronizerBuffer::_read_u64_value() {
	uint64_t value = 0;
	read_u64(value);
	return static_cast<int64_t>(value);
}


Error TickSynchronizerBuffer::_write_varuint_value(int64_t p_value) {
	if (p_value < 0) {
		return _set_error(ERR_INVALID_PARAMETER);
	}
	return write_varuint(static_cast<uint64_t>(p_value));
}


int64_t TickSynchronizerBuffer::_read_varuint_value() {
	const int64_t original_position = bit_position;
	uint64_t value = 0;
	if (read_varuint(value) != OK) {
		return 0;
	}
	if (value > static_cast<uint64_t>(INT64_MAX)) {
		bit_position = original_position;
		_set_error(ERR_PARAMETER_RANGE_ERROR);
		return 0;
	}
	return static_cast<int64_t>(value);
}


int64_t TickSynchronizerBuffer::_read_varint_value() {
	int64_t value = 0;
	read_varint(value);
	return value;
}


Error TickSynchronizerBuffer::_write_float32_value(double p_value) {
	if (std::isfinite(p_value) &&
			(p_value > std::numeric_limits<float>::max() ||
					p_value < -std::numeric_limits<float>::max())) {
		return _set_error(ERR_INVALID_PARAMETER);
	}
	return write_float32(static_cast<float>(p_value));
}


double TickSynchronizerBuffer::_read_float32_value() {
	float value = 0.0f;
	read_float32(value);
	return static_cast<double>(value);
}


double TickSynchronizerBuffer::_read_float64_value() {
	double value = 0.0;
	read_float64(value);
	return value;
}


void TickSynchronizerBuffer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("clear"), &TickSynchronizerBuffer::clear);
	ClassDB::bind_method(
			D_METHOD("set_max_size_bytes", "max_size_bytes"),
			&TickSynchronizerBuffer::set_max_size_bytes);
	ClassDB::bind_method(D_METHOD("get_max_size_bytes"), &TickSynchronizerBuffer::get_max_size_bytes);
	ClassDB::bind_method(D_METHOD("get_max_size_bits"), &TickSynchronizerBuffer::get_max_size_bits);
	ClassDB::bind_method(D_METHOD("get_remaining_write_bits"), &TickSynchronizerBuffer::get_remaining_write_bits);
	ClassDB::bind_method(D_METHOD("can_write_bits", "bit_count"), &TickSynchronizerBuffer::can_write_bits);
	ClassDB::bind_method(
			D_METHOD("begin_write", "initial_capacity_bytes"),
			&TickSynchronizerBuffer::begin_write,
			DEFVAL(0));
	ClassDB::bind_method(D_METHOD("begin_read", "data", "bit_size"), &TickSynchronizerBuffer::begin_read, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("write_bits", "value", "bit_count"), &TickSynchronizerBuffer::write_bits);
	ClassDB::bind_method(D_METHOD("read_bits", "bit_count"), &TickSynchronizerBuffer::_read_bits_value);
	ClassDB::bind_method(D_METHOD("is_byte_aligned"), &TickSynchronizerBuffer::is_byte_aligned);
	ClassDB::bind_method(D_METHOD("align_write_to_byte"), &TickSynchronizerBuffer::align_write_to_byte);
	ClassDB::bind_method(D_METHOD("align_read_to_byte"), &TickSynchronizerBuffer::align_read_to_byte);
	ClassDB::bind_method(D_METHOD("write_u8", "value"), &TickSynchronizerBuffer::_write_u8_value);
	ClassDB::bind_method(D_METHOD("write_u16", "value"), &TickSynchronizerBuffer::_write_u16_value);
	ClassDB::bind_method(D_METHOD("write_u32", "value"), &TickSynchronizerBuffer::_write_u32_value);
	ClassDB::bind_method(D_METHOD("write_u64", "value"), &TickSynchronizerBuffer::_write_u64_value);
	ClassDB::bind_method(D_METHOD("read_u8"), &TickSynchronizerBuffer::_read_u8_value);
	ClassDB::bind_method(D_METHOD("read_u16"), &TickSynchronizerBuffer::_read_u16_value);
	ClassDB::bind_method(D_METHOD("read_u32"), &TickSynchronizerBuffer::_read_u32_value);
	ClassDB::bind_method(D_METHOD("read_u64"), &TickSynchronizerBuffer::_read_u64_value);
	ClassDB::bind_method(D_METHOD("write_varuint", "value"), &TickSynchronizerBuffer::_write_varuint_value);
	ClassDB::bind_method(D_METHOD("read_varuint"), &TickSynchronizerBuffer::_read_varuint_value);
	ClassDB::bind_method(D_METHOD("write_varint", "value"), &TickSynchronizerBuffer::write_varint);
	ClassDB::bind_method(D_METHOD("read_varint"), &TickSynchronizerBuffer::_read_varint_value);
	ClassDB::bind_method(D_METHOD("write_float32", "value"), &TickSynchronizerBuffer::_write_float32_value);
	ClassDB::bind_method(D_METHOD("read_float32"), &TickSynchronizerBuffer::_read_float32_value);
	ClassDB::bind_method(D_METHOD("write_float64", "value"), &TickSynchronizerBuffer::write_float64);
	ClassDB::bind_method(D_METHOD("read_float64"), &TickSynchronizerBuffer::_read_float64_value);
	ClassDB::bind_method(D_METHOD("get_data"), &TickSynchronizerBuffer::get_data);
	ClassDB::bind_method(D_METHOD("get_byte_size"), &TickSynchronizerBuffer::get_byte_size);
	ClassDB::bind_method(D_METHOD("get_bit_size"), &TickSynchronizerBuffer::get_bit_size);
	ClassDB::bind_method(D_METHOD("get_bit_position"), &TickSynchronizerBuffer::get_bit_position);
	ClassDB::bind_method(D_METHOD("get_remaining_bits"), &TickSynchronizerBuffer::get_remaining_bits);
	ClassDB::bind_method(D_METHOD("get_mode"), &TickSynchronizerBuffer::get_mode);
	ClassDB::bind_method(D_METHOD("is_reading"), &TickSynchronizerBuffer::is_reading);
	ClassDB::bind_method(D_METHOD("is_writing"), &TickSynchronizerBuffer::is_writing);
	ClassDB::bind_method(D_METHOD("has_error"), &TickSynchronizerBuffer::has_error);
	ClassDB::bind_method(D_METHOD("get_last_error"), &TickSynchronizerBuffer::get_last_error);
	ClassDB::bind_method(D_METHOD("is_equal_to", "other"), &TickSynchronizerBuffer::is_equal_to);
	ClassDB::bind_method(D_METHOD("get_content_hash"), &TickSynchronizerBuffer::get_content_hash);

	BIND_CONSTANT(DEFAULT_MAX_SIZE_BYTES);
	BIND_CONSTANT(MAX_CONFIGURABLE_SIZE_BYTES);
	BIND_ENUM_CONSTANT(MODE_NONE);
	BIND_ENUM_CONSTANT(MODE_READ);
	BIND_ENUM_CONSTANT(MODE_WRITE);
}


void TickSynchronizerBuffer::clear() {
	_reset_state();
}


Error TickSynchronizerBuffer::set_max_size_bytes(int64_t p_max_size_bytes) {
	if (p_max_size_bytes <= 0 || p_max_size_bytes > MAX_CONFIGURABLE_SIZE_BYTES) {
		return ERR_INVALID_PARAMETER;
	}
	if (p_max_size_bytes < static_cast<int64_t>(data.capacity())) {
		return ERR_PARAMETER_RANGE_ERROR;
	}
	max_size_bytes = p_max_size_bytes;
	return OK;
}


int64_t TickSynchronizerBuffer::get_max_size_bytes() const {
	return max_size_bytes;
}


int64_t TickSynchronizerBuffer::get_max_size_bits() const {
	return max_size_bytes * 8;
}


int64_t TickSynchronizerBuffer::get_remaining_write_bits() const {
	if (mode != MODE_WRITE || bit_position >= get_max_size_bits()) {
		return 0;
	}
	return get_max_size_bits() - bit_position;
}


bool TickSynchronizerBuffer::can_write_bits(int64_t p_bit_count) const {
	if (last_error != OK || mode != MODE_WRITE || p_bit_count <= 0) {
		return false;
	}
	return p_bit_count <= get_max_size_bits() - bit_position;
}


Error TickSynchronizerBuffer::begin_write(int64_t p_initial_capacity_bytes) {
	_reset_state();

	if (p_initial_capacity_bytes < 0) {
		return _set_error(ERR_INVALID_PARAMETER);
	}
	if (p_initial_capacity_bytes > max_size_bytes) {
		return _set_error(ERR_PARAMETER_RANGE_ERROR);
	}
	if (p_initial_capacity_bytes > 0) {
		const Error reserve_error = data.reserve_exact(p_initial_capacity_bytes);
		if (reserve_error != OK) {
			return _set_error(reserve_error);
		}
	}

	mode = MODE_WRITE;
	return OK;
}


Error TickSynchronizerBuffer::begin_read(const PackedByteArray &p_data, int64_t p_bit_size) {
	_reset_state();

	if (p_bit_size < -1) {
		return _set_error(ERR_INVALID_PARAMETER);
	}
	if (p_data.size() > max_size_bytes) {
		return _set_error(ERR_PARAMETER_RANGE_ERROR);
	}
	if (p_data.size() > std::numeric_limits<int64_t>::max() / 8) {
		return _set_error(ERR_OUT_OF_MEMORY);
	}

	const int64_t available_bit_size = p_data.size() * 8;
	const int64_t requested_bit_size = p_bit_size == -1 ? available_bit_size : p_bit_size;
	if (requested_bit_size < 0 || requested_bit_size > available_bit_size) {
		return _set_error(ERR_INVALID_PARAMETER);
	}

	const int64_t logical_byte_size = bytes_for_bits(requested_bit_size);
	if (logical_byte_size > 0) {
		Error err = data.reserve_exact(logical_byte_size);
		if (err != OK) {
			return _set_error(err);
		}
		err = data.resize_initialized(logical_byte_size);
		if (err != OK) {
			return _set_error(err);
		}
		for (int64_t index = 0; index < logical_byte_size; index++) {
			data.ptrw()[index] = p_data[index];
		}
		const uint32_t final_bits = static_cast<uint32_t>(requested_bit_size % 8);
		if (final_bits != 0) {
			const uint8_t mask = static_cast<uint8_t>((UINT32_C(1) << final_bits) - 1);
			data.ptrw()[logical_byte_size - 1] &= mask;
		}
	}

	bit_size = requested_bit_size;
	mode = MODE_READ;
	return OK;
}


Error TickSynchronizerBuffer::write_bits(uint64_t p_value, uint32_t p_bit_count) {
	if (last_error != OK) {
		return last_error;
	}
	if (mode != MODE_WRITE) {
		return _set_error(ERR_UNCONFIGURED);
	}
	if (p_bit_count == 0 || p_bit_count > 64) {
		return _set_error(ERR_INVALID_PARAMETER);
	}
	if (!can_write_bits(p_bit_count)) {
		return _set_error(ERR_PARAMETER_RANGE_ERROR);
	}

	const int64_t required_bit_size = bit_position + static_cast<int64_t>(p_bit_count);
	const Error capacity_error = _ensure_write_capacity(required_bit_size);
	if (capacity_error != OK) {
		return capacity_error;
	}

	_write_bits_unchecked(p_value, p_bit_count);
	return OK;
}


Error TickSynchronizerBuffer::read_bits(uint32_t p_bit_count, uint64_t &r_value) {
	r_value = 0;

	if (last_error != OK) {
		return last_error;
	}
	if (mode != MODE_READ) {
		return _set_error(ERR_UNCONFIGURED);
	}
	if (p_bit_count == 0 || p_bit_count > 64) {
		return _set_error(ERR_INVALID_PARAMETER);
	}
	if (bit_position > bit_size || static_cast<int64_t>(p_bit_count) > bit_size - bit_position) {
		return _set_error(ERR_FILE_EOF);
	}

	r_value = _read_bits_unchecked(p_bit_count);
	return OK;
}


bool TickSynchronizerBuffer::is_byte_aligned() const {
	return (bit_position % 8) == 0;
}


Error TickSynchronizerBuffer::align_write_to_byte() {
	if (last_error != OK) {
		return last_error;
	}
	if (mode != MODE_WRITE) {
		return _set_error(ERR_UNCONFIGURED);
	}
	const uint32_t padding_bits = static_cast<uint32_t>((8 - (bit_position % 8)) % 8);
	return padding_bits == 0 ? OK : write_bits(0, padding_bits);
}


Error TickSynchronizerBuffer::align_read_to_byte() {
	if (last_error != OK) {
		return last_error;
	}
	if (mode != MODE_READ) {
		return _set_error(ERR_UNCONFIGURED);
	}

	const uint32_t padding_bits = static_cast<uint32_t>((8 - (bit_position % 8)) % 8);
	if (padding_bits == 0) {
		return OK;
	}
	if (static_cast<int64_t>(padding_bits) > bit_size - bit_position) {
		return _set_error(ERR_FILE_EOF);
	}

	const int64_t original_position = bit_position;
	const uint64_t padding = _read_bits_unchecked(padding_bits);
	if (padding != 0) {
		bit_position = original_position;
		return _set_error(ERR_INVALID_DATA);
	}
	return OK;
}


Error TickSynchronizerBuffer::write_u8(uint8_t p_value) {
	return write_bits(p_value, 8);
}


Error TickSynchronizerBuffer::write_u16(uint16_t p_value) {
	return write_bits(p_value, 16);
}


Error TickSynchronizerBuffer::write_u32(uint32_t p_value) {
	return write_bits(p_value, 32);
}


Error TickSynchronizerBuffer::write_u64(uint64_t p_value) {
	return write_bits(p_value, 64);
}


Error TickSynchronizerBuffer::read_u8(uint8_t &r_value) {
	r_value = 0;
	uint64_t value = 0;
	const Error error = read_bits(8, value);
	if (error == OK) {
		r_value = static_cast<uint8_t>(value);
	}
	return error;
}


Error TickSynchronizerBuffer::read_u16(uint16_t &r_value) {
	r_value = 0;
	uint64_t value = 0;
	const Error error = read_bits(16, value);
	if (error == OK) {
		r_value = static_cast<uint16_t>(value);
	}
	return error;
}


Error TickSynchronizerBuffer::read_u32(uint32_t &r_value) {
	r_value = 0;
	uint64_t value = 0;
	const Error error = read_bits(32, value);
	if (error == OK) {
		r_value = static_cast<uint32_t>(value);
	}
	return error;
}


Error TickSynchronizerBuffer::read_u64(uint64_t &r_value) {
	r_value = 0;
	return read_bits(64, r_value);
}


Error TickSynchronizerBuffer::write_varuint(uint64_t p_value) {
	if (last_error != OK) {
		return last_error;
	}
	if (mode != MODE_WRITE) {
		return _set_error(ERR_UNCONFIGURED);
	}
	if (!is_byte_aligned()) {
		return _set_error(ERR_INVALID_DATA);
	}

	uint8_t encoded[10];
	uint32_t encoded_size = 0;
	do {
		uint8_t byte = static_cast<uint8_t>(p_value & UINT64_C(0x7F));
		p_value >>= 7;
		if (p_value != 0) {
			byte |= 0x80;
		}
		encoded[encoded_size++] = byte;
	} while (p_value != 0);

	const int64_t required_bits = static_cast<int64_t>(encoded_size) * 8;
	if (!can_write_bits(required_bits)) {
		return _set_error(ERR_PARAMETER_RANGE_ERROR);
	}
	const Error capacity_error = _ensure_write_capacity(bit_position + required_bits);
	if (capacity_error != OK) {
		return capacity_error;
	}
	for (uint32_t index = 0; index < encoded_size; index++) {
		_write_bits_unchecked(encoded[index], 8);
	}
	return OK;
}


Error TickSynchronizerBuffer::read_varuint(uint64_t &r_value) {
	r_value = 0;
	if (last_error != OK) {
		return last_error;
	}
	if (mode != MODE_READ) {
		return _set_error(ERR_UNCONFIGURED);
	}
	if (!is_byte_aligned()) {
		return _set_error(ERR_INVALID_DATA);
	}

	const int64_t original_position = bit_position;
	uint64_t decoded = 0;
	for (uint32_t index = 0; index < 10; index++) {
		if (bit_size - bit_position < 8) {
			bit_position = original_position;
			return _set_error(ERR_FILE_EOF);
		}

		const uint8_t byte = static_cast<uint8_t>(_read_bits_unchecked(8));
		const uint8_t payload = byte & 0x7F;
		if (index == 9 && (payload > 1 || (byte & 0x80) != 0)) {
			bit_position = original_position;
			return _set_error(ERR_INVALID_DATA);
		}

		decoded |= static_cast<uint64_t>(payload) << (index * 7);
		if ((byte & 0x80) == 0) {
			if (index > 0 && payload == 0) {
				bit_position = original_position;
				return _set_error(ERR_INVALID_DATA);
			}
			r_value = decoded;
			return OK;
		}
	}

	bit_position = original_position;
	return _set_error(ERR_INVALID_DATA);
}


Error TickSynchronizerBuffer::write_varint(int64_t p_value) {
	return write_varuint(zigzag_encode(p_value));
}


Error TickSynchronizerBuffer::read_varint(int64_t &r_value) {
	r_value = 0;
	uint64_t encoded = 0;
	const Error error = read_varuint(encoded);
	if (error == OK) {
		r_value = zigzag_decode(encoded);
	}
	return error;
}


Error TickSynchronizerBuffer::write_float32(float p_value) {
	MarshallFloat value;
	value.f = p_value;
	return write_u32(value.i);
}


Error TickSynchronizerBuffer::read_float32(float &r_value) {
	r_value = 0.0f;
	uint32_t bits = 0;
	const Error error = read_u32(bits);
	if (error == OK) {
		MarshallFloat value;
		value.i = bits;
		r_value = value.f;
	}
	return error;
}


Error TickSynchronizerBuffer::write_float64(double p_value) {
	MarshallDouble value;
	value.d = p_value;
	return write_u64(value.l);
}


Error TickSynchronizerBuffer::read_float64(double &r_value) {
	r_value = 0.0;
	uint64_t bits = 0;
	const Error error = read_u64(bits);
	if (error == OK) {
		MarshallDouble value;
		value.l = bits;
		r_value = value.d;
	}
	return error;
}


PackedByteArray TickSynchronizerBuffer::get_data() const {
	return data;
}


int64_t TickSynchronizerBuffer::get_byte_size() const {
	return data.size();
}


int64_t TickSynchronizerBuffer::get_bit_size() const {
	return bit_size;
}


int64_t TickSynchronizerBuffer::get_bit_position() const {
	return bit_position;
}


int64_t TickSynchronizerBuffer::get_remaining_bits() const {
	if (mode != MODE_READ || bit_position >= bit_size) {
		return 0;
	}
	return bit_size - bit_position;
}


TickSynchronizerBuffer::Mode TickSynchronizerBuffer::get_mode() const {
	return mode;
}


bool TickSynchronizerBuffer::is_reading() const {
	return mode == MODE_READ;
}


bool TickSynchronizerBuffer::is_writing() const {
	return mode == MODE_WRITE;
}


bool TickSynchronizerBuffer::has_error() const {
	return last_error != OK;
}


Error TickSynchronizerBuffer::get_last_error() const {
	return last_error;
}


bool TickSynchronizerBuffer::is_equal_to(const Ref<TickSynchronizerBuffer> &p_other) const {
	if (p_other.is_null()) {
		return false;
	}
	return bit_size == p_other->bit_size && data == p_other->data;
}


uint32_t TickSynchronizerBuffer::get_content_hash() const {
	uint32_t hash = HASH_MURMUR3_SEED;
	if (!data.is_empty()) {
		hash = hash_murmur3_buffer(data.ptr(), data.size(), hash);
	}
	hash = hash_murmur3_one_64(static_cast<uint64_t>(bit_size), hash);
	return hash_fmix32(hash);
}
