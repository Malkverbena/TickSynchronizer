// Implements the root TickSynchronizer diagnostic node and Godot bindings.
// Exposes immutable build and control-protocol information to scripts.

#include "tick_synchronizer.h"

#include "core/object/class_db.h"
#include "src/protocol/tick_synchronizer_packet_codec.h"

void TickSynchronizer::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_build_precision"), &TickSynchronizer::get_build_precision);
	ClassDB::bind_method(D_METHOD("is_double_precision"), &TickSynchronizer::is_double_precision);
	ClassDB::bind_method(D_METHOD("get_protocol_magic"), &TickSynchronizer::get_protocol_magic);
	ClassDB::bind_method(D_METHOD("get_protocol_major"), &TickSynchronizer::get_protocol_major);
	ClassDB::bind_method(D_METHOD("get_protocol_minor"), &TickSynchronizer::get_protocol_minor);
	ClassDB::bind_method(D_METHOD("get_protocol_precision_mode"), &TickSynchronizer::get_protocol_precision_mode);
}


String TickSynchronizer::get_build_precision() const {
#ifdef REAL_T_IS_DOUBLE
	return "double";
#else
	return "single";
#endif
}


bool TickSynchronizer::is_double_precision() const {
#ifdef REAL_T_IS_DOUBLE
	return true;
#else
	return false;
#endif
}


String TickSynchronizer::get_protocol_magic() const {
	return tick_synchronizer::TickSynchronizerPacketCodec::get_magic_string();
}


int32_t TickSynchronizer::get_protocol_major() const {
	return tick_synchronizer::TickSynchronizerPacketCodec::PROTOCOL_MAJOR;
}


int32_t TickSynchronizer::get_protocol_minor() const {
	return tick_synchronizer::TickSynchronizerPacketCodec::PROTOCOL_MINOR;
}


int32_t TickSynchronizer::get_protocol_precision_mode() const {
	return static_cast<int32_t>(tick_synchronizer::TickSynchronizerPacketCodec::get_build_precision_mode());
}
