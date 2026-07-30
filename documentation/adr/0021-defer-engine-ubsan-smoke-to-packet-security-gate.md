# ADR 0021 — Adiar o smoke UBSAN completo da engine para o gate do packet decoder

## Status

Aceito em 2026-07-30.

## Contexto

A fundação do `TickSynchronizerBuffer` passou:

- ASAN completo em `single` e `double`, incluindo testes C++ e smoke GDScript;
- UBSAN nos 47 testes C++ em `single` e `double`, com 18.091 assertions;
- builds normais de editor e templates nas duas precisões.

O smoke GDScript executado pelo editor UBSAN para durante a inicialização de
SDL/HID/joystick em `thirdparty/sdl/thread/SDL_thread.c`. O diagnóstico ocorre
fora do TickSynchronizer, depois que a suíte C++ do módulo já terminou com
sucesso.

Continuar adicionando supressões neste momento deslocaria o esforço para a
engine e dependências de terceiros, sem aumentar materialmente a cobertura das
primitivas atuais do buffer.

## Decisão

A Fase 2 considera como gate suficiente:

1. ASAN completo em ambas as precisões;
2. UBSAN dos testes C++ do módulo em ambas as precisões;
3. builds normais e smoke test normais em ambas as precisões.

A resolução ou isolamento do smoke UBSAN completo da engine é adiada para o
**Gate de Segurança P1**.

O Gate P1 será executado depois que o packet decoder mínimo existir e antes que
qualquer endpoint externo possa entregar bytes não confiáveis ao protocolo.
Nesse momento serão obrigatórios:

- ASAN e UBSAN dos testes do packet codec;
- fuzz target isolado;
- corpus de entradas inválidas;
- regressões para falhas encontradas;
- reavaliação do smoke UBSAN da engine.

Se SDL/HID continuar bloqueando o smoke, a solução preferida será isolar o
subsistema por opção oficial de build ou usar um harness do módulo que não
inicialize componentes não relacionados. Supressões amplas não serão aceitas.

## Consequências positivas

- o desenvolvimento funcional não fica bloqueado por código externo;
- o esforço sanitizado retorna quando há superfície real de ataque;
- fuzzing e sanitizers passam a exercitar bytes controlados por peers;
- reduz-se a manutenção de supressões específicas da engine;
- o risco é controlado por um gate explícito antes do transporte externo.

## Consequências negativas

- o smoke completo da engine sob UBSAN permanece incompleto temporariamente;
- problemas de integração específicos da inicialização UBSAN podem continuar
  desconhecidos até o Gate P1;
- o roadmap deve impedir que endpoints externos sejam considerados concluídos
  antes desse gate.

## Gatilho de reabertura

Reabrir este ADR quando ocorrer qualquer um dos seguintes eventos:

- implementação do packet decoder mínimo;
- início da integração de um endpoint externo;
- atualização da baseline do Godot;
- correção upstream do diagnóstico SDL/HID;
- diagnóstico ASAN/UBSAN dentro do TickSynchronizer.
