# Disciplina — ELE3km

As oito regras de `Docs/ELE3km_firmware_PRD_v3.md` §12. Elas custam zero código,
valem para todas as issues, e cinco delas custam **hardware** se violadas.

Toda revisão de código passa por esta lista. Os greps do fim do arquivo são a
parte mecanicamente verificável.

| # | Regra | Consequência de violar |
|---|-------|------------------------|
| 1 | **Nenhum código de LED em lugar nenhum.** O pino do LED branco do módulo (GPIO25) é o habilitador de transmissão do E22 | Piscar o LED **liga o PA de 1 W** |
| 2 | **Só a biblioteca de rádio toca os pinos do comutador de RF** (GPIO12 e GPIO25), via `setRfSwitchPins()`. Nunca `digitalWrite()` direto | Os dois habilitadores altos ao mesmo tempo roteiam a saída do PA para o LNA — **módulo destruído** |
| 3 | **Nunca acionar o pino de strapping (GPIO12) antes do boot terminar**, e não montar pull-up externo nele | A placa **não dá boot**: MTDI seleciona a tensão do flash no reset |
| 4 | **Não usar ADC1, sensor Hall, touch nem coprocessador de baixo consumo** | Errata do MCU: GPIO36 e GPIO39, que o E22 usa para BUSY e IRQ, registram pulso espúrio quando o domínio analógico chaveia ⇒ IRQ fantasma |
| 5 | **Nunca confiar numa borda de IRQ sozinha** — sempre confirmar contra o registrador de status do rádio | "Rádio pronto" falso |
| 6 | **Nunca esperar indefinidamente no sinal de ocupado** — timeout limitado mais caminho de recuperação | Travamento permanente da task de I/O |
| 7 | **Não cortar a alimentação do rádio externo entre transmissões** | Não há chave de carga na placa, o standby já é ~2 mA, e o pino de reset não tem pull-up |
| 8 | **Nunca transmitir sem antena conectada**, nos dois rádios | 1 W em circuito aberto **destrói o PA**. O firmware não detecta isto: é regra de operação |

## Invariantes de arquitetura

Não são de §12, mas são igualmente inegociáveis e igualmente baratas:

- O `core/` não tem **nenhum header de Arduino, nenhum relógio global, nenhuma
  variável global**. O tempo entra como parâmetro em toda API.
- O mapa de pinos vive em **exatamente um lugar** (`include/pins.h`) e vem do
  netlist. Nenhum número de pino literal em outro arquivo.
- **Três chip-selects vivos no barramento SPI; exatamente um ativo por vez.**
  A ordem de boot — GPIO18, GPIO23 e GPIO32 em nível alto, todos **antes** de
  `SPI.begin()` — é obrigatória e mora em `hal::board_early_init()`.
- Nenhum teste inspeciona estado interno — só comportamento externo do núcleo.

## Verificação por grep

```bash
# Regra 1 — nenhuma referência a LED no código (este arquivo é a exceção)
grep -rniE "\bled\b|LED_BUILTIN|ledc" src/ include/ test/

# Regra 2 — nenhuma escrita direta nos pinos do comutador de RF
grep -rnE "(pinMode|digitalWrite)\s*\(\s*(PIN_E22_TXEN|PIN_E22_RXEN|12|25)\b" src/

# Regra 4 — nenhum uso do domínio analógico
grep -rniE "analogRead|hallRead|touchRead|ulp_" src/

# Núcleo puro — nenhum header de Arduino no core/
grep -rn "#include <Arduino.h>\|#include <Wire.h>\|#include <SPI.h>" src/core/

# Mapa de pinos único — GPIO literal fora de include/pins.h
grep -rnE "pinMode\([0-9]|digitalWrite\([0-9]" src/
```

Os cinco comandos devem sair vazios. O ambiente `native` é a verificação
executável do quarto: ele compila `src/core` sem nenhum framework Arduino
disponível, então uma dependência acidental quebra o build da suíte.
