#pragma once

#include "../common/tick_data_buffer.h"

#include "core/object/class_db.h"
#include "core/object/ref_counted.h"
#include "core/variant/variant.h"

// Network encoding of one synchronized variable: how it's written in a `TickDataBuffer`, how much it's quantized,
// how two values are compared and how they are interpolated.
//
// The authority and the predicting peers quantize their state with the same codec every tick
// (`quantize()`), so identical simulations produce identical values and never trigger a rewind. Two values are
// equal when they differ by no more than `get_tolerance()`, which defaults to half of the quantization step.
class TickCodec : public RefCounted {
	GDCLASS(TickCodec, RefCounted);

public:
	enum Kind {
		KIND_BOOL,
		KIND_INT,
		KIND_REAL,
		KIND_REAL_RANGED,
		KIND_VECTOR2,
		KIND_VECTOR2_RANGED,
		KIND_NORMALIZED_VECTOR2,
		KIND_VECTOR3,
		KIND_VECTOR3_RANGED,
		KIND_NORMALIZED_VECTOR3,
		KIND_QUATERNION,
		KIND_VARIANT,
	};

	// Precision of the floating point based kinds (reals, vectors, normalized vectors).
	enum Precision {
		PRECISION_FULL, // binary64; for normalized vectors, the finest level.
		PRECISION_SINGLE, // binary32.
		PRECISION_HALF, // binary16.
		PRECISION_LOW, // binary16 for reals; the coarsest level for normalized vectors.
	};

	// Maximum size, in bytes, of a value encoded by `variant()`.
	static constexpr int MAX_VARIANT_BYTES = UINT16_MAX;

private:
	Kind kind = KIND_VARIANT;
	Precision precision = PRECISION_FULL;
	int bits = 0;
	double range_min = 0.0;
	double range_max = 0.0;
	double tolerance = 0.0;
	bool tolerance_overridden = false;

	double compute_default_tolerance() const;
	Variant get_default_value() const;
	bool check_type(const Variant &p_value) const;

	static uint64_t quantize_ranged(double p_value, double p_min, double p_max, int p_bits);
	static double dequantize_ranged(uint64_t p_value, double p_min, double p_max, int p_bits);
	static TickDataBuffer::CompressionLevel get_compression_level(Precision p_precision);

protected:
	static void _bind_methods();

public:
	// Constructors, also exposed as static methods.
	static Ref<TickCodec> boolean();
	static Ref<TickCodec> integer(int p_bits = 32);
	static Ref<TickCodec> real(Precision p_precision = PRECISION_FULL);
	static Ref<TickCodec> real_ranged(double p_min, double p_max, int p_bits);
	static Ref<TickCodec> vector2(Precision p_precision = PRECISION_FULL);
	static Ref<TickCodec> vector2_ranged(double p_min, double p_max, int p_bits);
	static Ref<TickCodec> normalized_vector2(Precision p_precision = PRECISION_FULL);
	static Ref<TickCodec> vector3(Precision p_precision = PRECISION_FULL);
	static Ref<TickCodec> vector3_ranged(double p_min, double p_max, int p_bits);
	static Ref<TickCodec> normalized_vector3(Precision p_precision = PRECISION_FULL);
	static Ref<TickCodec> quaternion(int p_bits_per_component = 10);
	static Ref<TickCodec> variant();

	Kind get_kind() const { return kind; }
	Precision get_precision() const { return precision; }
	int get_bits() const { return bits; }
	double get_range_min() const { return range_min; }
	double get_range_max() const { return range_max; }

	// Maximum difference for two values to be considered equal. For reals and vectors it's absolute for the
	// ranged kinds and relative to the magnitude for the others.
	void set_tolerance(double p_tolerance);
	double get_tolerance() const;

	// Writes `p_value`. A value of the wrong type is an error and writes the default value, so the buffer keeps
	// its layout.
	void encode(const Variant &p_value, TickDataBuffer &r_buffer) const;
	// Reads a value; on failure `r_buffer.is_buffer_failed()` is set and the default value is returned. Decoding
	// never prints errors: the data may come from an untrusted peer.
	Variant decode(TickDataBuffer &r_buffer) const;

	// Returns the value as the readers receive it.
	Variant quantize(const Variant &p_value) const;
	bool is_equal(const Variant &p_a, const Variant &p_b) const;
	Variant interpolate(const Variant &p_from, const Variant &p_to, double p_weight) const;

	// Identifies the encoding, to verify that all the peers use the same schema.
	uint32_t hash(uint32_t p_seed) const;

	// Expected `Variant` type, or `Variant::NIL` for `variant()`.
	Variant::Type get_value_type() const;
};

VARIANT_ENUM_CAST(TickCodec::Kind);
VARIANT_ENUM_CAST(TickCodec::Precision);
