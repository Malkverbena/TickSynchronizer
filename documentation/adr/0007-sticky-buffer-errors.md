# ADR 0007 — Erros persistentes e operações atômicas no bitstream

## Status

Accepted

## Contexto

Falhas silenciosas ou leituras parciais tornam um pacote corrompido difícil de diagnosticar e podem deslocar todos os campos seguintes.

## Decisão

O primeiro erro de `TickSynchronizerBuffer` permanece ativo até reinicialização explícita. Operações que falham por modo, largura, limite ou fim de buffer não avançam parcialmente o cursor e não expõem valores parciais.

## Consequências

- Chamadores podem verificar o erro uma vez após uma sequência de operações.
- Um erro não pode ser ignorado por operações subsequentes.
- `clear()`, `begin_write()` e `begin_read()` são os únicos resets normais do estado de erro.
- Testes precisam confirmar cursor estável e saída zerada após leitura inválida.
