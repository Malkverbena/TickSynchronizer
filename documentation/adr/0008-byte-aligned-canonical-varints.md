# ADR 0008 — Varints canônicos e alinhados por byte

## Status

Accepted

## Contexto

Varints desalinhados ou com múltiplas representações equivalentes complicam inspeção de pacotes, validação, fuzzing, hashing e compatibilidade entre implementações.

## Decisão

- `varuint` e ZigZag `varint` começam obrigatoriamente em fronteira de byte.
- O alinhamento é sempre explícito; nenhum codec insere ou remove padding implicitamente.
- Padding de alinhamento é zero e o decoder rejeita padding não zero.
- O decoder aceita somente a representação mínima/canônica.
- O limite é 10 bytes para valores de 64 bits.
- Truncamento, overflow, continuação excessiva e formas redundantes são erros persistentes e atômicos.

## Consequências

- O wire format possui uma única codificação válida para cada inteiro.
- Packet inspectors e golden vectors ficam mais simples.
- O protocolo perde alguns bits potenciais ao alinhar varints, mas ganha clareza, robustez e interoperabilidade.
- Schemas precisam chamar explicitamente os métodos de alinhamento quando alternarem entre bit fields e varints.
