# 16 — HITL: contrato imposto à estação de solo

**Tipo:** HITL — depende da equipe de solo
**User stories:** 19, 30

## What to build

O firmware da estação de solo está **fora do escopo** deste PRD — é esforço separado. Mas dois requisitos são impostos a ela e **não são opcionais**, porque a estação de solo é o seguro do projeto contra perder o veículo. Esta issue existe para que eles não sumam na fronteira entre os dois esforços.

### Requisito 1 — Dois receptores, um por banda

Um receptor em **433 MHz** (E22 / SX1268) e um em **915–928 MHz** (SX1276). A redundância de rádio a bordo não vale nada se o chão só escuta uma banda.

Os dois rádios do foguete enviam o **mesmo formato e o mesmo conteúdo**, com o **mesmo número de sequência**, defasados em ~500 ms. A estação trata os dois receptores de forma idêntica, deduplica pela sequência e monta uma trajetória única a partir dos dois fluxos.

### Requisito 2 — Log bruto de todo pacote recebido

Todo pacote recebido, **bruto e com timestamp de recepção**, gravado em disco. Isso dá à equipe um registro do voo **mesmo que o foguete ou o cartão nunca sejam recuperados** — que é o cenário contra o qual todo o resto do projeto se protege.

### O que a estação precisa saber sobre o formato

- Duas formas de pacote: **completo (20 B)** e **só-altitude (12 B)**. **Altitude está presente nas duas.**
- Little-endian, layout fixo, documentado na issue 02.
- **Não há CRC de aplicação** — o LoRa já tem CRC de hardware e o driver não entrega pacote reprovado.
- **O campo `t` satura.** É `u16` em decissegundos desde o liftoff, ou seja **65535 ds ≈ 109 minutos**. Isso cobre o voo inteiro (~200 s) e ~107 min de beacon. **Depois da saturação, ordenar e deduplicar pelo número de sequência** (`u16` = 65535 pacotes ≈ 18 h a 1 Hz). Clock drift do cristal do ESP32 (~20 ppm) é desprezível: ~360 ms acumulados em 5 h.
- A flag de **fonte de posição** diz se a posição veio do GPS ou do fallback inercial. **A equipe precisa confiar mais nos fixes de origem GPS** — com uma IMU de 6 eixos sem magnetômetro, a posição inercial degrada em segundos a dezenas de segundos.
- O **bitmap de saúde** e a **fase de voo** em todo pacote são o diagnóstico de bordo visto do chão.
- Nos 3 bits de qualidade de fix, depois do pouso, vai o **número de amostras na média** de recuperação — é o que diz se a equipe caminha para um ponto ou para um círculo.

## Acceptance criteria

- [ ] Requisitos 1 e 2 comunicados por escrito à equipe da estação de solo, com a justificativa de cada um
- [ ] Especificação do formato de pacote (as duas formas) entregue e confirmada como suficiente para escrever o decodificador sem adivinhar
- [ ] Confirmação de que a estação terá **dois** receptores, um por banda
- [ ] Confirmação de que todo pacote recebido será gravado bruto com timestamp de recepção
- [ ] A saturação de `t` em ~109 min e a instrução de ordenar por sequência depois disso explicitamente aceitas
- [ ] Semântica da flag de fonte de posição comunicada, com a limitação honesta do fallback inercial
- [ ] Teste de ponta a ponta antes do voo: foguete transmitindo em bancada, os dois receptores gravando, e os dois logs deduplicados numa trajetória única pela sequência

## Blocked by

- Issue 02 (GPS e pacote completo — o formato precisa estar congelado antes de ser imposto como contrato)
