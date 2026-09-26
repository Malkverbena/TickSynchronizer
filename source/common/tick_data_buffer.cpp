#include "tick_data_buffer.h"

#include "core/error/error_macros.h"
#include "core/math/math_funcs.h"
#include "core/variant/variant.h"

#include <cstring>

TickDataBuffer::TickDataBuffer(const TickBitArray &p_buffer) :
		bit_size(p_buffer.size_in_bits()),
		is_reading(true),
		buffer(p_buffer) {
}

bool TickDataBuffer::operator==(const TickDataBuffer &p_other) const {
	if (bit_size != p_other.bit_size) {
		return false;
	}
	// Compare the payload in chunks of 64 bits; the metadata may have different sizes.
	for (int offset = 0; offset < bit_size; offset += 64) {
		const int bits = MIN(64, bit_size - offset);
		uint64_t a = 0;
		uint64_t b = 0;
		buffer.read_bits(metadata_size + offset, bits, a);
		p_other.buffer.read_bits(p_other.metadata_size + offset, bits, b);
		if (a != b) {
			return false;
		}
	}
	return true;
}

bool TickDataBuffer::operator!=(const TickDataBuffer &p_other) const {
	return !operator==(p_other);
}

void TickDataBuffer::copy(const TickDataBuffer &p_other) {
	metadata_size = p_other.metadata_size;
	bit_offset = p_other.bit_offset;
	bit_size = p_other.bit_size;
	is_reading = p_other.is_reading;
	buffer_failed = p_other.buffer_failed;
	buffer = p_other.buffer;
}

void TickDataBuffer::copy(const TickBitArray &p_buffer) {
	metadata_size = 0;
	bit_offset = 0;
	bit_size = p_buffer.size_in_bits();
	is_reading = true;
	buffer_failed = false;
	buffer = p_buffer;
}

bool TickDataBuffer::slice(TickDataBuffer &r_destination, int p_offset_in_bits, int p_count_in_bits) const {
	ERR_FAIL_COND_V_MSG(p_count_in_bits <= 0, false, "The number of bits to slice must be positive.");
	ERR_FAIL_COND_V_MSG(p_offset_in_bits < 0 || p_offset_in_bits > total_size() - p_count_in_bits, false, vformat("Can't slice %d bits starting from bit %d of a buffer of %d bits.", p_count_in_bits, p_offset_in_bits, total_size()));
	ERR_FAIL_COND_V(!r_destination.check_writing(), false);

	for (int offset = 0; offset < p_count_in_bits; offset += 64) {
		const int bits = MIN(64, p_count_in_bits - offset);
		uint64_t value = 0;
		buffer.read_bits(p_offset_in_bits + offset, bits, value);
		r_destination.write_bits(value, bits);
	}
	return true;
}

void TickDataBuffer::begin_write(int p_metadata_size) {
	ERR_FAIL_COND_MSG(p_metadata_size < 0, "The metadata size can't be negative.");
	metadata_size = p_metadata_size;
	bit_size = 0;
	bit_offset = 0;
	is_reading = false;
	buffer_failed = false;
	make_room_in_bits(0);
}

void TickDataBuffer::begin_read() {
	bit_offset = 0;
	is_reading = true;
	buffer_failed = false;
}

void TickDataBuffer::dry() {
	buffer.resize_in_bits(total_size());
}

void TickDataBuffer::seek(int p_bits) {
	if (p_bits < 0 || p_bits > total_size()) {
		buffer_failed = true;
		return;
	}
	bit_offset = p_bits;
}

void TickDataBuffer::shrink_to(int p_metadata_bit_size, int p_bit_size) {
	ERR_FAIL_COND_MSG(p_metadata_bit_size < 0, "The metadata size can't be negative.");
	ERR_FAIL_COND_MSG(p_bit_size < 0, "The bit size can't be negative.");
	if (buffer.size_in_bits() - p_metadata_bit_size < p_bit_size) {
		buffer_failed = true;
		ERR_FAIL_MSG(vformat("The buffer (%d bits) is smaller than the new size (%d + %d bits).", buffer.size_in_bits(), p_metadata_bit_size, p_bit_size));
	}
	metadata_size = p_metadata_bit_size;
	bit_size = p_bit_size;
}

void TickDataBuffer::skip(int p_bits) {
	if (p_bits < 0 || bit_offset > total_size() - p_bits) {
		buffer_failed = true;
		return;
	}
	bit_offset += p_bits;
}

void TickDataBuffer::zero() {
	buffer.zero();
}

bool TickDataBuffer::add_bool(bool p_input) {
	if (!check_writing()) {
		return p_input;
	}
	write_bits(p_input ? 1 : 0, 1);
	return p_input;
}

bool TickDataBuffer::read_bool() {
	if (!check_reading(1)) {
		return false;
	}
	return fetch_bits(1) != 0;
}

int64_t TickDataBuffer::add_int(int64_t p_input, CompressionLevel p_compression_level) {
	ERR_FAIL_COND_V(!is_valid_compression_level(p_compression_level), p_input);
	if (!check_writing()) {
		return p_input;
	}

	const int bits = get_bit_taken(DATA_TYPE_INT, p_compression_level);
	int64_t value = p_input;
	if (bits == 8) {
		value = CLAMP(value, int64_t(INT8_MIN), int64_t(INT8_MAX));
	} else if (bits == 16) {
		value = CLAMP(value, int64_t(INT16_MIN), int64_t(INT16_MAX));
	} else if (bits == 32) {
		value = CLAMP(value, int64_t(INT32_MIN), int64_t(INT32_MAX));
	}

	// Two's complement: the low bits carry the sign, restored by the sign extension on read.
	uint64_t uvalue;
	memcpy(&uvalue, &value, sizeof(uint64_t));
	write_bits(uvalue, bits);
	return value;
}

int64_t TickDataBuffer::read_int(CompressionLevel p_compression_level) {
	ERR_FAIL_COND_V(!is_valid_compression_level(p_compression_level), 0);
	const int bits = get_bit_taken(DATA_TYPE_INT, p_compression_level);
	if (!check_reading(bits)) {
		return 0;
	}

	const uint64_t uvalue = fetch_bits(bits);
	if (bits == 8) {
		return int8_t(uint8_t(uvalue));
	} else if (bits == 16) {
		return int16_t(uint16_t(uvalue));
	} else if (bits == 32) {
		return int32_t(uint32_t(uvalue));
	}
	int64_t value;
	memcpy(&value, &uvalue, sizeof(uint64_t));
	return value;
}

uint64_t TickDataBuffer::add_uint(uint64_t p_input, CompressionLevel p_compression_level) {
	ERR_FAIL_COND_V(!is_valid_compression_level(p_compression_level), p_input);
	if (!check_writing()) {
		return p_input;
	}

	const int bits = get_bit_taken(DATA_TYPE_UINT, p_compression_level);
	uint64_t value = p_input;
	if (bits == 8) {
		value = MIN(value, uint64_t(UINT8_MAX));
	} else if (bits == 16) {
		value = MIN(value, uint64_t(UINT16_MAX));
	} else if (bits == 32) {
		value = MIN(value, uint64_t(UINT32_MAX));
	}

	write_bits(value, bits);
	return value;
}

uint64_t TickDataBuffer::read_uint(CompressionLevel p_compression_level) {
	ERR_FAIL_COND_V(!is_valid_compression_level(p_compression_level), 0);
	const int bits = get_bit_taken(DATA_TYPE_UINT, p_compression_level);
	if (!check_reading(bits)) {
		return 0;
	}
	return fetch_bits(bits);
}

uint64_t TickDataBuffer::add_uint_bits(uint64_t p_input, int p_bits) {
	ERR_FAIL_COND_V_MSG(p_bits < 1 || p_bits > 64, p_input, vformat("The number of bits must be between 1 and 64, but it's %d.", p_bits));
	if (!check_writing()) {
		return p_input;
	}
	const uint64_t max_value = p_bits == 64 ? UINT64_MAX : (uint64_t(1) << p_bits) - 1;
	const uint64_t value = MIN(p_input, max_value);
	write_bits(value, p_bits);
	return value;
}

uint64_t TickDataBuffer::read_uint_bits(int p_bits) {
	ERR_FAIL_COND_V_MSG(p_bits < 1 || p_bits > 64, 0, vformat("The number of bits must be between 1 and 64, but it's %d.", p_bits));
	if (!check_reading(p_bits)) {
		return 0;
	}
	return fetch_bits(p_bits);
}

int64_t TickDataBuffer::add_int_bits(int64_t p_input, int p_bits) {
	ERR_FAIL_COND_V_MSG(p_bits < 1 || p_bits > 64, p_input, vformat("The number of bits must be between 1 and 64, but it's %d.", p_bits));
	if (!check_writing()) {
		return p_input;
	}
	int64_t value = p_input;
	if (p_bits < 64) {
		const int64_t max_value = (int64_t(1) << (p_bits - 1)) - 1;
		const int64_t min_value = -max_value - 1;
		value = CLAMP(value, min_value, max_value);
	}
	uint64_t uvalue;
	memcpy(&uvalue, &value, sizeof(uint64_t));
	write_bits(uvalue, p_bits);
	return value;
}

int64_t TickDataBuffer::read_int_bits(int p_bits) {
	ERR_FAIL_COND_V_MSG(p_bits < 1 || p_bits > 64, 0, vformat("The number of bits must be between 1 and 64, but it's %d.", p_bits));
	if (!check_reading(p_bits)) {
		return 0;
	}
	uint64_t uvalue = fetch_bits(p_bits);
	if (p_bits < 64 && (uvalue & (uint64_t(1) << (p_bits - 1)))) {
		// Sign extension.
		uvalue |= UINT64_MAX << p_bits;
	}
	int64_t value;
	memcpy(&value, &uvalue, sizeof(uint64_t));
	return value;
}

double TickDataBuffer::add_real(double p_input, CompressionLevel p_compression_level) {
	ERR_FAIL_COND_V(!is_valid_compression_level(p_compression_level), p_input);
	if (!check_writing()) {
		return p_input;
	}

	switch (p_compression_level) {
		case COMPRESSION_LEVEL_0: {
			uint64_t value;
			memcpy(&value, &p_input, sizeof(uint64_t));
			write_bits(value, 64);
			return p_input;
		}
		case COMPRESSION_LEVEL_1: {
			const float input = float(p_input);
			uint32_t value;
			memcpy(&value, &input, sizeof(uint32_t));
			write_bits(value, 32);
			return input;
		}
		case COMPRESSION_LEVEL_2:
		case COMPRESSION_LEVEL_3:
		default: {
			const uint16_t value = Math::make_half_float(float(p_input));
			write_bits(value, 16);
			return Math::half_to_float(value);
		}
	}
}

double TickDataBuffer::read_real(CompressionLevel p_compression_level) {
	ERR_FAIL_COND_V(!is_valid_compression_level(p_compression_level), 0.0);
	const int bits = get_bit_taken(DATA_TYPE_REAL, p_compression_level);
	if (!check_reading(bits)) {
		return 0.0;
	}

	const uint64_t value = fetch_bits(bits);
	switch (p_compression_level) {
		case COMPRESSION_LEVEL_0: {
			double output;
			memcpy(&output, &value, sizeof(double));
			return output;
		}
		case COMPRESSION_LEVEL_1: {
			const uint32_t value_32 = uint32_t(value);
			float output;
			memcpy(&output, &value_32, sizeof(float));
			return output;
		}
		case COMPRESSION_LEVEL_2:
		case COMPRESSION_LEVEL_3:
		default:
			return Math::half_to_float(uint16_t(value));
	}
}

float TickDataBuffer::add_positive_unit_real(float p_input, CompressionLevel p_compression_level) {
	ERR_FAIL_COND_V(!is_valid_compression_level(p_compression_level), p_input);
	if (!check_writing()) {
		return p_input;
	}

	const int bits = get_bit_taken(DATA_TYPE_POSITIVE_UNIT_REAL, p_compression_level);
	const double max_value = double((uint64_t(1) << bits) - 1);
	const uint64_t compressed = compress_unit_float(p_input, max_value);
	write_bits(compressed, bits);
	return float(decompress_unit_float(compressed, max_value));
}

float TickDataBuffer::read_positive_unit_real(CompressionLevel p_compression_level) {
	ERR_FAIL_COND_V(!is_valid_compression_level(p_compression_level), 0.0f);
	const int bits = get_bit_taken(DATA_TYPE_POSITIVE_UNIT_REAL, p_compression_level);
	if (!check_reading(bits)) {
		return 0.0f;
	}
	const double max_value = double((uint64_t(1) << bits) - 1);
	return float(decompress_unit_float(fetch_bits(bits), max_value));
}

float TickDataBuffer::add_unit_real(float p_input, CompressionLevel p_compression_level) {
	ERR_FAIL_COND_V(!is_valid_compression_level(p_compression_level), p_input);
	if (!check_writing()) {
		return p_input;
	}

	const float magnitude = add_positive_unit_real(Math::abs(p_input), p_compression_level);
	// Only a non zero magnitude carries the sign, so the encoding of zero is unique.
	const bool is_negative = p_input < 0.0f && magnitude > 0.0f;
	write_bits(is_negative ? 1 : 0, 1);
	return is_negative ? -magnitude : magnitude;
}

float TickDataBuffer::read_unit_real(CompressionLevel p_compression_level) {
	ERR_FAIL_COND_V(!is_valid_compression_level(p_compression_level), 0.0f);
	if (!check_reading(get_bit_taken(DATA_TYPE_UNIT_REAL, p_compression_level))) {
		return 0.0f;
	}
	const float magnitude = read_positive_unit_real(p_compression_level);
	const bool is_negative = fetch_bits(1) != 0;
	return is_negative ? -magnitude : magnitude;
}

Vector2 TickDataBuffer::add_vector2(const Vector2 &p_input, CompressionLevel p_compression_level) {
	Vector2 output;
	output.x = real_t(add_real(p_input.x, p_compression_level));
	output.y = real_t(add_real(p_input.y, p_compression_level));
	return output;
}

Vector2 TickDataBuffer::read_vector2(CompressionLevel p_compression_level) {
	Vector2 output;
	output.x = real_t(read_real(p_compression_level));
	output.y = real_t(read_real(p_compression_level));
	return output;
}

Vector2 TickDataBuffer::add_normalized_vector2(const Vector2 &p_input, CompressionLevel p_compression_level) {
	ERR_FAIL_COND_V(!is_valid_compression_level(p_compression_level), p_input);
	if (!check_writing()) {
		return p_input;
	}

	const int bits = get_bit_taken(DATA_TYPE_NORMALIZED_VECTOR2, p_compression_level);
	const int angle_bits = bits - 1;
	const double max_value = double((uint64_t(1) << angle_bits) - 1);

	const bool is_not_zero = !p_input.is_zero_approx();
	const double angle = is_not_zero ? Math::atan2(double(p_input.y), double(p_input.x)) : 0.0;
	const uint64_t compressed_angle = compress_unit_float((angle + Math::PI) / Math::TAU, max_value);

	write_bits(is_not_zero ? 1 : 0, 1);
	write_bits(compressed_angle, angle_bits);

	if (!is_not_zero) {
		return Vector2();
	}
	const double decompressed_angle = decompress_unit_float(compressed_angle, max_value) * Math::TAU - Math::PI;
	return Vector2(real_t(Math::cos(decompressed_angle)), real_t(Math::sin(decompressed_angle)));
}

Vector2 TickDataBuffer::read_normalized_vector2(CompressionLevel p_compression_level) {
	ERR_FAIL_COND_V(!is_valid_compression_level(p_compression_level), Vector2());
	const int bits = get_bit_taken(DATA_TYPE_NORMALIZED_VECTOR2, p_compression_level);
	if (!check_reading(bits)) {
		return Vector2();
	}

	const int angle_bits = bits - 1;
	const double max_value = double((uint64_t(1) << angle_bits) - 1);
	const bool is_not_zero = fetch_bits(1) != 0;
	const uint64_t compressed_angle = fetch_bits(angle_bits);

	if (!is_not_zero) {
		return Vector2();
	}
	const double decompressed_angle = decompress_unit_float(compressed_angle, max_value) * Math::TAU - Math::PI;
	return Vector2(real_t(Math::cos(decompressed_angle)), real_t(Math::sin(decompressed_angle)));
}

Vector3 TickDataBuffer::add_vector3(const Vector3 &p_input, CompressionLevel p_compression_level) {
	Vector3 output;
	output.x = real_t(add_real(p_input.x, p_compression_level));
	output.y = real_t(add_real(p_input.y, p_compression_level));
	output.z = real_t(add_real(p_input.z, p_compression_level));
	return output;
}

Vector3 TickDataBuffer::read_vector3(CompressionLevel p_compression_level) {
	Vector3 output;
	output.x = real_t(read_real(p_compression_level));
	output.y = real_t(read_real(p_compression_level));
	output.z = real_t(read_real(p_compression_level));
	return output;
}

Vector3 TickDataBuffer::add_normalized_vector3(const Vector3 &p_input, CompressionLevel p_compression_level) {
	const Vector3 input = p_input.is_zero_approx() ? Vector3() : p_input.normalized();
	Vector3 output;
	output.x = real_t(add_unit_real(float(input.x), p_compression_level));
	output.y = real_t(add_unit_real(float(input.y), p_compression_level));
	output.z = real_t(add_unit_real(float(input.z), p_compression_level));
	return output;
}

Vector3 TickDataBuffer::read_normalized_vector3(CompressionLevel p_compression_level) {
	Vector3 output;
	output.x = real_t(read_unit_real(p_compression_level));
	output.y = real_t(read_unit_real(p_compression_level));
	output.z = real_t(read_unit_real(p_compression_level));
	return output;
}

void TickDataBuffer::add_string(const String &p_input) {
	if (!check_writing()) {
		return;
	}
	const CharString utf8 = p_input.utf8();
	if (utf8.length() > MAX_STRING_BYTES) {
		buffer_failed = true;
		ERR_FAIL_MSG(vformat("A string can't be longer than %d bytes in UTF-8, but it's %d bytes.", MAX_STRING_BYTES, utf8.length()));
	}
	if (!is_valid_utf8(reinterpret_cast<const uint8_t *>(utf8.get_data()), utf8.length())) {
		buffer_failed = true;
		ERR_FAIL_MSG("The string can't be sent: it has a NUL character or an unpaired surrogate, which the readers refuse.");
	}
	add_uint(uint64_t(utf8.length()), COMPRESSION_LEVEL_2);
	add_bits(reinterpret_cast<const uint8_t *>(utf8.get_data()), utf8.length() * 8);
}

String TickDataBuffer::read_string(int p_max_bytes) {
	const int length = int(read_uint(COMPRESSION_LEVEL_2));
	if (length == 0 || buffer_failed) {
		return String();
	}
	if (length > p_max_bytes || !check_reading(length * 8)) {
		buffer_failed = true;
		return String();
	}
	LocalVector<char> chars;
	chars.resize(length);
	read_bits(reinterpret_cast<uint8_t *>(chars.ptr()), length * 8);
	// Checked first: the engine's decoder would print an error for every invalid byte.
	if (!is_valid_utf8(reinterpret_cast<const uint8_t *>(chars.ptr()), length)) {
		buffer_failed = true;
		return String();
	}
	return String::utf8(chars.ptr(), length);
}

void TickDataBuffer::add_data_buffer(const TickDataBuffer &p_input) {
	if (!check_writing()) {
		return;
	}
	const int input_bits = p_input.total_size();
	if (input_bits > MAX_NESTED_BUFFER_BITS) {
		buffer_failed = true;
		ERR_FAIL_MSG(vformat("A nested TickDataBuffer can't be bigger than %d bits, but it's %d bits.", MAX_NESTED_BUFFER_BITS, input_bits));
	}

	add_uint(uint64_t(input_bits), COMPRESSION_LEVEL_2);
	make_room_pad_to_next_byte();
	if (input_bits > 0) {
		p_input.slice(*this, 0, input_bits);
	}
}

void TickDataBuffer::read_data_buffer(TickDataBuffer &r_output) {
	r_output.begin_write(0);
	const int input_bits = int(read_uint(COMPRESSION_LEVEL_2));
	if (buffer_failed || !pad_to_next_byte() || !check_reading(input_bits)) {
		buffer_failed = true;
		r_output.begin_read();
		return;
	}
	if (input_bits > 0) {
		slice(r_output, bit_offset, input_bits);
		bit_offset += input_bits;
	}
	r_output.begin_read();
}

void TickDataBuffer::add_bits(const uint8_t *p_data, int p_bit_count) {
	ERR_FAIL_COND_MSG(p_bit_count < 0, "The bit count can't be negative.");
	ERR_FAIL_COND_MSG(p_bit_count > 0 && p_data == nullptr, "The source data is null.");
	if (!check_writing()) {
		return;
	}
	make_room_in_bits(p_bit_count);
	for (int i = 0; p_bit_count > 0; i++) {
		const int bits = MIN(p_bit_count, 8);
		write_bits(p_data[i], bits);
		p_bit_count -= bits;
	}
}

void TickDataBuffer::read_bits(uint8_t *r_data, int p_bit_count) {
	ERR_FAIL_COND_MSG(p_bit_count < 0, "The bit count can't be negative.");
	ERR_FAIL_COND_MSG(p_bit_count > 0 && r_data == nullptr, "The destination is null.");
	if (!check_reading(p_bit_count)) {
		return;
	}
	for (int i = 0; p_bit_count > 0; i++) {
		const int bits = MIN(p_bit_count, 8);
		r_data[i] = uint8_t(fetch_bits(bits));
		p_bit_count -= bits;
	}
}

void TickDataBuffer::skip_bool() {
	skip(get_bit_taken(DATA_TYPE_BOOL, COMPRESSION_LEVEL_0));
}

void TickDataBuffer::skip_int(CompressionLevel p_compression_level) {
	skip(get_bit_taken(DATA_TYPE_INT, p_compression_level));
}

void TickDataBuffer::skip_uint(CompressionLevel p_compression_level) {
	skip(get_bit_taken(DATA_TYPE_UINT, p_compression_level));
}

void TickDataBuffer::skip_real(CompressionLevel p_compression_level) {
	skip(get_bit_taken(DATA_TYPE_REAL, p_compression_level));
}

void TickDataBuffer::skip_positive_unit_real(CompressionLevel p_compression_level) {
	skip(get_bit_taken(DATA_TYPE_POSITIVE_UNIT_REAL, p_compression_level));
}

void TickDataBuffer::skip_unit_real(CompressionLevel p_compression_level) {
	skip(get_bit_taken(DATA_TYPE_UNIT_REAL, p_compression_level));
}

void TickDataBuffer::skip_vector2(CompressionLevel p_compression_level) {
	skip(get_bit_taken(DATA_TYPE_VECTOR2, p_compression_level));
}

void TickDataBuffer::skip_normalized_vector2(CompressionLevel p_compression_level) {
	skip(get_bit_taken(DATA_TYPE_NORMALIZED_VECTOR2, p_compression_level));
}

void TickDataBuffer::skip_vector3(CompressionLevel p_compression_level) {
	skip(get_bit_taken(DATA_TYPE_VECTOR3, p_compression_level));
}

void TickDataBuffer::skip_normalized_vector3(CompressionLevel p_compression_level) {
	skip(get_bit_taken(DATA_TYPE_NORMALIZED_VECTOR3, p_compression_level));
}

void TickDataBuffer::skip_string() {
	const int length = int(read_uint(COMPRESSION_LEVEL_2));
	skip(length * 8);
}

void TickDataBuffer::skip_data_buffer() {
	const int input_bits = int(read_uint(COMPRESSION_LEVEL_2));
	if (buffer_failed || !pad_to_next_byte()) {
		buffer_failed = true;
		return;
	}
	skip(input_bits);
}

int TickDataBuffer::get_bit_taken(DataType p_data_type, CompressionLevel p_compression_level) {
	ERR_FAIL_COND_V(!is_valid_compression_level(p_compression_level), 0);
	switch (p_data_type) {
		case DATA_TYPE_BOOL:
			return 1;
		case DATA_TYPE_INT:
		case DATA_TYPE_UINT: {
			static const int sizes[4] = { 64, 32, 16, 8 };
			return sizes[p_compression_level];
		}
		case DATA_TYPE_REAL: {
			static const int sizes[4] = { 64, 32, 16, 16 };
			return sizes[p_compression_level];
		}
		case DATA_TYPE_POSITIVE_UNIT_REAL: {
			static const int sizes[4] = { 10, 8, 6, 4 };
			return sizes[p_compression_level];
		}
		case DATA_TYPE_UNIT_REAL:
			// One extra bit for the sign.
			return get_bit_taken(DATA_TYPE_POSITIVE_UNIT_REAL, p_compression_level) + 1;
		case DATA_TYPE_VECTOR2:
			return get_bit_taken(DATA_TYPE_REAL, p_compression_level) * 2;
		case DATA_TYPE_NORMALIZED_VECTOR2: {
			// The angle, plus one bit that tells if the vector is zero.
			static const int sizes[4] = { 11 + 1, 10 + 1, 9 + 1, 8 + 1 };
			return sizes[p_compression_level];
		}
		case DATA_TYPE_VECTOR3:
			return get_bit_taken(DATA_TYPE_REAL, p_compression_level) * 3;
		case DATA_TYPE_NORMALIZED_VECTOR3:
			return get_bit_taken(DATA_TYPE_UNIT_REAL, p_compression_level) * 3;
		case DATA_TYPE_BITS:
		case DATA_TYPE_DATABUFFER:
			// Dynamically sized.
			return 0;
	}
	ERR_FAIL_V_MSG(0, vformat("Unknown data type %d.", int(p_data_type)));
}

double TickDataBuffer::get_real_epsilon(DataType p_data_type, CompressionLevel p_compression_level) {
	ERR_FAIL_COND_V(!is_valid_compression_level(p_compression_level), 0.0);
	switch (p_data_type) {
		case DATA_TYPE_REAL:
		case DATA_TYPE_VECTOR2:
		case DATA_TYPE_VECTOR3: {
			// Relative precision: 2^-(mantissa bits - 1) for binary64, binary32 and binary16.
			static const int mantissa_bits[4] = { 53, 24, 11, 11 };
			return Math::pow(2.0, -double(mantissa_bits[p_compression_level] - 1));
		}
		case DATA_TYPE_POSITIVE_UNIT_REAL:
		case DATA_TYPE_UNIT_REAL:
		case DATA_TYPE_NORMALIZED_VECTOR3: {
			// Half of the quantization step.
			const int bits = get_bit_taken(DATA_TYPE_POSITIVE_UNIT_REAL, p_compression_level);
			return 0.5 / double((uint64_t(1) << bits) - 1);
		}
		case DATA_TYPE_NORMALIZED_VECTOR2: {
			// Half of the angle step bounds the error of each component.
			const int angle_bits = get_bit_taken(DATA_TYPE_NORMALIZED_VECTOR2, p_compression_level) - 1;
			return Math::PI / double((uint64_t(1) << angle_bits) - 1);
		}
		default:
			return 0.0;
	}
}

uint64_t TickDataBuffer::compress_unit_float(double p_value, double p_scale_factor) {
	return uint64_t(Math::round(CLAMP(p_value, 0.0, 1.0) * p_scale_factor));
}

double TickDataBuffer::decompress_unit_float(uint64_t p_value, double p_scale_factor) {
	return MIN(double(p_value) / p_scale_factor, 1.0);
}

bool TickDataBuffer::is_valid_utf8(const uint8_t *p_bytes, int p_length) {
	ERR_FAIL_COND_V(p_length < 0 || (p_length > 0 && p_bytes == nullptr), false);
	// The well-formed byte sequences of the Unicode standard (table 3-7): no overlong encodings, no surrogates,
	// nothing above U+10FFFF.
	int offset = 0;
	while (offset < p_length) {
		const uint8_t lead = p_bytes[offset];
		if (lead < 0x80) {
			if (lead == 0) {
				return false;
			}
			offset++;
			continue;
		}
		int size = 0;
		// Range of the second byte; the others are always 0x80 to 0xBF.
		uint8_t second_min = 0x80;
		uint8_t second_max = 0xBF;
		if (lead >= 0xC2 && lead <= 0xDF) {
			size = 2;
		} else if (lead >= 0xE0 && lead <= 0xEF) {
			size = 3;
			if (lead == 0xE0) {
				second_min = 0xA0;
			} else if (lead == 0xED) {
				second_max = 0x9F;
			}
		} else if (lead >= 0xF0 && lead <= 0xF4) {
			size = 4;
			if (lead == 0xF0) {
				second_min = 0x90;
			} else if (lead == 0xF4) {
				second_max = 0x8F;
			}
		} else {
			// A continuation byte, an overlong lead (0xC0, 0xC1), or beyond Unicode (0xF5 and above).
			return false;
		}
		if (size > p_length - offset || p_bytes[offset + 1] < second_min || p_bytes[offset + 1] > second_max) {
			return false;
		}
		for (int i = 2; i < size; i++) {
			if ((p_bytes[offset + i] & 0xC0) != 0x80) {
				return false;
			}
		}
		offset += size;
	}
	return true;
}

bool TickDataBuffer::check_writing() {
	if (is_reading) {
		buffer_failed = true;
		ERR_FAIL_V_MSG(false, "Can't write to a TickDataBuffer while reading; call `begin_write()` first.");
	}
	return true;
}

bool TickDataBuffer::check_reading(int p_bits) {
	if (!is_reading) {
		buffer_failed = true;
		ERR_FAIL_V_MSG(false, "Can't read from a TickDataBuffer while writing; call `begin_read()` first.");
	}
	// Malformed or truncated data fails silently: it may come from an untrusted peer.
	if (buffer_failed || p_bits < 0 || bit_offset > total_size() - p_bits) {
		buffer_failed = true;
		return false;
	}
	return true;
}

void TickDataBuffer::write_bits(uint64_t p_value, int p_bits) {
	make_room_in_bits(p_bits);
	if (!buffer.store_bits(bit_offset, p_value, p_bits)) {
		buffer_failed = true;
	}
	bit_offset += p_bits;
}

uint64_t TickDataBuffer::fetch_bits(int p_bits) {
	uint64_t value = 0;
	if (!buffer.read_bits(bit_offset, p_bits, value)) {
		buffer_failed = true;
	}
	bit_offset += p_bits;
	return value;
}

void TickDataBuffer::make_room_in_bits(int p_bits) {
	const int min_size = bit_offset + p_bits;
	if (min_size > buffer.size_in_bits()) {
		// Grows geometrically, with at least 64 bits to spare: fewer reallocations, and writes take the 64-bit window
		// path of `TickBitArray`. `dry()` trims the buffer to the written size before it's sent.
		buffer.resize_in_bits(MAX(min_size + 64, buffer.size_in_bits() * 2));
	}
	if (min_size - metadata_size > bit_size) {
		bit_size = min_size - metadata_size;
	}
}

void TickDataBuffer::make_room_pad_to_next_byte() {
	const int bits_to_next_byte = ((bit_offset + 7) & ~7) - bit_offset;
	make_room_in_bits(bits_to_next_byte);
	bit_offset += bits_to_next_byte;
}

bool TickDataBuffer::pad_to_next_byte() {
	const int bits_to_next_byte = ((bit_offset + 7) & ~7) - bit_offset;
	if (bit_offset + bits_to_next_byte > total_size()) {
		return false;
	}
	bit_offset += bits_to_next_byte;
	return true;
}

bool TickDataBuffer::is_valid_compression_level(CompressionLevel p_compression_level) {
	return p_compression_level >= COMPRESSION_LEVEL_0 && p_compression_level <= COMPRESSION_LEVEL_3;
}
