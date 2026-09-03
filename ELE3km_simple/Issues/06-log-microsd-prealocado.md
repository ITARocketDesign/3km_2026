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
