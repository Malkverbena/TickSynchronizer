# ADR 0004 — Precisão uniforme por sessão

## Status

Accepted

## Contexto

Godot pode ser compilado com `precision=single` ou `precision=double`. Misturar peers pode causar divergência de simulação e interpretação incompatível de dados.

## Decisão

Suportar ambas as precisões, usar `double` como padrão e exigir que todos os peers de uma sessão usem a mesma precisão. O handshake deverá rejeitar incompatibilidade com `PRECISION_MISMATCH` e mensagem acionável.

## Consequências

- A precisão faz parte da compatibilidade da sessão.
- O protocolo não serializa `real_t` diretamente.
- A matriz de testes deve cobrir builds single e double e conexões incompatíveis.
