# Formato de pacote — ELE3km

Especificação do que sai pelo ar, para quem escreve o decodificador da estação de
solo. A fonte é `src/core/telemetry_codec.h`; este documento é a mesma coisa em
prosa. Se os dois divergirem, o código vence e este arquivo está desatualizado.

O contrato mais amplo com a estação de solo — o que ela precisa exibir, o que
precisa registrar, o que precisa fazer quando um pacote não decodifica — é da
issue 16. Aqui está só o formato.

## O que é comum às duas formas

- **Little-endian**, em todos os campos multibyte.
- **Sem CRC de aplicação.** O LoRa já tem CRC de hardware e o driver não entrega
  pacote reprovado. (No registro do cartão o CRC permanece: lá não existe camada
  de enlace.)
- **Os dois rádios enviam o mesmo formato e o mesmo conteúdo.** O SX1276
  (915–928 MHz) e o E22 (433 MHz) são dois receptores do mesmo fluxo, defasados
  ~500 ms. A estação trata os dois de forma idêntica e deduplica pelo número de
  sequência.
- **Altitude está presente em todo pacote.** É o payload prioritário, vem do
  barômetro, e não depende do GPS.

## As duas formas

Elas se distinguem pelo **comprimento do payload**, que o cabeçalho explícito do
LoRa entrega ao receptor. **Não há campo de tipo.** Qualquer comprimento que não
seja 12 ou 20 deve ser descartado.

### Pacote completo — 20 B

Transmitido quando há posição em que confiar.

| Off | Tipo | Campo |
|---|---|---|
| 0 | `u8` | magic (4 bits altos) + versão (4 bits baixos) |
| 1 | `u16` | sequência |
| 3 | `u16` | tempo, em decissegundos |
| 5 | `i32` | latitude × 1e7 |
| 9 | `i32` | longitude × 1e7 |
| 13 | `i16` | altitude, metros acima da referência |
| 15 | `i16` | velocidade vertical, dm/s |
| 17 | `u8` | flags |
| 18 | `u8` | saúde |
| 19 | `u8` | GPS: satélites + HDOP |

### Pacote só-altitude — 12 B

Idêntico, **sem latitude e longitude**. Transmitido quando não há fix.

| Off | Tipo | Campo |
|---|---|---|
| 0 | `u8` | magic + versão |
| 1 | `u16` | sequência |
| 3 | `u16` | tempo, em decissegundos |
| 5 | `i16` | altitude, metros acima da referência |
| 7 | `i16` | velocidade vertical, dm/s |
| 9 | `u8` | flags |
| 10 | `u8` | saúde |
| 11 | `u8` | GPS: satélites + HDOP |

Um pacote só-altitude **não** significa GPS morto. Os campos de satélites, HDOP e
qualidade continuam preenchidos, e o bit de saúde do GPS continua ligado se o
receptor está falando — é assim que se distingue "sem fix debaixo de céu ruim" de
"receptor mudo".

## Byte 0 — magic e versão

- bits 4–7: magic, **`0xE`**
- bits 0–3: versão, **`0x1`**

Pacote com magic ou versão diferentes é descartado sem tentar interpretar.

## Byte de flags

| Bits | Campo | Valores |
|---|---|---|
| 0–2 | fase de voo | 0 = na rampa, 1 = em voo, 2 = pousado |
| 3–4 | fonte de posição | 0 = nenhuma, 1 = GPS, 2 = inercial, 3 = última válida |
| 5–7 | qualidade do fix **/** amostras pós-pouso | ver abaixo |

Os **bits 5–7 mudam de significado com a fase**, e é a própria fase (bits 0–2) que
diz qual ler (issue 13):

- **Em voo (fase 0 ou 1):** qualidade do fix — indicador da sentença GGA, 0–7
  (0 = sem fix).
- **Pousado (fase 2):** número de **amostras de GPS pós-pouso** já acumuladas na
  média da posição transmitida, 0–7 (satura em 7 = "7 ou mais"). Depois do pouso o
  firmware não persegue mais a trajetória: ele filtra os fixes (satélites ≥ 4 e
  HDOP ≤ 5,0) e transmite a **média** dos aceitos. Este campo é o que diz ao
  operador se a posição é um ponto (≥ 3 amostras) ou um círculo largo (0–2). Com 0
  amostras, a posição é a última válida de voo e a fonte é 3 (última válida).

Não há campo novo: o formato de 20 B é orçamento de airtime gasto e não pode
crescer, então o campo de qualidade de fix — que não tem uso pousado — carrega as
amostras. A estação de solo desambigua pela fase.

A **fonte de posição** é o campo que diz quanto confiar na latitude e longitude.
Fonte inercial é uma ponte curta de dez a vinte segundos, não navegação: com uma
IMU de 6 eixos e sem magnetômetro, a posição horizontal integrada diverge rápido.
Um pacote com fonte 2 ou 3 merece menos peso que um com fonte 1.

A fase de voo passou a ser real na issue 06 (0 rampa, 1 voo, 2 pousado). A fonte
de posição acima de 0 chega na issue 08; até lá o firmware transmite 0 nela.

## Byte de saúde

Bitmap, **bit em 1 = subsistema OK**.

| Bit | Subsistema |
|---|---|
| 0 | IMU |
| 1 | barômetro |
| 2 | GPS (receptor vivo, não "com fix") |
| 3 | microSD |
| 4 | E22 |
| 5 | SX1276 |
| 6 | referência de altitude confiável |

O bit 6 (issue 06) é diferente dos outros: não é a saúde de um módulo, e sim se a
altitude do pacote é relativa à referência barométrica de solo (bit em 1) ou se
caiu para a altitude do GPS porque a referência reusada de um reset foi julgada
absurda (bit em 0). Quando ele apaga, a altitude mudou de fonte e de significado.

## Byte de GPS

- bits 0–3: **satélites**, saturando em 15
- bits 4–7: **HDOP × 2**, saturando em 15 — ou seja, 0,0 a 7,5 em passos de 0,5

Um HDOP lido como 15 significa "7,5 ou pior", não "7,5".

## Campo de tempo, e a saturação em 109 minutos

O tempo é `u16` em **decissegundos desde o liftoff** (antes do liftoff, desde o
boot). Ele **satura em 65535 ds ≈ 109 minutos** e ali fica parado.

Isso cobre o voo inteiro (~200 s) e cerca de 107 minutos de beacon pós-pouso.
Passado esse ponto, **a estação de solo deve ordenar e deduplicar pelo número de
sequência**, não pelo campo de tempo — que a partir daí é constante e não ordena
mais nada. Uma busca de recuperação que se arraste por mais de duas horas cai
exatamente nesse regime.

O número de sequência é `u16`, **global e compartilhado pelos dois rádios**: os
dois transmitem o mesmo pacote com o mesmo número, defasados ~500 ms. Ele dá a
volta em 65536 pacotes ≈ 18 h a 1 Hz — a estação precisa tratar o wrap.

O drift do cristal do ESP32 (~20 ppm) é desprezível para esse uso: ~360 ms
acumulados em 5 h.

## Por que 20 bytes, e não 22

Airtime LoRa é quantizada em blocos de 8 símbolos. No fator de espalhamento
escolhido para o voo isso são ~16 ms por degrau, e o teto do bucket-alvo é 21
bytes. Um pacote de 22 B pula um degrau e fura o teto de ciclo de trabalho já
dimensionado nos docs de hardware.

O tamanho não é folga de projeto: é orçamento gasto. Campo novo no pacote implica
refazer a tabela de airtime e o orçamento de bateria.
