# Compatibilidade com o Godot

## Baseline atual

O TickSynchronizer usa exclusivamente:

```text
Godot 4.7.1-stable
a13da4feb8d8aefc283c3763d33a2f170a18d541
```

As referências oficiais ficam em:

```text
GODOT_VERSION
GODOT_COMMIT
```

A engine deve permanecer em uma branch local criada a partir dessa release, sem
alterações no código-fonte.

Cada execução de `scripts/build_and_validate.sh` registra:

- branch local do Godot;
- `HEAD` completo;
- `git describe`;
- URL do remote `origin`;
- baseline configurada e resolvida;
- estado limpo ou modificado da working tree;
- comandos SCons e artefatos produzidos.

Por padrão, o pipeline interrompe a execução quando:

- o `HEAD` difere de `GODOT_COMMIT`;
- a árvore da engine possui alterações locais;
- a baseline não pode ser resolvida pelo Git.

Overrides existem apenas para diagnóstico:

```bash
./scripts/build_and_validate.sh \
  --allow-godot-mismatch \
  --allow-dirty-godot
```

Resultados produzidos com esses overrides não constituem validação oficial do
projeto.

## Normalização aplicada

### Headers

Os headers próprios do módulo usam:

```cpp
#pragma once
```

### SCons

O `SCsub` importa:

```python
Import("env")
Import("env_modules")
```

As fontes do módulo são compiladas em um clone de `env_modules` e adicionadas a
`env.modules_sources`.

O código próprio fica em `src/public`, `src/protocol` e `src/internal`.
`register_types.h/.cpp` permanecem na raiz porque integram o contrato convencional
de registro dos módulos do Godot; `SCsub` e `config.py` também permanecem na raiz.

### Includes diretos

O projeto não depende de includes transitivos. Todo arquivo inclui diretamente
os headers que definem os símbolos usados por ele.

### Documentação de classes

O `config.py` fornece `get_doc_classes()` e `get_doc_path()`. Os XMLs das classes
públicas ficam em `doc_classes/`.

### Testes

Os testes dos módulos permanecem em headers `tests/test_*.h`, coletados pelo
Godot quando o editor é compilado com `tests=yes`.

## Contrato entre scripts

`build_and_validate.sh` expõe uma versão de interface:

```bash
./scripts/build_and_validate.sh --print-api-version
```

A revisão atual retorna:

```text
4
```

`run_sanitized_tests.sh` verifica essa versão antes de iniciar o SCons. Se os
scripts vierem de revisões diferentes, a execução é interrompida com uma
mensagem explícita para extrair novamente o pacote completo.

Isso evita que um wrapper novo envie opções desconhecidas para um script de
build antigo.

## Política de atualização da engine

A baseline só pode ser alterada como uma mudança completa:

1. selecionar uma nova release estável;
2. atualizar `GODOT_VERSION` e `GODOT_COMMIT`;
3. revisar as APIs utilizadas pelo módulo;
4. compilar editor, `template_debug` e `template_release`;
5. executar todos os testes C++;
6. executar o smoke test automatizado;
7. registrar incompatibilidades e migrações;
8. concluir tudo em um único commit lógico.
