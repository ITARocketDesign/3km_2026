// Suíte do log durável (issue 06) — as invariantes do formato no cartão que o
// caminho durável depende, verificadas pelo núcleo puro.
//
// O codec de log foi copiado congelado do ELE3km na issue 01; esta suíte fixa as
// três propriedades que a issue 06 põe em uso e que a suíte de contrato ainda não
// cobria: o empacotamento de 8 registros num bloco de 512 B sem cruzar fronteira,
// a ida e volta do cabeçalho com o datum, e a varredura que separa este voo do
// anterior e descarta a cauda cortada por um corte de energia.
//
// Nenhum teste inspeciona estado interno — só o comportamento externo do codec.
#include <string.h>

#include <unity.h>

#include "core/log_codec.h"
#include "core/types.h"

using namespace core;

namespace {

// Um registro distinto por índice: sequência e brutos diferentes, para provar que
// cada slot de 64 B decodifica de volta o SEU registro, não o do vizinho.
LogRecord record_n(uint32_t seq) {
    LogRecord r;
    r.t_ms          = 1000u + seq * 20u;
    r.sequence      = seq;
    r.pressure_pa   = 90000.0f - static_cast<float>(seq);
    r.temperature_c = 20.0f + static_cast<float>(seq) * 0.5f;
    r.altitude_m    = 1000.0f + static_cast<float>(seq);
    r.baro_valid    = true;
    r.accel_mg[2]   = static_cast<int16_t>(1000 + seq);
    r.health        = health_bit::kBaro | health_bit::kSd;
    return r;
}

}  // namespace

// ── 8 registros de 64 B preenchem exatamente um bloco de 512 B, sem cruzar ────
// A fronteira do bloco é a unidade de transação do cartão; qualquer registro que
// a cruzasse poderia ser partido em dois por um corte de energia no meio do bloco.
void test_eight_records_fill_one_block_without_crossing(void) {
    // A geometria do formato: 8 registros de 64 B são um bloco de 512 B exato.
    TEST_ASSERT_EQUAL_UINT(512u, kLogBlockSize);
    TEST_ASSERT_EQUAL_UINT(64u, kLogRecordSize);
    TEST_ASSERT_EQUAL_UINT(kLogBlockSize, kLogRecordSize * 8u);

    const uint16_t boot = 7;
    uint8_t block[kLogBlockSize];
    memset(block, 0, sizeof(block));

    // Monta o bloco registro a registro, cada um no seu slot de 64 B.
    for (uint32_t i = 0; i < 8; ++i) {
        const size_t n =
            encode_record(record_n(i), boot, block + i * kLogRecordSize, kLogRecordSize);
        TEST_ASSERT_EQUAL_UINT(kLogRecordSize, n);  // coube em 64 B, não transbordou
    }

    // Cada slot decodifica de volta o SEU registro, lendo só os seus 64 B — prova
    // de que nenhum registro invade o slot do vizinho.
    for (uint32_t i = 0; i < 8; ++i) {
        LogRecord out;
        uint16_t  out_boot = 0;
        TEST_ASSERT_TRUE(
            decode_record(block + i * kLogRecordSize, kLogRecordSize, out, out_boot));
        TEST_ASSERT_EQUAL_UINT16(boot, out_boot);
        TEST_ASSERT_EQUAL_UINT32(i, out.sequence);
        TEST_ASSERT_EQUAL_INT16(static_cast<int16_t>(1000 + i), out.accel_mg[2]);
    }
}

// ── O cabeçalho sobrevive à ida e volta, com o datum barométrico ────────────
// O cabeçalho é a primeira das duas únicas escritas de metadados. Ele carrega o
// datum (101325 Pa) contra o qual a altitude do voo foi medida (hazard H4) e o
// contador de boot que o recuperador cruza com o de cada registro.
void test_header_roundtrips_with_datum(void) {
    LogHeader in;
    in.boot_count         = 42;
    in.reference_pa       = 101325.0f;  // datum ISA fixo (survival_computer)
    in.pin_map_revision   = 3;
    in.boot_loop          = true;
    in.recent_reset_count = 5;

    uint8_t buf[kLogHeaderSize];
    const size_t n = encode_header(in, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(kLogHeaderSize, n);  // o cabeçalho ocupa o bloco 0 inteiro

    // Magic e versão nos dois primeiros bytes, como no registro.
    TEST_ASSERT_EQUAL_HEX8(kLogRecordMagic, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(kLogFormatVersion, buf[1]);

    LogHeader out;
    TEST_ASSERT_TRUE(decode_header(buf, n, out));
    TEST_ASSERT_EQUAL_UINT16(42u, out.boot_count);
    TEST_ASSERT_EQUAL_FLOAT(101325.0f, out.reference_pa);
    TEST_ASSERT_EQUAL_UINT8(3, out.pin_map_revision);
    TEST_ASSERT_TRUE(out.boot_loop);
    TEST_ASSERT_EQUAL_UINT8(5, out.recent_reset_count);
    TEST_ASSERT_EQUAL_UINT8(kLogFormatVersion, out.format_version);
}

// ── Um cabeçalho com CRC quebrado é rejeitado ───────────────────────────────
// Um cabeçalho ilegível é motivo para NÃO confiar no contador de boot: sem ele
// não há como separar este voo do anterior no arquivo pré-alocado.
void test_header_rejects_corrupted_crc(void) {
    LogHeader in;
    in.boot_count   = 42;
    in.reference_pa = 101325.0f;

    uint8_t buf[kLogHeaderSize];
    encode_header(in, buf, sizeof(buf));
    buf[4] ^= 0xFF;  // corrompe um byte da referência barométrica

    LogHeader out;
    TEST_ASSERT_FALSE(decode_header(buf, sizeof(buf), out));
}

// ── A varredura entrega só este voo e descarta a cauda cortada ──────────────
// O arquivo pré-alocado cai nos clusters do voo anterior: um registro daquele voo
// valida por magic + CRC e entraria no meio desta trajetória se o contador de boot
// não o barrasse. E um corte de energia deixa a cauda com um registro incompleto,
// que a varredura não pode aceitar como bom.
void test_scan_returns_this_boot_and_drops_truncated_tail(void) {
    const uint16_t this_boot  = 7;
    const uint16_t other_boot = 9;  // um voo anterior nos mesmos clusters

    // Seis registros completos, intercalando este voo e o anterior, mais uma cauda
    // de 40 B (registro cortado por um corte de energia).
    uint8_t stream[6 * kLogRecordSize + 40];
    memset(stream, 0, sizeof(stream));

    const uint16_t boots[6]   = {this_boot, other_boot, this_boot,
                                 this_boot, other_boot, this_boot};
    size_t         this_count = 0;
    for (uint32_t i = 0; i < 6; ++i) {
        encode_record(record_n(i), boots[i], stream + i * kLogRecordSize, kLogRecordSize);
        if (boots[i] == this_boot) ++this_count;
    }
    // A cauda: os primeiros 40 B de um sétimo registro deste voo, sem o resto nem
    // o CRC. `scan` anda em passos de 64 B e nunca chega a lê-la.
    uint8_t partial[kLogRecordSize];
    encode_record(record_n(99), this_boot, partial, kLogRecordSize);
    memcpy(stream + 6 * kLogRecordSize, partial, 40);

    LogRecord out[8];
    const size_t found = scan_records(stream, sizeof(stream), this_boot, out, 8);

    // Só os quatro registros deste voo; nem os do voo anterior, nem a cauda.
    TEST_ASSERT_EQUAL_UINT(4u, this_count);
    TEST_ASSERT_EQUAL_UINT(this_count, found);
    // E vêm na ordem do arquivo: sequências 0, 2, 3, 5 (as posições deste voo).
    const uint32_t expected_seq[4] = {0, 2, 3, 5};
    for (size_t i = 0; i < found; ++i) {
        TEST_ASSERT_EQUAL_UINT32(expected_seq[i], out[i].sequence);
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_eight_records_fill_one_block_without_crossing);
    RUN_TEST(test_header_roundtrips_with_datum);
    RUN_TEST(test_header_rejects_corrupted_crc);
    RUN_TEST(test_scan_returns_this_boot_and_drops_truncated_tail);
    return UNITY_END();
}
