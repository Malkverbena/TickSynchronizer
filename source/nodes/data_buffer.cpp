// Implementation of `DataBuffer`: each method forwards to the `TickDataBuffer` it operates on, with the types a script
// uses (integers by number of bits, reals as `double`).

#include "data_buffer.h"

// Makes this object operate on `p_buffer` (owned by the caller) instead of its own buffer.
void DataBuffer::wrap(TickDataBuffer *p_buffer) {
	target = p_buffer ? p_buffer : &buffer;
}


// Starts writing from the beginning, discarding what the buffer had.
void DataBuffer::begin_write() {
	target->begin_write();
}


// Starts reading from the beginning.
void DataBuffer::begin_read() {
	target->begin_read();
}


// The size of what was written, in bits.
int DataBuffer::get_size() const {
	return target->size();
}


// The bit the next read or write happens at.
int DataBuffer::get_bit_offset() const {
	return target->get_bit_offset();
}


// Whether the buffer is being read rather than written.
bool DataBuffer::is_reading() const {
	return target->get_is_reading();
}


// Whether a read or a write failed: what was read after that can't be trusted.
bool DataBuffer::is_failed() const {
	return target->is_buffer_failed();
}


// Whether everything that was written has been read.
bool DataBuffer::is_end_of_buffer() const {
	return target->is_end_of_buffer();
}


// Writes a boolean, in 1 bit.
bool DataBuffer::add_bool(bool p_value) {
	return target->add_bool(p_value);
}


// Reads a boolean.
bool DataBuffer::read_bool() {
	return target->read_bool();
}


// Writes a signed integer in `p_bits` bits (1 to 64), clamped to the range of that size.
int64_t DataBuffer::add_int(int64_t p_value, int p_bits) {
	return target->add_int_bits(p_value, p_bits);
}


// Reads a signed integer of `p_bits` bits.
int64_t DataBuffer::read_int(int p_bits) {
	return target->read_int_bits(p_bits);
}


// Writes an unsigned integer in `p_bits` bits, clamped to the range of that size; a negative value is written as 0.
// At most 63 bits: a script's integer is signed.
int64_t DataBuffer::add_uint(int64_t p_value, int p_bits) {
	ERR_FAIL_COND_V_MSG(p_bits < 1 || p_bits > 63, 0, "Unsigned integers take between 1 and 63 bits.");
	return int64_t(target->add_uint_bits(uint64_t(MAX(p_value, int64_t(0))), p_bits));
}


// Reads an unsigned integer of `p_bits` bits (1 to 63).
int64_t DataBuffer::read_uint(int p_bits) {
	ERR_FAIL_COND_V_MSG(p_bits < 1 || p_bits > 63, 0, "Unsigned integers take between 1 and 63 bits.");
	return int64_t(target->read_uint_bits(p_bits));
}


// Writes a real as binary64, binary32 or binary16, by the compression level.
double DataBuffer::add_real(double p_value, CompressionLevel p_compression_level) {
	return target->add_real(p_value, TickDataBuffer::CompressionLevel(p_compression_level));
}


// Reads a real written at the same compression level.
double DataBuffer::read_real(CompressionLevel p_compression_level) {
	return target->read_real(TickDataBuffer::CompressionLevel(p_compression_level));
}


// Writes a value in the [0, 1] range; values outside it are clamped.
double DataBuffer::add_positive_unit_real(double p_value, CompressionLevel p_compression_level) {
	return target->add_positive_unit_real(float(p_value), TickDataBuffer::CompressionLevel(p_compression_level));
}


// Reads a value in the [0, 1] range.
double DataBuffer::read_positive_unit_real(CompressionLevel p_compression_level) {
	return target->read_positive_unit_real(TickDataBuffer::CompressionLevel(p_compression_level));
}


// Writes a value in the [-1, 1] range; values outside it are clamped.
double DataBuffer::add_unit_real(double p_value, CompressionLevel p_compression_level) {
	return target->add_unit_real(float(p_value), TickDataBuffer::CompressionLevel(p_compression_level));
}


// Reads a value in the [-1, 1] range.
double DataBuffer::read_unit_real(CompressionLevel p_compression_level) {
	return target->read_unit_real(TickDataBuffer::CompressionLevel(p_compression_level));
}


// Writes the two components of a vector as reals.
Vector2 DataBuffer::add_vector2(const Vector2 &p_value, CompressionLevel p_compression_level) {
	return target->add_vector2(p_value, TickDataBuffer::CompressionLevel(p_compression_level));
}


// Reads a vector written with `add_vector2()`.
Vector2 DataBuffer::read_vector2(CompressionLevel p_compression_level) {
	return target->read_vector2(TickDataBuffer::CompressionLevel(p_compression_level));
}


// Writes a 2D direction (or zero); the input is normalized when it isn't zero.
Vector2 DataBuffer::add_normalized_vector2(const Vector2 &p_value, CompressionLevel p_compression_level) {
	return target->add_normalized_vector2(p_value, TickDataBuffer::CompressionLevel(p_compression_level));
}


// Reads a 2D direction (or zero).
Vector2 DataBuffer::read_normalized_vector2(CompressionLevel p_compression_level) {
	return target->read_normalized_vector2(TickDataBuffer::CompressionLevel(p_compression_level));
}


// Writes the three components of a vector as reals.
Vector3 DataBuffer::add_vector3(const Vector3 &p_value, CompressionLevel p_compression_level) {
	return target->add_vector3(p_value, TickDataBuffer::CompressionLevel(p_compression_level));
}


// Reads a vector written with `add_vector3()`.
Vector3 DataBuffer::read_vector3(CompressionLevel p_compression_level) {
	return target->read_vector3(TickDataBuffer::CompressionLevel(p_compression_level));
}


// Writes a 3D direction (or zero); the input is normalized when it isn't zero.
Vector3 DataBuffer::add_normalized_vector3(const Vector3 &p_value, CompressionLevel p_compression_level) {
	return target->add_normalized_vector3(p_value, TickDataBuffer::CompressionLevel(p_compression_level));
}


// Reads a 3D direction (or zero).
Vector3 DataBuffer::read_normalized_vector3(CompressionLevel p_compression_level) {
	return target->read_normalized_vector3(TickDataBuffer::CompressionLevel(p_compression_level));
}


// Writes a string as UTF-8, of at most 65535 bytes.
void DataBuffer::add_string(const String &p_value) {
	target->add_string(p_value);
}


// Reads a string; one that isn't valid UTF-8 fails the buffer.
String DataBuffer::read_string() {
	return target->read_string();
}


// Writes any value with a codec, the same way a synchronized variable is written. Returns the value as the other
// side decodes it.
Variant DataBuffer::add_value(const Variant &p_value, const Ref<TickCodec> &p_codec) {
	ERR_FAIL_COND_V_MSG(p_codec.is_null(), p_value, "The codec is null.");
	const Variant quantized = p_codec->quantize(p_value);
	p_codec->encode(p_value, *target);
	return quantized;
}


// Reads a value written with `add_value()` and the same codec.
Variant DataBuffer::read_value(const Ref<TickCodec> &p_codec) {
	ERR_FAIL_COND_V_MSG(p_codec.is_null(), Variant(), "The codec is null.");
	return p_codec->decode(*target);
}


// Exposes the class to scripts.
void DataBuffer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("begin_write"), &DataBuffer::begin_write);
	ClassDB::bind_method(D_METHOD("begin_read"), &DataBuffer::begin_read);
	ClassDB::bind_method(D_METHOD("get_size"), &DataBuffer::get_size);
	ClassDB::bind_method(D_METHOD("get_bit_offset"), &DataBuffer::get_bit_offset);
	ClassDB::bind_method(D_METHOD("is_reading"), &DataBuffer::is_reading);
	ClassDB::bind_method(D_METHOD("is_failed"), &DataBuffer::is_failed);
	ClassDB::bind_method(D_METHOD("is_end_of_buffer"), &DataBuffer::is_end_of_buffer);

	ClassDB::bind_method(D_METHOD("add_bool", "value"), &DataBuffer::add_bool);
	ClassDB::bind_method(D_METHOD("read_bool"), &DataBuffer::read_bool);
	ClassDB::bind_method(D_METHOD("add_int", "value", "bits"), &DataBuffer::add_int, DEFVAL(32));
	ClassDB::bind_method(D_METHOD("read_int", "bits"), &DataBuffer::read_int, DEFVAL(32));
	ClassDB::bind_method(D_METHOD("add_uint", "value", "bits"), &DataBuffer::add_uint, DEFVAL(32));
	ClassDB::bind_method(D_METHOD("read_uint", "bits"), &DataBuffer::read_uint, DEFVAL(32));
	ClassDB::bind_method(D_METHOD("add_real", "value", "compression_level"), &DataBuffer::add_real, DEFVAL(COMPRESSION_LEVEL_1));
	ClassDB::bind_method(D_METHOD("read_real", "compression_level"), &DataBuffer::read_real, DEFVAL(COMPRESSION_LEVEL_1));
	ClassDB::bind_method(D_METHOD("add_positive_unit_real", "value", "compression_level"), &DataBuffer::add_positive_unit_real, DEFVAL(COMPRESSION_LEVEL_1));
	ClassDB::bind_method(D_METHOD("read_positive_unit_real", "compression_level"), &DataBuffer::read_positive_unit_real, DEFVAL(COMPRESSION_LEVEL_1));
	ClassDB::bind_method(D_METHOD("add_unit_real", "value", "compression_level"), &DataBuffer::add_unit_real, DEFVAL(COMPRESSION_LEVEL_1));
	ClassDB::bind_method(D_METHOD("read_unit_real", "compression_level"), &DataBuffer::read_unit_real, DEFVAL(COMPRESSION_LEVEL_1));
	ClassDB::bind_method(D_METHOD("add_vector2", "value", "compression_level"), &DataBuffer::add_vector2, DEFVAL(COMPRESSION_LEVEL_1));
	ClassDB::bind_method(D_METHOD("read_vector2", "compression_level"), &DataBuffer::read_vector2, DEFVAL(COMPRESSION_LEVEL_1));
	ClassDB::bind_method(D_METHOD("add_normalized_vector2", "value", "compression_level"), &DataBuffer::add_normalized_vector2, DEFVAL(COMPRESSION_LEVEL_1));
	ClassDB::bind_method(D_METHOD("read_normalized_vector2", "compression_level"), &DataBuffer::read_normalized_vector2, DEFVAL(COMPRESSION_LEVEL_1));
	ClassDB::bind_method(D_METHOD("add_vector3", "value", "compression_level"), &DataBuffer::add_vector3, DEFVAL(COMPRESSION_LEVEL_1));
	ClassDB::bind_method(D_METHOD("read_vector3", "compression_level"), &DataBuffer::read_vector3, DEFVAL(COMPRESSION_LEVEL_1));
	ClassDB::bind_method(D_METHOD("add_normalized_vector3", "value", "compression_level"), &DataBuffer::add_normalized_vector3, DEFVAL(COMPRESSION_LEVEL_1));
	ClassDB::bind_method(D_METHOD("read_normalized_vector3", "compression_level"), &DataBuffer::read_normalized_vector3, DEFVAL(COMPRESSION_LEVEL_1));
	ClassDB::bind_method(D_METHOD("add_string", "value"), &DataBuffer::add_string);
	ClassDB::bind_method(D_METHOD("read_string"), &DataBuffer::read_string);
	ClassDB::bind_method(D_METHOD("add_value", "value", "codec"), &DataBuffer::add_value);
	ClassDB::bind_method(D_METHOD("read_value", "codec"), &DataBuffer::read_value);

	BIND_ENUM_CONSTANT(COMPRESSION_LEVEL_0);
	BIND_ENUM_CONSTANT(COMPRESSION_LEVEL_1);
	BIND_ENUM_CONSTANT(COMPRESSION_LEVEL_2);
	BIND_ENUM_CONSTANT(COMPRESSION_LEVEL_3);
}
