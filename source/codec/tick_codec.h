// The network encoding of one synchronized variable: `TickCodec`.
//
// A codec says how a value is written in a `TickDataBuffer`, how much it's quantized, how two values are compared and
// how they are interpolated. Codecs are made by the static constructors (`boolean()`, `integer()`, `real()`, ...,
// `variant()`), which scripts call too.
//
// The authority and the predicting peers quantize their state with the same codec every tick (`quantize()`), so
// identical simulations produce identical values and never trigger a rewind. Two values are equal when they differ by
// no more than `get_tolerance()`, which defaults to half of the quantization step.

#pragma once

#include "../common/tick_data_buffer.h"

#include "core/object/class_db.h"
#include "core/object/ref_counted.h"
#include "core/variant/variant.h"

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

	// The tolerance of the encoding when none was set: half of its quantization step, and 0 for the kinds that aren't
	// quantized.
	double compute_default_tolerance() const;


	// The zero of the kind: written in place of a value of the wrong type, and returned when a read fails.
	Variant get_default_value() const;


	// Whether `p_value` has the type the codec writes; numbers and booleans convert between each other.
	bool check_type(const Variant &p_value) const;


	// Maps a value in [`p_min`, `p_max`] to an integer of `p_bits` bits. Values outside the range are clamped, and NaN
	// goes to the minimum.
	static uint64_t quantize_ranged(double p_value, double p_min, double p_max, int p_bits);


	// Maps an integer of `p_bits` bits back to [`p_min`, `p_max`].
	static double dequantize_ranged(uint64_t p_value, double p_min, double p_max, int p_bits);


	// The compression level of `TickDataBuffer` that matches a precision.
	static TickDataBuffer::CompressionLevel get_compression_level(Precision p_precision);


protected:
	// Exposes the class to scripts.
	static void _bind_methods();


public:
	// A codec for booleans: 1 bit.
	static Ref<TickCodec> boolean();


	// A codec for signed integers of `p_bits` bits (1 to 64); values beyond that size are clamped.
	static Ref<TickCodec> integer(int p_bits = 32);


	// A codec for reals, sent as binary64, binary32 or binary16 by the precision.
	static Ref<TickCodec> real(Precision p_precision = PRECISION_FULL);


	// A codec for reals between `p_min` and `p_max`, quantized in `p_bits` bits (1 to 32); values outside the range are
	// clamped.
	static Ref<TickCodec> real_ranged(double p_min, double p_max, int p_bits);


	// A codec for 2D vectors, each component as in `real()`.
	static Ref<TickCodec> vector2(Precision p_precision = PRECISION_FULL);


	// A codec for 2D vectors, each component as in `real_ranged()`.
	static Ref<TickCodec> vector2_ranged(double p_min, double p_max, int p_bits);


	// A codec for 2D directions (or zero), sent as an angle.
	static Ref<TickCodec> normalized_vector2(Precision p_precision = PRECISION_FULL);


	// A codec for 3D vectors, each component as in `real()`.
	static Ref<TickCodec> vector3(Precision p_precision = PRECISION_FULL);


	// A codec for 3D vectors, each component as in `real_ranged()`.
	static Ref<TickCodec> vector3_ranged(double p_min, double p_max, int p_bits);


	// A codec for 3D directions (or zero), each component quantized in the [-1, 1] range.
	static Ref<TickCodec> normalized_vector3(Precision p_precision = PRECISION_FULL);


	// A codec for rotations: the index of the largest component and the other three, in `p_bits_per_component` bits
	// each (2 to 32).
	static Ref<TickCodec> quaternion(int p_bits_per_component = 10);


	// A codec for any value the engine can serialize, of at most `MAX_VARIANT_BYTES` bytes. Nothing is quantized, and
	// it's the most expensive one.
	static Ref<TickCodec> variant();


	// What the codec encodes.
	Kind get_kind() const { return kind; }


	// The precision of the floating point based kinds.
	Precision get_precision() const { return precision; }


	// The bits of an integer or of each quantized component (ranged kinds, quaternions); 0 for the kinds sized by their
	// precision and for variants.
	int get_bits() const { return bits; }


	// The lowest value of a ranged kind.
	double get_range_min() const { return range_min; }


	// The highest value of a ranged kind.
	double get_range_max() const { return range_max; }


	// Maximum difference for two values to be considered equal. For reals and vectors it's absolute for the
	// ranged kinds and relative to the magnitude for the others.
	void set_tolerance(double p_tolerance);


	// The tolerance that was set, or the default of the encoding: half of its quantization step.
	double get_tolerance() const;


	// Writes `p_value`. A value of the wrong type is an error and writes the default value, so the buffer keeps
	// its layout.
	void encode(const Variant &p_value, TickDataBuffer &r_buffer) const;


	// Reads a value; on failure `r_buffer.is_buffer_failed()` is set and the default value is returned. Decoding
	// never prints errors: the data may come from an untrusted peer.
	Variant decode(TickDataBuffer &r_buffer) const;


	// Returns the value as the readers receive it.
	Variant quantize(const Variant &p_value) const;


	// Whether two values are the same for this codec: they differ by no more than the tolerance. For rotations, `q` and
	// `-q` are the same.
	bool is_equal(const Variant &p_a, const Variant &p_b) const;


	// The value between `p_from` and `p_to` at `p_weight` (0 to 1). Directions stay normalized and rotations take the
	// shortest arc; booleans, integers and variants keep `p_from` until the weight reaches 1.
	Variant interpolate(const Variant &p_from, const Variant &p_to, double p_weight) const;


	// Identifies the encoding, to verify that all the peers use the same schema.
	uint32_t hash(uint32_t p_seed) const;


	// Whether `p_value` can be sent to another peer: no objects (null ones are fine), callables, signals or RIDs,
	// which only mean something in this process, and no real that isn't finite (NaN, an infinity), which no codec
	// carries: it would spread through a simulation. Also inside arrays and dictionaries. A value received from
	// another peer is checked the same way.
	static bool is_sendable(const Variant &p_value);


	// Expected `Variant` type, or `Variant::NIL` for `variant()`.
	Variant::Type get_value_type() const;
};

VARIANT_ENUM_CAST(TickCodec::Kind);
VARIANT_ENUM_CAST(TickCodec::Precision);
