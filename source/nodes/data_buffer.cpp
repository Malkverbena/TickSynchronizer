#include "data_buffer.h"

void DataBuffer::wrap(TickDataBuffer *p_buffer) {
	target = p_buffer ? p_buffer : &buffer;
}

void DataBuffer::begin_write() {
	target->begin_write();
}

void DataBuffer::begin_read() {
	target->begin_read();
}

int DataBuffer::get_size() const {
	return target->size();
}

int DataBuffer::get_bit_offset() const {
	return target->get_bit_offset();
}

bool DataBuffer::is_reading() const {
	return target->get_is_reading();
}

bool DataBuffer::is_failed() const {
	return target->is_buffer_failed();
}

bool DataBuffer::is_end_of_buffer() const {
	return target->is_end_of_buffer();
}

bool DataBuffer::add_bool(bool p_value) {
	return target->add_bool(p_value);
}

bool DataBuffer::read_bool() {
	return target->read_bool();
}

int64_t DataBuffer::add_int(int64_t p_value, int p_bits) {
	return target->add_int_bits(p_value, p_bits);
}

int64_t DataBuffer::read_int(int p_bits) {
	return target->read_int_bits(p_bits);
}

int64_t DataBuffer::add_uint(int64_t p_value, int p_bits) {
	ERR_FAIL_COND_V_MSG(p_bits < 1 || p_bits > 63, 0, "Unsigned integers take between 1 and 63 bits.");
	return int64_t(target->add_uint_bits(uint64_t(MAX(p_value, int64_t(0))), p_bits));
}

int64_t DataBuffer::read_uint(int p_bits) {
	ERR_FAIL_COND_V_MSG(p_bits < 1 || p_bits > 63, 0, "Unsigned integers take between 1 and 63 bits.");
	return int64_t(target->read_uint_bits(p_bits));
}

double DataBuffer::add_real(double p_value, CompressionLevel p_compression_level) {
	return target->add_real(p_value, TickDataBuffer::CompressionLevel(p_compression_level));
}

double DataBuffer::read_real(CompressionLevel p_compression_level) {
	return target->read_real(TickDataBuffer::CompressionLevel(p_compression_level));
}

double DataBuffer::add_positive_unit_real(double p_value, CompressionLevel p_compression_level) {
	return target->add_positive_unit_real(float(p_value), TickDataBuffer::CompressionLevel(p_compression_level));
}

double DataBuffer::read_positive_unit_real(CompressionLevel p_compression_level) {
	return target->read_positive_unit_real(TickDataBuffer::CompressionLevel(p_compression_level));
}

double DataBuffer::add_unit_real(double p_value, CompressionLevel p_compression_level) {
	return target->add_unit_real(float(p_value), TickDataBuffer::CompressionLevel(p_compression_level));
}

double DataBuffer::read_unit_real(CompressionLevel p_compression_level) {
	return target->read_unit_real(TickDataBuffer::CompressionLevel(p_compression_level));
}

Vector2 DataBuffer::add_vector2(const Vector2 &p_value, CompressionLevel p_compression_level) {
	return target->add_vector2(p_value, TickDataBuffer::CompressionLevel(p_compression_level));
}

Vector2 DataBuffer::read_vector2(CompressionLevel p_compression_level) {
	return target->read_vector2(TickDataBuffer::CompressionLevel(p_compression_level));
}

Vector2 DataBuffer::add_normalized_vector2(const Vector2 &p_value, CompressionLevel p_compression_level) {
	return target->add_normalized_vector2(p_value, TickDataBuffer::CompressionLevel(p_compression_level));
}

Vector2 DataBuffer::read_normalized_vector2(CompressionLevel p_compression_level) {
	return target->read_normalized_vector2(TickDataBuffer::CompressionLevel(p_compression_level));
}

Vector3 DataBuffer::add_vector3(const Vector3 &p_value, CompressionLevel p_compression_level) {
	return target->add_vector3(p_value, TickDataBuffer::CompressionLevel(p_compression_level));
}

Vector3 DataBuffer::read_vector3(CompressionLevel p_compression_level) {
	return target->read_vector3(TickDataBuffer::CompressionLevel(p_compression_level));
}

Vector3 DataBuffer::add_normalized_vector3(const Vector3 &p_value, CompressionLevel p_compression_level) {
	return target->add_normalized_vector3(p_value, TickDataBuffer::CompressionLevel(p_compression_level));
}

Vector3 DataBuffer::read_normalized_vector3(CompressionLevel p_compression_level) {
	return target->read_normalized_vector3(TickDataBuffer::CompressionLevel(p_compression_level));
}

void DataBuffer::add_string(const String &p_value) {
	target->add_string(p_value);
}

String DataBuffer::read_string() {
	return target->read_string();
}

Variant DataBuffer::add_value(const Variant &p_value, const Ref<TickCodec> &p_codec) {
	ERR_FAIL_COND_V_MSG(p_codec.is_null(), p_value, "The codec is null.");
	const Variant quantized = p_codec->quantize(p_value);
	p_codec->encode(p_value, *target);
	return quantized;
}

Variant DataBuffer::read_value(const Ref<TickCodec> &p_codec) {
	ERR_FAIL_COND_V_MSG(p_codec.is_null(), Variant(), "The codec is null.");
	return p_codec->decode(*target);
}

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
