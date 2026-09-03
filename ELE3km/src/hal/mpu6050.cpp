#include "hal/mpu6050.h"

#include <Arduino.h>
#include <Wire.h>

#include "pins.h"

namespace hal {
namespace {

constexpr uint8_t kRegWhoAmI      = 0x75;
constexpr uint8_t kRegPwrMgmt1    = 0x6B;
constexpr uint8_t kRegSmplrtDiv   = 0x19;
constexpr uint8_t kRegConfig      = 0x1A;
constexpr uint8_t kRegGyroConfig  = 0x1B;
constexpr uint8_t kRegAccelConfig = 0x1C;
constexpr uint8_t kRegAccelXoutH  = 0x3B;  // accel[6] + temp[2] + gyro[6] = 14 bytes

constexpr uint8_t kWhoAmIMpu6050 = 0x68;

// PWR_MGMT_1: acorda a peça (sai do sleep) e usa o PLL do giroscópio X como clock,
// mais estável que o oscilador interno.
constexpr uint8_t kPwrMgmt1Wake = 0x01;
// DLPF ~44 Hz de acelerômetro / 42 Hz de giroscópio: corta o ruído de vibração do
// motor sem atrasar demais o boost.
constexpr uint8_t kConfigDlpf = 0x03;
// SMPLRT_DIV = 0: taxa interna cheia (1 kHz com este DLPF), decimada na task a 100 Hz.
constexpr uint8_t kSmplrtDiv = 0x00;
// Fundo de escala: giroscópio ±2000 °/s, acelerômetro ±16 g (bits FS_SEL/AFS_SEL).
constexpr uint8_t kGyroConfig2000dps = 0x03 << 3;
constexpr uint8_t kAccelConfig16g    = 0x03 << 3;

// Conversão de contagem para unidade física. ±16 g → 2048 LSB/g; ±2000 °/s →
// 16,4 LSB/(°/s). As unidades (mg e décimos de grau/s) são as do log de 64 B.
constexpr float kAccelMgPerLsb = 1000.0f / 2048.0f;
constexpr float kGyroDdpsPerLsb = 10.0f / 16.4f;

// Fundo de escala do ADC de 16 bits: um eixo aqui está clipado.
constexpr int16_t kAdcMax = 32767;
constexpr int16_t kAdcMin = -32768;

// Timeout duro de barramento, como no BMP280: ~50× a duração de uma transação de
// 14 bytes a 100 kHz. Só estoura se alguém estiver segurando a linha (H5).
constexpr uint16_t kBusTimeoutMs = 25;

int16_t saturate_i16(float value) {
    if (value > 32767.0f) return kAdcMax;
    if (value < -32768.0f) return kAdcMin;
    return static_cast<int16_t>(value);
}

}  // namespace

bool Mpu6050::begin() {
    bus_.setTimeOut(kBusTimeoutMs);

    address_ = 0;
    if (probe(ADDR_MPU6050)) {
        address_ = ADDR_MPU6050;
    } else if (probe(ADDR_MPU6050_ALT)) {
        address_ = ADDR_MPU6050_ALT;
    } else {
        return false;
    }

    if (!write_register(kRegPwrMgmt1, kPwrMgmt1Wake)) {
        address_ = 0;
        return false;
    }
    delay(5);  // estabilização do PLL

    if (!write_register(kRegConfig, kConfigDlpf) ||
        !write_register(kRegSmplrtDiv, kSmplrtDiv) ||
        !write_register(kRegGyroConfig, kGyroConfig2000dps) ||
        !write_register(kRegAccelConfig, kAccelConfig16g)) {
        address_ = 0;
        return false;
    }
    return true;
}

bool Mpu6050::read(int16_t accel_mg[3], int16_t gyro_ddps[3], bool& accel_saturated) {
    if (address_ == 0) {
        return false;
    }

    uint8_t raw[14];
    if (!read_registers(kRegAccelXoutH, raw, sizeof(raw))) {
        return false;
    }

    const auto be_i16 = [&raw](int i) {
        return static_cast<int16_t>((static_cast<uint16_t>(raw[i]) << 8) | raw[i + 1]);
    };

    accel_saturated = false;
    for (int axis = 0; axis < 3; ++axis) {
        const int16_t a = be_i16(axis * 2);          // accel: bytes 0..5
        const int16_t g = be_i16(8 + axis * 2);      // gyro:  bytes 8..13 (temp em 6..7)
        if (a >= kAdcMax || a <= kAdcMin) {
            accel_saturated = true;
        }
        accel_mg[axis]   = saturate_i16(static_cast<float>(a) * kAccelMgPerLsb);
        gyro_ddps[axis]  = saturate_i16(static_cast<float>(g) * kGyroDdpsPerLsb);
    }
    return true;
}

bool Mpu6050::probe(uint8_t address) {
    address_ = address;  // read_registers usa address_
    uint8_t who = 0;
    const bool ok = read_registers(kRegWhoAmI, &who, 1);
    address_ = 0;
    return ok && who == kWhoAmIMpu6050;
}

bool Mpu6050::write_register(uint8_t reg, uint8_t value) {
    bus_.beginTransmission(address_);
    bus_.write(reg);
    bus_.write(value);
    return bus_.endTransmission() == 0;
}

bool Mpu6050::read_registers(uint8_t reg, uint8_t* out, uint8_t len) {
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

}  // namespace hal
