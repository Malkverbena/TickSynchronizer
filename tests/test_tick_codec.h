#pragma once

#include "../source/codec/tick_codec.h"
#include "../source/sync/tick_sync_object.h"

#include "tests/test_macros.h"

namespace TestTickCodec {

inline Variant round_trip(const Ref<TickCodec> &p_codec, const Variant &p_value) {
	TickDataBuffer buffer;
	buffer.begin_write();
	p_codec->encode(p_value, buffer);
	buffer.begin_read();
	const Variant value = p_codec->decode(buffer);
	CHECK_FALSE(buffer.is_buffer_failed());
	CHECK(buffer.is_end_of_buffer());
	return value;
}

TEST_CASE("[Modules][TickSynchronizer][TickCodec] Round trips and quantization") {
	CHECK(round_trip(TickCodec::boolean(), true) == Variant(true));
	CHECK(round_trip(TickCodec::integer(5), -16) == Variant(-16));
	CHECK(round_trip(TickCodec::integer(5), 100) == Variant(15));
	CHECK(round_trip(TickCodec::real(TickCodec::PRECISION_FULL), 1.0 / 3.0) == Variant(1.0 / 3.0));

	const Ref<TickCodec> ranged = TickCodec::real_ranged(-10.0, 10.0, 8);
	const double ranged_value = round_trip(ranged, 3.3);
	CHECK(Math::abs(ranged_value - 3.3) <= ranged->get_tolerance() + 1e-12);
	// Quantizing a quantized value doesn't change it.
	CHECK(ranged->quantize(ranged_value) == Variant(ranged_value));
	CHECK(double(round_trip(ranged, 50.0)) == 10.0);

	const Ref<TickCodec> v3 = TickCodec::vector3(TickCodec::PRECISION_HALF);
	const Vector3 position(12.345, -0.5, 1000.25);
	const Vector3 quantized = v3->quantize(position);
	CHECK(v3->is_equal(quantized, position));
	CHECK(v3->quantize(quantized) == Variant(quantized));
	CHECK_FALSE(v3->is_equal(quantized, position + Vector3(1, 0, 0)));

	const Ref<TickCodec> v2 = TickCodec::vector2_ranged(-100.0, 100.0, 16);
	const Vector2 v2_value = round_trip(v2, Vector2(33.3, -99.99));
	CHECK(v2->is_equal(v2_value, Vector2(33.3, -99.99)));

	const Ref<TickCodec> direction = TickCodec::normalized_vector3(TickCodec::PRECISION_SINGLE);
	const Vector3 dir = round_trip(direction, Vector3(0, 3, 4));
	CHECK(direction->is_equal(dir, Vector3(0, 0.6, 0.8)));

	const Ref<TickCodec> rotation = TickCodec::quaternion(12);
	const Quaternion q = Quaternion(Vector3(1, 2, 3).normalized(), 1.2);
	const Quaternion q_read = round_trip(rotation, q);
	CHECK(rotation->is_equal(q_read, q));
	// q and -q are the same rotation.
	CHECK(rotation->is_equal(q_read, -q));
	CHECK(q_read.is_normalized());

	const Ref<TickCodec> any = TickCodec::variant();
	CHECK(round_trip(any, String("hello")) == Variant(String("hello")));
	CHECK(round_trip(any, Variant()) == Variant());
}

TEST_CASE("[Modules][TickSynchronizer][TickCodec] Wrong types keep the layout") {
	const Ref<TickCodec> v3 = TickCodec::vector3(TickCodec::PRECISION_SINGLE);
	TickDataBuffer buffer;
	buffer.begin_write();
	ERR_PRINT_OFF;
	v3->encode(String("not a vector"), buffer);
	ERR_PRINT_ON;
	buffer.add_bool(true);
	buffer.begin_read();
	CHECK(v3->decode(buffer) == Variant(Vector3()));
	CHECK(buffer.read_bool());
	CHECK_FALSE(buffer.is_buffer_failed());
}

TEST_CASE("[Modules][TickSynchronizer][TickCodec] Interpolation and schema hash") {
	const Ref<TickCodec> real = TickCodec::real(TickCodec::PRECISION_SINGLE);
	CHECK(double(real->interpolate(0.0, 10.0, 0.25)) == doctest::Approx(2.5));
	CHECK(TickCodec::boolean()->interpolate(false, true, 0.9) == Variant(false));
	const Vector3 mid = TickCodec::normalized_vector3()->interpolate(Vector3(1, 0, 0), Vector3(0, 1, 0), 0.5);
	CHECK(mid.is_normalized());

	TickSchema a;
	a.add("position", TickCodec::vector3(TickCodec::PRECISION_HALF));
	a.add("velocity", TickCodec::vector3(TickCodec::PRECISION_HALF));
	TickSchema b;
	b.add("position", TickCodec::vector3(TickCodec::PRECISION_HALF));
	b.add("velocity", TickCodec::vector3(TickCodec::PRECISION_HALF));
	CHECK(a.hash() == b.hash());
	TickSchema c;
	c.add("position", TickCodec::vector3(TickCodec::PRECISION_SINGLE));
	c.add("velocity", TickCodec::vector3(TickCodec::PRECISION_HALF));
	CHECK(a.hash() != c.hash());
	TickSchema d;
	d.add("velocity", TickCodec::vector3(TickCodec::PRECISION_HALF));
	d.add("position", TickCodec::vector3(TickCodec::PRECISION_HALF));
	CHECK(a.hash() != d.hash());
}

} // namespace TestTickCodec
