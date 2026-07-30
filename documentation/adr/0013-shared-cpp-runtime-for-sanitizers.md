# ADR 0013 — Runtime C++ compartilhado em builds sanitizados

## Status

Aceito.

## Contexto

O Godot 4.7.1-stable usa `use_static_cpp=yes` por padrão no Linux/BSD. Após remover `raycast`/Embree do perfil sanitizado, o editor ASAN+UBSAN continuou falhando no GNU BFD com estouros `R_X86_64_PC32`, agora dentro de `libstdc++.a`.

O executável sanitizado é uma ferramenta local de diagnóstico e não precisa da portabilidade do runtime C++ estático.

## Decisão

O perfil sanitizado padrão define, entre outras opções:

```text
use_llvm=yes
linker=lld
module_raycast_enabled=no
use_static_cpp=no
use_asan=yes
use_ubsan=yes
```

Os builds normais permanecem com as opções padrão do Godot. `--static-cpp` existe somente para reprodução e diagnóstico.

## Consequências

- Todo o TickSynchronizer continua instrumentado.
- O executável sanitizado depende das bibliotecas compartilhadas do toolchain.
- O tamanho e a pressão de relocação do link são reduzidos.
- O perfil sanitizado não é um artefato distribuível.
