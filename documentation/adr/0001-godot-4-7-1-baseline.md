# ADR 0001 — Baseline Godot 4.7.1-stable

## Status

Accepted

## Contexto

Desenvolver contra `master` ou uma release candidate adiciona mudanças não estabilizadas e dificulta separar regressões da engine de regressões do módulo.

## Decisão

Usar exclusivamente Godot `4.7.1-stable`, commit `a13da4feb8d8aefc283c3763d33a2f170a18d541`, em uma branch local criada a partir da tag. Não modificar o código da engine.

## Consequências

- Builds e bugs tornam-se reproduzíveis.
- APIs novas posteriores a 4.7.1 não podem ser usadas sem uma atualização formal da baseline.
- O script deve rejeitar commit divergente ou working tree suja por padrão.
