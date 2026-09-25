#pragma once

#include "core/templates/local_vector.h"
#include "core/typedefs.h"

// Growable array of bits, used as the storage of `TickDataBuffer`.
// Bits are stored little-endian: bit 0 is the least significant bit of byte 0.
class TickBitArray {
	LocalVector<uint8_t> bytes;

	// Little-endian 64-bit window, the same on every platform; compilers turn these into a single load or store on
	// little-endian CPUs.
	static _FORCE_INLINE_ uint64_t load_le64(const uint8_t *p_bytes) {
		uint64_t value = 0;
		for (int i = 0; i < 8; i++) {
			value |= uint64_t(p_bytes[i]) << (8 * i);
		}
		return value;
	}
	static _FORCE_INLINE_ void store_le64(uint8_t *r_bytes, uint64_t p_value) {
		for (int i = 0; i < 8; i++) {
			r_bytes[i] = uint8_t(p_value >> (8 * i));
		}
	}
	static _FORCE_INLINE_ uint64_t low_bits_mask(int p_bits) {
		return p_bits >= 64 ? ~uint64_t(0) : (uint64_t(1) << p_bits) - 1;
	}
	// Whether the bits fit in one 64-bit window inside the array (the inline fast path).
	_FORCE_INLINE_ bool fits_window(int p_bit_offset, int p_bits) const {
		return p_bit_offset >= 0 && p_bits > 0 && (p_bit_offset % 8) + p_bits <= 64 && p_bit_offset / 8 + 8 <= int(bytes.size());
	}
	bool store_bits_slow(int p_bit_offset, uint64_t p_value, int p_bits);
	bool read_bits_slow(int p_bit_offset, int p_bits, uint64_t &r_out) const;

public:
	TickBitArray() {}
	TickBitArray(int p_initial_size_in_bits);
	TickBitArray(const uint8_t *p_bytes, int p_size_in_bytes);

	const LocalVector<uint8_t> &get_bytes() const { return bytes; }
	LocalVector<uint8_t> &get_bytes_mut() { return bytes; }

	bool resize_in_bytes(int p_bytes_count);
	int size_in_bytes() const { return int(bytes.size()); }

	bool resize_in_bits(int p_bits_count);
	int size_in_bits() const { return int(bytes.size()) * 8; }

	// Stores the `p_bits` least significant bits of `p_value` starting at `p_bit_offset`.
	// Bits of `p_value` above `p_bits` are ignored.
	_FORCE_INLINE_ bool store_bits(int p_bit_offset, uint64_t p_value, int p_bits) {
		if (likely(fits_window(p_bit_offset, p_bits))) {
			const int shift = p_bit_offset % 8;
			const uint64_t mask = low_bits_mask(p_bits) << shift;
			uint8_t *window = bytes.ptr() + p_bit_offset / 8;
			store_le64(window, (load_le64(window) & ~mask) | ((p_value << shift) & mask));
			return true;
		}
		return store_bits_slow(p_bit_offset, p_value, p_bits);
	}
	_FORCE_INLINE_ bool read_bits(int p_bit_offset, int p_bits, uint64_t &r_out) const {
		if (likely(fits_window(p_bit_offset, p_bits))) {
			r_out = (load_le64(bytes.ptr() + p_bit_offset / 8) >> (p_bit_offset % 8)) & low_bits_mask(p_bits);
			return true;
		}
		return read_bits_slow(p_bit_offset, p_bits, r_out);
	}

	// Sets all the bytes to 0.
	void zero();
};
