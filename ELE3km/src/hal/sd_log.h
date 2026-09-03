// hal/sd_log.h — o log durável do voo no microSD.
//
// O modo de falha que destrói um voo inteiro não é um registro perdido: é uma
// escrita de FAT ou de entrada de diretório interrompida. Um corte de energia
// ali não corrompe um registro, corrompe o ARQUIVO — os dados que a equipe voou
// para coletar (hazard H1a, arbitragem C4).
//
// Este adaptador remove esse modo de falha em vez de tentar agendar em volta
// dele: o arquivo é pré-alocado e contíguo no boot, e durante o voo só entram
// blocos de dados de 512 B. A tabela de alocação não é tocada. Os metadados são
// escritos exatamente DUAS vezes — na criação e no fechamento. Um corte de
// energia em pleno voo passa a poder corromper só o COMPRIMENTO registrado do
// arquivo, e todo bloco de dados já está no cartão.
//
// É por isso que a biblioteca é SdFat e não a `SD` do Arduino: a padrão não
// suporta nem pré-alocação nem arquivo contíguo, que são o mecanismo inteiro.
//
// ── Onde ficou a elasticidade (issue 05) ────────────────────────────────────
//
// Até a issue 04 esta classe tinha um duplo buffer que absorvia a janela de
// contenção do rádio. Na issue 05 quem absorve é o ring buffer SPSC em core/
// (RingBuffer): a task `flight` enfileira lá, a task `io` drena e monta os blocos
// aqui. O descarte do mais antigo mora no ring, não mais aqui — esta classe deixou
// de descartar. Ela junta registros já codificados num bloco de 512 B e o grava
// quando o bloco enche.
//
// A escrita é bloqueante e roda na task `io` (core 0). É exatamente por isso que
// a travada do cartão não engole amostra: ela fica presa neste core enquanto a
// task `flight` segue adquirindo no core 1 e enfileirando no ring.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/log_codec.h"

namespace hal {

// Tamanho pré-alocado do arquivo. A 25 Hz, 64 B por registro dão 1,6 kB/s: 8 MB
// cobrem ~85 minutos, que é o voo inteiro mais uma busca longa de recuperação.
constexpr uint32_t kPreallocatedBytes = 8u * 1024u * 1024u;

// Uma escrita de bloco que passe disso é uma travada anormal do cartão (H13 fala
// em 100 ms típicos, 500 ms de pior caso).
constexpr uint32_t kWriteStallMs = 500;

// Travadas seguidas desligam o cartão. Uma chamada síncrona de biblioteca não
// pode ser interrompida no meio — o "timeout duro" que existe de verdade é a
// recusa de chamá-la de novo. Um cartão doente não pode roubar do link de
// recuperação o tempo que o link precisa.
constexpr uint8_t kMaxConsecutiveStalls = 3;

class SdLog {
  public:
    // Monta o cartão, cria o arquivo deste boot já pré-alocado e contíguo, e
    // escreve o bloco de cabeçalho. Primeira das duas escritas de metadados.
    bool begin(uint16_t boot_count, float reference_pa, bool boot_loop = false,
               uint8_t recent_reset_count = 0);

    // Copia um registro JÁ CODIFICADO de 64 B para o bloco em montagem. O
    // chamador (task io) tira o registro do ring buffer, onde ele já foi
    // serializado pela task flight. Não escreve no cartão, não descarta — o ring
    // é quem descarta. Pré-condição: !block_ready(); a task io não enfileira mais
    // enquanto o bloco pronto não foi gravado, deixando o backlog no ring.
    void stage(const uint8_t* encoded_record);

    // true quando o bloco em montagem encheu (8 registros) e espera a janela de
    // escrita. Enquanto verdadeiro, a task io para de drenar o ring.
    bool block_ready() const { return fill_offset_ >= core::kLogBlockSize; }

    // Grava o bloco pronto, se houver um. Devolve true se gravou. Única função da
    // classe que toca o cartão durante o voo, e a única que pode demorar — por isso
    // roda na task io e o chamador ergue write_in_progress em volta dela.
    bool service(uint32_t now_ms);

    // Fecha o arquivo. Segunda e última escrita de metadados.
    void close();

    bool     is_open() const { return open_; }
    uint32_t stall_count() const { return stall_count_; }

  private:
    void disable();

    uint8_t  block_[core::kLogBlockSize];
    size_t   fill_offset_ = 0;     // quanto do bloco já foi montado

    bool     open_ = false;
    uint16_t boot_count_ = 0;
    uint32_t stall_count_ = 0;
    uint8_t  consecutive_stalls_ = 0;
};

}  // namespace hal
