// core/log_codec.h — formato binário do log de voo no cartão.
//
// ESTE É O FORMATO DA ÚNICA CÓPIA DOS DADOS BRUTOS. O pacote de rádio é resumo;
// aqui está o registro completo, e é dele que sai o harness de replay da issue
// 14 — o log do primeiro voo real vira o teste de regressão de todas as versões
// futuras do estimador, e nenhum dado sintético iguala isso.
//
// Requisito escrito, para ninguém "otimizar" depois: o registro tem que ser
// suficiente para RECONSTRUIR a amostra de sensores que o produziu. Nenhum campo
// bruto sai daqui sob o argumento de que já está na saída fundida.
//
// ── Por que 64 B ────────────────────────────────────────────────────────────
//
// 64 B dão exatamente 8 registros por bloco de 512 B, que é a unidade de
// transação do cartão. Qualquer outro tamanho faz um registro cruzar a fronteira
// do bloco, e um corte de energia no meio de um bloco passaria a poder partir um
// registro em dois pedaços válidos.
//
// ── Layout usado do cabecalho — primeiros 64 B do bloco 0 ───────────────────
//
//   Off  Tipo    Campo
//    0   u8      magic
//    1   u8      versao do formato
//    2   u16     contador de boot
//    4   f32     referencia barometrica, Pa
//    8   u8      revisao do mapa de pinos
//    9   u8      boot loop (0/1)                                      12
//   10   u8      quantidade de resets recentes no disparo             12
//   11–61        reservado (zero)
//   62   u16     CRC-16 sobre os bytes 0–61
//
// Os bytes 9 e 10 eram reservados na v1. Leitores antigos os ignoram; leitores
// novos interpretam zero como ausencia do evento. O bloco e a versao continuam
// os mesmos, e o restante dos 512 B permanece reservado.
//
// ── Layout do registro — 64 B, little-endian ────────────────────────────────
//
//   Off  Tipo    Campo                                          Vem da issue
//    0   u8      magic
//    1   u8      flags: fase(3) + fonte(2) + baro + gps_valid + gps_rx
//    2   u16     contador de boot
//    4   u32     sequência do registro
//    8   u32     t_ms — instante da AQUISIÇÃO, nunca da escrita
//   12   f32     pressão bruta, Pa                                   02
//   16   i16     temperatura bruta × 100, °C                         02
//   18   i16     velocidade vertical fundida, dm/s                   07
//   20   f32     altitude derivada, m                                01
//   24   i32     latitude BRUTA do GPS × 1e7                         02
//   28   i32     longitude BRUTA do GPS × 1e7                        02
//   32   i32     latitude FUNDIDA × 1e7                              08
//   36   i32     longitude FUNDIDA × 1e7                             08
//   40   i16×3   aceleração bruta, mg                                07
//   46   i16×3   giro bruto, décimos de grau/s                       07
//   52   u8      GPS: satélites(4) + HDOP×2(4)                       02
//   53   u8      GPS: qualidade do fix                               02
//   54   u8      saúde: bitmap health_bit                            04
//   55   u8      contador de resets do estimador                     07
//   56   u16     overflows do buffer da UART do GPS                  02
//   58   u16     stack watermark da task de voo                      11
//   60   u16     stack watermark da task de I/O                      11
//   62   u16     CRC-16 sobre os bytes 0–61
//
// As posições BRUTA e FUNDIDA são campos diferentes de propósito: a fundida pode
// vir da ponte inercial ou da última válida com idade (issue 08), e sem a bruta
// ao lado não há como refazer essa decisão depois do voo.
//
// Os campos cuja issue produtora ainda não chegou são gravados como zero. Eles
// EXISTEM no layout desde já porque o formato vai para o cartão e para a
// ferramenta de recuperação: acrescentar campo depois obrigaria a versionar o
// formato e a carregar um decodificador por versão no harness de replay.
//
// ── Por que o CRC fica, e o contador de boot em cada registro ───────────────
//
// O CRC de aplicação foi removido do pacote de RÁDIO porque o LoRa já tem CRC de
// hardware. Aqui não existe camada de enlace: sem CRC, um bloco meio escrito na
// hora do corte de energia entraria no dataset como dado bom.
//
// O contador de boot vai em CADA registro, não só no cabeçalho, e é o campo que
// impede contaminação entre voos: o arquivo pré-alocado cai nos clusters do voo
// anterior, e como o recuperador varre por magic + CRC, um registro do voo
// passado valida PERFEITAMENTE e entraria no meio do voo atual.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/types.h"

namespace core {

constexpr size_t  kLogRecordSize = 64;
constexpr size_t  kLogBlockSize = 512;   // unidade de transação do cartão
constexpr size_t  kLogHeaderSize = kLogBlockSize;  // o cabeçalho ocupa o bloco 0

constexpr uint8_t kLogRecordMagic = 0xA5;
constexpr uint8_t kLogFormatVersion = 1;

// Cabeçalho do arquivo, no offset 0. Ocupa um bloco inteiro para que o primeiro
// registro caia em 512 e todo registro seguinte fique alinhado em 64 B.
struct LogHeader {
    uint16_t boot_count = 0;
    float    reference_pa = 0.0f;     // pressão de referência de solo (hazard H4)
    uint8_t  pin_map_revision = 0;    // contra qual fiação este voo rodou
    // Bytes 9 e 10 do bloco de cabecalho eram reservados e zero. Leitores v1
    // antigos continuam compativeis porque ignoram ambos; os registros de 64 B
    // permanecem congelados.
    bool     boot_loop = false;
    uint8_t  recent_reset_count = 0;
    uint8_t  format_version = kLogFormatVersion;
};

// Serializa o cabeçalho num bloco de kLogHeaderSize bytes. O resto do bloco é
// zerado. Devolve os bytes escritos, ou 0 se não couber.
size_t encode_header(const LogHeader& header, uint8_t* out, size_t capacity);

// Desserializa o cabeçalho. Rejeita magic, versão de formato ou CRC que não
// batam — um cabeçalho ilegível é motivo para NÃO confiar no contador de boot,
// e sem ele não há como separar este voo do anterior.
bool decode_header(const uint8_t* in, size_t len, LogHeader& out);

// Serializa um registro em exatamente kLogRecordSize bytes.
size_t encode_record(const LogRecord& record, uint16_t boot_count, uint8_t* out,
                     size_t capacity);

// Desserializa um registro, validando magic e CRC. `boot_count` sai preenchido
// com o do registro — quem decide se ele pertence a este voo é o chamador, ou
// scan_records() abaixo.
bool decode_record(const uint8_t* in, size_t len, LogRecord& out, uint16_t& boot_count);

// Varre um fluxo bruto de registros e entrega os que passam por magic, CRC e
// contador de boot. Devolve quantos foram escritos em `out`.
//
// A varredura anda em passos de kLogRecordSize porque é assim que o arquivo é
// escrito — o cabeçalho ocupa o bloco 0 e todo registro nasce alinhado. Varrer
// byte a byte procurando o magic seria pior: um magic de 1 byte casa a cada 256
// bytes de lixo em média, e cada casamento desses gastaria uma verificação de
// CRC para ser descartado.
//
// `data` deve apontar para o PRIMEIRO REGISTRO, não para o começo do arquivo.
size_t scan_records(const uint8_t* data, size_t len, uint16_t boot_count, LogRecord* out,
                    size_t capacity);

}  // namespace core
