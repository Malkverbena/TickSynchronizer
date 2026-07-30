# ADR 0018 — Desabilitar invalid pointer pairs no ASAN de aceitação

- Status: aceito
- Data: 2026-07-30

## Contexto

O Linux/BSD do Godot 4.7.1 compila `use_asan=yes` com instrumentação
`address,pointer-subtract,pointer-compare`. A execução dessas duas verificações
de ponteiros depende de `ASAN_OPTIONS=detect_invalid_pointer_pairs`.

O `StringName` da engine implementa os operadores de ordenação comparando os
endereços dos dados internados. Com `detect_invalid_pointer_pairs=2`, o editor
aborta em `StringName::operator<` durante `register_core_settings()`, antes de
qualquer teste do TickSynchronizer. O resultado ocorre tanto em precisão
`single` quanto `double`.

## Decisão

O perfil ASAN de aceitação acrescenta:

```text
detect_invalid_pointer_pairs=0
```

O restante do ASAN permanece habilitado. A opção
`--invalid-pointer-pairs` ativa explicitamente o valor `2` para diagnóstico da
engine, mas não faz parte do critério de aceitação do módulo.

Não será aplicado patch ao Godot 4.7.1.

## Consequências

- permanecem ativos heap/stack/global OOB, use-after-free, double-free e demais
  verificações normais do ASAN;
- comparações e subtrações de ponteiros entre objetos distintos não serão
  verificadas no perfil padrão;
- os testes conseguem alcançar o TickSynchronizer sem serem interrompidos por
  uma escolha de implementação da engine;
- a exceção fica documentada, explícita e reproduzível;
- um futuro upgrade da baseline poderá reavaliar esta decisão.
