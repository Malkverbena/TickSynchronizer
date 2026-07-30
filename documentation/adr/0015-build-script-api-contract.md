# ADR 0015 — Contrato de API entre scripts de build

## Status

Accepted

## Contexto

Uma revisão anterior do pacote combinou um `run_sanitized_tests.sh` novo com um
`build_and_validate.sh` antigo. O wrapper enviava opções que a outra revisão não
reconhecia, produzindo falhas ambíguas antes da compilação.

## Decisão

`build_and_validate.sh` expõe `--print-api-version`. Todo wrapper que depende de
sua interface deve validar essa versão antes de executar o pipeline.

A revisão atual usa a API de scripts `4`.

## Consequências

- Misturas de arquivos de revisões diferentes falham imediatamente.
- A mensagem informa que o pacote completo deve ser extraído novamente.
- Alterações incompatíveis na linha de comando exigem incrementar a versão.
- Mudanças internas que preservem a interface não exigem nova versão.
