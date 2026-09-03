// tools/replay.cpp — harness de replay do log de voo (issue 14).
//
// Lê um arquivo bruto gravado pelo computador de bordo, reconstrói a sequência de
// amostras a partir dos campos brutos, realimenta o núcleo (`FlightComputer`) no
// host, e compara a saída recalculada com a que o firmware gravou em voo. Roda no
// host, não na placa. É como uma mudança no estimador é avaliada: recompila este
// binário contra o novo `src/core` e reproduz um voo antigo.
//
// A lógica de reconstrução e comparação é pura e vive em `src/core/replay.*`, com
// suíte nativa própria (`test_replay`). Aqui está só a casca de host: ler o
// arquivo, separar o voo pelo contador de boot, e imprimir o relatório.
//
// ── Duas lacunas conhecidas ─────────────────────────────────────────────────
//
// O registro de 64 B não carrega `gps.altitude_m` nem `accel_saturated` (ver
// `src/core/replay.h`). Um voo real com GPS válido ou com saturação no boost NÃO se
// reproduz bit a bit — por isso a tolerância abaixo é declarada, não zero. Fora
// dessas janelas a reprodução é exata.
//
// Compilação:
//     make -C tools replay
//
// Uso:
//     tools/replay FLIGHT007.BIN [tolerancia_altitude_m] [tolerancia_posicao_graus]
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/log_codec.h"
#include "core/replay.h"

namespace {

std::vector<uint8_t> read_file(const char* path) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        std::fprintf(stderr, "não consegui abrir %s\n", path);
        std::exit(1);
    }
    std::vector<uint8_t> bytes;
    uint8_t              chunk[4096];
    size_t               read = 0;
    while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
        bytes.insert(bytes.end(), chunk, chunk + read);
    }
    std::fclose(file);
    return bytes;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 4) {
        std::fprintf(stderr,
                     "uso: %s ARQUIVO.BIN [tolerancia_altitude_m] [tolerancia_posicao_graus]\n",
                     argv[0]);
        return 2;
    }

    // Guarda de suficiência de campos (AC 8): se alguém tirou um campo bruto do
    // formato de 64 B, a reconstrução deixou de ser possível e reproduzir seria
    // mentir. Falha alto antes de tocar no arquivo.
    if (!core::record_reconstruction_fields_present()) {
        std::fprintf(stderr,
                     "ERRO: o registro de log perdeu um campo bruto necessário à "
                     "reconstrução.\nO harness de replay não é mais válido — ver "
                     "src/core/replay.h.\n");
        return 1;
    }

    core::ReplayTolerance tolerance;
    if (argc >= 3) tolerance.altitude_m = static_cast<float>(std::atof(argv[2]));
    if (argc >= 4) {
        tolerance.position_1e7 = static_cast<int32_t>(std::atof(argv[3]) * 1e7);
    }

    const std::vector<uint8_t> bytes = read_file(argv[1]);
    if (bytes.size() < core::kLogHeaderSize) {
        std::fprintf(stderr, "arquivo menor que o bloco de cabeçalho\n");
        return 1;
    }

    core::LogHeader header;
    if (!core::decode_header(bytes.data(), bytes.size(), header)) {
        std::fprintf(stderr,
                     "cabeçalho ilegível: magic, versão de formato ou CRC não batem.\n"
                     "sem o contador de boot não dá para separar este voo do anterior.\n");
        return 1;
    }
    std::fprintf(stderr, "voo %u, formato v%u, referência %.1f Pa, mapa de pinos rev. %u\n",
                 header.boot_count, header.format_version, header.reference_pa,
                 header.pin_map_revision);

    const uint8_t* data  = bytes.data() + core::kLogHeaderSize;
    const size_t   len   = bytes.size() - core::kLogHeaderSize;
    const size_t   slots = len / core::kLogRecordSize;

    std::vector<core::LogRecord> records(slots);
    const size_t recovered =
        slots == 0 ? 0
                   : core::scan_records(data, len, header.boot_count, records.data(), slots);
    std::fprintf(stderr, "%zu de %zu slots recuperados\n", recovered, slots);

    // A config precisa ser a que voou. Sem persistência de config no cartão, usa-se
    // o default do firmware; se o voo rodou com outra, ajuste-a aqui e recompile.
    const core::FlightComputerConfig config;
    const core::ReplayDivergence d =
        core::replay_and_compare(records.data(), recovered, config, tolerance);

    std::printf("registros reproduzidos: %u\n", d.records);
    std::printf("pior divergência de altitude: %.3f m (tolerância %.3f m)\n",
                d.max_altitude_diff_m, tolerance.altitude_m);
    std::printf("pior divergência de posição: %.7f graus (tolerância %.7f graus)\n",
                d.max_position_diff_1e7 / 1e7, tolerance.position_1e7 / 1e7);
    std::printf("divergências de fase: %u\n", d.phase_mismatches);
    std::printf("divergências de fonte de posição: %u\n", d.source_mismatches);
    std::printf("veredito: %s\n", d.within_tolerance ? "DENTRO da tolerância"
                                                       : "FORA da tolerância");

    // Sai não-zero na divergência: usável direto num passo de CI de regressão.
    return d.within_tolerance ? 0 : 1;
}
