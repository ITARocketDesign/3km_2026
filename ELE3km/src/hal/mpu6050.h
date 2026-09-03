// hal/mpu6050.h — adaptador do acelerômetro/giroscópio MPU6050 (I²C).
//
// Adaptador fino: converte registradores em mg e em décimos de grau/s, marca
// saturação de fundo de escala, e nada mais. Nenhuma política de voo mora aqui — a
// fusão, a compensação de gravidade e a máquina de fases são do núcleo.
//
// TODA operação tem timeout duro (PRD §11), pelo mesmo motivo do BMP280: nesta
// revisão da placa o GND da IMU flutua (hazard H5). Uma peça nesse estado segura a
// linha de dados e trava o barramento; uma leitura bloqueante sem limite trava a
// task inteira. A recuperação de barramento é da issue 09 — aqui só o timeout.
//
// ── Fundo de escala e saturação ─────────────────────────────────────────────
//
// O acelerômetro roda em ±16 g e o giroscópio em ±2000 °/s: o boost de um foguete
// passa de 10 g e a rolagem passa de centenas de °/s, e uma escala menor satura no
// exato instante que mais importa. Quando um eixo bate no fundo de escala, a
// amostra é MARCADA como saturada: o estimador (issue 07) exclui essa aceleração
// do passo de predição em vez de integrar um valor clipado como se fosse real.
#pragma once

#include <stdint.h>

class TwoWire;

namespace hal {

class Mpu6050 {
  public:
    explicit Mpu6050(TwoWire& bus) : bus_(bus) {}

    // Sonda 0x68 e depois 0x69, confere o WHO_AM_I, acorda a peça e aplica as
    // escalas de medição. Devolve false se nenhum dos dois endereços responder um
    // MPU6050.
    bool begin();

    // Uma leitura: aceleração dos três eixos em mg, velocidade angular em décimos
    // de grau/s, e se algum eixo do acelerômetro bateu no fundo de escala.
    // Devolve false em timeout, em erro de barramento ou se begin() não rodou.
    bool read(int16_t accel_mg[3], int16_t gyro_ddps[3], bool& accel_saturated);

    // 0 enquanto begin() não tiver encontrado a peça.
    uint8_t address() const { return address_; }

  private:
    bool probe(uint8_t address);
    bool write_register(uint8_t reg, uint8_t value);
    bool read_registers(uint8_t reg, uint8_t* out, uint8_t len);

    TwoWire& bus_;
    uint8_t  address_ = 0;
};

}  // namespace hal
