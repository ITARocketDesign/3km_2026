# ELE3km_simple

Firmware de **último recurso** para o computador de bordo ELE3km. Voa só se algo
der errado com o build principal (`../ELE3km`) perto do lançamento. Mesmo hardware
(Heltec WiFi LoRa 32 V2, rev. 1), mesma estação de solo **sem alteração**
(`../3km913hzReceiver`), mesma ferramenta de replay do cartão.

**A ideia:** onde o ELE3km escolhe qualidade ao custo de estado, este escolhe
menos estado ao custo de qualidade. As três missões continuam — gravar o voo,
transmitir ao vivo, achar o foguete depois do pouso — mas com muito menos lugares
onde o firmware pode travar numa transição de estado.

O que sai: máquina de fases (rampa/voo/pousado), detecção de liftoff e de pouso,
filtro de Kalman, fonte de posição, duas tasks FreeRTOS. Um **superloop único a
50 Hz** no lugar. Altitude é **pressão crua ao chão** (pressão-altitude contra um
datum ISA fixo); posição é **GPS bruto**; velocidade vertical é **diferença finita**.

O que fica: config do GPS (reaplicada burramente a cada 10 s), recuperação mínima
do I²C, beacon de sobrevivência por boot-loop, watchdog, e log durável pré-alocado.

- **PRD:** `Docs/ELE3km_simple_PRD.md`
- **Cursor de execução:** `Issues/NEXT.md`
- **Compatibilidade byte-a-byte:** pacote (magic `0xE`, v1, 20 B/12 B) e registro de
  log (64 B) são copiados do ELE3km. O beacon de sobrevivência usa **SF12** — o
  receptor precisa ser trocado para SF12 para ouvi-lo (recompile de uma linha).

Projeto **autônomo**: copia o código testado e os contratos congelados do ELE3km,
não compartilha nenhum arquivo. Uma mudança no ELE3km não pode quebrar este build.
