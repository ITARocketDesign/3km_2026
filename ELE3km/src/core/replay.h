// core/replay.h — harness de replay do log de voo (issue 14).
//
// Transforma o primeiro voo real no teste de regressão de todas as versões
// futuras do estimador: lê os registros brutos do cartão, reconstrói a sequência
// de `SensorSample` a partir dos campos BRUTOS de cada um, e realimenta
// `FlightComputer::update()` no host com o timestamp de aquisição de cada registro.
// Compara então a saída recalculada com a que o firmware gravou em voo.
//
// Puro: nenhum header de Arduino, nenhum relógio global, nenhuma alocação. O tempo
// entra pelos próprios registros. A leitura de ARQUIVO (host I/O) NÃO mora aqui —
// ela fica em `tools/`, para o `core/` continuar puro; aqui está só a lógica de
// reconstrução e comparação, que é o que a suíte nativa exercita.
//
// ── Duas lacunas conhecidas de reconstrução (issue 14) ──────────────────────
//
// O registro de 64 B (`log_codec.h`) NÃO carrega dois campos brutos que a
// `FlightComputer` consome, então um voo real não se reproduz bit a bit:
//
//   • `gps.altitude_m` — a altitude MSL da GGA. O estimador corrige o canal
//     vertical com ela a cada fix válido; o registro guarda só a altitude DERIVADA
//     do barômetro, não a bruta do GPS.
//   • `accel_saturated` — vem do ADC bruto batendo no fundo de escala (mpu6050), e
//     se perde na conversão para mg. Não é recuperável do `accel_mg` gravado.
//
// `imu_valid` é recuperável — do bit de saúde kImu. As duas lacunas ficam zeradas
// na reconstrução e são o motivo de `record_reconstruction_fields_present()` e da
// documentação nomearem-nas: quem quiser reprodução exata sob boost ou com GPS
// válido precisa primeiro levá-las ao formato do cartão, o que é decisão de
// contrato à parte. A reprodução é exata para o SUBCONJUNTO reconstruível — que é o
// que fecha o ciclo do teste sintético e o que guarda o voo em regressão fora
// dessas janelas.
#pragma once

#include <stddef.h>

#include "core/flight_computer.h"
#include "core/types.h"

namespace core {

// Reconstrói a amostra de sensores a partir dos campos BRUTOS do registro. Os dois
// campos que o registro não carrega (gps.altitude_m, accel_saturated) saem zerados.
SensorSample sensor_sample_from_record(const LogRecord& record);

// Tolerância declarada da comparação (AC 5). Altitude e posição comparam por
// distância; fase e fonte são enums e comparam por igualdade exata. Zerada em tudo,
// é a comparação exata que o ciclo sintético fecha.
struct ReplayTolerance {
    float   altitude_m = 0.0f;    // |recalc − gravado| aceitável em altitude, m
    int32_t position_1e7 = 0;     // idem na posição fundida, graus × 1e7
};

// Relatório da comparação: os piores desvios e quantas divergências de fase/fonte,
// mais o veredito contra a tolerância. Um único agregado para o fluxo inteiro —
// sem alocação, o chamador imprime o que quiser.
struct ReplayDivergence {
    uint32_t records = 0;                 // registros reproduzidos
    float    max_altitude_diff_m = 0.0f;
    int32_t  max_position_diff_1e7 = 0;
    uint32_t phase_mismatches = 0;
    uint32_t source_mismatches = 0;
    bool     within_tolerance = true;     // false se algo passou da tolerância
};

// Modo de comparação (AC 4): reproduz `records` por uma `FlightComputer` nova com a
// `config` dada, alimentando o timestamp de aquisição de cada registro, e compara a
// saída recalculada com a gravada em altitude, posição, fonte e fase. Devolve o
// agregado das divergências. É puro e sem alocação — a `FlightComputer` vive na
// pilha.
//
// Reproduz só os registros que estão no cartão. Em voo todo ciclo é gravado, então
// a reprodução do voo é exata; pousado o log cai para 1 Hz (issue 13) e a sequência
// reproduzida é subamostrada — mas a regressão do estimador vive no voo, não no
// repouso.
ReplayDivergence replay_and_compare(const LogRecord* records, size_t count,
                                    const FlightComputerConfig& config,
                                    const ReplayTolerance& tolerance);

// Verificação explícita (AC 8): todos os campos brutos que a reconstrução usa
// sobrevivem a uma volta pelo codec do registro (encode → scan → reconstrói)? Se
// alguém remover um campo bruto do formato de 64 B por parecer redundante, esta
// volta deixa de bater e a função devolve false — o harness para de fingir que um
// voo real é reproduzível. O teste a exige verdadeira; o `tools/replay` a checa no
// arranque. As duas lacunas conhecidas ficam de fora por serem constantes na
// reconstrução (ver replay.h) — a decisão de trazê-las ao formato é de contrato.
bool record_reconstruction_fields_present();

}  // namespace core
