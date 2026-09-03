// Suíte nativa do núcleo puro. Roda no notebook: `pio test -e native`.
//
// Nenhum teste aqui inspeciona estado interno — só comportamento externo.
#include <unity.h>

#include <cmath>

#include "core/flight_computer.h"
#include "core/telemetry_codec.h"

using namespace core;

namespace {

// Pressão de solo para uma altitude dada (inverte a ISA da troposfera).
float pressure_for_altitude(float altitude_m) {
    return 101325.0f * std::pow(1.0f - altitude_m / 44330.0f, 5.255f);
}

// Perfil sintético de subida: a pressão cai 200 Pa/s a partir do nível do mar,
// que é da ordem de 17 m/s. Serve para todos os testes que precisam de altitude
// mudando de forma conhecida.
SensorSample climbing_sample(uint32_t t_ms) {
    SensorSample sample;
    sample.baro_valid    = true;
    sample.pressure_pa   = 101325.0f - 0.2f * static_cast<float>(t_ms);
    sample.temperature_c = 25.0f;
    return sample;
}

// Um fix bom sobre o campo de lançamento, em graus × 1e7.
GpsFix good_fix() {
    GpsFix fix;
    fix.receiving      = true;
    fix.valid          = true;
    fix.latitude_1e7   = -232012345;  // hemisfério sul: o sinal precisa sobreviver
    fix.longitude_1e7  = -458765432;  // hemisfério oeste
    fix.fix_quality    = 1;
    fix.satellites     = 9;
    fix.hdop_half      = 3;
    return fix;
}

// Receptor vivo, falando NMEA, sem fix. É um estado diferente de "receptor
// mudo", e o pacote precisa dizer qual dos dois é.
GpsFix receiving_without_fix() {
    GpsFix fix;
    fix.receiving  = true;
    fix.satellites = 2;
    return fix;
}

}  // namespace

// O tamanho dos pacotes protege o orçamento de airtime e de ciclo de trabalho,
// que é a razão de o formato ser o que é (PRD §6). O completo em 21 B pularia um
// degrau de 8 símbolos e furaria o teto já dimensionado nos docs de hardware.
void test_altitude_packet_is_exactly_12_bytes(void) {
    TelemetryPacket packet;
    packet.form = PacketForm::AltitudeOnly;
    uint8_t bytes[32] = {0};

    TEST_ASSERT_EQUAL_size_t(12, encode_packet(packet, bytes, sizeof(bytes)));
}

void test_full_packet_is_exactly_20_bytes(void) {
    TelemetryPacket packet;
    packet.form = PacketForm::Full;
    uint8_t bytes[32] = {0};

    TEST_ASSERT_EQUAL_size_t(20, encode_packet(packet, bytes, sizeof(bytes)));
}

// Ida e volta: o que a estação de solo decodifica é o que o firmware quis dizer.
void test_altitude_packet_round_trip(void) {
    TelemetryPacket sent;
    sent.form               = PacketForm::AltitudeOnly;
    sent.sequence           = 4242;
    sent.t_ds               = 1234;
    sent.altitude_m         = -317;  // abaixo da referência: o sinal precisa sobreviver
    sent.vertical_speed_dms = -905;
    sent.phase              = FlightPhase::Landed;
    sent.position_source    = PositionSource::LastValid;
    sent.fix_quality        = 5;
    sent.health             = health_bit::kBaro | health_bit::kSx1276;
    sent.satellites         = 11;
    sent.hdop_half          = 3;

    uint8_t bytes[kAltitudePacketSize] = {0};
    TEST_ASSERT_EQUAL_size_t(kAltitudePacketSize,
                             encode_packet(sent, bytes, sizeof(bytes)));

    TelemetryPacket received;
    TEST_ASSERT_TRUE(decode_packet(bytes, sizeof(bytes), received));

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PacketForm::AltitudeOnly),
                            static_cast<uint8_t>(received.form));
    TEST_ASSERT_EQUAL_UINT16(sent.sequence, received.sequence);
    TEST_ASSERT_EQUAL_UINT16(sent.t_ds, received.t_ds);
    TEST_ASSERT_EQUAL_INT16(sent.altitude_m, received.altitude_m);
    TEST_ASSERT_EQUAL_INT16(sent.vertical_speed_dms, received.vertical_speed_dms);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(sent.phase), static_cast<uint8_t>(received.phase));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(sent.position_source),
                            static_cast<uint8_t>(received.position_source));
    TEST_ASSERT_EQUAL_UINT8(sent.fix_quality, received.fix_quality);
    TEST_ASSERT_EQUAL_UINT8(sent.health, received.health);
    TEST_ASSERT_EQUAL_UINT8(sent.satellites, received.satellites);
    TEST_ASSERT_EQUAL_UINT8(sent.hdop_half, received.hdop_half);
}

// Idem para a forma completa, com atenção aos dois sinais de posição: o campo de
// latitude/longitude é o único do pacote em que um erro de sinal manda a equipe
// de recuperação para o hemisfério errado.
void test_full_packet_round_trip(void) {
    TelemetryPacket sent;
    sent.form               = PacketForm::Full;
    sent.sequence           = 60001;
    sent.t_ds               = 65535;  // saturado: ~109 min depois do liftoff
    sent.latitude_1e7       = -232012345;
    sent.longitude_1e7      = -458765432;
    sent.altitude_m         = 2987;
    sent.vertical_speed_dms = 1750;
    sent.phase              = FlightPhase::Flight;
    sent.position_source    = PositionSource::Gps;
    sent.fix_quality        = 1;
    sent.health             = health_bit::kBaro | health_bit::kGps | health_bit::kSx1276;
    sent.satellites         = 9;
    sent.hdop_half          = 3;

    uint8_t bytes[kFullPacketSize] = {0};
    TEST_ASSERT_EQUAL_size_t(kFullPacketSize, encode_packet(sent, bytes, sizeof(bytes)));

    TelemetryPacket received;
    TEST_ASSERT_TRUE(decode_packet(bytes, sizeof(bytes), received));

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PacketForm::Full),
                            static_cast<uint8_t>(received.form));
    TEST_ASSERT_EQUAL_UINT16(sent.sequence, received.sequence);
    TEST_ASSERT_EQUAL_UINT16(sent.t_ds, received.t_ds);
    TEST_ASSERT_EQUAL_INT32(sent.latitude_1e7, received.latitude_1e7);
    TEST_ASSERT_EQUAL_INT32(sent.longitude_1e7, received.longitude_1e7);
    TEST_ASSERT_EQUAL_INT16(sent.altitude_m, received.altitude_m);
    TEST_ASSERT_EQUAL_INT16(sent.vertical_speed_dms, received.vertical_speed_dms);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(sent.phase), static_cast<uint8_t>(received.phase));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(sent.position_source),
                            static_cast<uint8_t>(received.position_source));
    TEST_ASSERT_EQUAL_UINT8(sent.fix_quality, received.fix_quality);
    TEST_ASSERT_EQUAL_UINT8(sent.health, received.health);
    TEST_ASSERT_EQUAL_UINT8(sent.satellites, received.satellites);
    TEST_ASSERT_EQUAL_UINT8(sent.hdop_half, received.hdop_half);
}

// A altitude é o payload prioritário e não pode depender de qual forma foi
// transmitida — é o que sustenta a decisão de degradar para só-altitude quando o
// GPS cai. O mesmo valor, decodificado das duas formas, precisa ser o mesmo.
void test_altitude_decodes_from_both_forms(void) {
    TelemetryPacket packet;
    packet.altitude_m = -3123;

    uint8_t altitude_bytes[kAltitudePacketSize] = {0};
    packet.form = PacketForm::AltitudeOnly;
    TEST_ASSERT_EQUAL_size_t(kAltitudePacketSize,
                             encode_packet(packet, altitude_bytes, sizeof(altitude_bytes)));

    uint8_t full_bytes[kFullPacketSize] = {0};
    packet.form = PacketForm::Full;
    TEST_ASSERT_EQUAL_size_t(kFullPacketSize,
                             encode_packet(packet, full_bytes, sizeof(full_bytes)));

    TelemetryPacket from_altitude;
    TelemetryPacket from_full;
    TEST_ASSERT_TRUE(decode_packet(altitude_bytes, sizeof(altitude_bytes), from_altitude));
    TEST_ASSERT_TRUE(decode_packet(full_bytes, sizeof(full_bytes), from_full));

    TEST_ASSERT_EQUAL_INT16(-3123, from_altitude.altitude_m);
    TEST_ASSERT_EQUAL_INT16(-3123, from_full.altitude_m);
}

// A regra desta fatia, e o critério que a define: fix válido manda posição, sem
// fix manda só altitude — e nos dois casos o receptor vivo aparece na saúde,
// porque "sem fix" e "sem GPS" são coisas diferentes para quem está no chão.
void test_valid_fix_produces_a_full_packet(void) {
    FlightComputer computer;

    SensorSample sample = climbing_sample(0);
    sample.gps          = good_fix();

    const UpdateResult result = computer.update(sample, 0);

    TEST_ASSERT_EQUAL_UINT8(1, result.packet_count);
    const TelemetryPacket& packet = result.packets[0];
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PacketForm::Full),
                            static_cast<uint8_t>(packet.form));
    TEST_ASSERT_EQUAL_INT32(-232012345, packet.latitude_1e7);
    TEST_ASSERT_EQUAL_INT32(-458765432, packet.longitude_1e7);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PositionSource::Gps),
                            static_cast<uint8_t>(packet.position_source));
    TEST_ASSERT_EQUAL_UINT8(health_bit::kGps, packet.health & health_bit::kGps);
}

void test_absent_fix_produces_an_altitude_only_packet(void) {
    FlightComputer computer;

    SensorSample sample = climbing_sample(0);
    sample.gps          = receiving_without_fix();

    const UpdateResult result = computer.update(sample, 0);

    TEST_ASSERT_EQUAL_UINT8(1, result.packet_count);
    const TelemetryPacket& packet = result.packets[0];
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PacketForm::AltitudeOnly),
                            static_cast<uint8_t>(packet.form));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PositionSource::None),
                            static_cast<uint8_t>(packet.position_source));
    // O receptor está vivo, e a contagem de satélites é a pista de quão perto
    // ele está de voltar a ter fix.
    TEST_ASSERT_EQUAL_UINT8(health_bit::kGps, packet.health & health_bit::kGps);
    TEST_ASSERT_EQUAL_UINT8(2, packet.satellites);
}

// Um receptor mudo apaga o bit de saúde do GPS. É o que distingue "sem fix" de
// "sem GPS" no chão.
void test_a_silent_receiver_clears_the_gps_health_bit(void) {
    FlightComputer computer;

    const UpdateResult result = computer.update(climbing_sample(0), 0);  // GpsFix vazio

    TEST_ASSERT_EQUAL_UINT8(1, result.packet_count);
    TEST_ASSERT_EQUAL_UINT8(0, result.packets[0].health & health_bit::kGps);
}

// A sequência anda uma vez por CICLO DE TELEMETRIA e não pula nem reinicia quando
// a forma do pacote muda. É por ela que a estação de solo ordena e deduplica
// depois que o campo de tempo satura.
void test_sequence_advances_per_cycle_across_packet_forms(void) {
    FlightComputer computer;

    int packets = 0;
    for (uint32_t t_ms = 0; t_ms <= 6000; t_ms += 40) {
        SensorSample sample = climbing_sample(t_ms);
        // O fix cai no meio da janela e volta: a forma do pacote muda duas vezes.
        sample.gps = (t_ms < 2000 || t_ms > 4000) ? good_fix() : receiving_without_fix();

        const UpdateResult result = computer.update(sample, t_ms);
        for (uint8_t i = 0; i < result.packet_count; ++i) {
            // Um pacote por ciclo: o de índice n é o ciclo n.
            TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(packets),
                                     result.packets[i].sequence);
            ++packets;
        }
    }

    // Sete ciclos (1 Hz de t = 0 a t = 6000 ms), um pacote cada.
    TEST_ASSERT_EQUAL_INT(7, packets);
}

// A altitude é o payload prioritário: ela precisa estar em TODO pacote emitido,
// e precisa acompanhar o barômetro de um ciclo para o outro. Com um pacote por
// ciclo e a subida contínua, cada pacote traz uma altitude maior que o anterior.
void test_every_emitted_packet_carries_altitude(void) {
    FlightComputer computer;

    int      packets = 0;
    int16_t  previous_altitude = 0;
    for (uint32_t t_ms = 0; t_ms <= 5000; t_ms += 40) {
        const UpdateResult result = computer.update(climbing_sample(t_ms), t_ms);
        for (uint8_t i = 0; i < result.packet_count; ++i) {
            const TelemetryPacket& packet = result.packets[i];
            if (packets > 0) {
                TEST_ASSERT_GREATER_THAN_INT16(previous_altitude, packet.altitude_m);
            }
            previous_altitude = packet.altitude_m;
            ++packets;
        }
    }

    // Seis ciclos (1 Hz de t = 0 a t = 5000 ms), um pacote cada.
    TEST_ASSERT_EQUAL_INT(6, packets);
    // O último pacote é o de t = 5000, 1000 Pa abaixo da leitura inicial. Desde a
    // issue 06 a referência na rampa é uma MÉDIA LENTA, não a primeira leitura: como
    // esta subida sintética não dispara liftoff (aceleração zero), a referência
    // escorrega atrás da pressão que cai e a altitude fica um pouco abaixo dos ~85 m
    // que uma referência de tiro único daria. O que este teste protege é que a
    // altitude está em todo pacote e cresce na subida, não o valor.
    TEST_ASSERT_INT16_WITHIN(5, 77, previous_altitude);
}

// User story 103: o registro precisa ser suficiente para reconstruir a amostra
// que o produziu. Nenhum campo bruto pode sumir porque "já está no derivado" —
// inclusive a leitura de GPS e o contador de overflow da UART.
void test_log_record_keeps_the_raw_sample(void) {
    FlightComputer computer;
    computer.update(climbing_sample(0), 0);  // fixa a referência

    SensorSample sample;
    sample.baro_valid         = true;
    sample.pressure_pa        = 98765.4f;
    sample.temperature_c      = -12.5f;
    sample.gps                = good_fix();
    sample.gps_uart_overflows = 3;

    const UpdateResult result = computer.update(sample, 40);

    TEST_ASSERT_EQUAL_UINT32(40, result.log.t_ms);
    TEST_ASSERT_EQUAL_FLOAT(98765.4f, result.log.pressure_pa);
    TEST_ASSERT_EQUAL_FLOAT(-12.5f, result.log.temperature_c);
    TEST_ASSERT_TRUE(result.log.baro_valid);
    TEST_ASSERT_EQUAL_INT32(-232012345, result.log.gps.latitude_1e7);
    TEST_ASSERT_EQUAL_INT32(-458765432, result.log.gps.longitude_1e7);
    TEST_ASSERT_EQUAL_UINT8(9, result.log.gps.satellites);
    TEST_ASSERT_EQUAL_UINT32(3, result.log.gps_uart_overflows);
    // 2559,6 Pa abaixo da referência ≈ 215 m
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 215.3f, result.log.altitude_m);
}

// Uma falha de barômetro não pode calar o link: a telemetria continua, com a
// última altitude conhecida e o bit de saúde do barômetro apagado.
void test_telemetry_survives_a_barometer_dropout(void) {
    FlightComputer computer;
    for (uint32_t t_ms = 0; t_ms <= 1000; t_ms += 40) {
        computer.update(climbing_sample(t_ms), t_ms);
    }
    const TelemetryPacket last_good = computer.update(climbing_sample(2000), 2000).packets[0];

    SensorSample dead_baro;  // baro_valid = false
    int packets = 0;
    for (uint32_t t_ms = 2040; t_ms <= 5000; t_ms += 40) {
        const UpdateResult result = computer.update(dead_baro, t_ms);
        for (uint8_t i = 0; i < result.packet_count; ++i) {
            // A altitude segue a última conhecida; e como estes ciclos são todos
            // posteriores ao último saudável, o bit de saúde do barômetro já apagou.
            TEST_ASSERT_EQUAL_INT16(last_good.altitude_m, result.packets[i].altitude_m);
            TEST_ASSERT_EQUAL_UINT8(0, result.packets[i].health & health_bit::kBaro);
            ++packets;
        }
    }

    // Três ciclos (t = 3000, 4000, 5000). O que o teste protege é que o link não
    // silencia quando o barômetro morre.
    TEST_ASSERT_EQUAL_INT(3, packets);
}

// Uma falha de log NUNCA cala o link de recuperação. Cartão ausente, cartão
// morto, cartão que nunca aceita uma escrita: nada disso é entrada do núcleo, e
// a telemetria sai na mesma cadência.
//
// A troca é explícita e está no documento de hazards: perder resolução de log é
// recuperável, perder o link de recuperação não é. O veículo que não é achado
// leva o cartão junto.
void test_telemetry_survives_an_absent_card(void) {
    FlightComputer with_card;
    FlightComputer without_card;

    int with_count = 0;
    int without_count = 0;
    for (uint32_t t_ms = 0; t_ms <= 5000; t_ms += 40) {
        const UpdateResult with = with_card.update(climbing_sample(t_ms), t_ms);

        // Sem cartão nunca há escrita em andamento — é o que "ausente" significa
        // do lado do núcleo, e é a única forma que o cartão tem de aparecer aqui.
        const UpdateResult without = without_card.update(climbing_sample(t_ms), t_ms, false);

        with_count += with.packet_count;
        without_count += without.packet_count;
    }

    TEST_ASSERT_GREATER_THAN_INT(0, without_count);
    TEST_ASSERT_EQUAL_INT(with_count, without_count);
}

// Issue 06 no seam do FlightComputer: a fase chega ao registro de log e ao
// pacote, e o campo de tempo do pacote passa a contar decissegundos desde o
// LIFTOFF, não desde o boot. O zero do tempo é a ignição — é o que faz o gráfico
// de voo do solo não ter um offset arbitrário de rampa.
void test_phase_and_liftoff_relative_time_reach_the_packet(void) {
    FlightComputer computer;

    SensorSample pad;
    pad.baro_valid  = true;
    pad.pressure_pa = 101325.0f;
    pad.accel_mg[2] = 1000;  // 1 g parado
    SensorSample boost = pad;
    boost.accel_mg[2] = 3000;  // 3 g

    // Rampa: a fase já aparece no registro de log, valendo "na rampa".
    UpdateResult r = computer.update(pad, 0);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Rail),
                            static_cast<uint8_t>(r.log.phase));

    for (uint32_t t = 40; t <= 1000; t += 40) {
        computer.update(pad, t);
    }

    // Liftoff: 3 g sustentado. Captura o instante pela ordem de persistência.
    uint32_t liftoff_ms = 0;
    for (uint32_t t = 1040; t <= 1400; t += 40) {
        r = computer.update(boost, t);
        if (r.persist_phase && liftoff_ms == 0) {
            liftoff_ms = t;
        }
    }
    TEST_ASSERT_NOT_EQUAL_UINT32(0, liftoff_ms);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Flight),
                            static_cast<uint8_t>(r.log.phase));

    // Segue em voo. O escalonador latcheia o candidato no início do ciclo, então
    // o primeiro pacote que reflete o liftoff é o do ciclo que COMEÇA depois dele —
    // o de t = 2000. Ele carrega a fase de voo, e seu campo de tempo é relativo ao
    // liftoff: bem menor que o tempo desde o boot no mesmo instante (~20 ds em
    // t = 2000), o que prova que o zero mudou para a ignição.
    bool checked = false;
    for (uint32_t t = 2000; t <= 3000 && !checked; t += 40) {
        r = computer.update(boost, t);
        for (uint8_t i = 0; i < r.packet_count; ++i) {
            TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Flight),
                                    static_cast<uint8_t>(r.packets[i].phase));
            const uint16_t since_boot = static_cast<uint16_t>(t / 100);
            TEST_ASSERT_LESS_THAN_UINT16(since_boot, r.packets[i].t_ds);
            TEST_ASSERT_LESS_OR_EQUAL_UINT16(
                static_cast<uint16_t>((t - liftoff_ms) / 100 + 1), r.packets[i].t_ds);
            checked = true;
        }
    }
    TEST_ASSERT_TRUE(checked);
}

// A razão de existir do projeto: telemetria ao vivo durante TODA a descida, não só
// depois do toque. Um voo completo pelo FlightComputer — liftoff, apogeu, descida
// — e os pacotes têm que continuar saindo na cadência de voo com a fase de voo o
// tempo todo da descida. Um detector de pouso que disparasse no apogeu mataria
// justamente esses pacotes.
void test_packets_keep_coming_at_flight_cadence_through_the_descent(void) {
    FlightComputer computer;

    uint32_t t = 0;
    auto feed = [&](float altitude_m, uint16_t accel_mg) -> UpdateResult {
        SensorSample s;
        s.baro_valid  = true;
        s.pressure_pa = pressure_for_altitude(altitude_m);
        s.accel_mg[2] = static_cast<int16_t>(accel_mg);
        const UpdateResult r = computer.update(s, t);
        t += 40;  // 25 Hz
        return r;
    };

    for (int i = 0; i < 50; ++i) feed(0.0f, 1000);        // rampa
    for (int i = 0; i < 10; ++i) feed(50.0f, 3000);       // liftoff + boost
    for (int i = 0; i < 200; ++i) feed(1500.0f, 0);       // coast/apogeu, 0 g

    // Descida sob paraquedas, ~30 s de 3000 m a 0, 1 g. Conta os pacotes e exige
    // que a fase deles seja "em voo" o tempo todo.
    int descent_packets = 0;
    for (int i = 0; i < 750; ++i) {
        const float alt = 3000.0f * (1.0f - (i + 1) / 750.0f);
        const UpdateResult r = feed(alt, 1000);
        for (uint8_t k = 0; k < r.packet_count; ++k) {
            TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Flight),
                                    static_cast<uint8_t>(r.packets[k].phase));
            ++descent_packets;
        }
    }

    // ~30 s a 1 Hz ≈ 30 pacotes. O que o teste protege é que não é zero e não caiu
    // para um regime de beacon.
    TEST_ASSERT_GREATER_THAN_INT(20, descent_packets);
}

void setUp(void) {}
void tearDown(void) {}

// Leva um FlightComputer novo até POUSADO e devolve o instante do primeiro ciclo
// já pousado. Perfil: rampa parada ao nível do solo, um boost curto que dispara o
// liftoff, e repouso ao solo até as quatro condições de pouso da issue 06 baterem.
uint32_t drive_to_landed(FlightComputer& computer) {
    SensorSample pad;
    pad.baro_valid  = true;
    pad.pressure_pa = 101325.0f;  // nível do mar → altitude ≈ 0, perto do solo
    pad.accel_mg[2] = 1000;       // 1 g parado

    SensorSample boost = pad;
    boost.accel_mg[2] = 3000;     // 3 g: dispara o liftoff

    uint32_t t = 0;
    for (int i = 0; i < 10; ++i) { computer.update(pad, t); t += 40; }    // referência + rampa
    for (int i = 0; i < 10; ++i) { computer.update(boost, t); t += 40; }  // liftoff sustentado
    for (int i = 0; i < 900; ++i) {                                       // repouso até pousar
        const UpdateResult r = computer.update(pad, t);
        if (r.log.phase == FlightPhase::Landed) return t;
        t += 40;
    }
    return t;
}

// Issue 13: em POUSADO o campo de qualidade de fix do byte 17 muda de significado —
// passa a carregar o NÚMERO DE AMOSTRAS pós-pouso acumuladas (a fase, também no
// pacote, diz ao solo qual dos dois sentidos ler). Depois de vários fixes bons a
// contagem satura em 7, e é esse valor, não a qualidade GGA do fix (1), que sai no
// ar — e sobrevive à serialização que a estação de solo decodifica.
void test_landed_samples_count_encoded_in_fix_quality(void) {
    FlightComputer computer;
    uint32_t t = drive_to_landed(computer);

    TelemetryPacket last;
    bool have = false;
    for (int i = 0; i < 400; ++i) {
        t += 40;
        SensorSample s;
        s.baro_valid  = true;
        s.pressure_pa = 101325.0f;
        s.accel_mg[2] = 1000;
        s.gps         = good_fix();  // qualidade GGA 1, 9 satélites, HDOP 1,5 → aceito
        const UpdateResult r = computer.update(s, t);
        for (uint8_t k = 0; k < r.packet_count; ++k) {
            last = r.packets[k];
            have = true;
        }
    }

    TEST_ASSERT_TRUE(have);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Landed),
                            static_cast<uint8_t>(last.phase));
    TEST_ASSERT_EQUAL_UINT8(7, last.fix_quality);  // amostras saturadas, não a qualidade GGA

    uint8_t bytes[kFullPacketSize] = {0};
    TEST_ASSERT_EQUAL_size_t(kFullPacketSize, encode_packet(last, bytes, sizeof(bytes)));
    TelemetryPacket decoded;
    TEST_ASSERT_TRUE(decode_packet(bytes, sizeof(bytes), decoded));
    TEST_ASSERT_EQUAL_UINT8(7, decoded.fix_quality);
}

// Perdido o GPS além da janela de ponte inercial, o pacote não fica mudo de
// posição nem transmite a posição inercial derivada: ele carrega a ÚLTIMA posição
// de GPS válida, com a fonte LastValid, e a altitude segue presente (issue 08).
void test_lost_gps_beyond_bridge_transmits_last_valid_position(void) {
    FlightComputer computer;

    TelemetryPacket last;
    bool have = false;
    for (uint32_t t = 0; t <= 17000; t += 100) {
        SensorSample sample = climbing_sample(t);
        sample.gps = (t <= 1000) ? good_fix() : receiving_without_fix();
        const UpdateResult result = computer.update(sample, t);
        for (uint8_t i = 0; i < result.packet_count; ++i) {
            last = result.packets[i];
            have = true;
        }
    }

    TEST_ASSERT_TRUE(have);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PacketForm::Full),
                            static_cast<uint8_t>(last.form));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PositionSource::LastValid),
                            static_cast<uint8_t>(last.position_source));
    TEST_ASSERT_EQUAL_INT32(-232012345, last.latitude_1e7);   // último fix válido
    TEST_ASSERT_EQUAL_INT32(-458765432, last.longitude_1e7);  // não a deriva inercial
}

// A fonte de posição vai ao registro de log, não só ao pacote: é dela e do t_ms de
// cada registro que a idade do fix se reconstrói no pós-voo, já que a idade não
// cabe nos 20 B do ar (issue 08, "idade para o log").
void test_log_record_carries_the_position_source(void) {
    FlightComputer computer;

    SensorSample sample = climbing_sample(0);
    sample.gps          = good_fix();
    const UpdateResult result = computer.update(sample, 0);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PositionSource::Gps),
                            static_cast<uint8_t>(result.log.position_source));
}

// Issue 10: o bitmap de saúde {imu, baro, gps, sd, sx1276} tem que estar em TODO
// pacote E em TODO registro — é assim que a equipe diagnostica do chão o que falhou
// a bordo. imu/baro/gps entram pela amostra; a saúde do cartão e do rádio (que a
// task de I/O possui) entra pelo parâmetro do núcleo. O bit 4 (era o E22) fica
// sempre em 0, reservado no layout do byte de saúde.
void test_health_bitmap_reflects_all_subsystems(void) {
    FlightComputer computer;

    SensorSample sample = climbing_sample(0);
    sample.imu_valid    = true;
    sample.gps          = good_fix();

    IoSubsystemHealth io;
    io.sd     = true;
    io.sx1276 = true;

    const UpdateResult result = computer.update(sample, 0, /*write_in_progress=*/false, io);

    TEST_ASSERT_EQUAL_UINT8(1, result.packet_count);
    const uint8_t bits = health_bit::kImu | health_bit::kBaro | health_bit::kGps |
                         health_bit::kSd | health_bit::kSx1276;
    TEST_ASSERT_EQUAL_UINT8(bits, result.packets[0].health & bits);
    // O bit reservado do E22 nunca acende.
    TEST_ASSERT_EQUAL_UINT8(0, result.packets[0].health & health_bit::kE22);
    // O mesmo bitmap no registro de log, não só no pacote.
    TEST_ASSERT_EQUAL_UINT8(result.packets[0].health, result.log.health);
}

// AC de degradação (issue 10): com cada subsistema marcado como ausente, um por
// vez, o sistema continua emitindo pacotes e o bitmap reflete a falha. Uma falha
// isolada de módulo nunca derruba o link.
void test_each_missing_subsystem_keeps_packets_flowing_and_clears_its_bit(void) {
    const uint8_t bit_of[5] = {health_bit::kImu, health_bit::kBaro, health_bit::kGps,
                               health_bit::kSd, health_bit::kSx1276};
    for (int drop = 0; drop < 5; ++drop) {
        FlightComputer computer;

        SensorSample sample = climbing_sample(0);
        sample.imu_valid    = true;
        sample.gps          = good_fix();
        IoSubsystemHealth io;
        io.sd     = true;
        io.sx1276 = true;

        switch (drop) {
            case 0: sample.imu_valid  = false; break;
            case 1: sample.baro_valid = false; break;
            case 2: sample.gps        = GpsFix{}; break;  // receptor mudo
            case 3: io.sd             = false; break;
            case 4: io.sx1276         = false; break;
        }

        const UpdateResult result = computer.update(sample, 0, /*write_in_progress=*/false, io);

        // O link continua: um pacote é emitido, com altitude...
        TEST_ASSERT_GREATER_OR_EQUAL_UINT8(1, result.packet_count);
        // ...e só o bit do subsistema derrubado está apagado.
        TEST_ASSERT_EQUAL_UINT8(0, result.packets[0].health & bit_of[drop]);
    }
}

// Issue 13: a taxa de log cai para 1 Hz em POUSADO. Sem isso o beacon gravaria
// horas de dados de um foguete parado a 6,4 kB/s. Antes de pousar, o log sai a cada
// ciclo — nenhuma amostra do voo é descartada; depois, o núcleo sinaliza gravação
// só uma vez por segundo, mesmo aquirindo a 25 Hz.
void test_log_cadence_drops_to_1hz_on_entering_landed(void) {
    FlightComputer computer;

    SensorSample pad;
    pad.baro_valid  = true;
    pad.pressure_pa = 101325.0f;
    pad.accel_mg[2] = 1000;
    SensorSample boost = pad;
    boost.accel_mg[2] = 3000;

    uint32_t t = 0;
    int flight_logs = 0;
    int flight_cycles = 0;
    for (int i = 0; i < 10; ++i) {
        if (computer.update(pad, t).should_log) ++flight_logs;
        ++flight_cycles;
        t += 40;
    }
    for (int i = 0; i < 10; ++i) { computer.update(boost, t); t += 40; }

    bool landed = false;
    for (int i = 0; i < 900 && !landed; ++i) {
        const UpdateResult r = computer.update(pad, t);
        if (r.log.phase == FlightPhase::Landed) {
            landed = true;
        } else {
            if (r.should_log) ++flight_logs;
            ++flight_cycles;
        }
        t += 40;
    }
    TEST_ASSERT_TRUE(landed);
    // Antes de pousar, o log sai a cada ciclo.
    TEST_ASSERT_EQUAL_INT(flight_cycles, flight_logs);

    // Pousado: 25 Hz de aquisição, mas ~1 log por segundo.
    int landed_logs = 0;
    int landed_cycles = 0;
    const uint32_t start = t;
    for (; t <= start + 10000; t += 40) {
        if (computer.update(pad, t).should_log) ++landed_logs;
        ++landed_cycles;
    }
    TEST_ASSERT_GREATER_THAN_INT(200, landed_cycles);  // ~250 amostras em 10 s a 25 Hz
    TEST_ASSERT_INT_WITHIN(2, 10, landed_logs);         // ~1 Hz, não 25 Hz
}

// Seletor de bancada: forçar a fase pina só a fase reportada, sem falsear sensor.
// Duas máquinas idênticas na rampa, sem liftoff, mesma amostra e mesmo tempo — uma
// em Auto, outra forçada em VOO. A forçada reporta VOO; a real segue na rampa; e a
// ALTITUDE das duas é idêntica, porque a sobrescrita não toca no barômetro.
void test_forced_phase_pins_the_reported_phase_only(void) {
    FlightComputer as_flown;
    FlightComputer forced;

    SensorSample s = climbing_sample(0);
    s.accel_mg[2] = 1000;  // 1 g parado: a máquina real fica na rampa

    const UpdateResult ra = as_flown.update(s, 0);
    const UpdateResult rf = forced.update(s, 0, /*write_in_progress=*/false, {},
                                          PhaseOverride::Flight);

    TEST_ASSERT_EQUAL_UINT8(1, ra.packet_count);
    TEST_ASSERT_EQUAL_UINT8(1, rf.packet_count);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Rail),
                            static_cast<uint8_t>(ra.packets[0].phase));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Flight),
                            static_cast<uint8_t>(rf.packets[0].phase));
    // Sensor intacto: a altitude transmitida é a mesma com e sem a sobrescrita.
    TEST_ASSERT_EQUAL_INT16(ra.packets[0].altitude_m, rf.packets[0].altitude_m);
    // O registro de log também carrega a fase forçada.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Landed),
                            static_cast<uint8_t>(
                                forced.update(s, 40, false, {}, PhaseOverride::Landed).log.phase));
}

// Forçar POUSADO aciona as regras a jusante de pouso — aqui, a queda do log para
// 1 Hz (issue 13) — sem levar o foguete por um voo inteiro. É o ponto do seletor:
// exercitar o comportamento de cada fase no metal com um comando.
void test_forced_landed_engages_the_1hz_landed_log(void) {
    FlightComputer computer;
    SensorSample s;
    s.baro_valid  = true;
    s.pressure_pa = 101325.0f;
    s.accel_mg[2] = 1000;

    UpdateResult r = computer.update(s, 0, false, {}, PhaseOverride::Landed);
    TEST_ASSERT_TRUE(r.should_log);  // primeiro ciclo pousado grava
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Landed),
                            static_cast<uint8_t>(r.log.phase));

    r = computer.update(s, 100, false, {}, PhaseOverride::Landed);
    TEST_ASSERT_FALSE(r.should_log);  // 100 ms depois: dentro do 1 s, não grava

    r = computer.update(s, 1000, false, {}, PhaseOverride::Landed);
    TEST_ASSERT_TRUE(r.should_log);   // passado 1 s: grava de novo
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_log_cadence_drops_to_1hz_on_entering_landed);
    RUN_TEST(test_health_bitmap_reflects_all_subsystems);
    RUN_TEST(test_each_missing_subsystem_keeps_packets_flowing_and_clears_its_bit);
    RUN_TEST(test_altitude_packet_is_exactly_12_bytes);
    RUN_TEST(test_full_packet_is_exactly_20_bytes);
    RUN_TEST(test_altitude_packet_round_trip);
    RUN_TEST(test_full_packet_round_trip);
    RUN_TEST(test_altitude_decodes_from_both_forms);
    RUN_TEST(test_valid_fix_produces_a_full_packet);
    RUN_TEST(test_absent_fix_produces_an_altitude_only_packet);
    RUN_TEST(test_a_silent_receiver_clears_the_gps_health_bit);
    RUN_TEST(test_sequence_advances_per_cycle_across_packet_forms);
    RUN_TEST(test_every_emitted_packet_carries_altitude);
    RUN_TEST(test_log_record_keeps_the_raw_sample);
    RUN_TEST(test_telemetry_survives_a_barometer_dropout);
    RUN_TEST(test_telemetry_survives_an_absent_card);
    RUN_TEST(test_phase_and_liftoff_relative_time_reach_the_packet);
    RUN_TEST(test_packets_keep_coming_at_flight_cadence_through_the_descent);
    RUN_TEST(test_lost_gps_beyond_bridge_transmits_last_valid_position);
    RUN_TEST(test_log_record_carries_the_position_source);
    RUN_TEST(test_landed_samples_count_encoded_in_fix_quality);
    RUN_TEST(test_forced_phase_pins_the_reported_phase_only);
    RUN_TEST(test_forced_landed_engages_the_1hz_landed_log);
    return UNITY_END();
}
