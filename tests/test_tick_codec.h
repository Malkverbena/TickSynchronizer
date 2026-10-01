#pragma once

#include "../source/codec/tick_codec.h"
#include "../source/sync/tick_sync_object.h"

#include "core/io/marshalls.h"
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

TEST_CASE("[Modules][TickSynchronizer][TickCodec] Variants: complex values round trip, malformed ones fail the buffer") {
	const Ref<TickCodec> codec = TickCodec::variant();
	Dictionary value;
	value["name"] = String::utf8("João 🎮");
	Array list;
	list.push_back(1);
	list.push_back(2.5);
	list.push_back(Vector3(1, 2, 3));
	list.push_back(String("x"));
	value[StringName("list")] = list;
	PackedByteArray bytes;
	bytes.resize(5);
	bytes.fill(7);
	value["bytes"] = bytes;
	PackedStringArray strings;
	strings.push_back("a");
	strings.push_back(String::utf8("ü"));
	value["strings"] = strings;
	value["path"] = NodePath("a/b:c");
	value["transform"] = Transform3D(Basis(Vector3(0, 1, 0), 0.5), Vector3(1, 2, 3));
	CHECK(round_trip(codec, value) == Variant(value));

	// A string variant whose bytes aren't UTF-8 (type 4, length 3, padding 1).
	const uint8_t bad_string[12] = { 4, 0, 0, 0, 3, 0, 0, 0, 0xFF, 0xFE, 0xFD, 0 };
	// A nil variant followed by 4 bytes that belong to no value.
	const uint8_t trailing[8] = { 0, 0, 0, 0, 1, 2, 3, 4 };
	const uint8_t *malformed[2] = { bad_string, trailing };
	const int sizes[2] = { 12, 8 };
	for (int i = 0; i < 2; i++) {
		TickDataBuffer buffer;
		buffer.begin_write();
		buffer.add_uint_bits(uint64_t(sizes[i]), 16);
		buffer.add_bits(malformed[i], sizes[i] * 8);
		buffer.begin_read();
		// The decoder's errors are silenced while it runs, and the setting is restored afterwards.
		CHECK(codec->decode(buffer) == Variant());
		CHECK(buffer.is_buffer_failed());
		CHECK(CoreGlobals::print_error_enabled);
	}
	ERR_PRINT_OFF;
	TickDataBuffer buffer;
	buffer.begin_write();
	buffer.add_uint_bits(12, 16);
	buffer.add_bits(bad_string, 96);
	buffer.begin_read();
	codec->decode(buffer);
	CHECK_FALSE(CoreGlobals::print_error_enabled);
	ERR_PRINT_ON;
}

TEST_CASE("[Modules][TickSynchronizer][TickCodec] Only values that mean something to another peer are sent") {
	Ref<RefCounted> object;
	object.instantiate();
	Array with_object;
	with_object.push_back(1);
	with_object.push_back(object);
	Dictionary keyed_by_object;
	keyed_by_object[object] = 1;
	CHECK(TickCodec::is_sendable(Variant()));
	CHECK(TickCodec::is_sendable(Variant((Object *)nullptr)));
	CHECK(TickCodec::is_sendable(Vector3(1, 2, 3)));
	CHECK_FALSE(TickCodec::is_sendable(object));
	CHECK_FALSE(TickCodec::is_sendable(with_object));
	CHECK_FALSE(TickCodec::is_sendable(keyed_by_object));
	CHECK_FALSE(TickCodec::is_sendable(Callable(object.ptr(), "get_class")));
	CHECK_FALSE(TickCodec::is_sendable(RID()));

	// The writer sends nil instead.
	const Ref<TickCodec> codec = TickCodec::variant();
	ERR_PRINT_OFF;
	CHECK(round_trip(codec, with_object) == Variant());
	ERR_PRINT_ON;

	// An object written as its id by a modified peer fails the reader.
	int length = 0;
	REQUIRE(encode_variant(object, nullptr, length, false) == OK);
	LocalVector<uint8_t> bytes;
	bytes.resize(length);
	encode_variant(object, bytes.ptr(), length, false);
	TickDataBuffer buffer;
	buffer.begin_write();
	buffer.add_uint_bits(uint64_t(length), 16);
	buffer.add_bits(bytes.ptr(), length * 8);
	buffer.begin_read();
	CHECK(codec->decode(buffer) == Variant());
	CHECK(buffer.is_buffer_failed());

	// NaN in a ranged codec is quantized to the minimum, not converted to an integer.
	CHECK(double(TickCodec::real_ranged(-10.0, 10.0, 8)->quantize(Math::NaN)) == -10.0);
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
