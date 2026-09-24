#pragma once

#include "core/templates/local_vector.h"
#include "core/typedefs.h"

// Growable array of bits, used as the storage of `TickDataBuffer`.
// Bits are stored little-endian: bit 0 is the least significant bit of byte 0.
class TickBitArray {
	LocalVector<uint8_t> bytes;

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
	bool store_bits(int p_bit_offset, uint64_t p_value, int p_bits);
	bool read_bits(int p_bit_offset, int p_bits, uint64_t &r_out) const;

	// Sets all the bytes to 0.
	void zero();
};
