// core/altitude.h — altitude barométrica.
#pragma once

namespace core {

// Altitude, em metros, acima do nível onde a pressão vale reference_pa.
// Fórmula da atmosfera padrão internacional para a troposfera.
//
// Nesta fatia é o caminho direto de pressão para altitude. A partir da issue 07
// ela vira a medição de altitude que alimenta o estimador, não a saída.
//
// PRD §10 / hazard H12: a leitura é ruim em regime transônico. Quem trata isso é
// o estimador, variando o peso do barômetro com a velocidade — não esta função.
float altitude_from_pressure(float pressure_pa, float reference_pa);

}  // namespace core
