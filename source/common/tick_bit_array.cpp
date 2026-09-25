#include "tick_bit_array.h"

#include "core/error/error_macros.h"
#include "core/variant/variant.h"

#include <cstring>

TickBitArray::TickBitArray(int p_initial_size_in_bits) {
	resize_in_bits(p_initial_size_in_bits);
}

TickBitArray::TickBitArray(const uint8_t *p_bytes, int p_size_in_bytes) {
	ERR_FAIL_COND_MSG(p_size_in_bytes < 0, "The bytes count can't be negative.");
	ERR_FAIL_COND_MSG(p_size_in_bytes > 0 && p_bytes == nullptr, "The source bytes are null.");
	bytes.resize(p_size_in_bytes);
	if (p_size_in_bytes > 0) {
		memcpy(bytes.ptr(), p_bytes, p_size_in_bytes);
	}
}

bool TickBitArray::resize_in_bytes(int p_bytes_count) {
	ERR_FAIL_COND_V_MSG(p_bytes_count < 0, false, "The bytes count can't be negative.");
	bytes.resize(p_bytes_count);
	return true;
}

bool TickBitArray::resize_in_bits(int p_bits_count) {
	ERR_FAIL_COND_V_MSG(p_bits_count < 0, false, "The bits count can't be negative.");
	// Round up to the next byte. Written this way to avoid overflowing near INT_MAX.
	bytes.resize(p_bits_count / 8 + (p_bits_count % 8 != 0 ? 1 : 0));
	return true;
}

// The bits don't fit in one window at the end of the array: byte by byte, with the checks.
bool TickBitArray::store_bits_slow(int p_bit_offset, uint64_t p_value, int p_bits) {
	ERR_FAIL_COND_V_MSG(p_bit_offset < 0, false, "The bit offset can't be negative.");
	ERR_FAIL_COND_V_MSG(p_bits <= 0 || p_bits > 64, false, vformat("The number of bits must be between 1 and 64, but it's %d.", p_bits));
	ERR_FAIL_COND_V_MSG(p_bit_offset > size_in_bits() - p_bits, false, vformat("The bit array size is %d bits, while trying to write %d bits starting from bit %d.", size_in_bits(), p_bits, p_bit_offset));

	int bits = p_bits;
	int bit_offset = p_bit_offset;
	uint64_t value = p_value;

	while (bits > 0) {
		const int bits_to_jump = bit_offset % 8;
		const int bits_to_write = MIN(bits, 8 - bits_to_jump);
		const int byte_offset = bit_offset / 8;

		// Mask of the bits of this byte that are written in this iteration.
		const uint8_t byte_mask = uint8_t(((1u << bits_to_write) - 1u) << bits_to_jump);
		const uint8_t byte_value = uint8_t((uint32_t(value & 0xFF) << bits_to_jump) & byte_mask);
		bytes[byte_offset] = uint8_t((bytes[byte_offset] & ~byte_mask) | byte_value);

		bits -= bits_to_write;
		bit_offset += bits_to_write;
		value >>= bits_to_write;
	}

	return true;
}

bool TickBitArray::read_bits_slow(int p_bit_offset, int p_bits, uint64_t &r_out) const {
	ERR_FAIL_COND_V_MSG(p_bit_offset < 0, false, "The bit offset can't be negative.");
	ERR_FAIL_COND_V_MSG(p_bits <= 0 || p_bits > 64, false, vformat("The number of bits must be between 1 and 64, but it's %d.", p_bits));
	ERR_FAIL_COND_V_MSG(p_bit_offset > size_in_bits() - p_bits, false, vformat("The bit array size is %d bits, while trying to read %d bits starting from bit %d.", size_in_bits(), p_bits, p_bit_offset));

	int bits = p_bits;
	int bit_offset = p_bit_offset;
	int value_shift = 0;
	uint64_t value = 0;

	while (bits > 0) {
		const int bits_to_jump = bit_offset % 8;
		const int bits_to_read = MIN(bits, 8 - bits_to_jump);
		const int byte_offset = bit_offset / 8;

		const uint32_t byte_mask = (1u << bits_to_read) - 1u;
		const uint64_t byte_value = (uint32_t(bytes[byte_offset]) >> bits_to_jump) & byte_mask;
		value |= byte_value << value_shift;

		bits -= bits_to_read;
		bit_offset += bits_to_read;
		value_shift += bits_to_read;
	}

	r_out = value;
	return true;
}

void TickBitArray::zero() {
	if (bytes.size() > 0) {
		memset(bytes.ptr(), 0, bytes.size());
	}
}
