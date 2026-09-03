// Suíte do formato de log. Roda no notebook: `pio test -e native`.
//
// O que esta suíte protege é a única cópia dos dados brutos do voo. O pacote de
// rádio é resumo; isto é o registro completo, e é o insumo do harness de replay
// da issue 14 — o log do primeiro voo vira o teste de regressão de todas as
// versões futuras do estimador.
//
// Por isso os testes daqui são desconfiados: fluxo truncado, CRC corrompido, e
// registro de OUTRO voo que valida perfeitamente porque o arquivo pré-alocado
// caiu nos clusters do voo anterior.
#include <unity.h>

#include "core/log_codec.h"

using namespace core;

namespace {

// Um registro com todos os campos que já têm produtor hoje, em valores que
// distinguem um erro de layout de um acerto: negativos onde o sinal precisa
// sobreviver, e nada igual a nada.
LogRecord sample_record() {
    LogRecord record;
    record.t_ms               = 123456;
    record.sequence           = 7890;
    record.pressure_pa        = 98765.4f;
    record.temperature_c      = -12.5f;
    record.altitude_m         = 2187.75f;
    record.baro_valid         = true;
    record.gps.receiving      = true;
    record.gps.valid          = true;
    record.gps.latitude_1e7   = -232012345;  // hemisfério sul
    record.gps.longitude_1e7  = -458765432;  // hemisfério oeste
    record.gps.fix_quality    = 1;
    record.gps.satellites     = 9;
    record.gps.hdop_half      = 3;
    record.gps_uart_overflows = 4;
    return record;
}

}  // namespace

// O tamanho não é arbitrário: 64 B dão exatamente 8 registros por bloco de
// 512 B, que é a unidade de transação do cartão. Um registro de 65 B faria cada
// bloco cruzar uma fronteira de registro e a varredura perderia o alinhamento.
void test_record_round_trips_through_exactly_64_bytes(void) {
    const LogRecord sent = sample_record();

    uint8_t bytes[128] = {0};
    TEST_ASSERT_EQUAL_size_t(kLogRecordSize, encode_record(sent, 42, bytes, sizeof(bytes)));

    LogRecord received;
    uint16_t  boot_count = 0;
    TEST_ASSERT_TRUE(decode_record(bytes, kLogRecordSize, received, boot_count));

    TEST_ASSERT_EQUAL_UINT16(42, boot_count);
    TEST_ASSERT_EQUAL_UINT32(sent.t_ms, received.t_ms);
    TEST_ASSERT_EQUAL_UINT32(sent.sequence, received.sequence);
    TEST_ASSERT_EQUAL_FLOAT(sent.pressure_pa, received.pressure_pa);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, sent.temperature_c, received.temperature_c);
    TEST_ASSERT_EQUAL_FLOAT(sent.altitude_m, received.altitude_m);
    TEST_ASSERT_TRUE(received.baro_valid);
    TEST_ASSERT_TRUE(received.gps.receiving);
    TEST_ASSERT_TRUE(received.gps.valid);
    TEST_ASSERT_EQUAL_INT32(sent.gps.latitude_1e7, received.gps.latitude_1e7);
    TEST_ASSERT_EQUAL_INT32(sent.gps.longitude_1e7, received.gps.longitude_1e7);
    TEST_ASSERT_EQUAL_UINT8(sent.gps.fix_quality, received.gps.fix_quality);
    TEST_ASSERT_EQUAL_UINT8(sent.gps.satellites, received.gps.satellites);
    TEST_ASSERT_EQUAL_UINT8(sent.gps.hdop_half, received.gps.hdop_half);
    TEST_ASSERT_EQUAL_UINT16(sent.gps_uart_overflows, received.gps_uart_overflows);
}

// Sem camada de enlace debaixo, o CRC é a única coisa que separa dado bom de um
// bloco escrito pela metade no instante em que a energia caiu. Um registro
// corrompido aceito em silêncio é pior que um registro perdido: ele entra no
// dataset com aparência de medição.
void test_a_corrupted_record_is_rejected(void) {
    uint8_t bytes[kLogRecordSize] = {0};
    TEST_ASSERT_EQUAL_size_t(kLogRecordSize,
                             encode_record(sample_record(), 42, bytes, sizeof(bytes)));

    LogRecord received;
    uint16_t  boot_count = 0;
    TEST_ASSERT_TRUE(decode_record(bytes, sizeof(bytes), received, boot_count));

    // Um bit no meio da carga útil — a corrupção que um bloco parcial produz,
    // não um registro obviamente destruído.
    bytes[20] ^= 0x01;
    TEST_ASSERT_FALSE(decode_record(bytes, sizeof(bytes), received, boot_count));
}

// O corte de energia em pleno voo é o caso normal, não o excepcional: o arquivo
// tem comprimento errado e a cauda está partida no meio de um registro. Todo
// registro COMPLETO antes do corte precisa sair inteiro da varredura, e o
// partido precisa ser descartado sem levar os outros junto.
void test_a_truncated_stream_still_yields_every_complete_record(void) {
    constexpr int kWritten = 20;

    uint8_t stream[kWritten * kLogRecordSize] = {0};
    for (int i = 0; i < kWritten; ++i) {
        LogRecord record = sample_record();
        record.sequence  = static_cast<uint32_t>(i);
        record.t_ms      = static_cast<uint32_t>(i) * 40u;
        encode_record(record, 42, stream + i * kLogRecordSize, kLogRecordSize);
    }

    // Corte num offset arbitrário, no meio do décimo terceiro registro: doze
    // registros completos e 37 bytes de sobra que não formam nada.
    const size_t truncated_len = 12 * kLogRecordSize + 37;

    LogRecord    recovered[kWritten];
    const size_t found = scan_records(stream, truncated_len, 42, recovered, kWritten);

    TEST_ASSERT_EQUAL_size_t(12, found);
    for (size_t i = 0; i < found; ++i) {
        TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(i), recovered[i].sequence);
        TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(i) * 40u, recovered[i].t_ms);
    }
}

// O modo de falha que o contador de boot existe para impedir, e é sutil: o
// arquivo pré-alocado cai nos clusters do voo ANTERIOR, e o voo atual só
// sobrescreve a parte que chegou a gravar. O resto do arquivo continua cheio de
// registros do voo passado, com magic bom e CRC bom — eles validam
// PERFEITAMENTE e entrariam no meio da trajetória atual, num ponto qualquer.
//
// Um teste de CRC não pega isso. Só o contador de boot pega.
void test_records_from_a_previous_flight_are_discarded(void) {
    constexpr int kSlots = 10;

    uint8_t stream[kSlots * kLogRecordSize] = {0};
    for (int i = 0; i < kSlots; ++i) {
        LogRecord record = sample_record();
        record.sequence  = static_cast<uint32_t>(i);
        // Os quatro primeiros slots são deste voo; o resto é resíduo do anterior,
        // que o voo atual ainda não alcançou.
        const uint16_t boot = (i < 4) ? 42 : 41;
        encode_record(record, boot, stream + i * kLogRecordSize, kLogRecordSize);
    }

    LogRecord    recovered[kSlots];
    const size_t found = scan_records(stream, sizeof(stream), 42, recovered, kSlots);

    TEST_ASSERT_EQUAL_size_t(4, found);
    for (size_t i = 0; i < found; ++i) {
        TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(i), recovered[i].sequence);
    }
}

// O cabeçalho ocupa o bloco inteiro para que o primeiro registro caia em 512 e
// todo registro seguinte nasça alinhado em 64 B — é o alinhamento que permite à
// varredura andar em passos de registro em vez de caçar magic byte a byte.
//
// A pressão de referência é o que torna a altitude do voo reinterpretável: sem
// ela, um log de altitude é relativo a um zero que ninguém sabe qual era.
void test_header_block_round_trips(void) {
    LogHeader sent;
    sent.boot_count       = 42;
    sent.reference_pa     = 101325.0f;
    sent.pin_map_revision = 1;

    uint8_t block[kLogHeaderSize] = {0};
    TEST_ASSERT_EQUAL_size_t(kLogHeaderSize, encode_header(sent, block, sizeof(block)));

    LogHeader received;
    TEST_ASSERT_TRUE(decode_header(block, sizeof(block), received));

    TEST_ASSERT_EQUAL_UINT16(42, received.boot_count);
    TEST_ASSERT_EQUAL_FLOAT(101325.0f, received.reference_pa);
    TEST_ASSERT_EQUAL_UINT8(1, received.pin_map_revision);
    TEST_ASSERT_EQUAL_UINT8(kLogFormatVersion, received.format_version);

    // Um cabeçalho ilegível não pode devolver um contador de boot plausível: sem
    // ele não há como separar este voo do anterior, e é melhor falhar alto.
    block[6] ^= 0x02;
    TEST_ASSERT_FALSE(decode_header(block, sizeof(block), received));
}

// O evento de boot loop e metadado deste boot, nao uma amostra de sensor. Ele
// usa bytes antes reservados do bloco de cabecalho, sem alterar os registros de
// 64 B que alimentam o harness de replay.
void test_header_records_the_boot_loop_event_and_recent_reset_count(void) {
    LogHeader sent;
    sent.boot_count        = 42;
    sent.boot_loop         = true;
    sent.recent_reset_count = 3;

    uint8_t block[kLogHeaderSize] = {0};
    TEST_ASSERT_EQUAL_size_t(kLogHeaderSize, encode_header(sent, block, sizeof(block)));

    LogHeader received;
    TEST_ASSERT_TRUE(decode_header(block, sizeof(block), received));

    TEST_ASSERT_TRUE(received.boot_loop);
    TEST_ASSERT_EQUAL_UINT8(3, received.recent_reset_count);
}

// O requisito que o texto da issue escreveu para ninguém "otimizar" depois: o
// registro precisa reconstruir a amostra que o produziu. Este teste existe para
// ficar vermelho no dia em que alguém remover um campo bruto argumentando que
// ele já está na saída fundida.
//
// Posição bruta e fundida convivem de propósito. A fundida pode vir da ponte
// inercial ou da última válida com idade; sem a bruta ao lado, a decisão da
// issue 08 vira irreversível depois do voo.
void test_the_record_keeps_raw_and_fused_side_by_side(void) {
    LogRecord sent          = sample_record();
    sent.accel_mg[0]        = -1234;  // sinal precisa sobreviver
    sent.accel_mg[1]        = 16000;
    sent.accel_mg[2]        = 981;
    sent.gyro_ddps[0]       = -2000;
    sent.gyro_ddps[1]       = 15;
    sent.gyro_ddps[2]       = 3000;
    sent.vertical_speed_dms = -905;
    // A posição fundida difere da bruta: é o caso em que a distinção importa.
    sent.fused_latitude_1e7  = -232019999;
    sent.fused_longitude_1e7 = -458769999;
    sent.phase               = FlightPhase::Landed;
    sent.position_source     = PositionSource::LastValid;
    sent.health              = health_bit::kBaro | health_bit::kGps | health_bit::kE22;
    sent.estimator_resets    = 3;
    sent.stack_watermark_flight = 1024;
    sent.stack_watermark_io     = 768;

    uint8_t bytes[kLogRecordSize] = {0};
    TEST_ASSERT_EQUAL_size_t(kLogRecordSize, encode_record(sent, 42, bytes, sizeof(bytes)));

    LogRecord received;
    uint16_t  boot_count = 0;
    TEST_ASSERT_TRUE(decode_record(bytes, sizeof(bytes), received, boot_count));

    // Bruto
    TEST_ASSERT_EQUAL_INT16(-1234, received.accel_mg[0]);
    TEST_ASSERT_EQUAL_INT16(16000, received.accel_mg[1]);
    TEST_ASSERT_EQUAL_INT16(981, received.accel_mg[2]);
    TEST_ASSERT_EQUAL_INT16(-2000, received.gyro_ddps[0]);
    TEST_ASSERT_EQUAL_INT16(15, received.gyro_ddps[1]);
    TEST_ASSERT_EQUAL_INT16(3000, received.gyro_ddps[2]);
    TEST_ASSERT_EQUAL_FLOAT(98765.4f, received.pressure_pa);
    TEST_ASSERT_EQUAL_INT32(-232012345, received.gps.latitude_1e7);

    // Fundido, ao lado e não no lugar
    TEST_ASSERT_EQUAL_INT16(-905, received.vertical_speed_dms);
    TEST_ASSERT_EQUAL_INT32(-232019999, received.fused_latitude_1e7);
    TEST_ASSERT_EQUAL_INT32(-458769999, received.fused_longitude_1e7);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FlightPhase::Landed),
                            static_cast<uint8_t>(received.phase));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PositionSource::LastValid),
                            static_cast<uint8_t>(received.position_source));

    // Diagnóstico
    TEST_ASSERT_EQUAL_UINT8(sent.health, received.health);
    TEST_ASSERT_EQUAL_UINT8(3, received.estimator_resets);
    TEST_ASSERT_EQUAL_UINT16(1024, received.stack_watermark_flight);
    TEST_ASSERT_EQUAL_UINT16(768, received.stack_watermark_io);
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_record_round_trips_through_exactly_64_bytes);
    RUN_TEST(test_a_corrupted_record_is_rejected);
    RUN_TEST(test_a_truncated_stream_still_yields_every_complete_record);
    RUN_TEST(test_records_from_a_previous_flight_are_discarded);
    RUN_TEST(test_header_block_round_trips);
    RUN_TEST(test_header_records_the_boot_loop_event_and_recent_reset_count);
    RUN_TEST(test_the_record_keeps_raw_and_fused_side_by_side);
    return UNITY_END();
}
