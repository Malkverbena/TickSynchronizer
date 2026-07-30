#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MODULE_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"

fail() {
    printf 'ERRO: %s\n' "$*" >&2
    exit 1
}

command -v python3 >/dev/null 2>&1 || fail "python3 não encontrado"

python3 - "$MODULE_DIR" <<'PY'
from __future__ import annotations

import pathlib
import re
import sys
import xml.etree.ElementTree as ET

module = pathlib.Path(sys.argv[1])
header_path = module / "src/public/tick_synchronizer_buffer.h"
source_path = module / "src/public/tick_synchronizer_buffer.cpp"
xml_path = module / "doc_classes/TickSynchronizerBuffer.xml"
smoke_path = module / "tests/smoke_project/smoke_test.gd"
build_script_path = module / "scripts/build_and_validate.sh"
sanitizer_script_path = module / "scripts/run_sanitized_tests.sh"
ubsan_suppression_path = module / "scripts/sanitizer_suppressions/godot-4.7.1-ubsan.supp"
scsub_path = module / "SCsub"
source_readme_path = module / "src/README.md"
mermaid_script_path = module / "scripts/verify_mermaid_diagrams.py"
vscode_settings_path = module / ".vscode/settings.json"
vscode_extensions_path = module / ".vscode/extensions.json"
markdownlint_path = module / ".markdownlint.json"
mermaid_doc_path = module / "documentation/MERMAID.md"
source_layout_adr_path = module / "documentation/adr/0025-source-code-under-src.md"
mermaid_adr_path = module / "documentation/adr/0026-mermaid-documentation-and-vscode-preview.md"

for path in (
    header_path, source_path, xml_path, smoke_path, build_script_path,
    sanitizer_script_path, ubsan_suppression_path, scsub_path,
    source_readme_path, mermaid_script_path, vscode_settings_path,
    vscode_extensions_path, markdownlint_path, mermaid_doc_path,
    source_layout_adr_path, mermaid_adr_path,
):
    if not path.is_file():
        raise SystemExit(f"arquivo obrigatório ausente: {path.relative_to(module)}")

header = header_path.read_text(encoding="utf-8")
source = source_path.read_text(encoding="utf-8")
smoke = smoke_path.read_text(encoding="utf-8")
build_script = build_script_path.read_text(encoding="utf-8")
sanitizer_script = sanitizer_script_path.read_text(encoding="utf-8")
ubsan_suppression = ubsan_suppression_path.read_text(encoding="utf-8")
scsub = scsub_path.read_text(encoding="utf-8")
xml_root = ET.parse(xml_path).getroot()

documented = {method.attrib["name"] for method in xml_root.findall("./methods/method")}
bound = set(re.findall(r'D_METHOD\("([A-Za-z_][A-Za-z0-9_]*)"', source))
declared = set(re.findall(r'\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{}]*\)\s*(?:const\s*)?;', header))

missing_bindings = sorted(documented - bound)
undocumented_bindings = sorted(bound - documented)
missing_declarations = sorted(documented - declared)

errors: list[str] = []
if missing_bindings:
    errors.append("métodos documentados sem binding: " + ", ".join(missing_bindings))
if undocumented_bindings:
    errors.append("bindings sem documentação XML: " + ", ".join(undocumented_bindings))
if missing_declarations:
    errors.append("métodos documentados ausentes no header: " + ", ".join(missing_declarations))

required_api = {
    "write_bits", "read_bits",
    "align_write_to_byte", "align_read_to_byte",
    "write_u8", "write_u16", "write_u32", "write_u64",
    "read_u8", "read_u16", "read_u32", "read_u64",
    "write_varuint", "read_varuint", "write_varint", "read_varint",
    "write_float32", "read_float32", "write_float64", "read_float64",
    "set_max_size_bytes", "get_max_size_bytes", "get_max_size_bits",
    "get_remaining_write_bits", "can_write_bits",
    "is_equal_to", "get_content_hash",
}
missing_required = sorted(required_api - declared)
if missing_required:
    errors.append("API obrigatória ausente no header: " + ", ".join(missing_required))

required_smoke_markers = {
    "TICKSYNCHRONIZER_BUFFER_SMOKE_TEST_OK",
    "TICKSYNCHRONIZER_INTEGER_CODEC_SMOKE_TEST_OK",
    "TICKSYNCHRONIZER_FLOAT_CODEC_SMOKE_TEST_OK",
    "TICKSYNCHRONIZER_RESOURCE_LIMIT_SMOKE_TEST_OK",
    "TICKSYNCHRONIZER_PROTOCOL_SMOKE_TEST_OK",
    "TICKSYNCHRONIZER_SMOKE_TEST_OK",
}
missing_markers = sorted(marker for marker in required_smoke_markers if marker not in smoke)
if missing_markers:
    errors.append("marcadores ausentes no smoke test: " + ", ".join(missing_markers))

test_cases = 0
for test_path in sorted((module / "tests").glob("test_tick_synchronizer*.h")):
    test_cases += test_path.read_text(encoding="utf-8").count("TEST_CASE(")
if test_cases < 98:
    errors.append(f"somente {test_cases} TEST_CASE encontrados; mínimo esperado: 98")

for constant in ("DEFAULT_MAX_SIZE_BYTES", "MAX_CONFIGURABLE_SIZE_BYTES"):
    if constant not in header or constant not in source:
        errors.append(f"constante obrigatória ausente ou não vinculada: {constant}")

build_api_match = re.search(r'readonly SCRIPT_API_VERSION="([0-9]+)"', build_script)
wrapper_api_match = re.search(r'readonly EXPECTED_BUILD_SCRIPT_API="([0-9]+)"', sanitizer_script)
if not build_api_match or not wrapper_api_match:
    errors.append("contrato de API dos scripts não encontrado")
elif build_api_match.group(1) != wrapper_api_match.group(1):
    errors.append(
        "scripts incompatíveis: build API "
        + build_api_match.group(1)
        + " != wrapper API "
        + wrapper_api_match.group(1)
    )

if "Skipped for sanitized editor" not in build_script:
    errors.append("validação estrutural de artefato sanitizado ausente")

if 'ASAN_OPTIONS+=":detect_invalid_pointer_pairs=0"' not in sanitizer_script:
    errors.append("perfil ASAN não desabilita por padrão a checagem engine-level de invalid pointer pairs")
if '--invalid-pointer-pairs' not in sanitizer_script:
    errors.append("opção diagnóstica --invalid-pointer-pairs ausente")

if 'nonnull-attribute:core/string/ustring.cpp' not in ubsan_suppression:
    errors.append("supressão UBSAN estrita do setup do Godot 4.7.1 ausente")
if 'suppressions=${UBSAN_SUPPRESSION_FILE}' not in sanitizer_script:
    errors.append("wrapper não aplica o arquivo de supressões UBSAN")
if '--no-godot-ubsan-suppressions' not in sanitizer_script:
    errors.append("opção diagnóstica --no-godot-ubsan-suppressions ausente")


protocol_header_path = module / "src/protocol/tick_synchronizer_packet_codec.h"
protocol_source_path = module / "src/protocol/tick_synchronizer_packet_codec.cpp"
protocol_test_path = module / "tests/test_tick_synchronizer_packet_codec.h"
handshake_header_path = module / "src/protocol/tick_synchronizer_handshake.h"
handshake_source_path = module / "src/protocol/tick_synchronizer_handshake.cpp"
handshake_test_path = module / "tests/test_tick_synchronizer_handshake.h"
protocol_golden_path = module / "tests/golden/control_hello_v2.bin"
module_id_script_path = module / "scripts/compute_module_build_id.py"
for path in (
    protocol_header_path, protocol_source_path, protocol_test_path, protocol_golden_path,
    handshake_header_path, handshake_source_path, handshake_test_path, module_id_script_path,
):
    if not path.is_file():
        errors.append(f"arquivo de protocolo obrigatório ausente: {path.relative_to(module)}")

if protocol_header_path.is_file():
    protocol_header = protocol_header_path.read_text(encoding="utf-8")
    for token in (
        "PROTOCOL_MAGIC", "PROTOCOL_MAJOR", "PROTOCOL_MINOR",
        "CONTROL_HEADER_SIZE", "ProtocolPacketType", "ProtocolCodecError",
        "ProtocolCompatibilityProfile", "CURRENT_SUPPORTED_CAPABILITIES",
        "encode_packet", "decode_packet", "inspect_control_header",
        "encode_hello_payload", "decode_hello_payload",
    ):
        if token not in protocol_header:
            errors.append(f"contrato do protocolo ausente: {token}")

expected_hello_golden = bytes.fromhex(
    "54 53 59 4E 01 01 01 28 00 00 00 00 "
    "EF CD AB 89 67 45 23 01 40 30 20 10 "
    "08 07 06 05 04 03 02 01 64 00 00 00 "
    "20 03 00 00 02 02 00 00 11 22 33 44 "
    "55 66 77 88 01 02 03 04 05 06 07 08 "
    "09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 "
    "21 22 23 24 25 26 27 28 29 2A 2B 2C "
    "2D 2E 2F 30 31 32 33 34 41 42 43 44 "
    "45 46 47 48 49 4A 4B 4C 4D 4E 4F 50 "
    "61 62 63 64 65 66 67 68 69 6A 6B 6C "
    "6D 6E 6F 70 07 00 00 00 00 00 00 00 "
    "03 00 00 00 00 00 00 00"
)
if protocol_golden_path.is_file():
    golden_bytes = protocol_golden_path.read_bytes()
    if golden_bytes != expected_hello_golden:
        errors.append("golden control_hello_v2.bin diverge do wire format 1.1")

if protocol_test_path.is_file():
    protocol_test_cases = protocol_test_path.read_text(encoding="utf-8").count("TEST_CASE(")
    if protocol_test_cases < 28:
        errors.append(
            f"somente {protocol_test_cases} testes de protocolo; mínimo esperado: 28"
        )

if handshake_header_path.is_file():
    handshake_header = handshake_header_path.read_text(encoding="utf-8")
    for token in (
        "ProtocolHandshakeEvaluator", "ProtocolHandshakeResult",
        "evaluate_profiles", "evaluate_hello", "validate_hello_ack",
        "make_protocol_version_disconnect",
    ):
        if token not in handshake_header:
            errors.append(f"contrato do handshake ausente: {token}")

if handshake_test_path.is_file():
    handshake_test_cases = handshake_test_path.read_text(encoding="utf-8").count("TEST_CASE(")
    if handshake_test_cases < 23:
        errors.append(
            f"somente {handshake_test_cases} testes de handshake; mínimo esperado: 23"
        )

if module_id_script_path.is_file():
    module_id_script = module_id_script_path.read_text(encoding="utf-8")
    for token in ("hashlib.sha256", "git", "module_build_id", "--format"):
        if token not in module_id_script:
            errors.append(f"gerador de module_build_id incompleto: {token}")

if 'add_source_files(module_obj, "src/protocol/*.cpp")' not in scsub:
    errors.append("SCsub não compila os fontes em src/protocol/*.cpp")
if 'add_source_files(module_obj, "src/public/*.cpp")' not in scsub:
    errors.append("SCsub não compila os fontes em src/public/*.cpp")
if "TICKSYNCHRONIZER_PROTOCOL_SMOKE_TEST_OK" not in build_script:
    errors.append("pipeline não exige o marcador de smoke do protocolo")


synchronizer_header_path = module / "src/public/tick_synchronizer.h"
synchronizer_source_path = module / "src/public/tick_synchronizer.cpp"
synchronizer_xml_path = module / "doc_classes/TickSynchronizer.xml"
for path in (synchronizer_header_path, synchronizer_source_path, synchronizer_xml_path):
    if not path.is_file():
        errors.append(f"arquivo público obrigatório ausente: {path.relative_to(module)}")

if all(path.is_file() for path in (synchronizer_header_path, synchronizer_source_path, synchronizer_xml_path)):
    synchronizer_header = synchronizer_header_path.read_text(encoding="utf-8")
    synchronizer_source = synchronizer_source_path.read_text(encoding="utf-8")
    synchronizer_xml = ET.parse(synchronizer_xml_path).getroot()
    synchronizer_documented = {
        method.attrib["name"] for method in synchronizer_xml.findall("./methods/method")
    }
    synchronizer_bound = set(re.findall(
        r'D_METHOD\("([A-Za-z_][A-Za-z0-9_]*)"', synchronizer_source
    ))
    required_diagnostics = {
        "get_build_precision", "is_double_precision", "get_protocol_magic",
        "get_protocol_major", "get_protocol_minor", "get_protocol_precision_mode",
    }
    for method in sorted(required_diagnostics):
        if method not in synchronizer_header:
            errors.append(f"método público ausente no header TickSynchronizer: {method}")
        if method not in synchronizer_bound:
            errors.append(f"binding público ausente em TickSynchronizer: {method}")
        if method not in synchronizer_documented:
            errors.append(f"documentação pública ausente em TickSynchronizer: {method}")

if errors:
    print("TickSynchronizer source consistency check failed:", file=sys.stderr)
    for error in errors:
        print(f"- {error}", file=sys.stderr)
    raise SystemExit(1)

print(f"TICKSYNCHRONIZER_SOURCE_CONSISTENCY_OK methods={len(documented)} tests={test_cases}")
PY

"$MODULE_DIR/scripts/verify_mermaid_diagrams.py" --root "$MODULE_DIR"
