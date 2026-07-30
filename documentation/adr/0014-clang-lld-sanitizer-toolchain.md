# ADR 0014 — Clang e LLD para sanitizers no Godot 4.7.1

## Status

Parcialmente substituído pelo ADR 0017.

## Contexto

No Godot 4.7.1-stable, o editor monolítico com ASAN e UBSAN pode exceder a
faixa de relocações com GCC/GNU BFD. O projeto mantém a baseline estável e não
aplica patches à engine.

## Decisão original

Usar Clang + LLD para o perfil sanitizado.

## Revisão

Clang + LLD permanece a escolha para ASAN. Para UBSAN, a instalação observada
não fornece ou não vincula o runtime C++ necessário às verificações `vptr`.
UBSAN passa a usar GCC + LLD, conforme ADR 0017.

## Consequências

- Builds normais continuam cobrindo GCC sem sanitizers.
- ASAN usa Clang + LLD.
- UBSAN usa GCC + LLD.
- O modo combinado Clang permanece apenas diagnóstico.
