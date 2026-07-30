# ADR 0022 — Cabeçalho de controle fixo v1

## Status

Aceito para o envelope fixo. Os payloads mínimos de handshake e a versão 1.0
foram substituídos pelo protocolo 1.1 no ADR 0023.

## Contexto

O primeiro decoder precisa ser simples de auditar, rejeitar entradas inválidas
antes de alocar e permanecer independente de transporte, ABI e `real_t`.

## Decisão

Adotar um cabeçalho de controle fixo de 40 bytes com:

- magic `TSYN`;
- versão estrita `1.0`;
- packet type e header size explícitos;
- flags e reservados obrigatoriamente zero;
- session ID `u64`;
- sequence `u32`;
- tick `u64`;
- tamanho físico e lógico do payload em `u32`;
- todos os inteiros little-endian.

O decoder valida todos os campos e limites antes de copiar o payload. A saída
permanece inalterada em qualquer erro. O tamanho físico é canônico quando é
exatamente `ceil(payload_bit_size / 8)`: 95 bits em 12 bytes são válidos se o
padding for zero, enquanto 88 bits em 12 bytes são rejeitados.

`HELLO`, `HELLO_ACK` e `DISCONNECT_REASON` usam payloads fixos e versionados.
A precisão é estruturada como `single=1` e `double=2`.

## Consequências positivas

- parsing previsível e auditável;
- ausência de dump de structs;
- validação de tamanho antes de alocação;
- golden vectors estáveis;
- erros específicos para truncamento, trailing data, padding e incompatibilidade;
- independência de endpoint.

## Custos

- 40 bytes são excessivos para pacotes realtime pequenos;
- compatibilidade minor ainda é estrita;
- campos reservados não podem ser usados sem nova revisão;
- payloads fixos do handshake exigirão extensão versionada para build IDs e schema.

O overhead é aceito apenas para controle. Um cabeçalho realtime compacto será
definido depois do handshake.
