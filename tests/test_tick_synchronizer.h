// Tests public class registration and root diagnostic behavior.
// Provides the top-level TickSynchronizer module test group.

#ifndef TEST_TICK_SYNCHRONIZER_H
#define TEST_TICK_SYNCHRONIZER_H

#include "tests/test_macros.h"

#include "../src/public/tick_synchronizer.h"
#include "../src/public/tick_synchronizer_buffer.h"
#include "../src/public/tick_synchronizer_object.h"
#include "../src/public/tick_synchronizer_schema.h"
#include "../src/public/tick_synchronizer_settings.h"

#include "core/io/resource.h"
#include "core/math/math_defs.h"
#include "core/object/class_db.h"
#include "core/object/ref_counted.h"
#include "scene/main/node.h"

#include <type_traits>

namespace TestTickSynchronizer {

TEST_CASE("[Modules][TickSynchronizer] Public classes are registered") {
	CHECK(ClassDB::class_exists("TickSynchronizer"));
	CHECK(ClassDB::class_exists("TickSynchronizerSettings"));
	CHECK(ClassDB::class_exists("TickSynchronizerBuffer"));
	CHECK(ClassDB::class_exists("TickSynchronizerObject"));
	CHECK(ClassDB::class_exists("TickSynchronizerSchema"));
}

TEST_CASE("[Modules][TickSynchronizer] Public inheritance is correct") {
	CHECK((std::is_base_of_v<Node, TickSynchronizer>));
	CHECK((std::is_base_of_v<Resource, TickSynchronizerSettings>));
	CHECK((std::is_base_of_v<RefCounted, TickSynchronizerBuffer>));
	CHECK((std::is_base_of_v<Node, TickSynchronizerObject>));
	CHECK((std::is_base_of_v<Resource, TickSynchronizerSchema>));
}

TEST_CASE("[Modules][TickSynchronizer] Public classes support basic lifecycle") {
	TickSynchronizer *synchronizer = memnew(TickSynchronizer);
	TickSynchronizerObject *sync_object = memnew(TickSynchronizerObject);
	Ref<TickSynchronizerSettings> settings;
	Ref<TickSynchronizerBuffer> buffer;
	Ref<TickSynchronizerSchema> schema;

	settings.instantiate();
	buffer.instantiate();
	schema.instantiate();

	CHECK(synchronizer != nullptr);
	CHECK(sync_object != nullptr);
	CHECK(settings.is_valid());
	CHECK(buffer.is_valid());
	CHECK(schema.is_valid());

	memdelete(sync_object);
	memdelete(synchronizer);
}

TEST_CASE("[Modules][TickSynchronizer] Multiple synchronizers can coexist") {
	TickSynchronizer *first = memnew(TickSynchronizer);
	TickSynchronizer *second = memnew(TickSynchronizer);
	TickSynchronizer *third = memnew(TickSynchronizer);

	CHECK(first != nullptr);
	CHECK(second != nullptr);
	CHECK(third != nullptr);
	CHECK(first != second);
	CHECK(first != third);
	CHECK(second != third);

	memdelete(third);
	memdelete(second);
	memdelete(first);
}

TEST_CASE("[Modules][TickSynchronizer] Repeated construction is stable") {
	for (int index = 0; index < 1000; index++) {
		TickSynchronizer *synchronizer = memnew(TickSynchronizer);
		CHECK(synchronizer != nullptr);
		memdelete(synchronizer);
	}
}

TEST_CASE("[Modules][TickSynchronizer] Build precision is reported correctly") {
	TickSynchronizer *synchronizer = memnew(TickSynchronizer);

#ifdef REAL_T_IS_DOUBLE
	CHECK(sizeof(real_t) == sizeof(double));
	CHECK(synchronizer->get_build_precision() == "double");
	CHECK(synchronizer->is_double_precision());
#else
	CHECK(sizeof(real_t) == sizeof(float));
	CHECK(synchronizer->get_build_precision() == "single");
	CHECK_FALSE(synchronizer->is_double_precision());
#endif

	CHECK(synchronizer->get_protocol_magic() == "TSYN");
	CHECK(synchronizer->get_protocol_major() == 1);
	CHECK(synchronizer->get_protocol_minor() == 1);
#ifdef REAL_T_IS_DOUBLE
	CHECK(synchronizer->get_protocol_precision_mode() == 2);
#else
	CHECK(synchronizer->get_protocol_precision_mode() == 1);
#endif

	memdelete(synchronizer);
}

} // namespace TestTickSynchronizer

#endif // TEST_TICK_SYNCHRONIZER_H
