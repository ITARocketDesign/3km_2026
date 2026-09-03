#include "hal/i2c_bus.h"

#include <Arduino.h>
#include <Wire.h>

#include "pins.h"

namespace hal {
namespace {

// Meio período de clock. A 100 kHz o meio período nominal é 5 µs; usamos uma folga
// para que um escravo lento — o MPU6050 parasita da H5 — enxergue cada borda.
// Não é caminho quente: a recuperação inteira são ~9 pulsos, dezenas de µs.
constexpr uint32_t kHalfClockUs = 5;

// Um byte de 8 bits mais o bit de ACK: o suficiente para um escravo preso no meio
// de uma transferência interrompida completá-la e soltar a linha de dados.
constexpr int kRecoveryPulses = 9;

}  // namespace

void i2c_bus_recover() {
    // Solta o driver: a partir daqui SDA e SCL são GPIO comuns, não do periférico.
    Wire.end();

    // Passo 1 — soltar a linha de dados. SDA como entrada (os pull-ups do barramento
    // a mantêm alta); se um escravo travado a estiver segurando baixa, o pull-up não
    // a levanta sozinho — é o clock abaixo que a solta. SCL fica sob nosso controle,
    // ocioso alto.
    pinMode(PIN_I2C_SDA, INPUT_PULLUP);
    pinMode(PIN_I2C_SCL, OUTPUT);
    digitalWrite(PIN_I2C_SCL, HIGH);
    delayMicroseconds(kHalfClockUs);

    // Passo 2 — pulsar o clock ~9 vezes. A cada pulso o escravo avança um bit; assim
    // que ele solta SDA (linha volta a subir), não há mais o que clocar e paramos.
    for (int i = 0; i < kRecoveryPulses; ++i) {
        digitalWrite(PIN_I2C_SCL, LOW);
        delayMicroseconds(kHalfClockUs);
        digitalWrite(PIN_I2C_SCL, HIGH);
        delayMicroseconds(kHalfClockUs);
        if (digitalRead(PIN_I2C_SDA) == HIGH) {
            break;
        }
    }

    // Passo 3 — condição de STOP: SDA sobe de baixo para alto ENQUANTO SCL está alto.
    // Deixa o barramento num estado ocioso limpo antes do begin().
    pinMode(PIN_I2C_SDA, OUTPUT);
    digitalWrite(PIN_I2C_SCL, LOW);
    delayMicroseconds(kHalfClockUs);
    digitalWrite(PIN_I2C_SDA, LOW);
    delayMicroseconds(kHalfClockUs);
    digitalWrite(PIN_I2C_SCL, HIGH);
    delayMicroseconds(kHalfClockUs);
    digitalWrite(PIN_I2C_SDA, HIGH);  // baixo→alto com SCL alto = STOP
    delayMicroseconds(kHalfClockUs);

    // Passo 4 — reinicializar o driver na MESMA taxa de 100 kHz (H9: SDA também
    // controla o Vext do módulo; 400 kHz é a causa #1 de barramento instável depois
    // do H5). O timeout duro por operação é reaplicado pelo begin() do dispositivo.
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, I2C_HZ);
}

}  // namespace hal
