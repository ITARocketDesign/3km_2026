// hal/boot_counter.h — contador de boot persistente (NVS).
//
// O número que separa este voo do anterior. Ele vai no cabeçalho do arquivo E em
// cada registro de 64 B, e é o único campo que impede contaminação entre voos: o
// arquivo pré-alocado cai nos clusters do voo passado, e como o recuperador
// varre por magic + CRC, um registro antigo valida perfeitamente e entraria no
// meio da trajetória atual.
//
// É também o índice do nome do arquivo, então o arquivo e o conteúdo dele
// concordam sobre a qual voo pertencem.
//
// Esta fatia usa NVS no mínimo necessário. Fase e referência barométrica
// persistidas entram na issue 06; o timestamp do último reset, na 11.
#pragma once

#include <stdint.h>

namespace hal {

class BootCounter {
  public:
    // Lê o contador, incrementa, persiste, e devolve o valor DESTE boot. Em caso
    // de falha da NVS devolve 0 — que é um número de voo como outro qualquer
    // para a varredura, e a alternativa seria não gravar nada.
    uint16_t next();
};

}  // namespace hal
