# Ordem de execução — próxima issue (ELE3km_simple)

Este arquivo é o **cursor de execução** do fallback. Quando eu disser
**"execute next issue"** (com `/tdd`), o agente lê este arquivo, pega a issue
marcada com **▶**, e a implementa. Nada mais precisa ser dito.

O ELE3km_simple é o firmware de **último recurso** — ver `Docs/ELE3km_simple_PRD.md`.
A barra é **sobrevivência**, não paridade: menos estado, menos lugares para travar.

---

## Fila `/tdd`

- ✅ 01 — Esqueleto autônomo + codecs congelados copiados + teste de compatibilidade (2026-09-02)
- ✅ 02 — Superloop de 50 Hz + bring-up da placa + watchdog (2026-09-02, falta verificação de bancada)
- ✅ 03 — Tracer bullet: altitude no ar (pressão-altitude, datum fixo) (2026-09-02, falta verificação de bancada)
- ✅ 04 — IMU + velocidade vertical por diferença finita (2026-09-02, falta verificação de bancada)
- ✅ 05 — GPS: config idempotente reaplicada + pacote completo com porta de fix (2026-09-02, falta verificação de bancada)
- ✅ 06 — Log microSD: arquivo pré-alocado (256 MB), escrita em blocos, registro por ciclo (2026-09-02, falta verificação de bancada)
- **▶ 07 — Recuperação mínima do barramento I²C**
- 08 — Byte de saúde honesto
- 09 — Beacon de sobrevivência por boot-loop + watchdog de cadência de TX
- 10 — Validação de bancada ponta-a-ponta (HITL contra receptor e replay)

As issues 01 e as partes de núcleo puro das seguintes passam por `/tdd` (env
`native`). As partes de HAL/`main.cpp` compilam só no target e são verificadas na
bancada — só o núcleo puro é testável no host, como no ELE3km.

---

## Regra de escopo — vale para toda issue, sem exceção

**Implemente exatamente o que os critérios de aceitação da issue pedem. Nada além.**

Se surgir **qualquer decisão que a issue não resolve** — escolha de arquitetura,
mudança num contrato copiado (formato do pacote `telemetry_codec`, do cartão
`log_codec`, invariantes do `core/`), novo pino, nova dependência, comportamento de
hardware não especificado — **pare e pergunte antes de codar.** Não escolha um default.

Motivo: cinco das regras de `DISCIPLINE.md` custam hardware se violadas, e os
contratos do pacote e do log são **fronteiras congeladas** — o ponto do fallback é
falar com o `3km913hzReceiver` e o replay do ELE3km **sem alteração**. Uma decisão
silenciosa aqui quebra exatamente essa compatibilidade.

## Leitura obrigatória antes de escrever código (toda issue)

- O próprio arquivo da issue, do começo ao fim, incluindo **Estado da implementação**
  se já existir.
- `Docs/ELE3km_simple_PRD.md` (a barra de sobrevivência e a lista de remoções).
- `DISCIPLINE.md` (copiado do ELE3km).
- Quando a issue toca o ar ou o cartão: o `PACKET_FORMAT.md` e o `log_codec.h` do
  ELE3km (os contratos que estamos reusando byte-a-byte).

## Definition of Done (toda issue)

1. Suíte nativa passa (`pio test -e native`).
2. O target compila (`pio run -e heltec_wifi_lora_32_V2`).
3. As cinco greps de `DISCIPLINE.md` saem vazias.
4. A issue ganha/atualiza sua seção **Estado da implementação**.
5. Este `NEXT.md` é atualizado — a issue vira ✅ com a data e o ▶ move para a próxima.
