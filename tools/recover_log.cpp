// tools/recover_log.cpp — recuperador de log de voo.
//
// Lê um arquivo bruto gravado pelo computador de bordo e imprime em CSV todo
// registro que sobrevive a magic, CRC e contador de boot. Roda no host, não na
// placa.
//
// O ponto de existir: o arquivo é pré-alocado, e a tabela de alocação não é
// tocada durante o voo. Se a energia cair no ar — que é o caso normal, não o
// excepcional — o COMPRIMENTO registrado do arquivo estará errado e a cauda
// estará partida no meio de um registro. Nenhuma ferramenta que confie no
// comprimento recupera esse voo. Esta varre.
//
// Também é o que separa este voo do anterior: o arquivo pré-alocado cai nos
// clusters do voo passado, e aqueles registros validam perfeitamente por magic e
// CRC. O contador de boot do cabeçalho é o que os descarta.
//
// Compilação:
//     make -C tools
//
// Uso:
//     tools/recover_log FLIGHT007.BIN > voo7.csv
//
// A saída é o insumo do harness de replay da issue 14: o log carrega os valores
// brutos E os fundidos, então um voo real pode ser reproduzido pelo núcleo no
// host e as saídas comparadas.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "core/log_codec.h"

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

void print_csv_header() {
    std::printf(
        "sequencia,t_ms,pressao_pa,temperatura_c,altitude_m,baro_ok,"
        "vel_vertical_dms,gps_lat,gps_lon,gps_valido,gps_recebendo,gps_sats,gps_hdop,"
        "gps_qualidade,fundida_lat,fundida_lon,fonte_posicao,fase,"
        "accel_x_mg,accel_y_mg,accel_z_mg,gyro_x_ddps,gyro_y_ddps,gyro_z_ddps,"
        "saude,resets_estimador,overflows_uart,watermark_voo,watermark_io\n");
}

void print_csv_row(const core::LogRecord& r) {
    std::printf(
        "%u,%u,%.2f,%.2f,%.3f,%d,"
        "%d,%.7f,%.7f,%d,%d,%u,%.1f,"
        "%u,%.7f,%.7f,%u,%u,"
        "%d,%d,%d,%d,%d,%d,"
        "%u,%u,%u,%u,%u\n",
        r.sequence, r.t_ms, r.pressure_pa, r.temperature_c, r.altitude_m,
        r.baro_valid ? 1 : 0, r.vertical_speed_dms, r.gps.latitude_1e7 / 1e7,
        r.gps.longitude_1e7 / 1e7, r.gps.valid ? 1 : 0, r.gps.receiving ? 1 : 0,
        r.gps.satellites, r.gps.hdop_half / 2.0, r.gps.fix_quality,
        r.fused_latitude_1e7 / 1e7, r.fused_longitude_1e7 / 1e7,
        static_cast<unsigned>(r.position_source), static_cast<unsigned>(r.phase),
        r.accel_mg[0], r.accel_mg[1], r.accel_mg[2], r.gyro_ddps[0], r.gyro_ddps[1],
        r.gyro_ddps[2], r.health, r.estimator_resets, r.gps_uart_overflows,
        r.stack_watermark_flight, r.stack_watermark_io);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "uso: %s ARQUIVO.BIN > saida.csv\n", argv[0]);
        return 2;
    }

    const std::vector<uint8_t> bytes = read_file(argv[1]);
    if (bytes.size() < core::kLogHeaderSize) {
        std::fprintf(stderr, "arquivo menor que o bloco de cabeçalho\n");
        return 1;
    }

    core::LogHeader header;
    if (!core::decode_header(bytes.data(), bytes.size(), header)) {
        // Sem cabeçalho legível não há contador de boot, e sem ele não há como
        // separar este voo do anterior. Melhor falhar alto do que entregar uma
        // trajetória com registros de dois voos misturados.
        std::fprintf(stderr,
                     "cabeçalho ilegível: magic, versão de formato ou CRC não batem.\n"
                     "sem o contador de boot não dá para separar este voo do anterior.\n");
        return 1;
    }

    std::fprintf(stderr, "voo %u, formato v%u, referência %.1f Pa, mapa de pinos rev. %u\n",
                 header.boot_count, header.format_version, header.reference_pa,
                 header.pin_map_revision);

    const uint8_t* data = bytes.data() + core::kLogHeaderSize;
    const size_t   len = bytes.size() - core::kLogHeaderSize;
    const size_t   slots = len / core::kLogRecordSize;

    std::vector<core::LogRecord> records(slots);
    const size_t recovered =
        slots == 0 ? 0
                   : core::scan_records(data, len, header.boot_count, records.data(), slots);

    print_csv_header();
    for (size_t i = 0; i < recovered; ++i) {
        print_csv_row(records[i]);
    }

    const size_t tail = len % core::kLogRecordSize;
    std::fprintf(stderr, "%zu de %zu slots recuperados (%zu descartados)\n", recovered, slots,
                 slots - recovered);
    if (tail != 0) {
        std::fprintf(stderr, "%zu bytes de cauda partida, descartados\n", tail);
    }
    return 0;
}
