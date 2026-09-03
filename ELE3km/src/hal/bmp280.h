// hal/bmp280.h — adaptador do barômetro GY-BMP280 (I²C).
//
// Adaptador fino: converte registradores em Pa e °C e nada mais. Nenhuma
// política de voo mora aqui.
//
// TODA operação tem timeout duro (PRD §11). O motivo é o hazard H5: o GND da
// IMU está flutuando nesta revisão da placa, e uma peça nesse estado não deixa
// de responder de forma limpa — ela segura a linha de dados e trava o
// barramento, levando o barômetro junto. Uma leitura bloqueante sem limite trava
// a task inteira. A rotina de recuperação de barramento é da issue 09.
#pragma once

#include <stdint.h>

class TwoWire;

namespace hal {

class Bmp280 {
  public:
    explicit Bmp280(TwoWire& bus) : bus_(bus) {}

    // Sonda 0x76 e depois 0x77, confere o CHIP_ID e aplica a configuração de
    // medição. Devolve false se nenhum dos dois endereços responder um BMP280.
    //
    // Um CHIP_ID 0x60 significa que a peça é um BME280 (mapa de registradores
    // diferente); esta função rejeita, em vez de ler lixo plausível.
    bool begin();

    // Uma leitura compensada. Devolve false em timeout, em erro de barramento ou
    // se begin() não tiver rodado.
    bool read(float& pressure_pa, float& temperature_c);

    // 0 enquanto begin() não tiver encontrado a peça.
    uint8_t address() const { return address_; }

  private:
    bool probe(uint8_t address);
    bool read_calibration();
    bool write_register(uint8_t reg, uint8_t value);
    bool read_registers(uint8_t reg, uint8_t* out, uint8_t len);

    float compensate_temperature(int32_t adc_t);
    float compensate_pressure(int32_t adc_p) const;

    TwoWire& bus_;
    uint8_t  address_ = 0;

    uint16_t dig_t1_ = 0;
    int16_t  dig_t2_ = 0, dig_t3_ = 0;
    uint16_t dig_p1_ = 0;
    int16_t  dig_p2_ = 0, dig_p3_ = 0, dig_p4_ = 0, dig_p5_ = 0;
    int16_t  dig_p6_ = 0, dig_p7_ = 0, dig_p8_ = 0, dig_p9_ = 0;

    int32_t t_fine_ = 0;  // acoplamento de temperatura para a pressão
};

}  // namespace hal
