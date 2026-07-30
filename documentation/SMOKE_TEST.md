# Smoke test manual no editor

## Objetivo

Confirmar rapidamente que o editor contém o módulo TickSynchronizer, que as cinco classes públicas estão registradas e que a precisão compilada é a esperada e que magic e versão estrita `1.1` do protocolo estão expostos corretamente.

## Pré-requisito

Compile o editor com o módulo externo:

```bash
cd /caminho/workspace/tick_synchronizer
./scripts/build_and_validate.sh --mode editor
```

Ou use o comando SCons documentado em [`BUILD.md`](BUILD.md).

## Abrir o projeto

Pelo terminal:

```bash
cd /caminho/workspace/godot

./bin/godot.linuxbsd.editor.dev.double.x86_64 \
  --editor \
  --path ../tick_synchronizer/tests/smoke_project
```

Também é possível importar manualmente `tests/smoke_project/project.godot` pelo Project Manager.

## Executar

No editor:

1. Aguarde a importação inicial terminar.
2. Abra `smoke_test.tscn` se ela não estiver aberta.
3. Pressione **F6** para executar a cena ou **F5** para executar o projeto.
4. Consulte o painel **Output**.

Resultado esperado em precisão dupla:

```text
TICKSYNCHRONIZER_BUILD_PRECISION=double
TICKSYNCHRONIZER_BUFFER_SMOKE_TEST_OK
TICKSYNCHRONIZER_INTEGER_CODEC_SMOKE_TEST_OK
TICKSYNCHRONIZER_FLOAT_CODEC_SMOKE_TEST_OK
TICKSYNCHRONIZER_RESOURCE_LIMIT_SMOKE_TEST_OK
TICKSYNCHRONIZER_PROTOCOL_SMOKE_TEST_OK
TICKSYNCHRONIZER_SMOKE_TEST_OK
```

Em precisão simples, os mesmos marcadores são exigidos e a primeira linha usa
`TICKSYNCHRONIZER_BUILD_PRECISION=single`.

## Verificação visual das classes

Na árvore de cena, use **Add Child Node** e procure:

```text
TickSynchronizer
TickSynchronizerObject
```

Na criação de Resources, procure:

```text
TickSynchronizerSettings
TickSynchronizerSchema
```

`TickSynchronizerBuffer` deriva de `RefCounted`; sua instanciação é verificada pelo script do smoke test, não pela criação de Resource no editor.

## Erros

O script usa `registered_class` como variável de iteração. `class_name` não é usado porque é uma palavra reservada do GDScript.

Se aparecer `TICKSYNCHRONIZER_SMOKE_TEST_FAILED`, consulte as mensagens imediatamente anteriores no painel **Output**. Elas indicam a classe ausente, falha de instanciação ou incompatibilidade de precisão.
