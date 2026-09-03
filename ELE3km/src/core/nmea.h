// core/nmea.h — analisador de sentenças NMEA.
//
// Vive no núcleo puro, e não na HAL, porque é a parte mais fácil de errar de
// forma silenciosa em todo o caminho do GPS: enquadramento, checksum e a
// conversão de "ddmm.mmmmm" para graus. Um erro em qualquer um dos três produz
// uma posição plausível e errada, que ninguém detecta olhando a telemetria. Aqui
// ele roda na suíte nativa, contra sentenças conhecidas, no notebook.
//
// Só a GGA é interpretada, porque só ela é deixada ligada no receptor
// (hal/gps_neo6m.h) e porque ela carrega exatamente os campos do pacote:
// posição, qualidade do fix, satélites e HDOP.
//
// O analisador não sabe que horas são, não sabe o que é um reset de receptor e
// não guarda posição anterior. Ele converte bytes em campos e diz que tipo de
// sentença acabou de fechar; o que fazer com isso — inclusive tratar uma
// sentença que devia estar desligada como sinal de reset — é do adaptador.
#pragma once

#include <stdint.h>

namespace core {

// O que a chegada de um byte completou.
enum class NmeaSentence : uint8_t {
    None    = 0,  // sentença ainda em curso
    Gga,          // GGA válida — os campos estão em gga()
    Other,        // sentença válida de outro tipo, não interpretada
    Rejected,     // sentença descartada: checksum, enquadramento ou tamanho
};

// Uma GGA já convertida para as unidades do pacote. É o que a sentença disse,
// não o melhor conhecimento do sistema: sem fix, a posição vem zerada e é
// `has_position` que diz isso.
struct GgaReading {
    bool    has_position = false;
    int32_t latitude_1e7 = 0;    // graus × 1e7, negativo ao sul
    int32_t longitude_1e7 = 0;   // graus × 1e7, negativo a oeste
    float   altitude_m = 0.0f;   // altitude MSL (campo 9 da GGA); só a issue 06 a usa
    uint8_t fix_quality = 0;     // indicador da GGA, saturado em 7 (3 bits)
    uint8_t satellites = 0;      // saturado em 15 (4 bits)
    uint8_t hdop_half = 0;       // HDOP × 2, saturado em 15 (4 bits)
};

class NmeaParser {
  public:
    // Consome um byte do fluxo. Um '$' reinicia o enquadramento em qualquer
    // ponto, então lixo na linha custa uma sentença, não a sincronia.
    NmeaSentence consume(char c);

    // Válido logo depois de consume() devolver Gga.
    const GgaReading& gga() const { return gga_; }

  private:
    NmeaSentence close_sentence();
    bool         checksum_ok(uint8_t star) const;
    void         parse_gga();

    // Uma sentença NMEA cabe em 82 caracteres por norma. O excedente é lixo, e
    // é descartado inteiro em vez de interpretado pela metade.
    static constexpr uint8_t kCapacity = 96;
    char    line_[kCapacity] = {0};
    uint8_t length_ = 0;
    bool    overrun_ = false;

    GgaReading gga_;
};

}  // namespace core
