# Harness de replay — ELE3km (issue 14)

Transforma um log de voo real no teste de regressão de todas as versões futuras do
estimador. O registro de 64 B (`ELE3km/src/core/log_codec.h`) carrega os valores
**brutos** ao lado dos fundidos, então um voo antigo pode ser reproduzido pelo
núcleo atual no host e as saídas recalculadas comparadas com as gravadas.

A lógica pura vive em `ELE3km/src/core/replay.{h,cpp}` e tem suíte nativa
(`test_replay`). A casca de host — ler o arquivo, separar o voo pelo contador de
boot, imprimir o relatório — é o `tools/replay`.

## Compilar e rodar

```bash
make -C tools replay
tools/replay FLIGHT007.BIN [tolerancia_altitude_m] [tolerancia_posicao_graus]
```

Sem tolerâncias, a comparação é exata (0 m, 0°). O binário sai com código **1** se a
saída recalculada divergir além da tolerância — usável direto num passo de CI. Para
avaliar uma mudança no estimador: recompile o `tools/replay` contra o novo
`src/core` e reproduza um voo conhecido.

## Duas lacunas conhecidas

O registro de 64 B **não** carrega dois campos brutos que a `FlightComputer`
consome, então um voo real **não** se reproduz bit a bit:

- **`gps.altitude_m`** — a altitude MSL da GGA. O estimador corrige o canal vertical
  com ela a cada fix válido; o registro guarda só a altitude derivada do barômetro.
- **`accel_saturated`** — vem do ADC bruto batendo no fundo de escala; se perde na
  conversão para mg.

Por isso a tolerância é declarada, não zero: espere divergência nas janelas com GPS
válido e no boost saturado. Fora delas a reprodução é exata. `imu_valid` é
recuperável (do bit de saúde), então não é lacuna. Trazer os dois campos ao formato
do cartão é uma decisão de contrato à parte (mexe em `log_codec.h`, no
`recover_log`, e obriga a versionar o formato) — não é este harness.

O guarda `record_reconstruction_fields_present()` (checado no arranque do
`tools/replay` e exigido pela suíte) falha alto se alguém **remover** um campo bruto
hoje presente: o harness para de fingir que o voo é reproduzível.

## Adicionar um voo real como caso de regressão

1. Recupere o voo do cartão e **guarde o `.BIN` bruto** (não o CSV): o harness lê o
   binário, e é o binário que preserva o contador de boot e o CRC.
2. Rode `tools/replay voo.BIN` com o `src/core` da versão que **voou** e anote as
   piores divergências. Elas são o "gabarito": a versão que gerou o log reproduz o
   log com divergência zero fora das duas lacunas.
3. Para travar a regressão na suíte nativa, coloque o `.BIN` sob
   `ELE3km/test/test_replay/fixtures/` e acrescente um caso a
   `test_replay.cpp` que:
   - lê o arquivo (o teste roda no host, `fopen` é permitido no teste — só o
     `core/` é que é puro),
   - `decode_header` → contador de boot, `scan_records` → registros,
   - `replay_and_compare` com a `FlightComputerConfig` que voou e uma
     `ReplayTolerance` **declarada** (a folga das duas lacunas mais uma margem),
   - falha se `within_tolerance` for falso.
4. A `config` precisa ser a que voou. Não há persistência de config no cartão: se o
   voo rodou com parâmetros fora do default, fixe-os no caso de teste.

Enquanto não houver voo real, o caso sintético
(`test_synthetic_flight_replays_identically`) é a regressão viva: um log gerado pelo
próprio núcleo, reproduzido com divergência zero. Ele prova o pipeline; o voo real
prova o estimador contra ruído que nenhum dado sintético iguala.
