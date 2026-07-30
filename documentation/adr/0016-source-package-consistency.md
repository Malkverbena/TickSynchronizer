# ADR 0016 — Consistência entre source, testes e documentação

## Status

Aceito.

## Contexto

Um pacote anterior combinou testes e documentação da revisão mais recente com
uma implementação antiga de `TickSynchronizerBuffer`. Builds normais que ainda
usavam a árvore anterior passaram, mas uma extração posterior falhou durante a
compilação dos testes por ausência de métodos como `set_max_size_bytes`.

## Decisão

- O pacote deve ser tratado como uma unidade; não substituir arquivos isolados
  entre revisões, salvo quando a mudança for explicitamente limitada e auditada.
- `scripts/verify_source_consistency.sh` compara:
  - métodos documentados no XML;
  - métodos vinculados no C++;
  - declarações no header;
  - API mínima do buffer;
  - marcadores do smoke test;
  - quantidade mínima de casos C++.
- `build_and_validate.sh` executa essa verificação antes do SCons.
- A criação do ZIP deve partir de uma única árvore validada.

## Consequências

- Pacotes híbridos falham imediatamente com diagnóstico claro.
- A verificação é estrutural e não substitui compilação, testes ou sanitizers.
- Alterações da API exigem atualização coordenada de header, implementação,
  bindings, XML, testes e do verificador quando aplicável.
