# 05 — GPS: config idempotente reaplicada + pacote completo com porta de fix

**Tipo:** Bancada
**User stories:** 8, 35, 36, 37, 45, 12 (só o rótulo de fonte, sem ponte inercial)

## What to build

Traz a posição ao pacote e conserta o GPS — sem a máquina de detecção de reset.

- **Config UBX no boot e reaplicada burramente:** blast da config inteira no boot —
  **Airborne <4g**, 5 Hz, sentenças NMEA inúteis desligadas, buffer de recepção da
  UART em **512 B** — e **reenvio incondicional da mesma config a cada ~10 s**, para
  sempre. Um receptor que resetou silenciosamente (voltando a *Portable*) é
  reconfigurado em ≤10 s. **Nenhuma** detecção de restart de NMEA, **nenhuma**
  heurística de sats-a-zero-após-TX, **nenhum** modo Stationary no pouso. A config é
  idempotente; reenviar quando nada resetou é inofensivo.
- **Parse NMEA:** o parser `nmea` copiado alimenta lat/lon brutos, satélites, HDOP e
  qualidade de fix na amostra.
- **Porta de fix e forma do pacote:** fix válido (**satélites ≥ 4 e HDOP ≤ 5,0**) →
  **pacote completo de 20 B** com lat/lon brutos e **fonte de posição = 1 (GPS)**;
  sem fix → **pacote só-altitude de 12 B** com **fonte = 0 (nenhuma)**. Os campos de
  satélites/HDOP/qualidade seguem preenchidos nos dois casos, e o bit 2 de saúde
  (GPS vivo) segue a "está falando", não "tem fix".
- **Brutos no registro:** lat/lon brutos vão para os campos brutos do registro de
  64 B; os campos de posição fundida recebem os **mesmos brutos** (não há fusão).

## Acceptance criteria

- [ ] Config UBX (Airborne <4g, 5 Hz, sentenças inúteis off, UART 512 B) enviada no
      boot e **reenviada a cada ~10 s incondicionalmente**
- [ ] Nenhuma detecção de reset do GPS e nenhum modo Stationary (verificável por
      revisão — é o ponto da simplificação)
- [ ] Parse NMEA preenche lat/lon, satélites, HDOP, qualidade de fix
- [ ] Fix válido = satélites ≥ 4 **e** HDOP ≤ 5,0 → pacote 20 B, fonte = 1
- [ ] Sem fix → pacote 12 B, fonte = 0, com satélites/HDOP/qualidade ainda preenchidos
- [ ] Bit 2 de saúde reflete "receptor vivo", não "tem fix"
- [ ] Campos de posição fundida do registro = brutos do GPS
- [ ] Teste nativo: a porta de fix escolhe 20 B vs 12 B nos limites (sats = 3/4,
      HDOP = 5,0/5,1)
- [ ] Teste nativo: ida e volta do pacote completo com lat/lon conhecidos
- [ ] `pio test -e native` passa; `pio run` compila
- [ ] No target: com fix, o receptor decodifica lat/lon coerentes; sem fix, cai para
      só-altitude sem silêncio
- [ ] As cinco greps de `DISCIPLINE.md` saem vazias

## Blocked by

- 04 (IMU, velocidade, registro completo)

## Estado da implementação (2026-09-02)

Feito e coberto por teste nativo (17 testes no `test/test_survival_computer/`, os
4 novos deste ciclo + a suíte de contrato). Falta só a verificação de bancada
(receptor decodificando lat/lon coerentes com fix; queda para só-altitude sem
silêncio sem fix; config Airborne pegando).

### Núcleo — porta de fix (por TDD, `native`)

- **Porta de fix em `SurvivalComputer::update`:** `sats ≥ 4 && hdop_half ≤ 10`.
  `hdop_half` é HDOP × 2, então HDOP ≤ 5,0 é `hdop_half ≤ 10`. Crua — sem
  staleness, sem última-válida: a máquina de fonte (None/Ins/LastValid) continua
  na issue 08. Fix bom → **pacote completo de 20 B**, `latitude/longitude` brutos,
  `position_source = Gps`. Sem fix → **só-altitude de 12 B**, `position_source =
  None`. `satellites`/`hdop_half`/`fix_quality` seguem no pacote nas **duas**
  formas.
- **Bit 2 de saúde (`kGps`)** segue `gps.receiving` ("o receptor está falando"),
  não "tem fix" — no byte de saúde do pacote **e** do registro. Um receptor vivo
  sem fix acende o bit e cai para só-altitude; um mudo o apaga.
- **Registro:** posição fundida (`fused_latitude/longitude_1e7`, offsets 32/36) =
  bruta do GPS — não há fusão nesta barra; fica ao lado da bruta (`r.gps`, offsets
  24/28), nunca no lugar dela. `record.position_source` acompanha a porta de fix,
  coerente com o pacote do mesmo ciclo.
- **Limite de HDOP e a quantização:** a issue pede o teste em "HDOP = 5,0/5,1",
  mas `hdop_half` quantiza em passos de 0,5 (é o campo do pacote e do log), então
  5,0 e 5,1 caem no mesmo `hdop_half = 10`. O teste de limite usa o degrau
  representável: `hdop_half = 10` (HDOP 5,0) aprova, `hdop_half = 11` (HDOP 5,5)
  reprova. A porta opera sobre `hdop_half`, o único HDOP que a amostra carrega.

### HAL — config burra (só bancada; `src/hal` não roda no `native`)

- **`hal/gps_neo6m` reescrito para a barra:** reaplica a config UBX **inteira a
  cada ~10 s, incondicionalmente** (`kReconfigureIntervalMs = 10000`, no
  `service()`). **Removidos os dois detectores de reset** (sentença não-GGA no
  fluxo; satélites-a-zero-após-TX), o `note_transmission()` que alimentava o
  segundo, e o `request_reconfigure()` com piso entre reconfigurações.
- **Removido o UBX-CFG-CFG** (save na memória com bateria de backup): a barra não
  depende da célula do breakout; a reaplicação periódica é o que traz um receptor
  resetado de volta a Airborne. Ficam NAV5 (Airborne <4g), CFG-MSG (GGA on, resto
  off) e CFG-RATE (5 Hz).
- **`main.cpp`:** já alimentava `sample.gps` a partir de `g_gps.fix()` desde a
  issue 02; esta fatia só ajustou o banner e passou a imprimir a forma do pacote
  (completo/só-altitude, bytes, fonte) no diagnóstico de TX para a leitura de
  bancada.

### Definition of Done

1. ✅ `pio test -e native` — 22/22 (5 de contrato + 17 do SurvivalComputer).
2. ✅ `pio run -e heltec_wifi_lora_32_V2` compila.
3. ✅ As cinco greps de `DISCIPLINE.md` saem vazias.
4. ✅ Esta seção.
5. ✅ `NEXT.md` atualizado (05 → ✅, ▶ move para a 06).

### Aberto para a bancada (precisa da placa e do receptor)

- Com fix, o `3km913hzReceiver` **sem alteração** decodifica lat/lon coerentes com
  a posição local; sem fix, o firmware cai para só-altitude **sem silêncio**.
- Config Airborne <4g pegando: revisão confirma que não há detecção de reset; a
  bancada confirma o reenvio a cada ~10 s (contador `reconfigure_count` subindo).
- ⚠️ O `radio_sx1276.cpp` copiado ainda está em **+2 dBm** (potência de bancada
  reduzida); a TX de voo é +20 dBm — reverter antes do teste de alcance, e nunca
  transmitir sem antena (herdado das issues 03/04).
