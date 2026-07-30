# ADR 0023 — Identidade estrita e capabilities no handshake

## Status

Aceito.

## Contexto

O protocolo experimental precisa impedir que peers com engine, módulo, jogo,
schema ou precisão incompatíveis iniciem gameplay. Ao mesmo tempo, recursos
opcionais precisam ser negociáveis sem usar strings ou nomes no wire format.

## Decisão

O protocolo passa para `1.1` e mantém compatibilidade estrita durante v0.x.
`HELLO` e `HELLO_ACK` transportam:

- precisão;
- commit do Godot, 20 bytes;
- module build ID, 20 bytes;
- game build ID, 16 bytes opacos;
- schema compatibility ID, 16 bytes opacos;
- capabilities suportadas e obrigatórias;
- nonce de correlação.

A conexão exige versão major/minor idêntica, identidades idênticas e requisitos
de capabilities satisfeitos nas duas direções. Bits opcionais desconhecidos
podem ser ignorados. Capabilities não relaxam a versão durante v0.x.

O module build ID de árvore limpa é o Git HEAD exato. Para árvore suja, um
script produz fingerprint determinístico do HEAD, diff e arquivos não ignorados.

## Consequências

- handshakes ficam maiores, mas continuam pequenos e raros;
- incompatibilidades produzem razões estruturadas e acionáveis;
- builds sujos podem ser distinguidos sem timestamp ou hostname;
- schema compatibility ID continua opaco até a fase de schemas;
- flexibilização por `protocol_minor` fica deliberadamente adiada.
