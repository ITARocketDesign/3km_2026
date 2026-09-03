// Suíte nativa do analisador NMEA. Roda no notebook: `pio test -e native`.
//
// Este é o trecho do caminho do GPS que erra em silêncio: enquadramento,
// checksum e a conversão de "ddmm.mmmmm" para graus. Um erro em qualquer um dos
// três produz uma posição plausível e errada, que ninguém detecta olhando a
// telemetria — daí exercitá-lo contra sentenças de valor conhecido.
//
// As sentenças abaixo têm checksum real. A primeira é o exemplo canônico de GGA
// da documentação de NMEA, e serve de âncora externa: se o cálculo de checksum
// ou a conversão de coordenadas divergirem da norma, é ela que quebra.
#include <unity.h>

#include "core/nmea.h"

using namespace core;

namespace {

// Alimenta o analisador byte a byte e devolve o último resultado que não foi
// None — ou seja, o que a última sentença fechada produziu.
NmeaSentence feed(NmeaParser& parser, const char* text) {
    NmeaSentence last = NmeaSentence::None;
    for (const char* p = text; *p != '\0'; ++p) {
        const NmeaSentence result = parser.consume(*p);
        if (result != NmeaSentence::None) {
            last = result;
        }
    }
    return last;
}

constexpr const char* kReferenceGga =
    "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n";

// Campo de lançamento no hemisfério sul e a oeste: os dois sinais invertidos.
constexpr const char* kSouthWestGga =
    "$GPGGA,181933.00,2312.01234,S,04552.87654,W,1,09,1.7,612.3,M,-3.5,M,,*78\r\n";

// GGA que o NEO-6M emite sem fix: posição vazia, qualidade 0, HDOP no teto.
constexpr const char* kNoFixGga = "$GPGGA,181933.00,,,,,0,00,99.99,,,,,,*67\r\n";

}  // namespace

// Âncora externa. 4807.038 N são 48° + 7,038' = 48,1173°; 01131.000 E são
// 11° + 31' = 11,516666…°, que trunca no último dígito de 1e7 — cerca de um
// centímetro.
void test_reference_sentence_decodes_to_known_coordinates(void) {
    NmeaParser parser;

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(NmeaSentence::Gga),
                            static_cast<uint8_t>(feed(parser, kReferenceGga)));

    const GgaReading& gga = parser.gga();
    TEST_ASSERT_TRUE(gga.has_position);
    TEST_ASSERT_EQUAL_INT32(481173000, gga.latitude_1e7);
    TEST_ASSERT_EQUAL_INT32(115166666, gga.longitude_1e7);
    TEST_ASSERT_EQUAL_UINT8(1, gga.fix_quality);
    TEST_ASSERT_EQUAL_UINT8(8, gga.satellites);
    TEST_ASSERT_EQUAL_UINT8(2, gga.hdop_half);  // 0,9 → 1,0
    // Campo 9: altitude MSL. Só o fallback de referência absurda (issue 06) a usa.
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 545.4f, gga.altitude_m);
}

// O único campo do pacote em que um erro de sinal manda a equipe de recuperação
// para o hemisfério errado.
void test_southern_and_western_hemispheres_are_negative(void) {
    NmeaParser parser;

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(NmeaSentence::Gga),
                            static_cast<uint8_t>(feed(parser, kSouthWestGga)));

    const GgaReading& gga = parser.gga();
    TEST_ASSERT_TRUE(gga.has_position);
    TEST_ASSERT_EQUAL_INT32(-232002056, gga.latitude_1e7);   // 23,2002056… S
    TEST_ASSERT_EQUAL_INT32(-458812756, gga.longitude_1e7);  // 45,8812756… W
    TEST_ASSERT_EQUAL_UINT8(9, gga.satellites);
    TEST_ASSERT_EQUAL_UINT8(3, gga.hdop_half);  // 1,7 → 1,5
}

// Um byte corrompido pelo PA de 1 W (hazard H16) vira uma posição plausível e
// errada se o checksum não for conferido. Aqui só o checksum está errado — todo
// o resto da sentença é válido, então nada além dele pode rejeitá-la.
void test_a_corrupted_checksum_is_rejected(void) {
    NmeaParser parser;
    const char* corrupted =
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48\r\n";

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(NmeaSentence::Rejected),
                            static_cast<uint8_t>(feed(parser, corrupted)));
}

// Sem fix, a sentença não tem posição — mas tem satélites e HDOP, que são a
// única pista de quão perto o receptor está de voltar. O pacote leva os dois.
void test_a_sentence_without_fix_keeps_the_counts(void) {
    NmeaParser parser;

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(NmeaSentence::Gga),
                            static_cast<uint8_t>(feed(parser, kNoFixGga)));

    const GgaReading& gga = parser.gga();
    TEST_ASSERT_FALSE(gga.has_position);
    TEST_ASSERT_EQUAL_UINT8(0, gga.fix_quality);
    TEST_ASSERT_EQUAL_UINT8(0, gga.satellites);
    TEST_ASSERT_EQUAL_UINT8(15, gga.hdop_half);  // 99,99 satura no teto de 4 bits
}

// Uma posição antiga sobrevivendo a uma sentença sem fix seria a pior falha
// possível deste arquivo: a telemetria continuaria mandando coordenadas que o
// receptor não está mais afirmando.
void test_a_lost_fix_does_not_leave_the_old_position_behind(void) {
    NmeaParser parser;
    feed(parser, kSouthWestGga);
    TEST_ASSERT_TRUE(parser.gga().has_position);

    feed(parser, kNoFixGga);

    TEST_ASSERT_FALSE(parser.gga().has_position);
    TEST_ASSERT_EQUAL_INT32(0, parser.gga().latitude_1e7);
    TEST_ASSERT_EQUAL_INT32(0, parser.gga().longitude_1e7);
}

// Os dois campos de 4 bits do pacote não comportam o que a GGA pode dizer.
void test_satellites_and_hdop_saturate_at_the_packet_field_width(void) {
    NmeaParser parser;
    const char* crowded_sky =
        "$GPGGA,181933.00,2312.01234,S,04552.87654,W,1,17,0.4,612.3,M,-3.5,M,,*75\r\n";

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(NmeaSentence::Gga),
                            static_cast<uint8_t>(feed(parser, crowded_sky)));

    TEST_ASSERT_EQUAL_UINT8(15, parser.gga().satellites);  // 17 não cabe em 4 bits
    TEST_ASSERT_EQUAL_UINT8(1, parser.gga().hdop_half);    // 0,4 → 0,5
}

// Qualquer sentença que não seja GGA é notícia para o adaptador: nós desligamos
// todas as outras, então vê-las significa que o receptor resetou.
void test_other_sentence_types_are_reported_as_other(void) {
    NmeaParser parser;

    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(NmeaSentence::Other),
        static_cast<uint8_t>(feed(
            parser,
            "$GPGSV,3,1,11,03,03,111,00,04,15,270,00,06,01,010,00,13,06,292,00*74\r\n")));

    // O $GPTXT de partida do NEO-6M é o primeiro sinal de que ele reiniciou.
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(NmeaSentence::Other),
        static_cast<uint8_t>(
            feed(parser, "$GPTXT,01,01,02,u-blox ag - www.u-blox.com*50\r\n")));
}

// Ruído na linha custa uma sentença, não a sincronia do fluxo: o '$' seguinte
// reenquadra em qualquer ponto.
void test_line_noise_costs_one_sentence_and_not_the_stream(void) {
    NmeaParser parser;

    // Lixo sem '$': fechado sem enquadramento válido.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(NmeaSentence::Rejected),
                            static_cast<uint8_t>(feed(parser, "GGA,1,2,3*7F\r\n")));

    // Lixo grudado no início de uma sentença boa: o '$' descarta o prefixo.
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(NmeaSentence::Gga),
        static_cast<uint8_t>(feed(
            parser,
            "\x01\x02lixo$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n")));
    TEST_ASSERT_EQUAL_INT32(481173000, parser.gga().latitude_1e7);
}

// Uma linha maior que qualquer sentença NMEA legítima é descartada inteira, em
// vez de interpretada pela metade — e a sentença seguinte é analisada normalmente.
void test_an_absurd_line_is_discarded_whole(void) {
    NmeaParser parser;

    char absurd[200];
    absurd[0] = '$';
    for (int i = 1; i < 197; ++i) {
        absurd[i] = 'A';
    }
    absurd[197] = '\r';
    absurd[198] = '\n';
    absurd[199] = '\0';

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(NmeaSentence::Rejected),
                            static_cast<uint8_t>(feed(parser, absurd)));

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(NmeaSentence::Gga),
                            static_cast<uint8_t>(feed(parser, kReferenceGga)));
    TEST_ASSERT_EQUAL_INT32(481173000, parser.gga().latitude_1e7);
}

// A UART entrega bytes quando quer. Uma sentença partida em dois pedaços, com
// outras chegadas no meio, precisa produzir exatamente o mesmo resultado.
void test_a_sentence_split_across_reads_parses_identically(void) {
    NmeaParser parser;

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(NmeaSentence::None),
                            static_cast<uint8_t>(feed(parser, "$GPGGA,123519,4807.0")));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(NmeaSentence::Gga),
        static_cast<uint8_t>(feed(parser, "38,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47\r\n")));

    TEST_ASSERT_EQUAL_INT32(481173000, parser.gga().latitude_1e7);
    TEST_ASSERT_EQUAL_INT32(115166666, parser.gga().longitude_1e7);
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_reference_sentence_decodes_to_known_coordinates);
    RUN_TEST(test_southern_and_western_hemispheres_are_negative);
    RUN_TEST(test_a_corrupted_checksum_is_rejected);
    RUN_TEST(test_a_sentence_without_fix_keeps_the_counts);
    RUN_TEST(test_a_lost_fix_does_not_leave_the_old_position_behind);
    RUN_TEST(test_satellites_and_hdop_saturate_at_the_packet_field_width);
    RUN_TEST(test_other_sentence_types_are_reported_as_other);
    RUN_TEST(test_line_noise_costs_one_sentence_and_not_the_stream);
    RUN_TEST(test_an_absurd_line_is_discarded_whole);
    RUN_TEST(test_a_sentence_split_across_reads_parses_identically);
    return UNITY_END();
}
