#include "hal/sd_log.h"

#include <Arduino.h>
#include <SdFat.h>

#include "pins.h"

namespace hal {
namespace {

// O cartão divide a VSPI com os dois rádios. 16 MHz é conservador de propósito:
// o barramento tem três dispositivos e fiação de protoboard, e a taxa de dados
// que precisamos é 1,6 kB/s — subir o clock não compra nada e compra erro.
constexpr uint32_t kSpiMhz = 16;

// Um SdFat por placa. Esta é a HAL; a regra de "nenhuma variável global" vale
// para o core/.
SdFat32  g_sd;
File32   g_file;

}  // namespace

bool SdLog::mount() {
    // Presença só: mesmo config de barramento do begin(), sem tocar no arquivo.
    const SdSpiConfig config(PIN_SD_CS, SHARED_SPI, SD_SCK_MHZ(kSpiMhz));
    return g_sd.begin(config);
}

bool SdLog::begin(uint16_t boot_count, float reference_pa, bool boot_loop,
                  uint8_t recent_reset_count) {
    open_ = false;
    full_ = false;
    boot_count_ = boot_count;
    fill_offset_ = 0;
    bytes_written_ = 0;
    consecutive_stalls_ = 0;

    const SdSpiConfig config(PIN_SD_CS, SHARED_SPI, SD_SCK_MHZ(kSpiMhz));
    if (!g_sd.begin(config)) {
        return false;
    }

    // Arquivo novo por boot, com índice incremental: um reset em pleno voo nunca
    // sobrescreve o arquivo anterior. O índice é o próprio contador de boot, que
    // é o mesmo número gravado em cada registro — o arquivo e o conteúdo dele
    // concordam sobre a qual voo pertencem.
    char name[16];
    snprintf(name, sizeof(name), "FLIGHT%03u.BIN", static_cast<unsigned>(boot_count % 1000u));

    if (!g_file.open(name, O_RDWR | O_CREAT | O_TRUNC)) {
        return false;
    }
    // Pré-alocação contígua. É isto que permite não tocar mais na tabela de
    // alocação até o fim do voo.
    if (!g_file.preAllocate(kPreallocatedBytes)) {
        g_file.close();
        return false;
    }

    uint8_t header[core::kLogHeaderSize];
    core::LogHeader meta;
    meta.boot_count       = boot_count;
    meta.reference_pa     = reference_pa;
    meta.pin_map_revision = PIN_MAP_REVISION;
    meta.boot_loop        = boot_loop;
    meta.recent_reset_count = recent_reset_count;
    core::encode_header(meta, header, sizeof(header));

    if (g_file.write(header, sizeof(header)) != static_cast<int>(sizeof(header))) {
        g_file.close();
        return false;
    }
    bytes_written_ = sizeof(header);
    // Primeira das DUAS escritas de metadados. A próxima é em close().
    g_file.sync();

    open_ = true;
    return true;
}

void SdLog::stage(const uint8_t* encoded_record) {
    if (!open_ || block_ready()) {
        return;  // pré-condição do chamador; nunca sobrescreve um bloco pronto
    }
    memcpy(block_ + fill_offset_, encoded_record, core::kLogRecordSize);
    fill_offset_ += core::kLogRecordSize;
}

bool SdLog::service(uint32_t now_ms) {
    (void)now_ms;
    if (!open_ || !block_ready()) {
        return false;
    }

    // Arquivo cheio: a próxima escrita cairia além da reserva contígua e obrigaria
    // a estender o arquivo, tocando a tabela de alocação. Em vez disso a gravação
    // para aqui. A telemetria não passa por esta classe — ela segue no superloop.
    // O bloco montado é descartado; o que já chegou ao cartão a varredura recupera.
    if (bytes_written_ + core::kLogBlockSize > kPreallocatedBytes) {
        full_ = true;
        open_ = false;
        return false;
    }

    const uint32_t started_ms = millis();
    const int      written = g_file.write(block_, core::kLogBlockSize);
    const uint32_t elapsed_ms = millis() - started_ms;

    if (written != static_cast<int>(core::kLogBlockSize)) {
        disable();
        return false;
    }
    fill_offset_ = 0;  // bloco gravado, volta a montar do zero
    bytes_written_ += core::kLogBlockSize;

    // Sem sync(): a tabela de alocação não é tocada durante o voo. O bloco já
    // está no cartão; o que fica desatualizado é só o comprimento registrado do
    // arquivo, e a varredura por magic + CRC não depende dele.
    if (elapsed_ms > kWriteStallMs) {
        ++stall_count_;
        if (++consecutive_stalls_ >= kMaxConsecutiveStalls) {
            // Um cartão doente não rouba mais tempo do link de recuperação.
            disable();
        }
    } else {
        consecutive_stalls_ = 0;
    }
    return true;
}

void SdLog::close() {
    if (!open_) {
        return;
    }
    // Última chance de gravar o bloco em montagem. Aqui já não há voo para
    // proteger, então grava-se mesmo o bloco parcial, com a cauda zerada — a
    // varredura por magic + CRC ignora o preenchimento.
    if (fill_offset_ > 0) {
        if (fill_offset_ < core::kLogBlockSize) {
            memset(block_ + fill_offset_, 0, core::kLogBlockSize - fill_offset_);
        }
        g_file.write(block_, core::kLogBlockSize);
        fill_offset_ = 0;
    }

    // Segunda e última escrita de metadados.
    g_file.truncate();
    g_file.close();
    open_ = false;
}

void SdLog::disable() {
    // Sem close(): um cartão que já falhou uma escrita não vai completar as de
    // metadados, e insistir só gasta tempo. O arquivo fica com o comprimento
    // pré-alocado e a varredura recupera tudo o que chegou ao cartão.
    open_ = false;
}

}  // namespace hal
