#include "hal/gps_neo6m.h"

#include <Arduino.h>

#include "pins.h"

namespace hal {
namespace {

// ── Janelas de tempo ────────────────────────────────────────────────────────

// Um fix parado por mais que isto deixa de valer como posição corrente. Dois
// períodos de GGA a 5 Hz seriam 400 ms; 2 s tolera perdas isoladas de sentença
// sem declarar perda de fix a cada respingo.
constexpr uint32_t kFixFreshMs = 2000;

// "O receptor está vivo", que é o bit de saúde — não "há fix".
constexpr uint32_t kReceivingFreshMs = 2000;

// Cadência da reaplicação burra da configuração (issue 05): a config inteira é
// reenviada a cada ~10 s, incondicionalmente, para sempre. Um receptor que
// resetou silenciosamente (voltando a Portable) volta a Airborne em ≤10 s. Sem
// detecção de reset — a reaplicação periódica É a recuperação.
constexpr uint32_t kReconfigureIntervalMs = 10000;

// Ocupação a partir da qual o buffer conta como transbordado.
//
// A margem existe porque o buffer circular do driver não é garantidamente
// preenchível até o último byte, e um limiar em 512 exato poderia nunca
// disparar — um contador de diagnóstico que nunca dispara é pior que nenhum.
// Os 32 B de margem são ~33 ms a 9600 baud: quem chega aqui já perdeu sentença
// ou está a um piscar de perder. O contador erra para o lado de reportar demais,
// que é o lado certo para um diagnóstico.
constexpr int kOverflowThresholdBytes = kGpsRxBufferBytes - 32;

// ── UBX ─────────────────────────────────────────────────────────────────────

constexpr uint8_t kUbxSync1 = 0xB5;
constexpr uint8_t kUbxSync2 = 0x62;

constexpr uint8_t kClassCfg = 0x06;
constexpr uint8_t kIdCfgMsg  = 0x01;  // taxa de uma mensagem na porta corrente
constexpr uint8_t kIdCfgRate = 0x08;  // taxa de navegação
constexpr uint8_t kIdCfgNav5 = 0x24;  // motor de navegação

constexpr uint8_t kNmeaClass = 0xF0;
constexpr uint8_t kNmeaGga = 0x00;
constexpr uint8_t kNmeaGll = 0x01;
constexpr uint8_t kNmeaGsa = 0x02;
constexpr uint8_t kNmeaGsv = 0x03;
constexpr uint8_t kNmeaRmc = 0x04;
constexpr uint8_t kNmeaVtg = 0x05;
constexpr uint8_t kNmeaZda = 0x08;

// As seis que o NEO-6M liga de fábrica e que não usamos. Desligá-las é o que faz
// a GGA caber com folga a 5 Hz em 9600 baud. (Na barra não há detector que trate
// uma sentença não-GGA como sinal de reset — a reaplicação periódica cobre isso.)
constexpr uint8_t kDisabledNmea[] = {kNmeaGll, kNmeaGsa, kNmeaGsv,
                                     kNmeaRmc, kNmeaVtg, kNmeaZda};

}  // namespace

bool GpsNeo6m::begin(uint32_t now_ms) {
    // Os dois buffers ANTES do begin(): depois disso o driver já está instalado
    // e as chamadas não têm efeito.
    if (uart_.setRxBufferSize(kGpsRxBufferBytes) != kGpsRxBufferBytes) {
        return false;
    }
    // Sem buffer de transmissão o driver escreve direto na FIFO de 128 B e
    // BLOQUEIA até caber. A rajada de configuração passa de 150 B, e um bloqueio
    // desses no meio do voo — que é exatamente quando a reaplicação acontece —
    // atrasaria a amostragem e a cadência de transmissão.
    uart_.setTxBufferSize(256);

    uart_.begin(GPS_BAUD, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);

    apply_configuration(now_ms);
    return true;
}

void GpsNeo6m::service(uint32_t now_ms) {
    // A ocupação é medida ANTES de drenar: um buffer encontrado cheio significa
    // que bytes foram descartados enquanto ninguém estava lendo. Não há ação
    // corretiva — a sentença perdida já foi perdida. O contador é para a próxima
    // revisão: se ele subir durante o voo, o timeout de I²C está roubando tempo
    // demais da drenagem (PRD §18).
    if (uart_.available() >= kOverflowThresholdBytes) {
        ++uart_overflow_count_;
    }

    drain(now_ms);

    // Reaplicação burra: a config inteira a cada ~10 s, sem olhar para nada. É a
    // única recuperação de reset da barra (issue 05). begin() já configurou uma
    // vez, então o primeiro reenvio cai ~10 s depois do boot.
    if (static_cast<int32_t>(now_ms - last_reconfigure_ms_) >=
        static_cast<int32_t>(kReconfigureIntervalMs)) {
        apply_configuration(now_ms);
        ++reconfigure_count_;
    }
}

void GpsNeo6m::drain(uint32_t now_ms) {
    while (uart_.available() > 0) {
        switch (parser_.consume(static_cast<char>(uart_.read()))) {
            case core::NmeaSentence::Gga:
                last_sentence_ms_ = now_ms;
                has_sentence_     = true;
                accept_gga(parser_.gga(), now_ms);
                break;

            case core::NmeaSentence::Other:
                // Uma sentença não-GGA (as que desligamos, ou o $GPTXT de partida)
                // só conta como sinal de vida. Não dispara reconfiguração: a barra
                // não detecta reset, reaplica a config no tique periódico.
                last_sentence_ms_ = now_ms;
                has_sentence_     = true;
                break;

            case core::NmeaSentence::Rejected:
                // Checksum ruim ou enquadramento quebrado. Não conta nem como
                // sinal de vida nem como sinal de reset: pode ser ruído de linha
                // acoplado pelo PA (hazard H16), e não uma decisão do receptor.
                break;

            case core::NmeaSentence::None:
                break;
        }
    }
}

void GpsNeo6m::accept_gga(const core::GgaReading& reading, uint32_t now_ms) {
    fix_.fix_quality = reading.fix_quality;
    fix_.satellites  = reading.satellites;
    fix_.hdop_half   = reading.hdop_half;

    if (!reading.has_position) {
        has_fix_ = false;  // posição anterior preservada, mas não mais publicada
        return;
    }

    fix_.latitude_1e7  = reading.latitude_1e7;
    fix_.longitude_1e7 = reading.longitude_1e7;
    fix_.altitude_m    = reading.altitude_m;
    last_fix_ms_       = now_ms;
    has_fix_           = true;
}

core::GpsFix GpsNeo6m::fix(uint32_t now_ms) const {
    core::GpsFix out = fix_;
    out.receiving = has_sentence_ && (now_ms - last_sentence_ms_) <= kReceivingFreshMs;
    out.valid     = has_fix_ && (now_ms - last_fix_ms_) <= kFixFreshMs;
    return out;
}

// A configuração é reenviada INTEIRA, nunca em parte. Um receptor que resetou
// perdeu tudo, e adivinhar o que ele ainda tem é como se descobre em voo que a
// adivinhação estava errada.
void GpsNeo6m::apply_configuration(uint32_t now_ms) {
    // UBX-CFG-NAV5: modelo dinâmico Airborne <4g. A máscara em 0x0001 diz ao
    // receptor para aplicar só o modelo e ignorar o resto do payload.
    uint8_t nav5[36] = {0};
    nav5[0] = 0x01;  // mask, byte baixo: apenas dyn
    nav5[2] = 0x08;  // dynModel = 8 = Airborne <4g
    send_ubx(kClassCfg, kIdCfgNav5, nav5, sizeof(nav5));

    // UBX-CFG-MSG: desliga o que não usamos, garante a GGA em toda solução.
    for (uint8_t i = 0; i < sizeof(kDisabledNmea); ++i) {
        const uint8_t off[3] = {kNmeaClass, kDisabledNmea[i], 0};
        send_ubx(kClassCfg, kIdCfgMsg, off, sizeof(off));
    }
    const uint8_t gga_on[3] = {kNmeaClass, kNmeaGga, 1};
    send_ubx(kClassCfg, kIdCfgMsg, gga_on, sizeof(gga_on));

    // UBX-CFG-RATE: 200 ms entre soluções = 5 Hz. A 278 m/s no boost, 1 Hz
    // deixaria quase 300 m entre fixes.
    const uint8_t rate[6] = {0xC8, 0x00,   // measRate = 200 ms
                             0x01, 0x00,   // navRate = 1 solução por medição
                             0x01, 0x00};  // timeRef = tempo GPS
    send_ubx(kClassCfg, kIdCfgRate, rate, sizeof(rate));

    // Sem UBX-CFG-CFG: a barra NÃO salva na memória com bateria de backup. Um
    // reset por queda de rail devolve o receptor a Portable, e é a reaplicação
    // incondicional a cada ~10 s que o traz de volta a Airborne — sem depender de
    // a célula de backup do breakout estar viva (issue 05 / PRD).
    last_reconfigure_ms_ = now_ms;
}

void GpsNeo6m::send_ubx(uint8_t message_class, uint8_t message_id, const uint8_t* payload,
                        uint8_t length) {
    const uint8_t header[6] = {kUbxSync1, kUbxSync2, message_class, message_id, length, 0};

    // Checksum de Fletcher de 8 bits sobre classe, id, comprimento e payload.
    uint8_t ck_a = 0;
    uint8_t ck_b = 0;
    for (uint8_t i = 2; i < sizeof(header); ++i) {
        ck_a = static_cast<uint8_t>(ck_a + header[i]);
        ck_b = static_cast<uint8_t>(ck_b + ck_a);
    }
    for (uint8_t i = 0; i < length; ++i) {
        ck_a = static_cast<uint8_t>(ck_a + payload[i]);
        ck_b = static_cast<uint8_t>(ck_b + ck_a);
    }
    const uint8_t checksum[2] = {ck_a, ck_b};

    uart_.write(header, sizeof(header));
    if (length > 0) {
        uart_.write(payload, length);
    }
    uart_.write(checksum, sizeof(checksum));
}

}  // namespace hal
