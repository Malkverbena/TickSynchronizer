#include "tick_codec.h"

#include "core/io/marshalls.h"
#include "core/math/math_funcs.h"
#include "core/math/quaternion.h"
#include "core/templates/hashfuncs.h"

// Largest absolute value of the three smallest components of a unit quaternion: 1 / sqrt(2).
static constexpr double QUATERNION_COMPONENT_MAX = 0.70710678118654752440;

Ref<TickCodec> TickCodec::boolean() {
	Ref<TickCodec> codec;
	codec.instantiate();
	codec->kind = KIND_BOOL;
	codec->bits = 1;
	return codec;
}

Ref<TickCodec> TickCodec::integer(int p_bits) {
	ERR_FAIL_COND_V_MSG(p_bits < 1 || p_bits > 64, Ref<TickCodec>(), vformat("The number of bits must be between 1 and 64, but it's %d.", p_bits));
	Ref<TickCodec> codec;
	codec.instantiate();
	codec->kind = KIND_INT;
	codec->bits = p_bits;
	return codec;
}

Ref<TickCodec> TickCodec::real(Precision p_precision) {
	Ref<TickCodec> codec;
	codec.instantiate();
	codec->kind = KIND_REAL;
	codec->precision = p_precision;
	return codec;
}

Ref<TickCodec> TickCodec::real_ranged(double p_min, double p_max, int p_bits) {
	ERR_FAIL_COND_V_MSG(!(p_min < p_max), Ref<TickCodec>(), "The minimum must be smaller than the maximum.");
	ERR_FAIL_COND_V_MSG(p_bits < 1 || p_bits > 32, Ref<TickCodec>(), vformat("The number of bits must be between 1 and 32, but it's %d.", p_bits));
	Ref<TickCodec> codec;
	codec.instantiate();
	codec->kind = KIND_REAL_RANGED;
	codec->range_min = p_min;
	codec->range_max = p_max;
	codec->bits = p_bits;
	return codec;
}

Ref<TickCodec> TickCodec::vector2(Precision p_precision) {
	Ref<TickCodec> codec = real(p_precision);
	codec->kind = KIND_VECTOR2;
	return codec;
}

Ref<TickCodec> TickCodec::vector2_ranged(double p_min, double p_max, int p_bits) {
	Ref<TickCodec> codec = real_ranged(p_min, p_max, p_bits);
	ERR_FAIL_COND_V(codec.is_null(), codec);
	codec->kind = KIND_VECTOR2_RANGED;
	return codec;
}

Ref<TickCodec> TickCodec::normalized_vector2(Precision p_precision) {
	Ref<TickCodec> codec = real(p_precision);
	codec->kind = KIND_NORMALIZED_VECTOR2;
	return codec;
}

Ref<TickCodec> TickCodec::vector3(Precision p_precision) {
	Ref<TickCodec> codec = real(p_precision);
	codec->kind = KIND_VECTOR3;
	return codec;
}

Ref<TickCodec> TickCodec::vector3_ranged(double p_min, double p_max, int p_bits) {
	Ref<TickCodec> codec = real_ranged(p_min, p_max, p_bits);
	ERR_FAIL_COND_V(codec.is_null(), codec);
	codec->kind = KIND_VECTOR3_RANGED;
	return codec;
}

Ref<TickCodec> TickCodec::normalized_vector3(Precision p_precision) {
	Ref<TickCodec> codec = real(p_precision);
	codec->kind = KIND_NORMALIZED_VECTOR3;
	return codec;
}

Ref<TickCodec> TickCodec::quaternion(int p_bits_per_component) {
	ERR_FAIL_COND_V_MSG(p_bits_per_component < 2 || p_bits_per_component > 32, Ref<TickCodec>(), vformat("The bits per component must be between 2 and 32, but it's %d.", p_bits_per_component));
	Ref<TickCodec> codec;
	codec.instantiate();
	codec->kind = KIND_QUATERNION;
	codec->bits = p_bits_per_component;
	codec->range_min = -QUATERNION_COMPONENT_MAX;
	codec->range_max = QUATERNION_COMPONENT_MAX;
	return codec;
}

Ref<TickCodec> TickCodec::variant() {
	Ref<TickCodec> codec;
	codec.instantiate();
	codec->kind = KIND_VARIANT;
	return codec;
}

void TickCodec::set_tolerance(double p_tolerance) {
	ERR_FAIL_COND_MSG(!(p_tolerance >= 0.0), "The tolerance can't be negative.");
	tolerance = p_tolerance;
	tolerance_overridden = true;
}

double TickCodec::get_tolerance() const {
	return tolerance_overridden ? tolerance : compute_default_tolerance();
}

double TickCodec::compute_default_tolerance() const {
	switch (kind) {
		case KIND_REAL:
		case KIND_VECTOR2:
		case KIND_VECTOR3:
			return TickDataBuffer::get_real_epsilon(TickDataBuffer::DATA_TYPE_REAL, get_compression_level(precision));
		case KIND_REAL_RANGED:
		case KIND_VECTOR2_RANGED:
		case KIND_VECTOR3_RANGED:
		case KIND_QUATERNION:
			// Half of the quantization step.
			return (range_max - range_min) / double((uint64_t(1) << bits) - 1) * 0.5;
		case KIND_NORMALIZED_VECTOR2:
			return TickDataBuffer::get_real_epsilon(TickDataBuffer::DATA_TYPE_NORMALIZED_VECTOR2, get_compression_level(precision));
		case KIND_NORMALIZED_VECTOR3:
			return TickDataBuffer::get_real_epsilon(TickDataBuffer::DATA_TYPE_NORMALIZED_VECTOR3, get_compression_level(precision));
		case KIND_BOOL:
		case KIND_INT:
		case KIND_VARIANT:
			return 0.0;
	}
	return 0.0;
}

Variant::Type TickCodec::get_value_type() const {
	switch (kind) {
		case KIND_BOOL:
			return Variant::BOOL;
		case KIND_INT:
			return Variant::INT;
		case KIND_REAL:
		case KIND_REAL_RANGED:
			return Variant::FLOAT;
		case KIND_VECTOR2:
		case KIND_VECTOR2_RANGED:
		case KIND_NORMALIZED_VECTOR2:
			return Variant::VECTOR2;
		case KIND_VECTOR3:
		case KIND_VECTOR3_RANGED:
		case KIND_NORMALIZED_VECTOR3:
			return Variant::VECTOR3;
		case KIND_QUATERNION:
			return Variant::QUATERNION;
		case KIND_VARIANT:
			return Variant::NIL;
	}
	return Variant::NIL;
}

Variant TickCodec::get_default_value() const {
	switch (kind) {
		case KIND_BOOL:
			return false;
		case KIND_INT:
			return int64_t(0);
		case KIND_REAL:
		case KIND_REAL_RANGED:
			return 0.0;
		case KIND_VECTOR2:
		case KIND_VECTOR2_RANGED:
		case KIND_NORMALIZED_VECTOR2:
			return Vector2();
		case KIND_VECTOR3:
		case KIND_VECTOR3_RANGED:
		case KIND_NORMALIZED_VECTOR3:
			return Vector3();
		case KIND_QUATERNION:
			return Quaternion();
		case KIND_VARIANT:
			return Variant();
	}
	return Variant();
}

bool TickCodec::check_type(const Variant &p_value) const {
	const Variant::Type expected = get_value_type();
	if (expected == Variant::NIL || p_value.get_type() == expected) {
		return true;
	}
	// Numbers convert between each other.
	const bool is_number = p_value.get_type() == Variant::INT || p_value.get_type() == Variant::FLOAT || p_value.get_type() == Variant::BOOL;
	return is_number && (expected == Variant::INT || expected == Variant::FLOAT || expected == Variant::BOOL);
}

TickDataBuffer::CompressionLevel TickCodec::get_compression_level(Precision p_precision) {
	return TickDataBuffer::CompressionLevel(CLAMP(int(p_precision), 0, 3));
}

uint64_t TickCodec::quantize_ranged(double p_value, double p_min, double p_max, int p_bits) {
	const double max_value = double((uint64_t(1) << p_bits) - 1);
	const double unit = (CLAMP(p_value, p_min, p_max) - p_min) / (p_max - p_min);
	return uint64_t(Math::round(unit * max_value));
}

double TickCodec::dequantize_ranged(uint64_t p_value, double p_min, double p_max, int p_bits) {
	const double max_value = double((uint64_t(1) << p_bits) - 1);
	return p_min + (double(p_value) / max_value) * (p_max - p_min);
}

void TickCodec::encode(const Variant &p_value, TickDataBuffer &r_buffer) const {
	Variant value = p_value;
	if (!check_type(p_value)) {
		ERR_PRINT(vformat("TickCodec expected a value of type %s, but got %s; the default value is sent instead.", Variant::get_type_name(get_value_type()), Variant::get_type_name(p_value.get_type())));
		value = get_default_value();
	}

	const TickDataBuffer::CompressionLevel level = get_compression_level(precision);
	switch (kind) {
		case KIND_BOOL:
			r_buffer.add_bool(bool(value));
			break;
		case KIND_INT:
			r_buffer.add_int_bits(int64_t(value), bits);
			break;
		case KIND_REAL:
			r_buffer.add_real(double(value), level);
			break;
		case KIND_REAL_RANGED:
			r_buffer.add_uint_bits(quantize_ranged(double(value), range_min, range_max, bits), bits);
			break;
		case KIND_VECTOR2:
			(void)r_buffer.add_vector2(Vector2(value), level);
			break;
		case KIND_VECTOR2_RANGED: {
			const Vector2 v = value;
			r_buffer.add_uint_bits(quantize_ranged(v.x, range_min, range_max, bits), bits);
			r_buffer.add_uint_bits(quantize_ranged(v.y, range_min, range_max, bits), bits);
		} break;
		case KIND_NORMALIZED_VECTOR2:
			(void)r_buffer.add_normalized_vector2(Vector2(value), level);
			break;
		case KIND_VECTOR3:
			(void)r_buffer.add_vector3(Vector3(value), level);
			break;
		case KIND_VECTOR3_RANGED: {
			const Vector3 v = value;
			r_buffer.add_uint_bits(quantize_ranged(v.x, range_min, range_max, bits), bits);
			r_buffer.add_uint_bits(quantize_ranged(v.y, range_min, range_max, bits), bits);
			r_buffer.add_uint_bits(quantize_ranged(v.z, range_min, range_max, bits), bits);
		} break;
		case KIND_NORMALIZED_VECTOR3:
			(void)r_buffer.add_normalized_vector3(Vector3(value), level);
			break;
		case KIND_QUATERNION: {
			// Smallest three: the index of the largest component, then the other three. The quaternion is negated
			// when needed so the largest component is positive (q and -q are the same rotation).
			Quaternion q = value;
			q = q.length_squared() > 0.0 ? q.normalized() : Quaternion();
			int largest = 0;
			for (int i = 1; i < 4; i++) {
				if (Math::abs(q[i]) > Math::abs(q[largest])) {
					largest = i;
				}
			}
			const double sign = q[largest] < 0.0 ? -1.0 : 1.0;
			r_buffer.add_uint_bits(uint64_t(largest), 2);
			for (int i = 0; i < 4; i++) {
				if (i != largest) {
					r_buffer.add_uint_bits(quantize_ranged(q[i] * sign, range_min, range_max, bits), bits);
				}
			}
		} break;
		case KIND_VARIANT: {
			int length = 0;
			Error err = encode_variant(value, nullptr, length, false);
			if (err != OK || length > MAX_VARIANT_BYTES) {
				ERR_PRINT(vformat("TickCodec can't encode this value (maximum %d bytes); nil is sent instead.", MAX_VARIANT_BYTES));
				value = Variant();
				err = encode_variant(value, nullptr, length, false);
				ERR_FAIL_COND(err != OK);
			}
			LocalVector<uint8_t> bytes;
			bytes.resize(length);
			encode_variant(value, bytes.ptr(), length, false);
			r_buffer.add_uint_bits(uint64_t(length), 16);
			r_buffer.add_bits(bytes.ptr(), length * 8);
		} break;
	}
}

Variant TickCodec::decode(TickDataBuffer &r_buffer) const {
	const TickDataBuffer::CompressionLevel level = get_compression_level(precision);
	switch (kind) {
		case KIND_BOOL:
			return r_buffer.read_bool();
		case KIND_INT:
			return r_buffer.read_int_bits(bits);
		case KIND_REAL:
			return r_buffer.read_real(level);
		case KIND_REAL_RANGED:
			return dequantize_ranged(r_buffer.read_uint_bits(bits), range_min, range_max, bits);
		case KIND_VECTOR2:
			return r_buffer.read_vector2(level);
		case KIND_VECTOR2_RANGED: {
			Vector2 v;
			v.x = real_t(dequantize_ranged(r_buffer.read_uint_bits(bits), range_min, range_max, bits));
			v.y = real_t(dequantize_ranged(r_buffer.read_uint_bits(bits), range_min, range_max, bits));
			return v;
		}
		case KIND_NORMALIZED_VECTOR2:
			return r_buffer.read_normalized_vector2(level);
		case KIND_VECTOR3:
			return r_buffer.read_vector3(level);
		case KIND_VECTOR3_RANGED: {
			Vector3 v;
			v.x = real_t(dequantize_ranged(r_buffer.read_uint_bits(bits), range_min, range_max, bits));
			v.y = real_t(dequantize_ranged(r_buffer.read_uint_bits(bits), range_min, range_max, bits));
			v.z = real_t(dequantize_ranged(r_buffer.read_uint_bits(bits), range_min, range_max, bits));
			return v;
		}
		case KIND_NORMALIZED_VECTOR3:
			return r_buffer.read_normalized_vector3(level);
		case KIND_QUATERNION: {
			const int largest = int(r_buffer.read_uint_bits(2));
			Quaternion q;
			double sum_squared = 0.0;
			for (int i = 0; i < 4; i++) {
				if (i != largest) {
					const double component = dequantize_ranged(r_buffer.read_uint_bits(bits), range_min, range_max, bits);
					q[i] = real_t(component);
					sum_squared += component * component;
				}
			}
			q[largest] = real_t(Math::sqrt(MAX(0.0, 1.0 - sum_squared)));
			if (r_buffer.is_buffer_failed()) {
				return Quaternion();
			}
			return q.normalized();
		}
		case KIND_VARIANT: {
			const int length = int(r_buffer.read_uint_bits(16));
			if (r_buffer.is_buffer_failed() || length == 0) {
				return Variant();
			}
			LocalVector<uint8_t> bytes;
			bytes.resize(length);
			r_buffer.read_bits(bytes.ptr(), length * 8);
			if (r_buffer.is_buffer_failed()) {
				return Variant();
			}
			Variant value;
			// Objects are never decoded: the data may come from an untrusted peer.
			if (decode_variant(value, bytes.ptr(), length, nullptr, false) != OK) {
				return Variant();
			}
			return value;
		}
	}
	return Variant();
}

Variant TickCodec::quantize(const Variant &p_value) const {
	TickDataBuffer buffer;
	buffer.begin_write();
	encode(p_value, buffer);
	buffer.begin_read();
	return decode(buffer);
}

bool TickCodec::is_equal(const Variant &p_a, const Variant &p_b) const {
	const double tol = get_tolerance();
	switch (kind) {
		case KIND_BOOL:
			return bool(p_a) == bool(p_b);
		case KIND_INT:
			return int64_t(p_a) == int64_t(p_b);
		case KIND_REAL: {
			const double a = p_a;
			const double b = p_b;
			// Relative tolerance: the float precision depends on the magnitude.
			return Math::abs(a - b) <= tol * MAX(1.0, MAX(Math::abs(a), Math::abs(b)));
		}
		case KIND_REAL_RANGED:
			return Math::abs(double(p_a) - double(p_b)) <= tol;
		case KIND_VECTOR2:
		case KIND_VECTOR2_RANGED:
		case KIND_NORMALIZED_VECTOR2: {
			if (p_a.get_type() != Variant::VECTOR2 || p_b.get_type() != Variant::VECTOR2) {
				return false;
			}
			const Vector2 a = p_a;
			const Vector2 b = p_b;
			double scale = 1.0;
			if (kind == KIND_VECTOR2) {
				// Relative tolerance: the float precision depends on the magnitude.
				scale = MAX(scale, double(MAX(MAX(Math::abs(a.x), Math::abs(a.y)), MAX(Math::abs(b.x), Math::abs(b.y)))));
			}
			return Math::abs(double(a.x - b.x)) <= tol * scale && Math::abs(double(a.y - b.y)) <= tol * scale;
		}
		case KIND_VECTOR3:
		case KIND_VECTOR3_RANGED:
		case KIND_NORMALIZED_VECTOR3: {
			if (p_a.get_type() != Variant::VECTOR3 || p_b.get_type() != Variant::VECTOR3) {
				return false;
			}
			const Vector3 a = p_a;
			const Vector3 b = p_b;
			double scale = 1.0;
			if (kind == KIND_VECTOR3) {
				const Vector3 abs_a = a.abs();
				const Vector3 abs_b = b.abs();
				scale = MAX(scale, double(MAX(abs_a[abs_a.max_axis_index()], abs_b[abs_b.max_axis_index()])));
			}
			return Math::abs(double(a.x - b.x)) <= tol * scale && Math::abs(double(a.y - b.y)) <= tol * scale && Math::abs(double(a.z - b.z)) <= tol * scale;
		}
		case KIND_QUATERNION: {
			if (p_a.get_type() != Variant::QUATERNION || p_b.get_type() != Variant::QUATERNION) {
				return false;
			}
			const Quaternion a = p_a;
			const Quaternion b = p_b;
			// q and -q are the same rotation.
			const double sign = a.dot(b) < 0.0 ? -1.0 : 1.0;
			for (int i = 0; i < 4; i++) {
				if (Math::abs(double(a[i]) - sign * double(b[i])) > tol * 2.0) {
					return false;
				}
			}
			return true;
		}
		case KIND_VARIANT:
			return p_a == p_b;
	}
	return false;
}

Variant TickCodec::interpolate(const Variant &p_from, const Variant &p_to, double p_weight) const {
	switch (kind) {
		case KIND_REAL:
		case KIND_REAL_RANGED:
			return Math::lerp(double(p_from), double(p_to), p_weight);
		case KIND_VECTOR2:
		case KIND_VECTOR2_RANGED:
			return Vector2(p_from).lerp(Vector2(p_to), real_t(p_weight));
		case KIND_NORMALIZED_VECTOR2: {
			const Vector2 v = Vector2(p_from).lerp(Vector2(p_to), real_t(p_weight));
			return v.is_zero_approx() ? v : v.normalized();
		}
		case KIND_VECTOR3:
		case KIND_VECTOR3_RANGED:
			return Vector3(p_from).lerp(Vector3(p_to), real_t(p_weight));
		case KIND_NORMALIZED_VECTOR3: {
			const Vector3 v = Vector3(p_from).lerp(Vector3(p_to), real_t(p_weight));
			return v.is_zero_approx() ? v : v.normalized();
		}
		case KIND_QUATERNION:
			return Quaternion(p_from).normalized().slerp(Quaternion(p_to).normalized(), real_t(p_weight));
		case KIND_BOOL:
		case KIND_INT:
		case KIND_VARIANT:
			// Discrete values keep the past value until the next one is reached.
			return p_weight < 1.0 ? p_from : p_to;
	}
	return p_to;
}

uint32_t TickCodec::hash(uint32_t p_seed) const {
	uint32_t h = hash_murmur3_one_32(uint32_t(kind), p_seed);
	h = hash_murmur3_one_32(uint32_t(precision), h);
	h = hash_murmur3_one_32(uint32_t(bits), h);
	h = hash_murmur3_one_double(range_min, h);
	h = hash_murmur3_one_double(range_max, h);
	return h;
}

void TickCodec::_bind_methods() {
	ClassDB::bind_static_method("TickCodec", D_METHOD("boolean"), &TickCodec::boolean);
	ClassDB::bind_static_method("TickCodec", D_METHOD("integer", "bits"), &TickCodec::integer, DEFVAL(32));
	ClassDB::bind_static_method("TickCodec", D_METHOD("real", "precision"), &TickCodec::real, DEFVAL(PRECISION_FULL));
	ClassDB::bind_static_method("TickCodec", D_METHOD("real_ranged", "min", "max", "bits"), &TickCodec::real_ranged);
	ClassDB::bind_static_method("TickCodec", D_METHOD("vector2", "precision"), &TickCodec::vector2, DEFVAL(PRECISION_FULL));
	ClassDB::bind_static_method("TickCodec", D_METHOD("vector2_ranged", "min", "max", "bits"), &TickCodec::vector2_ranged);
	ClassDB::bind_static_method("TickCodec", D_METHOD("normalized_vector2", "precision"), &TickCodec::normalized_vector2, DEFVAL(PRECISION_FULL));
	ClassDB::bind_static_method("TickCodec", D_METHOD("vector3", "precision"), &TickCodec::vector3, DEFVAL(PRECISION_FULL));
	ClassDB::bind_static_method("TickCodec", D_METHOD("vector3_ranged", "min", "max", "bits"), &TickCodec::vector3_ranged);
	ClassDB::bind_static_method("TickCodec", D_METHOD("normalized_vector3", "precision"), &TickCodec::normalized_vector3, DEFVAL(PRECISION_FULL));
	ClassDB::bind_static_method("TickCodec", D_METHOD("quaternion", "bits_per_component"), &TickCodec::quaternion, DEFVAL(10));
	ClassDB::bind_static_method("TickCodec", D_METHOD("variant"), &TickCodec::variant);

	ClassDB::bind_method(D_METHOD("get_kind"), &TickCodec::get_kind);
	ClassDB::bind_method(D_METHOD("get_precision"), &TickCodec::get_precision);
	ClassDB::bind_method(D_METHOD("get_bits"), &TickCodec::get_bits);
	ClassDB::bind_method(D_METHOD("set_tolerance", "tolerance"), &TickCodec::set_tolerance);
	ClassDB::bind_method(D_METHOD("get_tolerance"), &TickCodec::get_tolerance);
	ClassDB::bind_method(D_METHOD("quantize", "value"), &TickCodec::quantize);
	ClassDB::bind_method(D_METHOD("is_equal", "a", "b"), &TickCodec::is_equal);
	ClassDB::bind_method(D_METHOD("interpolate", "from", "to", "weight"), &TickCodec::interpolate);

	BIND_ENUM_CONSTANT(KIND_BOOL);
	BIND_ENUM_CONSTANT(KIND_INT);
	BIND_ENUM_CONSTANT(KIND_REAL);
	BIND_ENUM_CONSTANT(KIND_REAL_RANGED);
	BIND_ENUM_CONSTANT(KIND_VECTOR2);
	BIND_ENUM_CONSTANT(KIND_VECTOR2_RANGED);
	BIND_ENUM_CONSTANT(KIND_NORMALIZED_VECTOR2);
	BIND_ENUM_CONSTANT(KIND_VECTOR3);
	BIND_ENUM_CONSTANT(KIND_VECTOR3_RANGED);
	BIND_ENUM_CONSTANT(KIND_NORMALIZED_VECTOR3);
	BIND_ENUM_CONSTANT(KIND_QUATERNION);
	BIND_ENUM_CONSTANT(KIND_VARIANT);

	BIND_ENUM_CONSTANT(PRECISION_FULL);
	BIND_ENUM_CONSTANT(PRECISION_SINGLE);
	BIND_ENUM_CONSTANT(PRECISION_HALF);
	BIND_ENUM_CONSTANT(PRECISION_LOW);
}
