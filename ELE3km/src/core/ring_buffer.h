// core/ring_buffer.h — ring buffer SPSC lock-free, descarte do mais antigo.
//
// A separação de concorrência que impede o cartão de engolir amostras de sensor
// (issue 05). Um microSD para de responder por 100–250 ms durante coleta de lixo
// interna — comportamento normal, sem erro. Num superloop isso engole as amostras
// da IMU, e o passo de predição do filtro integra aceleração com um Δt errado,
// sistematicamente durante o boost. Uma lacuna nas amostras não é ruído, é viés.
//
// Com tasks separadas a travada fica no core 0 (task `io`, consumidor) e o core 1
// (task `flight`, produtor) não percebe: ele enfileira e segue a 100 Hz.
//
// ── Duas instanciações, um só primitivo ─────────────────────────────────────
//
// O tipo é um template em capacidade e tamanho de slot para servir os DOIS
// caminhos que a separação de barramentos cria, com a mesma prova de correção:
//
//   • `RingBuffer` (512 × 64 B = 32 KB) — o log do voo, flight → io. Cobre a pior
//     janela de rádio (SF12 = 1712 ms) somada à pior travada de cartão (500 ms)
//     com folga de ~5 s.
//   • `TxRing` (pequeno) — os comandos de transmissão, flight → io. Só a task io
//     toca o SPI; a task flight decide o que transmitir e enfileira aqui. Um
//     segundo anel de 32 KB estouraria o orçamento de SRAM, e a telemetria cabe
//     em poucos slots — o escalonador solta ~1 transmissão por segundo por rádio.
//
// O descarte do mais antigo é seguro para os dois: no log, perde-se a amostra
// mais velha; na telemetria, se a task io travasse a ponto de encher, mandar a
// posição MAIS NOVA é melhor que recusá-la.
//
// ── Por que os índices são atomic, e por que o descarte é observado na leitura ─
//
// Os índices de escrita e leitura são std::atomic com acquire/release. Sem mutex,
// sem semáforo, sem primitiva FreeRTOS — qualquer uma introduziria inversão de
// prioridade ou latência no loop de aquisição a 100 Hz. std::atomic é C++11
// padrão e funciona no Xtensa e no x86/ARM, o que permite testar a sincronização
// no host.
//
// Cada índice tem UM ÚNICO escritor: o produtor escreve write_index_, o consumidor
// escreve read_index_. Essa é a diferença deliberada em relação ao texto da issue,
// que descreve "o produtor avança o índice de leitura no cheio". Fazer os dois
// lados escreverem o índice de leitura seria, ele mesmo, a corrida de dados que a
// suíte concorrente proíbe. O comportamento observável é idêntico — o mais antigo
// é descartado, o consumidor vê a lacuna, o produtor nunca bloqueia —, mas o
// descarte é observado na leitura: quando o consumidor percebe que o produtor deu
// mais de kCapacity voltas à frente, ele salta para o mais antigo ainda válido e
// conta o que foi sobrescrito. Um único escritor por índice é o que torna isso
// livre de corrida.
//
// ── Por que os slots são palavras atômicas ──────────────────────────────────
//
// No descarte, o produtor sobrescreve exatamente o slot que o consumidor pode
// estar copiando. O seqlock (reler write_index_ depois da cópia) DETECTA essa
// leitura rasgada e a descarta — o valor lógico nunca contamina o log. Mas uma
// cópia byte a byte de memória compartilhada com um escritor concorrente é, ao pé
// do modelo de memória de C++, uma corrida de dados (UB), e o critério da issue
// proíbe corrida — não só corrupção observável. Por isso cada slot é um vetor de
// palavras std::atomic de 64 bits: o produtor as grava relaxed, o consumidor as lê
// relaxed, e a sincronização real fica nos índices (release/acquire). Toda leitura
// é definida; o seqlock só descarta a MISTURA de palavras nova e velha. É o que
// deixa o ThreadSanitizer limpo além de o log correto.
#pragma once

#include <atomic>
#include <cstring>
#include <stddef.h>
#include <stdint.h>

namespace core {

// SPSC: um único produtor (task `flight`), um único consumidor (task `io`). Isso
// é estrutural, não convencional — decorre da separação de barramentos. Um segundo
// consumidor não se adapta a este buffer, ele exige reprojetá-lo.
//
// Header-only por ser template: a instanciação com o mesmo código prova a
// correção das duas (o log de 512×64 é a que a suíte exercita a fundo).
template <uint32_t Capacity, size_t SlotSizeBytes>
class RingBufferT {
  public:
    static constexpr uint32_t kCapacity = Capacity;
    static constexpr size_t   kSlotSize = SlotSizeBytes;

    // O slot é feito de palavras de 64 bits, então o tamanho tem que ser múltiplo
    // de 8. Vale para as duas instâncias: 64 B (8 palavras) e 24 B (3 palavras).
    static_assert(SlotSizeBytes % sizeof(uint64_t) == 0,
                  "o slot precisa ser múltiplo de 8 B para as palavras atômicas");
    static constexpr size_t kWords = SlotSizeBytes / sizeof(uint64_t);

    // Um slot fica de guarda. O slot em voo do produtor é sempre write_index_ %
    // kCapacity; se o consumidor pudesse ler até encher os kCapacity, o mais
    // antigo dele coincidiria com esse slot em voo e sairia rasgado. Reservar um
    // slot mantém o mais antigo legível sempre atrás do slot em voo.
    static constexpr uint32_t kUsableSlots = kCapacity - 1;

    // Produtor. Copia kSlotSize bytes para dentro. NUNCA bloqueia e nunca falha:
    // quando cheio, o slot não lido mais antigo é sobrescrito (descartado). Chamar
    // só da task `flight`.
    void push(const uint8_t* slot) {
        uint64_t words[kWords];
        std::memcpy(words, slot, kSlotSize);

        // O produtor é o único que lê e escreve write_index_, então relaxed basta
        // para a própria carga. As palavras vão relaxed; a sincronização com o
        // consumidor é o release do write_index_ abaixo, que torna visíveis todas
        // as escritas sequenciadas antes dele.
        const uint32_t w = write_index_.load(std::memory_order_relaxed);
        std::atomic<uint64_t>* s = slots_[w % kCapacity];
        for (size_t k = 0; k < kWords; ++k) {
            s[k].store(words[k], std::memory_order_relaxed);
        }
        write_index_.store(w + 1, std::memory_order_release);
    }

    // Consumidor. Copia o slot válido mais antigo para fora. Devolve false quando
    // vazio. Chamar só da task `io`.
    bool pop(uint8_t* out) {
        // O consumidor é o único que escreve read_index_; relaxed para a carga.
        uint32_t r = read_index_.load(std::memory_order_relaxed);
        for (;;) {
            const uint32_t w = write_index_.load(std::memory_order_acquire);
            if (r == w) {
                return false;  // vazio
            }
            // Backlog acima de kUsableSlots significa que o produtor passou por
            // cima dos mais antigos ainda não lidos (ou está sobrescrevendo o mais
            // antigo agora). Salta para o mais antigo que ainda é seguro ler e
            // conta o que se perdeu. Escrito com backlog para não haver underflow
            // com poucos registros.
            const uint32_t backlog = w - r;
            if (backlog > kUsableSlots) {
                const uint32_t drop = backlog - kUsableSlots;
                dropped_.fetch_add(drop, std::memory_order_relaxed);
                r += drop;
            }
            uint64_t words[kWords];
            std::atomic<uint64_t>* s = slots_[r % kCapacity];
            for (size_t k = 0; k < kWords; ++k) {
                words[k] = s[k].load(std::memory_order_relaxed);
            }
            // Seqlock: o produtor sobrescreve o slot r quando seu índice alcança
            // r + kCapacity. Se, depois da cópia, ele já chegou lá, a cópia
            // misturou palavras nova e velha. Descarta e recomeça com r
            // ressincronizado no topo do laço. É o que impede um registro rasgado
            // de entrar no log como medição.
            if (write_index_.load(std::memory_order_acquire) - r >= kCapacity) {
                continue;
            }
            std::memcpy(out, words, kSlotSize);
            read_index_.store(r + 1, std::memory_order_release);
            return true;
        }
    }

    // Total de slots que o produtor sobrescreveu antes de o consumidor os ler.
    // Derivável da diferença de sequência; mantido aqui para a saúde da issue 10.
    uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

  private:
    std::atomic<uint64_t> slots_[kCapacity][kWords];
    std::atomic<uint32_t> write_index_{0};  // escrito só pelo produtor
    std::atomic<uint32_t> read_index_{0};   // escrito só pelo consumidor
    std::atomic<uint32_t> dropped_{0};      // escrito só pelo consumidor
};

// O log do voo: 512 slots de 64 B, exatamente um registro de log por slot.
using RingBuffer = RingBufferT<512, 64>;

// Os comandos de transmissão, flight → io. Cada slot é [len | payload], e a maior
// carga útil é o pacote completo de 20 B, então 24 B sobram. 16 slots são folga
// larga: o escalonador solta ~1 transmissão por segundo e a task io as drena a
// cada volta. 16 × 24 = 384 B, ante os 32 KB que um segundo anel de log custaria.
using TxRing = RingBufferT<16, 24>;

}  // namespace core
