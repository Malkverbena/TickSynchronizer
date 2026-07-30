# ADR 0011 — Baseline de linguagem C++17

## Status

Aceito.

## Contexto

O TickSynchronizer é um módulo nativo compilado junto ao Godot 4.7.1-stable. A engine usa um subconjunto de C++17. Adotar recursos de C++20 aumentaria a distância em relação à baseline da engine, poderia exigir toolchains diferentes e reduzir portabilidade em Linux, Windows e Android.

## Decisão

- O código do módulo permanece em C++17.
- O módulo usa o padrão C++17 configurado pela própria baseline Godot 4.7.1-stable (`/std:c++17` no MSVC e `-std=gnu++17` nos demais compiladores).
- O `SCsub` não duplica flags de linguagem da engine, evitando conflito de ordem ou divergência entre targets.
- Um header de configuração falha em compilação quando o toolchain não fornece pelo menos C++17.
- O projeto não deve usar conceitos, ranges, corrotinas, `std::span`, `std::bit_cast` ou outras APIs exclusivas de C++20.
- Tipos e containers do Godot continuam preferenciais no runtime.

## Consequências

- A linguagem do módulo fica alinhada à engine estável adotada.
- A compatibilidade entre toolchains é mais previsível.
- Recursos de C++20 só poderão ser considerados em uma alteração arquitetural futura, com nova baseline, ADR e matriz completa de testes.
