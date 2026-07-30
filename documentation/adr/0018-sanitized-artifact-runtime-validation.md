# ADR 0018 — Validação de execução de artefatos sanitizados

## Status

Aceito.

## Contexto

O editor sanitizado do Godot é um executável monolítico grande. As configurações ASAN
foram compiladas e ligadas com sucesso, mas o pipeline abortou em uma execução separada
`godot --version` com timeout curto, antes dos testes do TickSynchronizer. Essa prova era
redundante e podia produzir falsos negativos por custo de inicialização ou comportamento
de encerramento dos runtimes de instrumentação.

## Decisão

Para artefatos normais, manter `--version` como prova rápida de inicialização.

Para artefatos sanitizados:

1. validar existência, tamanho e permissão de execução;
2. inspecionar o cabeçalho ELF quando `readelf` estiver disponível;
3. listar dependências dinâmicas e rejeitar qualquer `not found`;
4. não executar um probe separado `--version`;
5. exigir os testes C++ filtrados e o smoke test como validação de execução.

Falhas na validação estrutural devem imprimir o final do log imediatamente.

`detect_stack_use_after_return=1` deixa de ser padrão e passa a ser uma opção explícita
do wrapper, devido ao custo adicional.

## Consequências

- elimina o falso negativo observado após links ASAN bem-sucedidos;
- não reduz a cobertura funcional, pois testes e smoke continuam obrigatórios;
- torna erros de dependência dinâmica mais claros;
- evita pagar sempre o custo de use-after-return;
- templates sanitizados sem testes exigiriam outra política antes de serem usados como
  critério de aceitação.
