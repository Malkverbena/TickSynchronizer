# ADR 0025 — código do módulo sob `src/`

## Status

Aceito.

## Contexto

O repositório passou a conter API pública, codec binário, handshake e componentes
internos. Manter `public/`, `protocol/` e `internal/` na raiz misturava código,
testes, documentação, scripts e arquivos de integração SCons.

## Decisão

Mover o código C++ próprio para:

```text
src/public/
src/protocol/
src/internal/
```

`register_types.h/.cpp`, `SCsub` e `config.py` permanecem na raiz porque são glue
de integração convencional de um módulo Godot e são descobertos ou referenciados
pela infraestrutura de módulos.

## Consequências

- a raiz fica reservada a integração, metadados e documentação;
- includes cruzando subdiretórios usam prefixo `src/` explícito;
- testes e scripts precisam validar os novos caminhos;
- a migração deve ocorrer antes que o número de componentes cresça mais;
- não há mudança de wire format, ABI pública do Godot ou comportamento runtime.
