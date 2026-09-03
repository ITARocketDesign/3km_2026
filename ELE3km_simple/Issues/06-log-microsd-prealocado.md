# 06 — Log microSD: arquivo pré-alocado, escrita em blocos, registro por ciclo

**Tipo:** Bancada
**User stories:** 20, 22, 23, 24, 25, 26, 27

## What to build

O caminho durável: grava todo ciclo no cartão, num formato recuperável, minimizando
as travadas de coleta de lixo do cartão.

- **Arquivo novo por boot**, nomeado pelo contador de boot (`boot_counter` copiado):
  um reset em voo nunca sobrescreve o anterior. O contador de boot vai em **cada
  registro** (não só no cabeçalho) — é o que impede contaminação entre voos quando o
  arquivo pré-alocado cai nos clusters de um voo antigo.
- **Pré-alocação contígua** de um arquivo grande no boot (SdFat `preAllocate` / arquivo
  contíguo; alvo 256–512 MB). Reduz a churn de FAT/GC que causa as travadas de
  100–250 ms. Ver `Docs/ELE3km_simple_PRD.md` → arquitetura, e a nota de projeto do
  barramento VSPI compartilhado.
- **Escrita em blocos com flush periódico**, tamanho de bloco alinhado ao 512 B do
  cartão (8 registros de 64 B por bloco). A escrita é **sequencial** com a TX — mesma
  thread, sem regra de arbitragem SPI.
- **Cabeçalho de 64 B** no início do arquivo: magic, versão, contador de boot,
  referência barométrica (o datum 101325 Pa), revisão do mapa de pinos, CRC.
- **Grava todo ciclo (50 Hz), sem troca de taxa.** Se o arquivo pré-alocado encher, a
  gravação para e a telemetria continua.
- **Timestamp de aquisição:** o `t_ms` do registro é o instante da leitura do sensor,
  nunca o da escrita.

## Acceptance criteria

- [ ] Arquivo novo por boot, nomeado pelo contador de boot; contador de boot em cada
      registro
- [ ] Arquivo pré-alocado contíguo no boot (alvo 256–512 MB)
- [ ] Escrita em blocos de 512 B (8 registros), flush periódico, sequencial com a TX
- [ ] Cabeçalho de 64 B com magic/versão/boot/referência(101325)/revisão/CRC
- [ ] Registro gravado a cada ciclo (50 Hz); sem lógica de troca de taxa
- [ ] Arquivo cheio → gravação para, telemetria segue (verificável)
- [ ] `t_ms` do registro é da aquisição, não da escrita
- [ ] Teste nativo: o registro codificado é 64 B e o CRC fecha; 8 cabem num bloco de
      512 B sem cruzar fronteira
- [ ] `pio test -e native` passa; `pio run` compila
- [ ] No target: um voo de bancada gera um arquivo que a ferramenta de replay do
      ELE3km lê sem alteração; uma queda de energia perde só a cauda
- [ ] As cinco greps de `DISCIPLINE.md` saem vazias

## Blocked by

- 05 (registro completo com GPS, para gravar o registro inteiro)

## Estado da implementação (2026-09-02)

Fatia `/tdd`: testes de núcleo puro no host; a adoção do caminho durável na HAL e
no superloop compila no target e é verificada na bancada, como nas issues 02–05.

**Núcleo (nativo, `test/test_log/`, 4 testes novos, suíte inteira 26/26):**
- `test_eight_records_fill_one_block_without_crossing` — 8 registros de 64 B
  preenchem um bloco de 512 B exato e cada slot decodifica de volta o seu próprio
  registro (o critério nativo da issue). Fixa `kLogBlockSize == kLogRecordSize*8`.
- `test_header_roundtrips_with_datum` / `test_header_rejects_corrupted_crc` — ida e
  volta do cabeçalho (magic, versão, boot, datum 101325 Pa, revisão de pinos, CRC)
  e rejeição de CRC quebrado.
- `test_scan_returns_this_boot_and_drops_truncated_tail` — a varredura entrega só os
  registros deste boot (barra os do voo anterior nos mesmos clusters) e descarta a
  cauda de um registro cortado por corte de energia.
- O codec (`encode_record/header`, `scan_records`) foi copiado congelado na issue
  01; estes testes o **fixam**, não o introduzem — vão a verde na escrita.

**HAL / superloop (target, compila; verificação de bancada pendente):**
- `main.cpp` lê o contador de boot persistente e chama `g_log.begin(boot, datum)` no
  setup (substitui o `mount()` só-presença da issue 02): cria o arquivo pré-alocado
  contíguo deste boot e escreve o cabeçalho com o datum. No laço, cada ciclo
  serializa `out.record` com o contador de boot e o empilha; ao encher o bloco de
  512 B, grava — sequencial com a TX, uma thread só.
- `kPreallocatedBytes` passou de 8 MB para **256 MB** (alvo da issue; ~23 h a 50 Hz,
  então o arquivo não enche numa missão real). Decisão do tamanho confirmada com o
  operador (256 MB, base da faixa 256–512 MB, por caber contíguo mais fácil que 512).
- **Guarda de arquivo cheio** em `sd_log.cpp::service()`: quando a próxima escrita
  cairia além da reserva contígua, a gravação para (`is_full()`/`is_open()` caem) sem
  estender o arquivo; a telemetria não passa por esta classe e segue no superloop.

**Fora de escopo desta fatia (deixado explícito):**
- `close()` não é chamado: a barra de sobrevivência roda do power-on à morte da
  bateria, sem gatilho de desligamento; a recuperação por magic + CRC tolera o
  arquivo nunca fechado. As duas escritas de metadados viram uma só (o cabeçalho).
- O bit `kSd` do byte de saúde continua apagado: o bitmap honesto (IMU/baro/GPS/SD/
  SX1276 a partir de `IoSubsystemHealth`) é da issue 08. Nada aqui o toca.

**Verificação de bancada pendente (critérios que só o target fecha):**
- Um voo de bancada gera um arquivo que o replay do ELE3km lê sem alteração; um corte
  de energia perde só a cauda.
- Tempo de pré-alocação de 256 MB no boot e presença de região contígua no cartão de
  ensaio (se faltar, `begin()` devolve false e o guarda de cheio é a rede).
