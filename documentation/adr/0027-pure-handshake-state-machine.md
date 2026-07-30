# ADR 0027 — Máquina de estados pura do handshake

## Estado

Aceito.

## Contexto

O codec de pacotes e o avaliador de compatibilidade já validam payloads,
identidade, precisão, nonce e capabilities, mas não determinam a ordem legal das
mensagens. Essa responsabilidade não deve ser colocada no transporte nem no
codec, porque ambos precisam permanecer reutilizáveis e independentes da sessão.

## Decisão

Adicionar `ProtocolHandshakeStateMachine` como componente interno, C++17 e sem
dependência de `Node`, singleton, relógio ou endpoint.

Estados iniciais:

- `IDLE`;
- `WAITING_FOR_HELLO`;
- `WAITING_FOR_HELLO_ACK`;
- `ESTABLISHED`;
- `REJECTED`;
- `CLOSED`.

A máquina recebe `ProtocolPacket` já decodificado e devolve uma ação declarativa.
A ação pode conter `HELLO`, `HELLO_ACK` ou `DISCONNECT_REASON`, mas não envia nem
serializa o pacote final. `build_outbound_packet()` converte a ação tipada em um
`ProtocolPacket`; o codec existente continua responsável pelo wire format.

O iniciador fornece `session_id` e nonce não zero. O respondedor aprende o
`session_id` do primeiro `HELLO` válido. Durante o handshake, `sequence` e `tick`
devem ser zero.

Duplicatas e mensagens fora de ordem são fatais nesta revisão. Retransmissão e
idempotência serão definidas somente quando existirem requisitos concretos dos
endpoints.

## Consequências

- transições impossíveis deixam de ser representadas por combinações de booleans;
- testes podem reproduzir toda a negociação sem rede;
- a futura `SyncSession` recebe decisões prontas e não duplica regras;
- a política estrita de duplicatas poderá precisar ser revisada ao introduzir
  transporte não confiável e retransmissão;
- timeouts permanecem fora da máquina pura e dependem de um relógio externo;
- o respondedor entra em `ESTABLISHED` quando a ação `HELLO_ACK` é produzida; se o
  envio falhar, a futura sessão deve fechar a negociação antes de liberar gameplay.
