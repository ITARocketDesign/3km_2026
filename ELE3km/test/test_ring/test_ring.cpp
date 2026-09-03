// Suíte do ring buffer SPSC lock-free (issue 05). Roda no notebook:
// `pio test -e native`.
//
// O que esta suíte protege é a separação de concorrência que impede o cartão de
// engolir amostras de sensor. A task `flight` (produtor único) enfileira; a task
// `io` (consumidor único) drena e escreve. Uma travada de coleta de lixo do
// microSD de 100–250 ms não pode virar lacuna nas amostras — uma lacuna no boost
// é viés no passo de predição do filtro, não ruído.
//
// Os slots são bytes opacos de propósito: o ring buffer não conhece o formato do
// log. O padrão auto-verificável que os testes gravam em cada slot é o que separa
// um registro íntegro de um lido pela metade enquanto o produtor o sobrescrevia.
#include <unity.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "core/ring_buffer.h"

using namespace core;

namespace {

// Preenche um slot de 64 B com um padrão auto-verificável derivado de `seq`: os
// quatro primeiros bytes são a sequência (LE), e o resto é (seq+i) mod 256. Se o
// produtor sobrescrever o slot no meio de uma leitura do consumidor, os bytes do
// padrão deixam de concordar com a sequência gravada, e slot_sequence_if_intact()
// devolve UINT32_MAX.
void fill_slot(uint8_t* slot, uint32_t seq) {
    slot[0] = static_cast<uint8_t>(seq);
    slot[1] = static_cast<uint8_t>(seq >> 8);
    slot[2] = static_cast<uint8_t>(seq >> 16);
    slot[3] = static_cast<uint8_t>(seq >> 24);
    for (size_t i = 4; i < RingBuffer::kSlotSize; ++i) {
        slot[i] = static_cast<uint8_t>(seq + i);
    }
}

// Devolve a sequência gravada se o slot é internamente consistente, ou UINT32_MAX
// se está rasgado — parte de um registro, parte de outro.
uint32_t slot_sequence_if_intact(const uint8_t* slot) {
    const uint32_t seq = static_cast<uint32_t>(slot[0]) |
                         (static_cast<uint32_t>(slot[1]) << 8) |
                         (static_cast<uint32_t>(slot[2]) << 16) |
                         (static_cast<uint32_t>(slot[3]) << 24);
    for (size_t i = 4; i < RingBuffer::kSlotSize; ++i) {
        if (slot[i] != static_cast<uint8_t>(seq + i)) {
            return UINT32_MAX;
        }
    }
    return seq;
}

// Um destino descartável para os pops cujo conteúdo não interessa, só o retorno.
uint8_t* out_scratch() {
    static uint8_t scratch[RingBuffer::kSlotSize];
    return scratch;
}

}  // namespace

// Tracer: um slot entra, o mesmo slot sai, byte a byte. Prova o caminho ponta a
// ponta antes de qualquer coisa sobre ordem, capacidade ou concorrência.
void test_a_slot_round_trips(void) {
    RingBuffer ring;

    uint8_t in[RingBuffer::kSlotSize];
    fill_slot(in, 12345);
    ring.push(in);

    uint8_t out[RingBuffer::kSlotSize] = {0};
    TEST_ASSERT_TRUE(ring.pop(out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(in, out, RingBuffer::kSlotSize);
}

// A ordem é FIFO: a task `io` grava no cartão na ordem em que a task `flight`
// adquiriu, e o número de sequência do registro tem que sair monotônico do log.
void test_slots_come_out_in_order(void) {
    RingBuffer ring;

    for (uint32_t i = 0; i < 8; ++i) {
        uint8_t in[RingBuffer::kSlotSize];
        fill_slot(in, i);
        ring.push(in);
    }

    for (uint32_t i = 0; i < 8; ++i) {
        uint8_t expected[RingBuffer::kSlotSize];
        fill_slot(expected, i);

        uint8_t out[RingBuffer::kSlotSize] = {0};
        TEST_ASSERT_TRUE(ring.pop(out));
        TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, RingBuffer::kSlotSize);
    }
}

// Um pop no buffer vazio devolve false e não inventa um slot. A task `io` chama
// pop em rajada até esvaziar; o false é o sinal de parada da rajada.
void test_pop_on_empty_returns_false(void) {
    RingBuffer ring;

    uint8_t out[RingBuffer::kSlotSize] = {0};
    TEST_ASSERT_FALSE(ring.pop(out));

    uint8_t in[RingBuffer::kSlotSize];
    fill_slot(in, 1);
    ring.push(in);
    TEST_ASSERT_TRUE(ring.pop(out));
    TEST_ASSERT_FALSE(ring.pop(out));  // drenou, voltou a vazio
}

// Enche até a capacidade útil sem consumir nada e drena depois: enquanto o buffer
// não enche, nenhum registro se perde e a ordem se mantém. É a rajada da task `io`
// com pausa de até 500 ms — o produtor a 100 Hz deposita 50 slots numa pausa
// dessas, muito abaixo dos 511, então nada é descartado.
void test_a_full_buffer_drains_without_loss(void) {
    RingBuffer ring;

    for (uint32_t i = 0; i < RingBuffer::kUsableSlots; ++i) {
        uint8_t in[RingBuffer::kSlotSize];
        fill_slot(in, i);
        ring.push(in);
    }

    for (uint32_t i = 0; i < RingBuffer::kUsableSlots; ++i) {
        uint8_t expected[RingBuffer::kSlotSize];
        fill_slot(expected, i);

        uint8_t out[RingBuffer::kSlotSize] = {0};
        TEST_ASSERT_TRUE(ring.pop(out));
        TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, RingBuffer::kSlotSize);
    }
    TEST_ASSERT_FALSE(ring.pop(out_scratch()));
    TEST_ASSERT_EQUAL_UINT32(0, ring.dropped());
}

// A política de descarte, o coração da issue: com o buffer cheio o produtor NUNCA
// bloqueia — ele sobrescreve o slot não lido mais antigo. O consumidor então vê
// os kUsableSlots registros MAIS NOVOS, na ordem, e a lacuna é observável:
// dropped() conta os descartados, e o primeiro slot que sai já é o de sequência
// `first_surviving`, não o 0. Atrasar uma leitura de sensor para não descartar
// seria o erro oposto.
void test_overflow_drops_the_oldest_and_the_gap_is_visible(void) {
    RingBuffer ring;

    const uint32_t total = RingBuffer::kCapacity + 100;
    for (uint32_t i = 0; i < total; ++i) {
        uint8_t in[RingBuffer::kSlotSize];
        fill_slot(in, i);
        ring.push(in);  // nunca bloqueia; void por isso
    }

    // Sobra a janela dos mais novos que cabe na capacidade útil.
    const uint32_t first_surviving = total - RingBuffer::kUsableSlots;

    // O descarte é observado pelo consumidor, não contabilizado no push: é isso
    // que mantém dropped_ com um único escritor e sem corrida. O primeiro slot a
    // sair já é o de sequência first_surviving — a descontinuidade que revela a
    // lacuna.
    for (uint32_t i = first_surviving; i < total; ++i) {
        uint8_t expected[RingBuffer::kSlotSize];
        fill_slot(expected, i);

        uint8_t out[RingBuffer::kSlotSize] = {0};
        TEST_ASSERT_TRUE(ring.pop(out));
        TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, RingBuffer::kSlotSize);
    }
    TEST_ASSERT_FALSE(ring.pop(out_scratch()));
    TEST_ASSERT_EQUAL_UINT32(first_surviving, ring.dropped());
}

// O teste que justifica a existência do módulo: produtor e consumidor em threads
// de host reais, o consumidor drenando em rajadas com pausas — a travada do
// cartão. Três invariantes têm que valer sob concorrência de verdade:
//
//   1. Nenhum registro corrompido — todo slot que sai é internamente consistente.
//      Um slot rasgado seria a prova de que o produtor sobrescreveu enquanto o
//      consumidor lia, e o registro entraria no log como medição.
//   2. Sequência estritamente crescente — o consumidor nunca vê um registro duas
//      vezes nem fora de ordem; sob descarte ela SALTA, mas nunca retrocede.
//   3. Contabilidade fechada — lidos + descartados == produzidos. Nenhum registro
//      some sem ser lido ou contado como descartado.
void test_concurrent_producer_and_consumer_lose_nothing_silently(void) {
    RingBuffer ring;

    constexpr uint32_t kPush = 300000;
    std::atomic<bool> producing{true};

    // Produtor: enfileira sem pausa, gerando pressão de estouro de propósito —
    // é a task `flight` a 100 Hz que não pode esperar o cartão.
    std::thread producer([&] {
        uint8_t buf[RingBuffer::kSlotSize];
        for (uint32_t seq = 0; seq < kPush; ++seq) {
            fill_slot(buf, seq);
            ring.push(buf);
        }
        producing.store(false, std::memory_order_release);
    });

    // Consumidor: rajadas de drenagem separadas por pausas curtas — a travada de
    // coleta de lixo do microSD. As pausas deixam o backlog crescer e forçam o
    // descarte.
    uint32_t popped = 0;
    bool     corrupted = false;
    bool     went_backwards = false;
    bool     have_last = false;
    uint32_t last_seq = 0;

    std::thread consumer([&] {
        uint8_t out[RingBuffer::kSlotSize];
        for (;;) {
            // Uma rajada: drena tudo o que estiver disponível agora.
            bool drained_any = false;
            while (ring.pop(out)) {
                drained_any = true;
                const uint32_t seq = slot_sequence_if_intact(out);
                if (seq == UINT32_MAX) {
                    corrupted = true;
                } else {
                    if (have_last && seq <= last_seq) {
                        went_backwards = true;
                    }
                    last_seq  = seq;
                    have_last = true;
                }
                ++popped;
            }
            if (!producing.load(std::memory_order_acquire) && !drained_any) {
                break;  // produtor terminou e o buffer secou
            }
            // A pausa da travada do cartão.
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    producer.join();
    consumer.join();

    TEST_ASSERT_FALSE_MESSAGE(corrupted, "um slot saiu rasgado");
    TEST_ASSERT_FALSE_MESSAGE(went_backwards, "a sequencia retrocedeu ou repetiu");
    // Lidos + descartados fecham com o total produzido: nada some em silencio.
    TEST_ASSERT_EQUAL_UINT32(kPush, popped + ring.dropped());
    // Com pausas forcando backlog, o descarte tem que ter acontecido de verdade —
    // senao o teste nao exercitou o caminho de estouro concorrente.
    TEST_ASSERT_TRUE_MESSAGE(ring.dropped() > 0, "o teste nao chegou a estourar o buffer");
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_a_slot_round_trips);
    RUN_TEST(test_slots_come_out_in_order);
    RUN_TEST(test_pop_on_empty_returns_false);
    RUN_TEST(test_a_full_buffer_drains_without_loss);
    RUN_TEST(test_overflow_drops_the_oldest_and_the_gap_is_visible);
    RUN_TEST(test_concurrent_producer_and_consumer_lose_nothing_silently);
    return UNITY_END();
}
