# ADR 0005 — Abstração SyncTransportEndpoint

## Status

Accepted

## Contexto

O projeto precisa comparar RPC, raw bytes, MultiplayerPeer e ENet direto sem acoplar snapshots e prediction a um transporte.

## Decisão

O runtime dependerá de `SyncTransportEndpoint`. Endpoints conhecem peers, canais, confiabilidade, envio, recebimento e métricas de transporte, mas não conhecem objetos sincronizados ou rollback.

## Consequências

- O mesmo protocolo pode ser exercitado por vários transports.
- Um endpoint loopback será implementado antes dos endpoints reais.
- Benchmarks podem comparar implementações sob a mesma carga.
