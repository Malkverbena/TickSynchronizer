# ADR 0026 — Mermaid na documentação e preview do VS Code

## Status

Aceito.

## Contexto

A arquitetura e o protocolo já possuem relações que ficam difíceis de comunicar
somente por texto. GitHub e o VS Code atual renderizam Mermaid diretamente em
blocos Markdown cercados.

## Decisão

Adicionar diagramas Mermaid às páginas arquiteturais e versionar configuração
Markdown mínima em `.vscode/`. O subconjunto inicial permitido é `flowchart`,
`sequenceDiagram`, `stateDiagram-v2` e `classDiagram`.

Uma verificação estática local garante blocos fechados e tipos aprovados. O uso
do Mermaid CLI é opcional e não vira dependência do build do módulo.

## Consequências

- diagramas permanecem revisáveis como texto no Git;
- GitHub e VS Code apresentam visualizações sem imagens binárias duplicadas;
- o texto continua sendo a fonte normativa;
- tipos Mermaid experimentais não devem ser usados sem validar compatibilidade;
- versões do VS Code anteriores a 1.121 podem exigir extensão, mas o repositório
  não a recomenda para versões atuais.
