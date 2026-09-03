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
