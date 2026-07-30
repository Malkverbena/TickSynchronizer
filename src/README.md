# Árvore de código-fonte

O código C++ do módulo fica sob `src/` para separar implementação, testes,
documentação e integração de build.

```text
src/
├── internal/   # configuração interna de compilação
├── protocol/   # wire format, codec e handshake independentes de transporte
└── public/     # classes registradas no ClassDB e API exposta ao Godot
```

Os arquivos `register_types.h/.cpp` permanecem na raiz porque fazem parte do
contrato convencional de registro de módulos do Godot. `SCsub` e `config.py`
também permanecem na raiz como arquivos de integração SCons.

Regras:

- código público não deve depender de um endpoint de transporte concreto;
- código de protocolo não deve depender de SceneMultiplayer, ENet ou sockets;
- headers internos não fazem parte da API pública do módulo;
- includes do próprio módulo usam caminhos iniciados por `src/` quando cruzam
  subdiretórios, evitando dependência implícita da ordem de include paths.
