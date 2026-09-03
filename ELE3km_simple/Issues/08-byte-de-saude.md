# 08 — Byte de saúde honesto

**Tipo:** Bancada
**User stories:** 16, 50

## What to build

Preenche o byte de saúde (byte 18 do pacote, e o campo correspondente do registro)
com o estado real de cada subsistema, para a estação diagnosticar do chão.

Bitmap, bit em 1 = subsistema OK, usando as constantes de `health.h` copiadas:

- bit 0 — IMU (já produziu leitura válida e não está ausente)
- bit 1 — barômetro
- bit 2 — GPS (receptor vivo, "está falando", não "tem fix")
- bit 3 — microSD
- bit 4 — E22: **reservado, sempre 0** (o E22 foi abandonado; mantém o formato)
- bit 5 — SX1276
- bit 6 — **altitude é barométrica (1)** ou caiu para a altitude do GPS por baro
  ausente (0). No fallback não há referência reusada; o bit reflete só "tem baro".

No superloop os bits vêm todos do mesmo lugar (uma thread) — não há publicação
entre tasks como no ELE3km.

## Acceptance criteria

- [ ] Byte de saúde preenchido com os bits reais de IMU/baro/GPS-vivo/SD/SX1276
- [ ] Bit 4 (E22) sempre 0
- [ ] Bit 6 = 1 quando a altitude é barométrica; 0 quando cai para GPS por baro ausente
- [ ] Bit 2 (GPS) reflete "receptor vivo", não "tem fix" (consistente com a issue 05)
- [ ] O mesmo bitmap vai ao pacote e ao registro de log
- [ ] Teste nativo: cada condição de subsistema liga/desliga o bit certo; baro ausente
      apaga bit 1 e bit 6 juntos
- [ ] `pio test -e native` passa; `pio run` compila
- [ ] No target: desconectar um módulo apaga o bit correspondente no pacote recebido
- [ ] As cinco greps de `DISCIPLINE.md` saem vazias

## Blocked by

- 05 (GPS vivo/fix), 06 (SD)
