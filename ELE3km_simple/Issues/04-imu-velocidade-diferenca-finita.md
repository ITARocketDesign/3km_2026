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
