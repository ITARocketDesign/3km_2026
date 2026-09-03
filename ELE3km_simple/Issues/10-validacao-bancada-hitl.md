# 10 — Validação de bancada ponta-a-ponta (HITL contra receptor e replay)

**Tipo:** Bancada
**User stories:** 15, 16, 19, 30 (estação grava tudo), 23 (recuperável)

## What to build

A prova final de que o fallback cumpre a barra de sobrevivência no metal, contra as
ferramentas **existentes** e sem alterá-las. Não é código novo de firmware — é a
sessão de verificação e o registro do resultado.

Roteiro:

1. **Compatibilidade byte-a-byte com o receptor:** com o `3km913hzReceiver` sem
   nenhuma alteração (SF7, 915,019 MHz), confirmar que ele decodifica os dois formatos
   — completo (20 B, com fix) e só-altitude (12 B, sem fix) — com magic/versão/campos
   corretos, sequência incrementando e defasagem coerente com 1 Hz.
2. **A altitude nunca some:** cobrir o baro/derrubar o fix e confirmar que o pacote
   cai para só-altitude sem silêncio, e a altitude segue coerente.
3. **Cartão recuperável:** rodar um voo de bancada, tirar o cartão, e confirmar que a
   ferramenta de replay do ELE3km (issue 14) lê o arquivo — cabeçalho, registros de
   64 B, contador de boot, CRC — sem alteração. Uma queda de energia no meio perde só
   a cauda.
4. **GPS reconfig burro:** resetar o GPS por energia e confirmar que ele volta a
   Airborne <4g em ≤10 s (fix reaparece sob céu aberto após o reenvio da config).
5. **I²C:** soltar/reconectar o baro e confirmar recuperação sem reset da placa.
6. **Boot-loop → beacon SF12:** forçar resets repetidos e confirmar o beacon SF12 num
   receptor trocado para SF12.
7. **Watchdog:** confirmar que um travamento provocado (ex.: segurar o barramento)
   vira reset em vez de silêncio permanente.

O resultado de cada item vira a seção **Estado da implementação** desta issue, com o
que passou, o que ficou pendente de bancada e qualquer desvio observado.

## Acceptance criteria

- [ ] `3km913hzReceiver` sem alteração decodifica 20 B e 12 B corretamente
- [ ] Perda de fix / baro coberto cai para só-altitude sem silêncio; altitude coerente
- [ ] Ferramenta de replay do ELE3km lê o cartão do fallback sem alteração
- [ ] Corte de energia perde só a cauda do log, não o arquivo
- [ ] Reset do GPS → volta configurado em ≤10 s
- [ ] Recuperação I²C sem reset da placa
- [ ] Boot-loop → beacon SF12 ouvido num receptor SF12
- [ ] Travamento provocado → reset (TWDT ou watchdog de TX), não silêncio
- [ ] Seção **Estado da implementação** preenchida com o resultado de cada item

## Blocked by

- 03, 05, 06, 07, 08, 09 (o sistema inteiro precisa estar no ar para a verificação)
