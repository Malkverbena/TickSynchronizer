# ADR 0006 — Commits por alteração lógica completa

## Status

Accepted

## Contexto

Commits para cada rename, include ou ajuste pequeno criam histórico ruidoso e dificultam revisão e bisect.

## Decisão

Criar commits somente quando uma alteração coerente estiver completa, compilando, passando testes e com documentação atualizada.

## Consequências

- Trabalho intermediário permanece no working tree, stash ou patch local.
- Commits ficam maiores que microcommits, mas devem continuar focados em um único objetivo.
- Refactors não relacionados não devem ser misturados.
