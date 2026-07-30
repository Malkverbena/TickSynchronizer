# ADR 0020: supressão UBSAN estrita no setup de testes do Godot 4.7.1

## Status

Aceito.

## Contexto

`Main::test_setup()` cria `ProjectSettings`, mas não carrega um projeto antes de
`register_core_extensions()`. `project_data_dir_name` permanece vazio.
`GDExtension::get_extension_list_config_file()` concatena `"res://"` com essa
string e `String::append_utf32_unchecked()` chega a `memcpy(dst, nullptr, 0)`.
GCC UBSAN reporta `nonnull-attribute` antes de qualquer teste TickSynchronizer.

## Decisão

O perfil UBSAN de aceitação usa uma supressão runtime única:

```text
nonnull-attribute:core/string/ustring.cpp
```

O arquivo fica em
`scripts/sanitizer_suppressions/godot-4.7.1-ubsan.supp`.

## Consequências

- a suíte do módulo consegue iniciar sem alterar a engine;
- apenas esse check nesse arquivo é suprimido;
- relatórios em outros arquivos e todos os relatórios do TickSynchronizer
  continuam fatais;
- `--no-godot-ubsan-suppressions` reproduz o diagnóstico da engine;
- a supressão deve ser revisada ao mudar a baseline do Godot.
