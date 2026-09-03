// core/types.h — tipos compartilhados do núcleo puro.
//
// Contrato do core/ (PRD v3 §1, user story 100), válido para todo arquivo deste
// diretório e para todas as issues seguintes:
//   • nenhum header de Arduino
//   • nenhuma leitura de relógio global — o tempo entra como parâmetro
//   • nenhuma variável global
//   • nenhuma alocação dinâmica
#pragma once

#include <stdint.h>

namespace core {

// Uma solução do receptor GPS, já convertida para as unidades do pacote.
//
// `receiving` e `valid` são coisas diferentes e o pacote precisa das duas: o
// receptor pode estar vivo e falando NMEA sem ter fix (bit de saúde ligado,
// posição inutilizável), e pode estar mudo (bit apagado).
struct GpsFix {
    bool    receiving = false;     // chegou sentença NMEA recentemente
    bool    valid = false;         // há fix utilizável, e ele não está velho
    int32_t latitude_1e7 = 0;      // graus × 1e7
    int32_t longitude_1e7 = 0;     // graus × 1e7
    float   altitude_m = 0.0f;     // altitude MSL da GGA; só o fallback da issue 06 a usa
    uint8_t fix_quality = 0;       // indicador da GGA, 0–7 (0 = sem fix)
    uint8_t satellites = 0;        // saturando em 15
    uint8_t hdop_half = 0;         // HDOP × 2, saturando em 15
};

// Uma leitura de sensores, com o instante em que foi adquirida (PRD §8:
// timestamp na aquisição, nunca na gravação).
//
// O GPS entra aqui como o melhor fix conhecido no instante da amostra, não como
// "chegou fix agora": a 25 Hz de barômetro contra 5 Hz de GPS, a maioria das
// amostras repete o fix anterior. A IMU (issue 07) entra como campo novo aqui.
struct SensorSample {
    bool   baro_valid = false;     // a leitura desta amostra é confiável
    float  pressure_pa = 0.0f;     // pressão bruta, Pa
    float  temperature_c = 0.0f;   // temperatura bruta, °C
    GpsFix gps;

    // Aceleração bruta dos três eixos, mg (issue 07 preenche pelo MPU6050). A
    // máquina de fases (issue 06) consome o MÓDULO |a|: liftoff quando passa de
    // ~2,5 g sustentado, pouso quando volta a ~1 g. Sem `imu_valid`, o driver vale
    // zero, e sem aceleração não há liftoff — a máquina fica na rampa.
    int16_t accel_mg[3] = {0, 0, 0};

    // Velocidade angular bruta dos três eixos, décimos de grau/s (issue 07). Vai
    // ao log ao lado da aceleração; o estimador desta fatia não a consome — sem
    // magnetômetro a atitude horizontal não é observável de qualquer forma.
    int16_t gyro_ddps[3] = {0, 0, 0};

    // A leitura inercial desta amostra é fresca e confiável (issue 07). Falso é o
    // default honesto de quem não tem IMU: o estimador prediz o canal vertical em
    // velocidade constante em vez de integrar uma aceleração que não existe.
    bool imu_valid = false;

    // Ao menos um eixo do acelerômetro bateu no fundo de escala nesta amostra
    // (issue 07). Dado de boost clipado não pode ser integrado como válido: o
    // estimador exclui a aceleração do passo de predição e infla a covariância.
    bool accel_saturated = false;

    // Diagnóstico do caminho de aquisição, não do sensor (PRD §18, user story
    // 34): quantas vezes o buffer da UART do GPS foi encontrado cheio. Sem ação
    // corretiva — a sentença perdida já foi perdida. Se subir durante o voo, o
    // timeout de I²C está roubando tempo demais da drenagem.
    uint32_t gps_uart_overflows = 0;
};

// PRD §9. A transição para Landed é de mão única.
enum class FlightPhase : uint8_t {
    Rail   = 0,
    Flight = 1,
    Landed = 2,
};

// PRD §1, máquina de fonte de posição (issue 08).
enum class PositionSource : uint8_t {
    None      = 0,  // sem posição ainda
    Gps       = 1,
    Ins       = 2,  // ponte inercial curta
    LastValid = 3,  // última posição GPS válida, com idade
};

// As duas formas do pacote de rádio. O receptor as distingue pelo COMPRIMENTO
// do payload — não há campo de tipo (telemetry_codec.h).
enum class PacketForm : uint8_t {
    AltitudeOnly = 0,  // 12 B
    Full         = 1,  // 20 B, com latitude e longitude
};

// Único rádio da placa: o SX1276 onboard do Heltec. O E22 de 433 MHz foi removido
// do projeto — bench bring-up falhou por trilha de MOSI aberta e o rail U6 vinha
// alto demais (Docs/ELE3km_drop_e22_single_radio.md). O enum é de um valor só de
// propósito: o campo `radio` do pacote continua existindo, mas só há um destino.
enum class Radio : uint8_t {
    Sx1276 = 0,  // onboard, 915–928 MHz, rail de 5 V
};

// Bitmap de saúde do byte 18 do pacote (PRD §6). Bit em 1 = subsistema OK.
namespace health_bit {
constexpr uint8_t kImu    = 1 << 0;
constexpr uint8_t kBaro   = 1 << 1;
constexpr uint8_t kGps    = 1 << 2;
constexpr uint8_t kSd     = 1 << 3;
// Bit 4 era o E22, removido do projeto. Fica RESERVADO e sempre em 0 para não
// mexer no layout do byte de saúde que a estação de solo decodifica (PACKET_FORMAT
// byte 18). Não reaproveitar sem versionar o pacote.
constexpr uint8_t kE22    = 1 << 4;
constexpr uint8_t kSx1276 = 1 << 5;
// Bit 6 (issue 06): altitude relativa à referência barométrica é confiável. Em 0
// quando a referência reusada foi julgada absurda e a altitude caiu para o GPS —
// é o sinal que diz ao solo que a altitude mudou de fonte (H4.4).
constexpr uint8_t kAltRef = 1 << 6;
// Bit 7 (issue 04): ao menos um eixo do acelerômetro bateu no fundo de escala
// nesta amostra. Ao contrário dos demais bits, 1 = evento RUIM (dado de boost
// clipado), não subsistema OK — o ELE3km original deixava esta informação morrer
// na conversão para mg (lacuna documentada no replay). Aqui ela é gravada, mas SÓ
// no byte de saúde do REGISTRO: o pacote de rádio não a carrega, para não dar
// significado novo ao byte de saúde congelado que a estação de solo decodifica.
constexpr uint8_t kAccelSat = 1 << 7;
}  // namespace health_bit

// Saúde de subsistema que o núcleo NÃO consegue ler de forma estável pela amostra.
// A HAL a monta (as flags g_*_ok, mantidas pela recuperação da issue 07) e a passa
// ao núcleo, que compõe o bitmap de saúde do byte 18 (issue 08). Default tudo falso:
// o caso honesto de quem ainda não tem esses subsistemas — os bits ficam apagados,
// que é o que significam.
//
//  • sd, sx1276 — subsistemas do barramento SPI, que não aparecem na amostra.
//  • baro       — o baro APARECE na amostra (baro_valid), mas ele é lido a 25 Hz
//                 dentro do laço de 50 Hz, então baro_valid é falso em metade dos
//                 ciclos mesmo com o sensor são: pisca. O bit de saúde precisa da
//                 verdade estável "baro vivo" (g_baro_ok), que só a HAL tem, e que a
//                 issue 07 apaga quando o begin() pós-recuperação falha. imu e gps
//                 são lidos a cada volta, então entram pela amostra (imu_valid,
//                 gps.receiving) sem piscar.
struct IoSubsystemHealth {
    bool sd     = false;
    bool sx1276 = false;
    bool baro   = false;
};

// Pacote de telemetria em forma decodificada. O codec (telemetry_codec.h)
// converte para o layout binário que a estação de solo consome.
//
// `form` é decidido pelo FlightComputer, não pelo codec: é uma decisão de
// política (há posição em que confiar?), não de serialização.
struct TelemetryPacket {
    Radio          radio = Radio::Sx1276;
    PacketForm     form = PacketForm::AltitudeOnly;
    uint16_t       sequence = 0;             // global, compartilhada pelos rádios
    uint16_t       t_ds = 0;                 // decissegundos; ver nota abaixo
    int32_t        latitude_1e7 = 0;         // só na forma completa
    int32_t        longitude_1e7 = 0;        // só na forma completa
    int16_t        altitude_m = 0;           // metros acima da referência
    int16_t        vertical_speed_dms = 0;   // dm/s (issue 07)
    FlightPhase    phase = FlightPhase::Rail;
    PositionSource position_source = PositionSource::None;
    uint8_t        fix_quality = 0;          // 3 bits
    uint8_t        health = 0;               // bitmap health_bit
    uint8_t        satellites = 0;           // 4 bits
    uint8_t        hdop_half = 0;            // 4 bits: HDOP × 2
};
// Nota sobre t_ds: nesta fatia é relativo ao boot. A partir da issue 06 passa a
// ser relativo ao liftoff, sem mudança de layout.

// Registro de voo em forma decodificada. A serialização de 64 B com magic,
// contador de boot e CRC é da issue 04; aqui só a estrutura de dados.
//
// PRD §8 / user story 103: o registro carrega os valores BRUTOS e os derivados.
// Nenhum campo bruto pode ser removido alegando que já está na saída fundida.
struct LogRecord {
    uint32_t t_ms = 0;             // instante da aquisição, ms desde o boot
    uint32_t sequence = 0;         // sequência do registro, incrementa por ciclo
    float    pressure_pa = 0.0f;   // bruto
    float    temperature_c = 0.0f; // bruto
    float    altitude_m = 0.0f;    // derivado
    bool     baro_valid = false;
    GpsFix   gps;                  // bruto (user story 20)
    uint32_t gps_uart_overflows = 0;

    // ── Campos com lugar reservado no formato de 64 B, sem produtor ainda ───
    //
    // Eles existem desde a issue 04 porque o formato do cartão não pode mudar
    // depois: acrescentar campo obrigaria a versionar o formato e a carregar um
    // decodificador por versão no harness de replay da issue 14. Até a issue
    // produtora chegar, cada um vale zero — que é exatamente o que significam.

    // Bruto da IMU (issue 07). Unidades escolhidas para caber em i16 sem perder
    // resolução útil: mg cobre ±32 g, e décimos de grau/s cobrem ±3276 °/s
    // contra os 2000 °/s de fundo de escala do MPU6050.
    int16_t accel_mg[3] = {0, 0, 0};
    int16_t gyro_ddps[3] = {0, 0, 0};

    // Saída fundida (issues 07 e 08). A posição fundida fica ao LADO da bruta,
    // nunca no lugar dela: ela pode vir da ponte inercial ou da última válida
    // com idade, e sem a bruta ao lado não há como refazer a decisão depois.
    int16_t        vertical_speed_dms = 0;
    int32_t        fused_latitude_1e7 = 0;
    int32_t        fused_longitude_1e7 = 0;
    FlightPhase    phase = FlightPhase::Rail;                 // issue 06
    PositionSource position_source = PositionSource::None;    // issue 08

    // Diagnóstico de robustez (PRD §8, v3). O bitmap de saúde é o mesmo do
    // pacote de rádio; o resto não cabe nos 20 B e só existe aqui.
    uint8_t  health = 0;
    uint8_t  estimator_resets = 0;      // issue 07
    uint16_t stack_watermark_flight = 0;  // issue 11
    uint16_t stack_watermark_io = 0;      // issue 11
};

}  // namespace core
