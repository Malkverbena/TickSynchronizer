# TickSynchronizer — Agent Instructions

Este arquivo é a entrada obrigatória para qualquer pessoa ou agente automatizado que trabalhe no repositório.

## Fonte de verdade

Antes de alterar código, leia nesta ordem:

1. `AGENTS.md`;
2. `documentation/PROJECT_STATE.md`;
3. `documentation/ARCHITECTURE.md`;
4. ADRs relevantes em `documentation/adr/`;
5. `documentation/ROADMAP.md`;
6. documentação específica da tarefa.

Não trate conversas, mensagens antigas ou memória do agente como fonte principal. Em caso de conflito, os arquivos versionados e os testes prevalecem.

## Baseline da engine

- Godot: `4.7.1-stable`.
- Commit exato: armazenado em `GODOT_COMMIT`.
- Versão legível: armazenada em `GODOT_VERSION`.
- A árvore do Godot fica fora deste repositório, normalmente em `../godot`.
- O módulo é carregado com `custom_modules=../tick_synchronizer`.
- Não modificar, aplicar patches ou manter cherry-picks locais no código da engine.
- O script de validação deve rejeitar por padrão uma engine em commit diferente ou com working tree suja.

## Build e precisão

- Usar apenas SCons.
- Não introduzir CMake.
- Precisão padrão: `double`.
- Precisões suportadas: `single` e `double`.
- Todos os peers de uma sessão devem usar a mesma precisão.
- O wire format nunca deve serializar `real_t` diretamente.
- Plataformas iniciais: Linux, Windows e Android.
- Web não é suportada; macOS e iOS ficam adiados.

## Convenções C++ e Godot

- Seguir as convenções do Godot 4.7.1.
- Usar `#pragma once` nos headers próprios.
- Incluir diretamente cada tipo utilizado; não depender de includes transitivos.
- Preferir tipos e containers do Godot no runtime: `PackedByteArray`, `Vector`, `LocalVector`, `HashMap`, `StringName`, `ObjectID`, `Ref<T>` e `Variant` quando necessário.
- Não usar exceções ou RTTI no runtime do módulo.
- Usar `Error`, `ERR_FAIL_*`, `WARN_PRINT` e `ERR_PRINT` conforme os padrões da engine.
- Não serializar structs C++ por cópia de memória.
- Wire format: little-endian explícito e ordem de bits LSB-first.
- `TickSynchronizerBuffer` aceita campos de 1 a 64 bits; contagem zero é erro.
- Inteiros fixos são little-endian e podem ser escritos desalinhados.
- Varints são alinhados por byte, canônicos, limitados a 10 bytes e rejeitam formas redundantes.
- Floats usam codecs explícitos IEEE 754 `float32`/`float64`; nunca serializar `real_t`.
- O codec de baixo nível preserva padrões de bits, mas schemas determinísticos devem rejeitar ou canonicalizar NaN quando necessário.
- Padding de alinhamento deve ser zero e validado pelo decoder.
- O primeiro erro do buffer é persistente até reinicialização e não pode avançar parcialmente o cursor.
- O buffer possui limite máximo configurável; o padrão é 1 MiB e camadas superiores devem usar limites menores conforme o tipo de pacote.
- Limites por buffer não substituem orçamentos agregados por sessão, peer, fila ou histórico.
- Verificar limites antes de reservar, copiar ou expandir armazenamento.
- Medir a cópia canônica de leitura nos benchmarks antes de criar qualquer modo sem cópia.
- `begin_read()` deve produzir bytes canônicos: sem trailing bytes e com bits não lógicos do último byte zerados.
- Igualdade e hash do buffer consideram somente bytes canônicos e `bit_size`; cursor, modo, erro e capacidade não participam.
- Hash de conteúdo não é criptográfico, não prova igualdade e não deve ser colocado no wire format como identificador de segurança.
- Usar `get_bit_size()` junto com `get_data()` para preservar o tamanho lógico do último byte parcial.
- Não introduzir singleton obrigatório para representar uma sessão.
- O código próprio fica sob `src/public`, `src/protocol` e `src/internal`.
- `register_types.h/.cpp`, `SCsub` e `config.py` permanecem na raiz por integração com o sistema de módulos do Godot.
- Diagramas arquiteturais usam Mermaid em Markdown; o texto continua normativo.
- Usar apenas `flowchart`, `sequenceDiagram`, `stateDiagram-v2` e `classDiagram` até nova decisão em ADR.

## Segurança e compatibilidade

- Não implementar primitivas criptográficas próprias.
- A criptografia futura deverá usar backend baseado em mbedTLS.
- Nenhum input, snapshot ou dado de gameplay deve ser aceito antes da conclusão do handshake.
- Incompatibilidades devem produzir erros acionáveis, especialmente `PRECISION_MISMATCH`.

## Validação obrigatória

Uma alteração funcional completa deve:

1. passar `scripts/verify_source_consistency.sh`, incluindo a validação Mermaid;
2. compilar o editor com `tests=yes`;
3. passar todos os testes C++ do TickSynchronizer;
4. passar o smoke test GDScript automatizado;
5. compilar `template_debug`;
6. compilar `template_release`;
7. atualizar documentação, ADRs e `PROJECT_STATE.md` quando aplicável.

Use:

```bash
./scripts/build_and_validate.sh --mode all --precision double
```

O ciclo rápido pode usar:

```bash
./scripts/build_and_validate.sh --mode quick --precision double
```

## Política de Git

- Não criar commit para cada pequena edição.
- Agrupar alterações em unidades lógicas completas.
- Um commit deve compilar, passar nos testes e conter a documentação correspondente.
- Não misturar refactors não relacionados com uma funcionalidade.
- Não alterar o código da engine Godot.

## Qualidade das solicitações

Alerte explicitamente quando uma solicitação:

- contrariar boas práticas;
- introduzir acoplamento desnecessário;
- reduzir desempenho, segurança, portabilidade ou reprodutibilidade;
- criar dívida técnica evitável;
- produzir histórico Git ruidoso;
- violar uma decisão aceita em ADR.

Não execute silenciosamente uma decisão prejudicial sem registrar suas consequências.

## Atualização de contexto

Ao concluir uma alteração completa:

- atualizar `documentation/PROJECT_STATE.md`;
- atualizar o checklist relevante em `documentation/ROADMAP.md`;
- criar ou atualizar ADR quando houver decisão arquitetural;
- executar `./scripts/generate_context.sh` para verificar o resumo de contexto;
- evitar incluir logs extensos ou código completo no contexto gerado.


## Perfil sanitizado

- Builds sanitizados Linux são artefatos locais de diagnóstico.
- Executar ASAN e UBSAN em passagens independentes por padrão.
- ASAN usa Clang + LLD.
- UBSAN usa GCC + LLD para não depender do runtime C++ UBSAN do Clang.
- Ambas as passagens usam `dev_build=no`, `optimize=debug`,
  `debug_symbols=yes`, `module_raycast_enabled=no`, `accesskit=no` e
  `use_static_cpp=no`.
- O modo combinado ASAN+UBSAN é apenas diagnóstico e não é requisito de
  aceitação.
- Builds normais mantêm a configuração regular de editor e templates.
- A validação pré-teste de um editor sanitizado deve ser estrutural; não use um
  `--version` curto como requisito de aceitação. Os testes C++ filtrados e o
  smoke test são a prova obrigatória de execução.
- `detect_stack_use_after_return=1` não é padrão; habilite-o apenas em
  investigações específicas devido ao custo adicional.
- O perfil ASAN de aceitação deve forçar `detect_invalid_pointer_pairs=0`.
- O perfil UBSAN pode suprimir apenas `nonnull-attribute` em `core/string/ustring.cpp`, ocorrência do setup de testes do Godot 4.7.1; nunca ampliar a supressão para o módulo ou para grupos inteiros de checks.
- A fundação do buffer considera ASAN completo e UBSAN dos testes C++ do módulo como gate suficiente.
- O smoke UBSAN completo da engine, atualmente bloqueado em SDL/HID, volta a ser obrigatório no Gate de Segurança P1, depois que existir packet decoder e antes de endpoints externos entregarem bytes não confiáveis.
- No Gate P1, preferir isolar subsistemas externos por opções oficiais ou usar harness do módulo; não acumular supressões para transformar a engine inteira em requisito do módulo.
- O protocolo de controle usa magic `TSYN`, versão estrita `1.1` e cabeçalho fixo de 40 bytes.
- Durante v0.x, capabilities nunca relaxam `protocol_major/minor` nem identidade.
- `HELLO`/`HELLO_ACK` transportam commit do Godot, module build ID, game build ID, schema compatibility ID e capabilities suportadas/obrigatórias.
- IDs de compatibilidade totalmente zerados são inválidos.
- O avaliador de handshake deve permanecer puro, interno e independente de transporte.
- Use `inspect_control_header()` apenas para diagnóstico; ele não transforma um pacote incompatível em pacote aceito.
- Inteiros do protocolo são little-endian e nunca são dumps de structs.
- Flags e campos reservados desconhecidos devem ser rejeitados, não ignorados.
- O decoder valida tamanho bruto, tamanho declarado, bit length e padding antes de copiar payload.
- Erros de decode devem preservar integralmente a estrutura de saída.
- O limite padrão de pacote é 64 KiB; o codec não aceita configuração acima de 1 MiB.
- Textos humanos não pertencem ao wire format; use códigos estruturados e formate mensagens na camada de sessão/UI.
- O Godot 4.7.1 ordena `StringName` por endereço e a checagem opcional aborta durante o setup da engine antes dos testes do módulo. Use `--invalid-pointer-pairs` somente para reproduzir esse diagnóstico da engine.
