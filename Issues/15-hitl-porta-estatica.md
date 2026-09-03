# 15 — HITL: especificação da porta estática

**Tipo:** HITL — depende da equipe de estrutura
**User stories:** 51

## What to build

Não é firmware. É a entrega de uma especificação mecânica para a equipe de estrutura, rastreada aqui porque **a qualidade da altitude — o payload prioritário do projeto — depende dela, e nenhum tratamento em software corrige pressão de estagnação vazando para o bay.**

**Especificação:**

- **3 ou 4 furos de ~2 mm, igualmente espaçados na circunferência.** A simetria é a parte que realmente conta: ela cancela erro de ângulo de ataque e o bombeamento causado pelo rolamento.
- A **pelo menos um diâmetro de corpo** de qualquer mudança de geometria
- A **um diâmetro à frente das aletas**
- **Sem rebarba**
- **Furo perpendicular à pele**

**O dimensionamento tem folga enorme, e vale explicar por quê** — para a conversa com a equipe não virar uma discussão sobre o diâmetro:

- O erro de atraso em regime permanente cresce com o **quadrado** da vazão necessária e cai com o **quadrado** da área. Para um bay típico com três furos de 2 mm, isso dá ordem de centímetros na velocidade máxima.
- O erro é **proporcional à velocidade**, e no apogeu a velocidade é zero. O número que a competição pontua sai praticamente livre de atraso.

**O risco real não é furo pequeno demais.** É furo **assimétrico** ou **bay selado**. Um bay selado torna a leitura barométrica inútil independentemente do firmware.

## Acceptance criteria

- [ ] Especificação entregue por escrito à equipe de estrutura, com número, diâmetro, espaçamento e posicionamento dos furos
- [ ] A justificativa da simetria comunicada, não só o número — é o requisito que mais provavelmente é relaxado por conveniência de montagem
- [ ] Confirmação da equipe de estrutura de que o bay do bay de aviônica **não é selado**
- [ ] Posição dos furos confirmada em relação à mudança de geometria mais próxima e às aletas
- [ ] Furos inspecionados quanto a rebarba e perpendicularidade após a usinagem
- [ ] Configuração final (número e diâmetro dos furos) registrada no repositório, para o log de voo poder ser interpretado depois

## Blocked by

- Nenhuma — pode começar imediatamente, e quanto antes melhor: é uma ação de fabricação com prazo próprio
