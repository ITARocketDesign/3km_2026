# E22 — ajuste da tensão do TCXO (bancada)

Quando o boot imprime `E22: FALHOU`, o suspeito nº 1 é a tensão do TCXO. O
E22-400M30S usa um TCXO controlado pelo DIO3 do SX1268, e a tensão certa **não
está registrada em nenhum documento do projeto** (nem netlist, nem manual) — só
dá para descobrir testando no target.

`E22: FALHOU` é falha de **inicialização SPI** (`begin()` não achou/configurou o
chip), não de antena. Antena solta não faz o `begin()` falhar — mas conecte-a
mesmo assim antes de qualquer transmissão, pois o PA de 1 W pode se danificar sem
carga.

## A constante

Arquivo: `ELE3km/src/hal/radio_e22.cpp`

```cpp
constexpr float kTcxoVoltage = 1.8f;
```

Depois de trocar o valor: recompilar e regravar o foguete.

```bash
~/.platformio/penv/bin/pio run -e heltec_wifi_lora_32_V2 -t upload
```

## Valores a testar, em ordem

| Ordem | Valor    | Significado                                        |
|-------|----------|----------------------------------------------------|
| 1     | `1.8f`   | Default da série E22 da Ebyte (valor atual)        |
| 2     | `1.6f`   | Default da biblioteca RadioLib                     |
| 3     | `0.0f`   | Sem TCXO — o SX1268 usa o oscilador XTAL interno   |

Outros valores válidos que o SX1268 aceita, se os três acima falharem:
`1.7f`, `2.4f`, `2.7f`, `3.0f`, `3.3f`.

Sucesso = o boot imprime `E22: ok, 433 MHz, SF8, +30 dBm` em vez de `E22: FALHOU`.

## Registro dos testes

| Data | Valor testado | Resultado (`ok` / `FALHOU`) | Observação |
|------|---------------|-----------------------------|------------|
|      | 1.8f          |                             |            |
|      |               |                             |            |
|      |               |                             |            |
