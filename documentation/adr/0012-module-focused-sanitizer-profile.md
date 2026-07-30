# ADR 0012 — Perfil sanitizado focado no módulo

## Status

Aceito; atualizado pelo ADR 0017.

## Contexto

O editor monolítico sanitizado falhou durante o link antes da execução dos
testes em componentes não relacionados ao TickSynchronizer. O módulo não
depende de `raycast`/Embree, e sanitizar toda a engine não é requisito para
aceitar uma alteração do módulo.

## Decisão

O perfil sanitizado padrão:

- compila editor e testes do TickSynchronizer;
- desabilita `raycast`/Embree;
- define `dev_build=no`, `optimize=debug` e `debug_symbols=yes`;
- define `accesskit=no` e `use_static_cpp=no`;
- executa testes C++ e smoke test;
- não altera o código da engine;
- executa ASAN e UBSAN em passagens independentes, conforme ADR 0017.

Um perfil opcional `--full-engine` mantém raycast/Embree, mas não é requisito
de aceitação.

## Consequências

- Todo o código do TickSynchronizer continua instrumentado.
- O build sanitizado não representa a configuração completa da engine.
- A suíte permanece disponível por `tests=yes`.
- Falhas exclusivas da engine ou do linker não bloqueiam o módulo.
- Antes de usar raycast como dependência, esta decisão deverá ser revisada.
