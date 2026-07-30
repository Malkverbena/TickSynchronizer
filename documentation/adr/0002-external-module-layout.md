# ADR 0002 — Módulo externo à árvore do Godot

## Status

Accepted

## Contexto

Manter o módulo dentro da árvore da engine mistura históricos, dificulta atualização e incentiva alterações acidentais no Godot.

## Decisão

Manter `tick_synchronizer/` como repositório irmão de `godot/` e compilar com `custom_modules=../tick_synchronizer`.

## Consequências

- Engine e módulo permanecem independentes.
- O caminho relativo precisa ser preservado ou explicitamente configurado.
- Includes e `SCsub` devem funcionar como custom module externo.
