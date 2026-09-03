#include "core/nmea.h"

namespace core {
namespace {

// Delimita o campo de índice `index` (0 = identificador da sentença). Devolve
// false se o campo não existir. Campos vazios existem e têm tamanho 0 — numa GGA
// sem fix, latitude e longitude são exatamente isso.
bool field(const char* line, uint8_t length, uint8_t index, const char*& begin,
           uint8_t& size) {
    uint8_t current = 0;
    uint8_t start = 0;
    for (uint8_t i = 0; i <= length; ++i) {
        const char c = (i == length) ? '\0' : line[i];
        if (c != '\0' && c != ',' && c != '*') {
            continue;
        }
        if (current == index) {
            begin = &line[start];
            size  = static_cast<uint8_t>(i - start);
            return true;
        }
        ++current;
        start = static_cast<uint8_t>(i + 1);
        if (c == '*') {
            break;  // o checksum não é um campo
        }
    }
    return false;
}

// Inteiro decimal sem sinal, parando no primeiro caractere não-dígito. Campo
// vazio devolve 0.
uint32_t to_uint(const char* begin, uint8_t size) {
    uint32_t value = 0;
    for (uint8_t i = 0; i < size; ++i) {
        if (begin[i] < '0' || begin[i] > '9') {
            break;
        }
        value = value * 10u + static_cast<uint32_t>(begin[i] - '0');
    }
    return value;
}

// Decimal com sinal, uma casa fracionária basta para altitude ("545.4"). Campo
// vazio ou malformado devolve 0. Só a altitude do GPS usa isto, e só no fallback
// da referência absurda (issue 06), então a precisão de decímetro sobra.
float to_float(const char* begin, uint8_t size) {
    if (size == 0) {
        return 0.0f;
    }
    uint8_t i = 0;
    float   sign = 1.0f;
    if (begin[0] == '-') {
        sign = -1.0f;
        i = 1;
    }
    float value = 0.0f;
    for (; i < size && begin[i] >= '0' && begin[i] <= '9'; ++i) {
        value = value * 10.0f + static_cast<float>(begin[i] - '0');
    }
    if (i < size && begin[i] == '.') {
        ++i;
        float scale = 0.1f;
        for (; i < size && begin[i] >= '0' && begin[i] <= '9'; ++i) {
            value += static_cast<float>(begin[i] - '0') * scale;
            scale *= 0.1f;
        }
    }
    return sign * value;
}

uint8_t index_of(const char* begin, uint8_t size, char c) {
    for (uint8_t i = 0; i < size; ++i) {
        if (begin[i] == c) {
            return i;
        }
    }
    return size;
}

uint8_t saturate(uint32_t value, uint8_t ceiling) {
    return value > ceiling ? ceiling : static_cast<uint8_t>(value);
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Converte "ddmm.mmmmm" (ou "dddmm.mmmmm") em graus × 1e7.
//
// A conversão é inteira de ponta a ponta. Um float de 32 bits tem 24 bits de
// mantissa e não representa 1e7 graus sem perder os últimos dígitos — que aqui
// valem cerca de um centímetro cada, mas que na soma dos erros de arredondamento
// deslocam a posição transmitida de forma difícil de explicar em campo.
int32_t nmea_degrees_1e7(const char* begin, uint8_t size) {
    const uint8_t dot = index_of(begin, size, '.');
    if (dot < 3) {
        return 0;  // curto demais para conter graus e minutos
    }

    const uint32_t whole   = to_uint(begin, dot);  // ddmm ou dddmm
    const uint32_t degrees = whole / 100u;
    const uint32_t minutes = whole % 100u;

    // Frações de minuto em unidades de 1e-5 min, que é a resolução que o NEO-6M
    // publica. Campos mais curtos são preenchidos com zeros à direita.
    uint32_t fraction = 0;
    uint8_t  digits = 0;
    for (uint8_t i = static_cast<uint8_t>(dot + 1); i < size && digits < 5; ++i) {
        if (begin[i] < '0' || begin[i] > '9') {
            break;
        }
        fraction = fraction * 10u + static_cast<uint32_t>(begin[i] - '0');
        ++digits;
    }
    for (; digits < 5; ++digits) {
        fraction *= 10u;
    }

    const uint32_t minutes_1e5 = minutes * 100000u + fraction;
    // graus = minutos / 60 ⇒ em 1e7: minutos_1e5 × 1e7 / (1e5 × 60) = × 5 / 3.
    // O maior valor intermediário é 6e6 × 5 = 3e7, e o resultado máximo é
    // 180 × 1e7 + 3e7 / 3 ≈ 1,81e9 — dentro de int32.
    return static_cast<int32_t>(degrees * 10000000u + (minutes_1e5 * 5u) / 3u);
}

}  // namespace

NmeaSentence NmeaParser::consume(char c) {
    if (c == '$') {
        // Ressincroniza em qualquer ponto: lixo na linha custa uma sentença, não
        // a sincronia do fluxo.
        length_  = 0;
        overrun_ = false;
        line_[length_++] = c;
        return NmeaSentence::None;
    }

    if (c == '\r' || c == '\n') {
        NmeaSentence result = NmeaSentence::None;
        if (overrun_) {
            result = NmeaSentence::Rejected;
        } else if (length_ > 0) {
            result = close_sentence();
        }
        length_  = 0;
        overrun_ = false;
        return result;
    }

    if (!overrun_) {
        if (length_ >= kCapacity) {
            overrun_ = true;
        } else {
            line_[length_++] = c;
        }
    }
    return NmeaSentence::None;
}

NmeaSentence NmeaParser::close_sentence() {
    // "$ttSSS" no mínimo: dois caracteres de talker, três de tipo.
    if (length_ < 6 || line_[0] != '$') {
        return NmeaSentence::Rejected;
    }

    const uint8_t star = index_of(line_, length_, '*');
    // O '*' precisa existir e ser seguido por dois dígitos hexadecimais.
    if (star >= length_ || static_cast<uint8_t>(star + 2) >= length_) {
        return NmeaSentence::Rejected;
    }
    if (!checksum_ok(star)) {
        return NmeaSentence::Rejected;
    }

    if (line_[3] == 'G' && line_[4] == 'G' && line_[5] == 'A') {
        parse_gga();
        return NmeaSentence::Gga;
    }
    return NmeaSentence::Other;
}

// XOR de tudo entre '$' e '*'. Sem esta verificação, um byte corrompido pelo PA
// de 1 W (hazard H16) vira uma posição plausível e errada.
bool NmeaParser::checksum_ok(uint8_t star) const {
    uint8_t computed = 0;
    for (uint8_t i = 1; i < star; ++i) {
        computed ^= static_cast<uint8_t>(line_[i]);
    }
    const int high = hex_digit(line_[star + 1]);
    const int low  = hex_digit(line_[star + 2]);
    if (high < 0 || low < 0) {
        return false;
    }
    return computed == static_cast<uint8_t>((high << 4) | low);
}

void NmeaParser::parse_gga() {
    gga_ = GgaReading();  // nada de campo anterior sobrevivendo a esta sentença

    const char* begin = nullptr;
    uint8_t     size = 0;

    if (field(line_, length_, 6, begin, size)) {
        gga_.fix_quality = saturate(to_uint(begin, size), 7);  // 3 bits no pacote
    }
    if (field(line_, length_, 7, begin, size)) {
        gga_.satellites = saturate(to_uint(begin, size), 15);
    }
    if (field(line_, length_, 8, begin, size) && size > 0) {
        // HDOP em décimos, depois em meios: 1,7 → 17 → 3 (1,5). O passo de 0,5 é
        // o que o campo de 4 bits do pacote comporta, e 15 significa "7,5 ou
        // pior", não "7,5".
        const uint8_t  dot    = index_of(begin, size, '.');
        const uint32_t units  = to_uint(begin, dot);
        const uint32_t tenths = (static_cast<uint8_t>(dot + 1) < size)
                                    ? to_uint(&begin[dot + 1], 1)
                                    : 0;
        gga_.hdop_half = saturate((units * 10u + tenths + 2u) / 5u, 15);
    }

    if (gga_.fix_quality == 0) {
        return;  // sem fix: os campos de posição da sentença estão vazios
    }

    const char* latitude = nullptr;
    uint8_t     latitude_size = 0;
    const char* north_south = nullptr;
    uint8_t     north_south_size = 0;
    const char* longitude = nullptr;
    uint8_t     longitude_size = 0;
    const char* east_west = nullptr;
    uint8_t     east_west_size = 0;
    if (!field(line_, length_, 2, latitude, latitude_size) || latitude_size == 0 ||
        !field(line_, length_, 3, north_south, north_south_size) || north_south_size == 0 ||
        !field(line_, length_, 4, longitude, longitude_size) || longitude_size == 0 ||
        !field(line_, length_, 5, east_west, east_west_size) || east_west_size == 0) {
        return;  // qualidade diz que há fix, mas a posição não veio: não há
    }

    int32_t latitude_1e7  = nmea_degrees_1e7(latitude, latitude_size);
    int32_t longitude_1e7 = nmea_degrees_1e7(longitude, longitude_size);
    if (north_south[0] == 'S') {
        latitude_1e7 = -latitude_1e7;
    }
    if (east_west[0] == 'W') {
        longitude_1e7 = -longitude_1e7;
    }

    gga_.latitude_1e7  = latitude_1e7;
    gga_.longitude_1e7 = longitude_1e7;
    gga_.has_position  = true;

    // Campo 9: altitude MSL, em metros. Presente junto com a posição.
    if (field(line_, length_, 9, begin, size)) {
        gga_.altitude_m = to_float(begin, size);
    }
}

}  // namespace core
