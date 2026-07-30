# ADR 0009 — Codecs IEEE 754 explícitos e independentes de real_t

## Status

Accepted

## Contexto

A largura de `real_t` muda entre builds `precision=single` e `precision=double`. Serializá-lo diretamente tornaria o wire format dependente da configuração local da engine. Floats também possuem padrões distintos para zero negativo, infinidades, subnormais e NaNs.

## Decisão

- Fornecer codecs separados `float32` e `float64`.
- Codificar o padrão IEEE 754 em little-endian usando exatamente 32 ou 64 bits.
- Não serializar `real_t` diretamente.
- Permitir campos alinhados ou desalinhados, como os inteiros fixos subjacentes.
- Preservar o padrão de bits recebido pelo codec de baixo nível, incluindo zero negativo, infinidades, subnormais e payloads de quiet NaN.
- Não usar NaN como valor semântico no protocolo. Schemas e codecs de nível superior deverão rejeitar ou canonicalizar valores não finitos quando a propriedade exigir determinismo numérico.
- Na API GDScript, `write_float32()` converte o `float` de 64 bits para binary32 e rejeita valores finitos fora da faixa representável, em vez de transformá-los silenciosamente em infinito.

## Consequências

- Os mesmos bytes são produzidos em builds single e double.
- O protocolo pode escolher explicitamente custo e precisão por campo.
- O codec de baixo nível é lossless em relação ao padrão binary32/binary64 fornecido em C++.
- A conversão GDScript para `float32` pode perder precisão, o que é inerente à largura escolhida e deve ser explícito no schema.
- NaNs possuem múltiplos padrões válidos; por isso, hashing e comparação de estado não devem depender diretamente de NaNs não canonicalizados.
