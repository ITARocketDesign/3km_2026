// core/position_source.h — a máquina que decide DE ONDE vem a posição do pacote.
//
// Sente-se entre o estimador (issue 07) e o pacote. O estimador já entrega uma
// distinção grosseira Gps/Ins por ciclo, mas ela pisca a cada amostra: a 25 Hz de
// barômetro contra 5 Hz de GPS, quatro em cada cinco ciclos não têm correção e
// pareceriam Ins. Esta máquina decide a fonte pela IDADE do último fix válido, não
// pelo ciclo — e é ela que introduz o estado LastValid, que o estimador não tem.
//
// Puro: nenhum header de Arduino, nenhum relógio global, nenhuma alocação. O tempo
// entra como parâmetro.
//
// ── Os quatro estados ───────────────────────────────────────────────────────
//
//   • None      — nunca houve fix. Pacote só-altitude.
//   • Gps       — fix fresco (idade ≤ gps_fresh_ms). Pacote completo, posição fundida.
//   • Ins       — fix perdido além do limiar, mas dentro da janela de ponte. Pacote
//                 completo, posição PROPAGADA pelo estimador. Ponte curta, não
//                 navegação: com IMU de 6 eixos sem magnetômetro a posição diverge
//                 em segundos.
//   • LastValid — além da janela de ponte. Pacote completo com a última posição de
//                 GPS VÁLIDA (não a propagada) e a idade dela: passada a janela útil,
//                 um fix antigo declarado vale mais para achar o foguete que uma
//                 posição inercial derivada.
//
// GPS reaquirido volta a Gps. Se a IMU está ausente ou morta, o estado Ins é
// desabilitado: a perda de GPS cai direto para LastValid (que não depende de IMU).
//
// ── A idade não vai no ar ────────────────────────────────────────────────────
//
// O pacote de 20 B não tem campo de idade, e acrescentá-lo furaria o orçamento de
// airtime (PACKET_FORMAT.md). No ar, a fonte LastValid (2 bits do byte de flags) é
// o sinal de que a posição é velha. A idade numérica sai aqui, para o log, e é
// reconstruível no pós-voo a partir da fonte e do t_ms de cada registro.
#pragma once

#include <stdint.h>

#include "core/types.h"

namespace core {

struct PositionSourceConfig {
    // Idade do último fix abaixo da qual a fonte ainda é Gps. Precisa cobrir com
    // folga um período de GPS (5 Hz → 200 ms) para a recepção estável não piscar
    // para Ins entre fixes.
    uint32_t gps_fresh_ms = 400;

    // Duração da ponte inercial: acima de gps_fresh_ms e até aqui, a fonte é Ins.
    // Passada esta janela, LastValid. A issue pede 10–20 s.
    uint32_t bridge_window_ms = 15000;

    // Critério de aceitação de um fix na média pós-pouso (issue 13). Quatro
    // satélites é o mínimo para solução 3D; HDOP 5,0 (× 2 = 10 na unidade do
    // pacote) corresponde a ~10–15 m — abaixo disso o fix é ruim demais para
    // melhorar a média.
    uint8_t landed_min_satellites = 4;
    uint8_t landed_max_hdop_half = 10;  // HDOP 5,0 × 2
};

struct PositionSourceInput {
    // Fix de GPS válido nesta amostra.
    bool    have_gps_fix = false;
    int32_t gps_lat_1e7 = 0;
    int32_t gps_lon_1e7 = 0;

    // Posição fundida do estimador (issue 07): a que se transmite em Gps e em Ins.
    bool    has_fused = false;
    int32_t fused_lat_1e7 = 0;
    int32_t fused_lon_1e7 = 0;

    // A IMU existe e está viva no sistema. Falso desabilita a ponte Ins.
    bool imu_present = true;

    // A fase é POUSADO (issue 13). Verdadeiro troca o regime da máquina: em vez de
    // Gps/Ins/LastValid, ela filtra os fixes e transmite a MÉDIA dos aceitos.
    bool is_landed = false;

    // Qualidade do fix desta amostra, para o filtro pós-pouso. Fora de POUSADO
    // não são lidos.
    uint8_t satellites = 0;
    uint8_t hdop_half = 0;  // HDOP × 2, como no pacote
};

struct PositionSourceOutput {
    PositionSource source = PositionSource::None;

    // true → pacote completo (há posição em que confiar); false → só-altitude.
    bool confident = false;

    bool    has_position = false;
    int32_t latitude_1e7 = 0;
    int32_t longitude_1e7 = 0;

    // Idade da posição transmitida, em ms. Zero no ciclo de um fix fresco; cresce
    // durante o vão. Vai ao log; não vai ao ar.
    uint32_t fix_age_ms = 0;

    // Fixes pós-pouso aceitos e acumulados na média (issue 13), saturando em 7 para
    // caber nos 3 bits de qualidade de fix do byte 17. Zero fora de POUSADO e nos
    // ciclos pós-pouso sem nenhum fix aceito. É o que diz ao operador se a posição
    // é um ponto (≥ 3) ou um círculo largo (0–2).
    uint8_t samples = 0;
};

class PositionSourceMachine {
  public:
    PositionSourceMachine() = default;
    explicit PositionSourceMachine(const PositionSourceConfig& config)
        : config_(config) {}

    PositionSourceOutput update(const PositionSourceInput& input, uint32_t t_ms);

  private:
    PositionSourceConfig config_;

    bool     have_last_fix_ = false;
    int32_t  last_lat_1e7_ = 0;
    int32_t  last_lon_1e7_ = 0;
    uint32_t last_fix_ms_ = 0;

    // Acumulador da média pós-pouso (issue 13). Somas em 64 bits: um voo de busca
    // pode acumular milhares de fixes, e a soma de graus × 1e7 estoura o i32.
    int64_t  landed_sum_lat_1e7_ = 0;
    int64_t  landed_sum_lon_1e7_ = 0;
    uint32_t landed_count_ = 0;
};

}  // namespace core
