# ADR 0010 — Limites de recurso e identidade lógica do buffer

## Status

Aceito.

## Contexto

O buffer processará dados provenientes de peers não confiáveis. Crescimento sem limite, retenção de bytes fora do tamanho lógico e comparação baseada no estado do cursor criariam riscos de consumo excessivo de memória, divergências de snapshot e diagnósticos inconsistentes.

## Decisão

- Cada `TickSynchronizerBuffer` possui um limite máximo configurável em bytes.
- O padrão é 1 MiB; camadas de protocolo e transporte deverão usar limites menores conforme o tipo de pacote e o MTU.
- Reservas e expansões são verificadas antes da alocação e nunca podem exceder o limite configurado.
- Entradas de leitura acima do limite são rejeitadas antes da cópia.
- Dados de leitura são copiados para uma representação canônica contendo apenas os bytes lógicos; bits não utilizados do último byte são zerados.
- Igualdade e hash consideram somente bytes canônicos e `bit_size`.
- Cursor, modo, erro e limite configurado não participam da identidade lógica.
- O hash é destinado a tabelas, diagnósticos e comparação rápida; não é criptográfico e não faz parte do wire format.

## Consequências

- Entradas maliciosas não podem provocar crescimento ilimitado pelo buffer.
- Falhas por limite são atômicas e usam `ERR_PARAMETER_RANGE_ERROR`.
- `begin_read()` faz uma cópia canônica deliberada, trocando algum custo de CPU por isolamento, memória previsível e semântica inequívoca.
- O valor padrão de 1 MiB não autoriza pacotes de rede desse tamanho. Endpoints e codecs superiores devem impor limites específicos e normalmente muito menores.
- O limite é por buffer e não controla o consumo agregado de filas, peers, snapshots ou históricos; essas camadas precisam de orçamentos próprios.
- A cópia canônica de leitura deve ser medida nos benchmarks. Um futuro caminho sem cópia só deve ser criado se os dados mostrarem benefício material sem enfraquecer os invariantes.
- Alterar o algoritmo de hash não quebra compatibilidade de protocolo, mas pode invalidar caches locais; mudanças devem ser documentadas.
