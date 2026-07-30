# ADR 0003 — PackedByteArray como armazenamento wire

## Status

Accepted

## Contexto

O protocolo precisa de armazenamento compatível com o Godot, eficiente e independente de ABI. Também precisa representar campos não alinhados sem depender dos métodos de codificação internos de `PackedByteArray`.

## Decisão

Usar `PackedByteArray` como container canônico do pacote pronto e implementar um codec explícito sobre ele. O bitstream consome cada campo LSB-first. Inteiros alinhados a byte aparecem em little-endian. O tamanho lógico em bits é preservado separadamente do número físico de bytes.

## Consequências

- Não copiar structs C++ diretamente para a rede.
- Endianness, largura e ordem de bits são explícitas e cobertas por testes.
- Padding do último byte não é payload e exige `bit_size`.
- O buffer possui golden vectors multiplataforma.
- Métodos `PackedByteArray.encode_*()` não definem o wire format do projeto.
