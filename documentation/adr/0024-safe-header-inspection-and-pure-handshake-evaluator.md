# ADR 0024 — Inspeção segura de header e avaliador puro de handshake

## Status

Aceito.

## Contexto

O decoder principal precisa ser atômico e rejeitar versões incompatíveis, mas a
camada de sessão precisa conhecer a versão observada para responder com um
disconnect estruturado. Retornar um pacote parcialmente decodificado seria
perigoso e ambíguo.

## Decisão

Adicionar `inspect_control_header()`, que lê apenas o envelope fixo após validar
tamanho mínimo e magic. Ele não valida a versão, não copia payload e não aceita
o pacote.

Adicionar `ProtocolHandshakeEvaluator`, componente interno, puro e sem estado
global. Ele avalia perfis, produz ACK/disconnect e valida ACKs, preservando os
outputs que não correspondem ao resultado.

## Consequências

- diagnóstico de versão não enfraquece o decoder;
- transporte e máquina de estados permanecem separados da serialização;
- testes podem cobrir compatibilidade sem inicializar rede ou SceneTree;
- a futura sessão poderá consumir decisões determinísticas do avaliador.
