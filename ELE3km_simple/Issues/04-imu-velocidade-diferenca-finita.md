# 04 — IMU + velocidade vertical por diferença finita

**Tipo:** Bancada
**User stories:** 20, 29 (brutos no log); velocidade vertical do pacote

## What to build

Adiciona o canal inercial ao registro e preenche a velocidade vertical do pacote —
sem filtro. É diferença finita, não fusão.

- **IMU a 50 Hz:** lê aceleração e giro brutos a cada volta (o driver `mpu6050`
  copiado marca saturação de fundo de escala). Os brutos vão para os campos de
  aceleração e giro do registro de 64 B; a flag de saturação vai para onde o
  `log_codec` já a espera.
- **Velocidade vertical:** **diferença finita** da pressão-altitude entre leituras de
  baro — `(alt_agora - alt_antes) / Δt`, em dm/s, saturando no alcance do i16. Sem
  integração de acelerômetro, sem peso variável, sem estado de filtro. Uma
  suavização leve de primeira ordem (média móvel curta) é permitida **se** não
  introduzir estado que possa divergir; na dúvida, crua.
- O campo de velocidade vertical entra no pacote (completo e só-altitude) e no
  registro.

Nada de fase, nada de fonte de posição ainda.

## Acceptance criteria

- [ ] IMU lida a 50 Hz; aceleração e giro brutos gravados no registro de 64 B
- [ ] Amostra de IMU no fundo de escala marcada como saturada no registro
- [ ] Velocidade vertical = diferença finita da pressão-altitude, em dm/s, no i16
- [ ] Sem integração inercial e sem estado de filtro (verificável por revisão)
- [ ] Velocidade vertical presente no pacote completo e no só-altitude
- [ ] Teste nativo: diferença finita produz a velocidade esperada para uma sequência
      de altitudes conhecidas com Δt conhecido; satura sem overflow
- [ ] Teste nativo: saturação de IMU propaga para o registro
- [ ] `pio test -e native` passa; `pio run` compila
- [ ] No target: velocidade vertical coerente (sobe/desce a mão muda o sinal)
- [ ] As cinco greps de `DISCIPLINE.md` saem vazias

## Blocked by

- 03 (tracer de altitude, pressão-altitude, registro)

## Estado da implementação (2026-09-02)

Feito e coberto por teste nativo (12 testes no `test/test_survival_computer/`,
os 5 novos deste ciclo). Falta só a verificação de bancada (velocidade vertical
mudando de sinal ao subir/descer a mão), que espera o hardware.

- **IMU bruto no registro:** `SurvivalComputer::update` copia `sample.accel_mg[3]`
  e `sample.gyro_ddps[3]` para os campos homônimos do registro de 64 B (offsets 40
  e 46, já congelados). O `main.cpp` já lia a IMU a 50 Hz para a amostra desde a
  issue 02; esta fatia só passou o bruto adiante.
- **Velocidade vertical por diferença finita:** `(alt_agora − alt_antes)·10000/Δt_ms`,
  em dm/s, saturando no i16 (`saturate_i16`, generalizado a partir do saturador de
  altitude). O Δt é o real **entre leituras FRESCAS de baro** (25 Hz), não o do
  ciclo de 50 Hz — diferir contra o ciclo anterior daria 0 na maioria dos ciclos,
  onde a altitude é só mantida. O primeiro baro não tem anterior → velocidade 0.
  Entra no registro **e** no pacote (só-altitude e completo). Crua: sem integração
  inercial, sem estado de filtro que possa divergir (critério de aceitação).
- **Saturação de IMU → bit 7 de saúde do REGISTRO (`kAccelSat = 1<<7`):** decisão
  deste ciclo. A issue dizia "vai para onde o `log_codec` já a espera", mas não há
  tal lugar: o formato de 64 B está cheio (byte 1 de flags com 8/8 bits) e o
  ELE3km original documenta a saturação como **lacuna conhecida** que o registro
  não carrega (só alimentava o estimador, que este fallback apagou). O único bit
  livre é o 7 do byte de saúde. Ele é gravado **só no byte de saúde do registro**
  (byte 54); o pacote de rádio **não** o recebe — o byte de saúde do ar é contrato
  congelado com o solo. Ao contrário dos outros bits (1 = subsistema OK), 1 aqui é
  evento RUIM (boost clipado). Round-trip pelo `log_codec` congelado preserva o
  bit sem mudar layout nem versão de formato.
- **`main.cpp`:** o diagnóstico de TX agora imprime a velocidade vertical do pacote
  (`vz=%d dm/s`) para a leitura de bancada.

### Definition of Done

1. ✅ `pio test -e native` — 17/17 (5 de contrato + 12 do SurvivalComputer).
2. ✅ `pio run -e heltec_wifi_lora_32_V2` compila.
3. ✅ As cinco greps de `DISCIPLINE.md` saem vazias.
4. ✅ Esta seção.
5. ✅ `NEXT.md` atualizado (04 → ✅, ▶ move para a 05).

### Aberto para a bancada (precisa da placa)

- Velocidade vertical coerente: subir/descer a placa na mão muda o sinal de `vz`.
- ⚠️ O `radio_sx1276.cpp` copiado ainda está em **+2 dBm** (potência de bancada
  reduzida); a TX de voo é +20 dBm — reverter antes do teste de alcance, e nunca
  transmitir sem antena (herdado da issue 03).
