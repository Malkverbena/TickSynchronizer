# ADR 0017 — Passagens separadas para ASAN e UBSAN

## Status

Aceito.

## Contexto

O build combinado Clang/LLD instrumentou verificações C++ de UBSAN, mas o link
falhou com símbolos indefinidos:

```text
__ubsan_vptr_type_cache
__ubsan_handle_dynamic_type_cache_miss
```

Esses símbolos pertencem ao runtime C++ de UBSAN. Uma instalação de Clang pode
estar funcional para ASAN e ainda não possuir o componente
`ubsan_standalone_cxx` correspondente. Insistir no build combinado introduziria
uma dependência externa desnecessária e frágil.

GCC fornece seu próprio runtime UBSAN e já permanece obrigatório para os builds
normais do projeto.

## Decisão

O perfil sanitizado de aceitação no Linux executa duas passagens:

1. ASAN com Clang + LLD;
2. UBSAN com GCC + LLD.

Configuração comum:

```text
target=editor
tests=yes
dev_build=no
optimize=debug
debug_symbols=yes
module_raycast_enabled=no
accesskit=no
use_static_cpp=no
```

O script compila, liga e executa um programa C++17 mínimo com cada sanitizer
antes de iniciar o SCons. O modo ASAN+UBSAN combinado permanece disponível por
`--combined`, mas não é requisito de aceitação.

## Consequências

- Não é necessário instalar um runtime UBSAN C++ específico do Clang.
- ASAN e UBSAN continuam cobrindo todo o código do TickSynchronizer.
- Existem dois artefatos e dois diretórios de relatório por validação completa.
- O tempo total pode ser maior, mas falhas ficam melhor isoladas.
- Resultados dos dois toolchains aumentam a diversidade de cobertura.
