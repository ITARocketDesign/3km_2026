#include "hal/bmp280.h"

#include <Arduino.h>
#include <Wire.h>

#include "pins.h"

namespace hal {
namespace {

constexpr uint8_t kRegCalibration = 0x88;  // dig_T1..dig_P9, 24 bytes
constexpr uint8_t kRegChipId      = 0xD0;
constexpr uint8_t kRegReset       = 0xE0;
constexpr uint8_t kRegCtrlMeas    = 0xF4;
constexpr uint8_t kRegConfig      = 0xF5;
constexpr uint8_t kRegPressure    = 0xF7;  // press MSB..temp XLSB, 6 bytes

constexpr uint8_t kChipIdBmp280 = 0x58;
constexpr uint8_t kResetCommand = 0xB6;

// osrs_t ×1, osrs_p ×4, modo normal. Tempo de conversão típico ~7 ms, folgado
// para os 25 Hz de amostragem do barômetro (PRD §3).
constexpr uint8_t kCtrlMeas = (0b001 << 5) | (0b011 << 2) | 0b11;
// t_sb 0,5 ms, filtro IIR coeficiente 4. O filtro é pedido pelo hazard H12; ele
// custa atraso numa subida rápida, e é por isso que a pressão bruta vai para o
// log ao lado da altitude derivada.
constexpr uint8_t kConfig = (0b000 << 5) | (0b010 << 2);

// Timeout duro de barramento. 25 ms é ~50× a duração de uma transação de 6 bytes
// a 100 kHz: só estoura se alguém estiver segurando a linha.
constexpr uint16_t kBusTimeoutMs = 25;

}  // namespace

bool Bmp280::begin() {
    bus_.setTimeOut(kBusTimeoutMs);

    address_ = 0;
    if (probe(ADDR_BMP280)) {
        address_ = ADDR_BMP280;
    } else if (probe(ADDR_BMP280_ALT)) {
        address_ = ADDR_BMP280_ALT;
    } else {
        return false;
    }

    if (!write_register(kRegReset, kResetCommand)) {
        address_ = 0;
        return false;
    }
    delay(5);  // t_startup do datasheet é 2 ms

    if (!read_calibration() || !write_register(kRegConfig, kConfig) ||
        !write_register(kRegCtrlMeas, kCtrlMeas)) {
        address_ = 0;
        return false;
    }
    return true;
}

bool Bmp280::read(float& pressure_pa, float& temperature_c) {
    if (address_ == 0) {
        return false;
    }

    uint8_t raw[6];
    if (!read_registers(kRegPressure, raw, sizeof(raw))) {
        return false;
    }

    const int32_t adc_p = (static_cast<int32_t>(raw[0]) << 12) |
                          (static_cast<int32_t>(raw[1]) << 4) |
                          (static_cast<int32_t>(raw[2]) >> 4);
    const int32_t adc_t = (static_cast<int32_t>(raw[3]) << 12) |
                          (static_cast<int32_t>(raw[4]) << 4) |
                          (static_cast<int32_t>(raw[5]) >> 4);

    // A temperatura precisa vir primeiro: ela produz o t_fine que a pressão usa.
    temperature_c = compensate_temperature(adc_t);
    pressure_pa   = compensate_pressure(adc_p);
    return pressure_pa > 0.0f;
}

bool Bmp280::probe(uint8_t address) {
    address_ = address;  // read_registers usa address_
    uint8_t chip_id = 0;
    const bool ok = read_registers(kRegChipId, &chip_id, 1);
    address_ = 0;
    return ok && chip_id == kChipIdBmp280;
}

bool Bmp280::read_calibration() {
    uint8_t c[24];
    if (!read_registers(kRegCalibration, c, sizeof(c))) {
        return false;
    }
    const auto u16 = [&c](int i) {
        return static_cast<uint16_t>(c[i] | (c[i + 1] << 8));
    };
    const auto i16 = [&u16](int i) { return static_cast<int16_t>(u16(i)); };

    dig_t1_ = u16(0);
    dig_t2_ = i16(2);
    dig_t3_ = i16(4);
    dig_p1_ = u16(6);
    dig_p2_ = i16(8);
    dig_p3_ = i16(10);
    dig_p4_ = i16(12);
    dig_p5_ = i16(14);
    dig_p6_ = i16(16);
    dig_p7_ = i16(18);
    dig_p8_ = i16(20);
    dig_p9_ = i16(22);

    return dig_t1_ != 0 && dig_p1_ != 0;
}

bool Bmp280::write_register(uint8_t reg, uint8_t value) {
    bus_.beginTransmission(address_);
    bus_.write(reg);
    bus_.write(value);
    return bus_.endTransmission() == 0;
}

bool Bmp280::read_registers(uint8_t reg, uint8_t* out, uint8_t len) {
    bus_.beginTransmission(address_);
    bus_.write(reg);
    if (bus_.endTransmission(false) != 0) {  // repeated start
        return false;
    }
    if (bus_.requestFrom(address_, len) != len) {
        return false;
    }
    for (uint8_t i = 0; i < len; ++i) {
        out[i] = static_cast<uint8_t>(bus_.read());
    }
    return true;
}

// Compensação em ponto flutuante, direto do datasheet do BMP280 (§8.2).
float Bmp280::compensate_temperature(int32_t adc_t) {
    const float t = static_cast<float>(adc_t);
    const float var1 = (t / 16384.0f - static_cast<float>(dig_t1_) / 1024.0f) *
                       static_cast<float>(dig_t2_);
    const float d = t / 131072.0f - static_cast<float>(dig_t1_) / 8192.0f;
    const float var2 = d * d * static_cast<float>(dig_t3_);

    t_fine_ = static_cast<int32_t>(var1 + var2);
    return (var1 + var2) / 5120.0f;
}

float Bmp280::compensate_pressure(int32_t adc_p) const {
    float var1 = static_cast<float>(t_fine_) / 2.0f - 64000.0f;
    float var2 = var1 * var1 * static_cast<float>(dig_p6_) / 32768.0f;
    var2 = var2 + var1 * static_cast<float>(dig_p5_) * 2.0f;
    var2 = var2 / 4.0f + static_cast<float>(dig_p4_) * 65536.0f;
    var1 = (static_cast<float>(dig_p3_) * var1 * var1 / 524288.0f +
            static_cast<float>(dig_p2_) * var1) /
           524288.0f;
    var1 = (1.0f + var1 / 32768.0f) * static_cast<float>(dig_p1_);
    if (var1 == 0.0f) {
        return 0.0f;  // divisão por zero: peça não calibrada
    }

    float p = 1048576.0f - static_cast<float>(adc_p);
    p = (p - var2 / 4096.0f) * 6250.0f / var1;
    var1 = static_cast<float>(dig_p9_) * p * p / 2147483648.0f;
    var2 = p * static_cast<float>(dig_p8_) / 32768.0f;
    return p + (var1 + var2 + static_cast<float>(dig_p7_)) / 16.0f;
}

}  // namespace hal
