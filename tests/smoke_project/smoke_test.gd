extends Node

const REQUIRED_CLASSES = [
    &"TickSynchronizer",
    &"TickSynchronizerSettings",
    &"TickSynchronizerBuffer",
    &"TickSynchronizerObject",
    &"TickSynchronizerSchema",
]


func _ready() -> void:
    var failures := PackedStringArray()
    var retained_references: Array = []

    for registered_class in REQUIRED_CLASSES:
        if not ClassDB.class_exists(registered_class):
            failures.append("Class not registered: %s" % registered_class)
            continue

        # Direct construction is the effective validation. can_instantiate() is not
        # required for this smoke test and may be more restrictive than the
        # actual ClassDB.instantiate() path for native classes.
        var instance = ClassDB.instantiate(registered_class)
        if instance == null:
            failures.append("ClassDB.instantiate returned null: %s" % registered_class)
            continue

        if instance is Node:
            instance.free()
        else:
            # Keeps Resources/RefCounted instances alive until validation completes.
            retained_references.append(instance)

    var expected_precision := OS.get_environment("TICKSYNC_EXPECTED_PRECISION")
    var synchronizer = ClassDB.instantiate(&"TickSynchronizer")

    if synchronizer == null:
        failures.append("Could not instantiate TickSynchronizer to validate precision.")
    else:
        var actual_precision: String = synchronizer.get_build_precision()
        print("TICKSYNCHRONIZER_BUILD_PRECISION=%s" % actual_precision)

        if expected_precision != "" and actual_precision != expected_precision:
            failures.append(
                "Binary precision does not match the run: expected=%s, detected=%s"
                % [expected_precision, actual_precision]
            )

        var expected_precision_mode := 2 if actual_precision == "double" else 1
        if synchronizer.get_protocol_magic() != "TSYN":
            failures.append("Protocol magic differs from TSYN.")
        elif synchronizer.get_protocol_major() != 1 or synchronizer.get_protocol_minor() != 1:
            failures.append(
                "Unexpected protocol version: %d.%d"
                % [synchronizer.get_protocol_major(), synchronizer.get_protocol_minor()]
            )
        elif synchronizer.get_protocol_precision_mode() != expected_precision_mode:
            failures.append("Protocol precision mode differs from the build.")
        else:
            print("TICKSYNCHRONIZER_PROTOCOL_SMOKE_TEST_OK")

        synchronizer.free()

    var bit_buffer = ClassDB.instantiate(&"TickSynchronizerBuffer")
    if bit_buffer == null:
        failures.append("Could not instantiate TickSynchronizerBuffer to validate the bitstream.")
    else:
        if bit_buffer.begin_write(2) != OK:
            failures.append("TickSynchronizerBuffer.begin_write failed.")
        elif bit_buffer.write_bits(0b101, 3) != OK:
            failures.append("TickSynchronizerBuffer.write_bits failed on the first field.")
        elif bit_buffer.write_bits(0b11011, 5) != OK:
            failures.append("TickSynchronizerBuffer.write_bits failed on the second field.")
        else:
            var encoded: PackedByteArray = bit_buffer.get_data()
            if encoded.size() != 1 or encoded[0] != 0xDD:
                failures.append("Incorrect bitstream golden vector: expected DD, got %s" % encoded.hex_encode())
            elif bit_buffer.begin_read(encoded, 8) != OK:
                failures.append("TickSynchronizerBuffer.begin_read failed.")
            else:
                var first_value: int = bit_buffer.read_bits(3)
                var second_value: int = bit_buffer.read_bits(5)
                if bit_buffer.has_error():
                    failures.append(
                        "TickSynchronizerBuffer.read_bits recorded an unexpected error: %d"
                        % bit_buffer.get_last_error()
                    )
                elif first_value != 0b101 or second_value != 0b11011:
                    failures.append("The bitstream round-trip returned incorrect values.")
                else:
                    print("TICKSYNCHRONIZER_BUFFER_SMOKE_TEST_OK")

        if not bit_buffer.has_error():
            if bit_buffer.begin_write(8) != OK:
                failures.append("TickSynchronizerBuffer.begin_write failed in the codec smoke test.")
            elif bit_buffer.write_u16(0x1234) != OK:
                failures.append("TickSynchronizerBuffer.write_u16 failed.")
            elif bit_buffer.write_varuint(300) != OK:
                failures.append("TickSynchronizerBuffer.write_varuint failed.")
            elif bit_buffer.write_varint(-1000) != OK:
                failures.append("TickSynchronizerBuffer.write_varint failed.")
            else:
                var codec_data: PackedByteArray = bit_buffer.get_data()
                var expected_codec_data := PackedByteArray([0x34, 0x12, 0xAC, 0x02, 0xCF, 0x0F])
                if codec_data != expected_codec_data:
                    failures.append(
                        "Incorrect codec golden vector: expected %s, got %s"
                        % [expected_codec_data.hex_encode(), codec_data.hex_encode()]
                    )
                elif bit_buffer.begin_read(codec_data) != OK:
                    failures.append("TickSynchronizerBuffer.begin_read failed in the codec smoke test.")
                else:
                    var fixed_value: int = bit_buffer.read_u16()
                    var unsigned_value: int = bit_buffer.read_varuint()
                    var signed_value: int = bit_buffer.read_varint()
                    if bit_buffer.has_error():
                        failures.append(
                            "The codecs recorded an unexpected error: %d"
                            % bit_buffer.get_last_error()
                        )
                    elif fixed_value != 0x1234 or unsigned_value != 300 or signed_value != -1000:
                        failures.append("The codec round-trip returned incorrect values.")
                    else:
                        print("TICKSYNCHRONIZER_INTEGER_CODEC_SMOKE_TEST_OK")

        if not bit_buffer.has_error():
            if bit_buffer.begin_write(16) != OK:
                failures.append("TickSynchronizerBuffer.begin_write failed in the float smoke test.")
            elif bit_buffer.write_float32(1.0) != OK:
                failures.append("TickSynchronizerBuffer.write_float32 failed.")
            elif bit_buffer.write_float64(-2.5) != OK:
                failures.append("TickSynchronizerBuffer.write_float64 failed.")
            else:
                var float_data: PackedByteArray = bit_buffer.get_data()
                var expected_float_data := PackedByteArray([
                    0x00, 0x00, 0x80, 0x3F,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0xC0,
                ])
                if float_data != expected_float_data:
                    failures.append(
                        "Incorrect float golden vector: expected %s, got %s"
                        % [expected_float_data.hex_encode(), float_data.hex_encode()]
                    )
                elif bit_buffer.begin_read(float_data) != OK:
                    failures.append("TickSynchronizerBuffer.begin_read failed in the float smoke test.")
                else:
                    var float32_value: float = bit_buffer.read_float32()
                    var float64_value: float = bit_buffer.read_float64()
                    if bit_buffer.has_error():
                        failures.append(
                            "The float codecs recorded an unexpected error: %d"
                            % bit_buffer.get_last_error()
                        )
                    elif float32_value != 1.0 or float64_value != -2.5:
                        failures.append("The float codec round-trip returned incorrect values.")
                    else:
                        print("TICKSYNCHRONIZER_FLOAT_CODEC_SMOKE_TEST_OK")

        if not bit_buffer.has_error():
            bit_buffer.clear()
            if bit_buffer.set_max_size_bytes(2) != OK:
                failures.append("TickSynchronizerBuffer.set_max_size_bytes failed.")
            elif bit_buffer.begin_write() != OK:
                failures.append("TickSynchronizerBuffer.begin_write failed in the limit test.")
            elif bit_buffer.write_u16(0xBEEF) != OK:
                failures.append("TickSynchronizerBuffer did not write up to the configured limit.")
            elif bit_buffer.write_bits(1, 1) != ERR_PARAMETER_RANGE_ERROR:
                failures.append("TickSynchronizerBuffer did not reject a write above the limit.")
            elif bit_buffer.get_bit_size() != 16 or bit_buffer.get_data() != PackedByteArray([0xEF, 0xBE]):
                failures.append("A failed over-limit write changed the buffer contents.")
            else:
                var limited_data: PackedByteArray = bit_buffer.get_data()
                var limited_hash: int = bit_buffer.get_content_hash()
                bit_buffer.clear()
                if bit_buffer.set_max_size_bytes(4) != OK:
                    failures.append("TickSynchronizerBuffer did not update the limit after clear.")
                elif bit_buffer.begin_read(limited_data, 16) != OK:
                    failures.append("TickSynchronizerBuffer.begin_read failed in the equality test.")
                elif bit_buffer.get_content_hash() != limited_hash:
                    failures.append("Logical hash differed after the round-trip.")
                else:
                    print("TICKSYNCHRONIZER_RESOURCE_LIMIT_SMOKE_TEST_OK")

    retained_references.clear()

    if failures.is_empty():
        print("TICKSYNCHRONIZER_SMOKE_TEST_OK")
        get_tree().quit(0)
        return

    for failure in failures:
        push_error(failure)

    print("TICKSYNCHRONIZER_SMOKE_TEST_FAILED")
    get_tree().quit(1)
