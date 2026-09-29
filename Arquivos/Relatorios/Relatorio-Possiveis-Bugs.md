# Relatório: Possíveis Bugs e Regressões — Alterações Locais não Commitadas

**Data:** 2026-09-29
**Base:** `main` @ `a4e0d85` (sincronizado com `origin/main`)
**Escopo:** 14 arquivos de código modificados + 2 documentos novos, ainda não commitados
**Status:** Análise estática (sem compilação — ver seção 8)
**Correções:** ✅ todos os 17 achados tratados (ver seção 2.1)

---

## 1. Objetivo

Levantar possíveis bugs, regressões e riscos de consistência introduzidos pelas
alterações locais, que implementam:

1. Novo algoritmo de Chroma Key em `PreviewWidget.cpp` (distância perceptual
   BT.601, borda suave com smoothstep, supressão de spill).
2. Novos campos `chromaKeySoftness` e `chromaKeySpillSuppress` no modelo
   `Clip`, serializados em `Project.cpp`.
3. Novos controles na UI (painel de propriedades e painel Express).
4. Otimizações de arraste na timeline (remoção de `updateScrollRanges()` do
   loop de `mouseMoveEvent`, nova função `invalidateSceneContent()`,
   guia de alinhamento `m_snapLineX`).

---

## 2. Resumo Executivo

| Sev. | ID | Achado | Arquivo | Status |
|---|---|---|---|---|
| **Crítica** | B-01 | Conversão de escala duplicada corrompe Similaridade/Suavidade/Spill | `ClipPropertiesWidget.cpp` | ✅ corrigido |
| **Crítica** | B-02 | Preview do clipe do topo ignora Suavidade e Spill | `PreviewWidget.cpp/.h` | ✅ corrigido |
| **Alta** | B-03 | Otimização `invalidateSceneContent()` é anulada na mesma chamada | `TimelineWidget.cpp` | ✅ corrigido |
| **Alta** | B-04 | Faixa de scroll não cresce ao estender o último clipe | `TimelineDrag.cpp` | ✅ corrigido |
| **Média** | B-05 | Presets perdem Suavidade e Spill | `clipattrs.h` | ✅ corrigido |
| **Média** | B-06 | Exportador ignora Suavidade/Spill e fixa `blend=0.1` | `ProjectExporter.cpp` | ✅ parcial (blend) |
| **Média** | B-07 | Guia de alinhamento aparece a cada mousemove | `TimelineDrag.cpp` | ✅ corrigido |
| **Média** | B-08 | Supressão de spill funciona apenas para chave verde | `PreviewWidget.cpp` | ✅ corrigido |
| **Média** | B-09 | Peso e critério da supressão de spill invertidos/limitados | `PreviewWidget.cpp` | ✅ corrigido |
| **Média** | B-10 | Resumo de expressão `modified()` duplicado nos 3 spinboxes | `ClipPropertiesWidget.cpp` | ✅ corrigido |
| **Baixa** | B-11 | Guarda `m_dragMode == None` inalcançável (código morto) | `TimelineDrag.cpp` | ✅ corrigido |
| **Baixa** | B-12 | Guia de alinhamento não é limpa ao sair das faixas | `TimelineDrag.cpp` | ✅ corrigido |
| **Baixa** | B-13 | Regressão de qualidade: faixa suave default muito estreita | `PreviewWidget.cpp` | ✅ corrigido |
| **Baixa** | B-14 | Constantes de loop recalculadas por pixel | `PreviewWidget.cpp` | ✅ corrigido |
| **Baixa** | B-15 | Testes de serialização sem os novos campos | `tests/tst_serialization.cpp` | ✅ corrigido |
| **Info** | B-16 | Branches de Ripple inalcançáveis (preexistente) | `TimelineDrag.cpp` | ✅ corrigido |
| **Média** | B-17 | Não havia indicador de trim; só a guia de alinhamento | `TimelinePaint.cpp` | ✅ corrigido |

> **B-01 é o achado mais grave:** o usuário não conseguia ajustar os três novos
> controles pelo painel de propriedades — qualquer valor digitado saturava a
> chave em 1.0 (100%).

## 2.1 Correções aplicadas

| ID | O que foi feito |
|---|---|
| B-01 | `dim` corrigido para `100.0` e lambdas passaram a operar em valor cru, seguindo o contrato de `addSpinRow()` (`read()*dim` na exibição, `write(v/dim)` na gravação). |
| B-02 | Adicionados `m_clipChromaKeySoftness` e `m_clipChromaKeySpillSuppress` em `PreviewWidget.h`, copiados em `setClipFrom` e atribuídos ao `Clip` temporário de `applyBasicEffects()`. |
| B-03 | `FadeIn`/`ClipOpacity` passaram a despachar para `invalidateSceneContent()` em vez do ramo `else` que chamava `invalidateScene()` e refazia `rebuildClipIndex()`. |
| B-04 | Nova função `ensureScrollRangeReaches(double tEnd)`: só alarga a barra horizontal, nunca encolhe, não recalcula o range vertical. Chamada no arraste; `mouseReleaseEvent()` passou a chamar `updateScrollRanges()` para ressincronizar ao fim do gesto. |
| B-05 | `clipattrs.h` grava e lê `chromaKeySoftness` e `chromaKeySpillSuppress`, com fallback ao default do struct para presets antigos. |
| B-06 | `blend` do `chromakey` no export passa a derivar de `chromaKeySoftness` (`clamp(softness, 0.01, 0.9)`), reproduzindo o 0.1 no default. A supressão de spill continua sem equivalente no ffmpeg — limitação documentada no código. |
| B-07 | A guia passa a comparar o resultado do `snapToEdges()` com a **grid quantizada**, e não com a posição bruta — assim só o snap de borda acende. Em `TrimLeft`/`TrimRight` a comparação é pré-`clamp`. A fórmula X duplicada foi trocada por `timeToX()`. |
| B-08 | O canal da chave (`keyCh`) é derivado de `kr/kg/kb` e o despill passa a operar nele, cobrindo verde, azul e vermelho. Ties (ciano, magenta, amarelo, cinza) ficam sem despill por não haver canal dominante definido. |
| B-09 | Peso trocado para uma curva em sino `4a(1-a)` (máxima em alpha intermediário, onde a franja é visível) e o gate `pixLum > keyLum` foi trocado por "o canal da chave domina o pixel", que não descarta pixels escuros contaminados. Arredondamento unificado em `lround`. |
| B-10 | `beginEdit()`/`emitEdited()` removidos dos três setters — `addSpinRow()` já os emite. |
| B-11 | Bloco de hit-test de volume removido por inteiro (era inalcançável sob `if (m_dragMode == MoveClip)`), junto com as variáveis `vrow`/`cvrow`. Declarações realinhadas. |
| B-12 | `m_snapLineX` é zerado no início de `mouseMoveEvent`, então a guia não sobrevive a um mousemove fora das faixas. |
| B-17 | **Indicadores de trim no estilo Premiere.** A guia de alinhamento era a única linha da timeline; não havia nenhum indicador de corte. Novo `m_trimEdgeX`/`m_trimEdgeRow`/`m_trimEdgeAudio`/`m_trimEdgeRipple`, preenchidos por `markTrimEdge(tEdge, ripple)` nos cinco ramos de arraste de borda (`TrimLeft` e `TrimRight` regular e com ripple, e `RollingEdit`), zerados em `mouseMoveEvent` e em `mouseReleaseEvent`. O desenho em `renderOverlays()` marca a borda com 3px na cor que o Premiere associa à operação — **amarelo = ripple, vermelho = trim regular e roll** — mais duas barras horizontais nas pontas, cobrindo só a altura da faixa. A guia de alinhamento saiu do `else if` do razor e ganhou halo de 3px sob o traço de 1px. `snapToEdges()` passou a considerar `m_project->markers`, que o Premiere usa como alvo de snap. Slip e Slide ficaram de fora de propósito: o Premiere tem indicadores próprios para eles, que não caberiam no mesmo estado de borda única. |
| B-13 | Mapeamento da tolerância absoluto: `fullRange = coreRange + soft * 255.0`. A banda com os defaults passou de 11.5 para 25.5 (4,5% → 10% da escala), e `soft` deixa de ser estrangulado pela similaridade. Com `soft = 0`, `fullRange == coreRange` e o ramo do smoothstep fica inalcançável — sem divisão por zero. |
| B-14 | `coreSq`/`fullSq` movidos para fora dos laços. |
| B-15 | `tst_serialization.cpp` cobre os dois campos novos no round-trip e ganhou dois casos de preset: round-trip completo e preset legado sem as chaves (que não pode zerar os defaults). |
| B-16 | Os dois ramos de Ripple foram movidos **antes** dos ramos genéricos de `TrimLeft`/`TrimRight`. Como `rippleTrimLeft/Right` medem o delta contra o estado corrente do clipe, o arraste ao vivo congelaria no primeiro mousemove — por isso o novo `m_rippleOrig`, snapshot da faixa inteira tirado no `mousePressEvent` e restaurado a cada mousemove. |

---

## 3. Bugs Críticos

### B-01 — Conversão de escala duplicada nos spinboxes do Chroma Key

**Arquivo:** `src/ui/ClipPropertiesWidget.cpp:601-622`
**Severidade:** Crítica (corrupção de dados)

**Contrato de `addSpinRow()`** (`ClipPropertiesWidget.cpp:209-277`):

| Ponto | Código | Semântica |
|---|---|---|
| Exibição | `refreshValues()` → `r.spin->setValue(r.read() * r.dim)` (linha 779) | `read()` retorna valor **cru**; `dim` é o fator de exibição |
| Gravação | `connect(valueChanged)` → `write(v / dim)` (linha 268) | `write()` recebe o valor **cru** |

O padrão já existente segue esse contrato. Exemplo, linha 539-541:

```cpp
addSpinRow(tr("Brilho:"), -100, 100, 1, 0, 60, 100.0,
    [clip]  () { return clip->brightness; },        // cru
    [clip]  (double v) { clip->brightness = v; });  // cru
```

**Os três spinboxes novos violam o contrato** (`ClipPropertiesWidget.cpp:601-622`):
passam `dim = 0.01` **e** convertem dentro dos lambdas:

```cpp
addSpinRow(tr("Similaridade:"), 0, 100, 1, 0, 50, 0.01,
    [clip]() { return clip->chromaKeySimilarity * 100.0; },   // já multiplica
    [this, clip](double v) {
        beginEdit();
        clip->chromaKeySimilarity = v / 100.0;                 // já divide
        emitEdited();
    });
```

**Consequências (valores default `similarity=0.15`, `softness=0.10`, `spill=0.5`):**

1. **Exibição errada.** `refreshValues()` calcula `0.15 * 100 * 0.01 = 0.15`.
   O spinbox mostra `0` (arredonda para 0 decimais) em vez de `15`.
2. **Gravação errada.** Ao digitar `15`, o framework chama
   `write(15 / 0.01) = write(1500)`, e o lambda grava
   `1500 / 100.0 = 15.0` em `chromaKeySimilarity`.
3. **Saturação.** `PreviewWidget.cpp:3514` faz
   `std::clamp(c.chromaKeySimilarity, 0.0, 1.0)`, então o valor effective é
   `1.0` — a chave passa a remover **tudo** dentro de `fullRange = 1.0 * 255 * 1.3 = 331.5`,
   ou seja, praticamente a imagem inteira.

**Reprodução:**
1. Selecionar um clipe de vídeo.
2. Ativar o Chroma Key.
3. Observar "Similaridade:" mostrando `0` (deveria ser `15`).
4. Digitar `15` → a imagem fica totalmente removida.

**Correção sugerida:** manter o contrato `read`/`write` em valores crus:

```cpp
addSpinRow(tr("Similaridade:"), 0, 100, 1, 0, 50, 100.0,
    [clip]() { return clip->chromaKeySimilarity; },
    [this, clip](double v) {
        clip->chromaKeySimilarity = v;
    });
```

> O painel Express (`ExpressWidget.cpp:419,430,1068-1069`) faz a conversão
> corretamente (`v / 100.0`), o que confirma que `dim` não deveria ser `0.01`.
> Isso também cria uma **inconsistência entre as duas UIs**: o Express grava
> certo, o painel de propriedades grava errado.

---

### B-02 — Preview do clipe do topo ignora Suavidade e Spill

**Arquivos:** `src/ui/PreviewWidget.h:200-202`, `src/ui/PreviewWidget.cpp:2809-2811`, `PreviewWidget.cpp:3422-3428`
**Severidade:** Crítica (recurso novo inoperante no caminho principal)

`PreviewWidget` mantém uma cópia local dos parâmetros do clipe do topo para o
thread de render. Somente três campos de chroma key são copiados:

```cpp
// PreviewWidget.h:200-202
bool   m_clipChromaKey = false;
QColor m_clipChromaKeyColor{Qt::green};
double m_clipChromaKeySimilarity = 0.15;
```

```cpp
// PreviewWidget.cpp:2809-2811  (setClipFrom)
m_clipChromaKey         = clip->chromaKey;
m_clipChromaKeyColor    = clip->chromaKeyColor;
m_clipChromaKeySimilarity = clip->chromaKeySimilarity;
// ← chromaKeySoftness e chromaKeySpillSuppress NÃO são copiados
```

Em seguida `applyBasicEffects()` (`PreviewWidget.cpp:3422-3428`) monta um
`Clip` temporário e **não preenche** os dois campos novos:

```cpp
Clip c;                       // ← herda os defaults do struct
c.chromaKey          = m_clipChromaKey;
c.chromaKeyColor     = m_clipChromaKeyColor;
c.chromaKeySimilarity = m_clipChromaKeySimilarity;
// c.chromaKeySoftness e c.chromaKeySpillSuppress ficam 0.10 e 0.5
```

**Consequência:** o clipe do topo (o que o usuário está olhando) **sempre** usa
`softness = 0.10` e `spill = 0.5`, ignorando o que foi configurado. As camadas
inferiores renderizam pelo caminho real (`PreviewWidget.cpp:1805`, `3271`) e
**usam** os valores corretos — então o vídeo fica inconsistente entre camadas,
e a prévia de qualquer ajuste feito nos sliders não corresponde ao resultado.

Além disso, `updateBtnColor`/sync do painel de propriedades continua lendo
`clip->chromaKeySoftness`, o que leva o usuário a acreditar que o ajuste teve
efeito.

**Reprodução:**
1. Empilhar 2 clipes de vídeo.
2. Ativar Chroma Key no clipe do topo e definir Suavidade = 100.
3. Observar que nada muda no clipe do topo.
4. Reduzir o clipe de baixo: o spill aparece lá, mas não no topo.

**Correção sugerida:**
- `PreviewWidget.h`: adicionar
  `double m_clipChromaKeySoftness = 0.10;` e
  `double m_clipChromaKeySpillSuppress = 0.5;`
- `PreviewWidget.cpp:2811`: copiar os dois campos em `setClipFrom`.
- `PreviewWidget.cpp:3428`: atribuí-los ao `Clip c` temporário.

---

## 4. Bugs de Severidade Alta

### B-03 — `invalidateSceneContent()` é neutralizado por `emit modified()`

**Arquivos:** `src/ui/TimelineWidget.cpp:184-190`, `TimelineWidget.cpp:232-238`, `TimelineDrag.cpp:1225`, `TimelineDrag.cpp:1242-1246`
**Severidade:** Alta (otimização sem efeito)

O objetivo declarado da nova função (`TimelineWidget.h:94`) é evitar
`rebuildClipIndex()` durante o arraste contínuo de fade/opacidade. Porém, na
mesma iteração do `mouseMoveEvent`, logo a seguir, é emitido `modified()`:

```cpp
// TimelineDrag.cpp:1242-1246
} else if (m_dragMode == ClipOpacity) {
    ...
    invalidateSceneContent();      // "leve"
}
...
update();
emit modified();                  // ← dispara o handler abaixo
```

E o handler conectado trata `FadeIn`/`ClipOpacity` no ramo `else`:

```cpp
// TimelineWidget.cpp:184-190
connect(this, &TimelineWidget::modified, this, [this]() {
    if (m_dragMode == MoveClip || m_dragMode == TrimLeft
        || m_dragMode == TrimRight || m_dragMode == ResizeTrack)
        refreshView();
    else
        invalidateScene();         // ← FadeIn/ClipOpacity caem aqui
});
```

`invalidateScene()` (`TimelineWidget.cpp:223-230`) faz exatamente o que a
versão "leve" evita:

```cpp
void TimelineWidget::invalidateScene() {
    ++m_clipEpoch;
    m_staticDirty = true;
    rebuildClipIndex();            // ← reconstruído assim mesmo
    update();
}
```

**Consequência:** em **todo** `mousemove` de arraste de fade ou opacidade
executam-se `invalidateSceneContent()` **e** `invalidateScene()`. O trabalho
duplo é pior que o original. Além disso, `m_staticDirty = true` está em ambas,
portanto a re-renderização completa da camada estática (a parte cara) continua
ocorrendo — a otimização só pouparia a reconstrução do índice, que é a parte
barata.

**Correção sugerida:** incluir `FadeIn` e `ClipOpacity` na lista de drag modes
que chamam `refreshView()`, ou remover a chamada a `invalidateSceneContent()`
e manter apenas `emit modified()`. Recomenda-se também mover
`m_dragMode = FadeIn/ClipOpacity` para o ramo de `refreshView()` e reavaliar
se `m_staticDirty = true` é realmente necessário para fade/opacidade (o cache
de conteúdo dos clipes não muda nesses casos).

---

### B-04 — Faixa de scroll não cresce ao estender o último clipe

**Arquivo:** `src/ui/TimelineDrag.cpp:1243-1246`
**Severidade:** Alta (regressão funcional)

`updateScrollRanges()` foi removido do loop de arraste com o comentário:

```cpp
// updateScrollRanges() é pesado e desnecessário durante arraste:
// a duração total do projeto não muda ao mover/trimar clipes.
```

**A premissa é factualmente incorreta.** `updateScrollRanges()` depende
diretamente de `Project::duration()`:

```cpp
// TimelineWidget.cpp:636-647
const double total = m_project->duration();
...
const int totalW = kHeaderW + (int)(total * m_pps) + kMarginR;
```

E `duration()` é o **máximo** de `pos + dur` de todos os clipes:

```cpp
// Project.h:771-783
double duration() const {
    double d = 0.0;
    for (const auto& t : videoTracks)
        for (const auto& c : t.clips)
            d = std::max(d, c.pos + c.dur);
    ...
    return d;
}
```

Portanto, **mover** um clipe para a direita e **esticar (`TrimRight`)** o clipe
mais à direita **mudam** a duração do projeto.

Verificado também que `mouseReleaseEvent` (`TimelineDrag.cpp:1298-1393`) **não**
chama `updateScrollRanges()` — nem antes (`HEAD`) nem agora. Ou seja, não há
atualização no fim do arraste.

**Consequência:** ao arrastar a borda direita do último clipe para além do
limite atual da barra horizontal, a barra não cresce. Como o auto-scroll é
limitado por `m_hbar->maximum()`, a view **não acompanha o mouse** e o clipe
"trava" visualmente no limite direito, sem retorno visual ao usuário. O range
só é corrigido quando alguma outra ação chamar `updateScrollRanges()` (adicionar
clipe, redimensionar faixa, importar mídia, etc.).

**Reprodução:**
1. Projeto com um único clipe de 10 s.
2. Arrastar a borda direita dele para a direita, bem além da largura visível.
3. A barra horizontal não cresce; a borda de trim para de acompanhar o cursor.

**Correção sugerida:** remover o comentário incorreto e tratar o caso
específico sem regenerar o range a cada `mousemove`. Opções:
- Chamar `updateScrollRanges()` apenas quando `m_project->duration()` realmente
  mudou (comparar com o valor em cache), ou
- Movimentar a chamada para `mouseReleaseEvent()` e adicionar um
  `ensureHbarReaches(endTime)` durante o arraste (ajuste pontual do `maximum`).

---

## 5. Bugs de Severidade Média

### B-05 — Presets perdem Suavidade e Spill

**Arquivo:** `src/clipattrs.h:66-68` (serialização), `175-180` (desserialização)
**Severidade:** Média

O formato de preset grava somente três campos de chroma key:

```cpp
o["chromaKey"]          = c.chromaKey;
o["chromaKeyColor"]     = c.chromaKeyColor.name(QColor::HexArgb);
o["chromaKeySimilarity"] = c.chromaKeySimilarity;
// ← faltam chromaKeySoftness e chromaKeySpillSuppress
```

**Consequência:** salvar um preset com softening/spill ajustados e aplicá-lo
depois restaura apenas `similarity`; os dois campos novos voltam ao default
(`0.10`/`0.5`). Como `pasteAttributes()` **foi** atualizado para incluir os
campos, copiar/colar atributos funciona, mas presets não — comportamento
inconsistente entre dois mecanismos que deveriam ser equivalentes.

**Correção sugerida:** adicionar as duas chaves em `clipToJson`/`clipFromJson`,
com `d("chromaKeySoftness", c.chromaKeySoftness)` para preservar compatibilidade
com presets antigos.

---

### B-06 — Exportador ignora Suavidade/Spill e fixa `blend=0.1`

**Arquivo:** `src/colombina/export/ProjectExporter.cpp:1289-1292`
**Severidade:** Média (divergência preview ↔ export)

```cpp
if (v.c->chromaKey)
    fc.last().append(QStringLiteral(",chromakey=color=%1:similarity=%2:blend=0.1")
                         .arg(hexColor(v.c->chromaKeyColor))
                         .arg(num(std::clamp(v.c->chromaKeySimilarity, 0.0, 1.0))));
```

O filtro `chromakey` do FFmpeg não tem equivalente direto para *softness* ou
*spill suppression*. Dois problemas:

1. **Dados perdidos:** nenhum ajuste de Suavidade ou Spill chega ao render final.
2. **Divergência algorítmica:** o preview agora usa distância perceptual
   ponderada (BT.601) + smoothstep, enquanto o export usa a distância simples
   do FFmpeg. Como o código antigo também tinha essa divergência, ela é
   preexistente — mas o novo algoritmo **aumentou** a diferença, porque mudou
   tanto a métrica de distância quanto a curva de transição.

**Correção sugerida (curto prazo):** documentar a limitação e derivar `blend`
de `chromaKeySoftness` em vez de fixar `0.1`, por exemplo
`blend = 0.01 + softness * 0.2`. **Longo prazo:** avaliar um segundo filtro
(p. ex. `despill`) no grafo, ou um `geq`/`colorkey` com a mesma métrica do
preview.

---

### B-07 — Guia de alinhamento aparece praticamente sempre

**Arquivo:** `src/ui/TimelineDrag.cpp:1134-1141`
**Severidade:** Média (UX)

```cpp
const double raw     = m_dragOrigPos + dt;
const double snapped = snapToEdges(snapTime(raw), m_dragClip);
// Guia de alinhamento: se o snap mudou a posição, mostra a linha vertical
if (std::fabs(snapped - raw) > 1e-6)
    m_snapLineX = kHeaderW + (snapped - m_viewStart) * m_pps;
else
    m_snapLineX = -1.0;
```

`snapTime()` quantiza o tempo para a grade de frames. Como `dt` provém de uma
divisão de pixels por `m_pps`, `raw` quase nunca está alinhado a frames —
logo `snapTime(raw) != raw` na esmagadora maioria dos `mousemove`. A condição
de exibição está, portanto, quase sempre satisfeita.

**Consequência:** a linha branca de alinhamento fica **sempre** visível durante
`MoveClip`/`TrimLeft`/`TrimRight`, indicando um snap que na verdade é apenas
quantização de frame. O recurso perde o significado pretendido (indicar
alinhamento com a borda de outro clipe).

**Reprodução:** arrastar um clipe sobre uma área vazia da timeline, sem
nenhuma outra borda por perto → a guia aparece mesmo assim.

**Correção sugerida:** detectar apenas o snap de borda, comparando antes e
depois de `snapToEdges()`:

```cpp
const double grid  = snapTime(raw);
const double snapped = snapToEdges(grid, m_dragClip);
if (std::fabs(snapped - grid) > 1e-6)   // houve snap de BORDA, não só de frame
    m_snapLineX = timeToX(snapped);
else
    m_snapLineX = -1.0;
```

Isso também elimina a duplicação da fórmula `kHeaderW + (t - m_viewStart) * m_pps`,
que deveria usar `timeToX()`.

> Observação adicional: em `TrimLeft` (linha 1155-1161) e `TrimRight` (linha
> 1173-1177) a comparação é feita contra a posição **já clampada** (`np`/`end`),
> que difere de `raw` por causa do `std::clamp`. Isso faz a guia aparecer na
> borda do limite de duração mesmo sem nenhum snap envolvido.

---

### B-08 — Supressão de spill funciona apenas para chave verde

**Arquivo:** `src/ui/PreviewWidget.cpp:3568-3574`
**Severidade:** Média (funcionalidade incompleta)

```cpp
const int chMax = std::max({pr, pg, pb});
if (chMax == pg && kg > kr && kg > kb) {          // ← só verde
    dst[si + 1] = (uchar)std::clamp(
        (int)(pg - spillAmt * (pg - pixLum)), 0, 255);
}
```

As três condições exigem que o **canal da chave seja o verde**. Para qualquer
outra cor de fundo — azul, ciano, magenta, amarelo — a supressão de spill é
**completamente inerte**, mesmo com o controle em 100%.

**Consequência:** o controle "Supressão spill" é enganoso: o usuário ajusta o
slider e nada muda, sem indicação de que a cor de fundo atual não é suportada.

**Correção sugerida:** generalizar para o canal da chave:

```cpp
const int chIdx = (kg >= kr && kg >= kb) ? 1 : (kb >= kr ? 2 : 0);
const int pv[3] = {pr, pg, pb};
const int kc[3] = {kr, kg, kb};
const int chMax = std::max({pr, pg, pb});
if (pv[chIdx] == chMax && kc[chIdx] > kc[(chIdx + 1) % 3]
                      && kc[chIdx] > kc[(chIdx + 2) % 3]) {
    int corrected = pv[chIdx] - (int)(spillAmt * (pv[chIdx] - pixLum));
    dst[si + chIdx] = (uchar)std::clamp(corrected, 0, 255);
}
```

Idealmente, desabilitar o slider com uma dica quando a cor escolhida não for
suportada, em vez de deixá-lo sem efeito.

---

### B-09 — Peso do spill invertido e critério de luminância restritivo

**Arquivo:** `src/ui/PreviewWidget.cpp:3562-3567`
**Severidade:** Média (algoritmo)

```cpp
if (spill > 0.0 && alpha > 10.0) {
    const double spillAmt = spill * (1.0 - alpha / 255.0);
    const double keyLum = (wR * kr + wG * kg + wB * kb);
    const double pixLum = (wR * pr + wG * pg + wB * pb);
    if (pixLum > keyLum) {
```

Três problemas no peso e no gate:

1. **Peso invertido em relação à visibilidade.** `spillAmt` é máximo quando
   `alpha → 10` (pixel praticamente invisível) e **zero** quando
   `alpha = 255` (pixel totalmente opaco). A franja de spill é visível nos
   pixels **semi**-opacos (alpha ~128), que recebem `0.5 × spill`. O efeito é
   máximo onde não se vê nada e mínimo onde a franja é mais visível.

2. **Gate `pixLum > keyLum` descarta pixels escuros contaminados.** Para chave
   verde pura `(0,255,0)`, `keyLum = 0.587 × 255 ≈ 149.7`. Um pixel de
   primeiro plano escuro com respingo, por exemplo `(50,120,50)`, tem
   `pixLum ≈ 91 < 149.7` → **nenhuma supressão é aplicada**, mesmo sendo
   claramente contaminado. Pixel escuro contaminado é justamente um caso
   comum (cabelo preto, sombra).

3. **Truncamento inconsistente.** O canal de spill usa `(int)` (trunca),
   enquanto o alpha usa `std::lround()` (arredonda). Diferença de até 1 LSB
   entre os dois cálculos na mesma iteração.

**Correção sugerida:** o fator de peso deveria ser **máximo em alpha
intermediário** (uma curva em forma de sino, ou simplesmente `1.0`
constante), e o gate de luminância deveria ser removido ou substituído por um
teste de "o canal da chave excede a média dos outros dois":

```cpp
const double spillAmt = spill;                 // ou sin(pi * alpha/255)
if (spill > 0.0 && alpha > 0.0) {
    const int others = pv[chIdx == 0 ? 1 : 0] + pv[chIdx == 2 ? 1 : 2];
    if (pv[chIdx] * 2 > others) { /* domina a cor da chave → suprimir */ }
}
```

---

### B-10 — `beginEdit()`/`emitEdited()` duplicados nos novos spinboxes

**Arquivo:** `src/ui/ClipPropertiesWidget.cpp:603-621`
**Severidade:** Média (desempenho/consistência)

`addSpinRow()` já chama `beginEdit()` e `emitEdited()` em seu próprio handler
(`ClipPropertiesWidget.cpp:267`, `270`):

```cpp
connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
        this, [this, write = std::move(write), dim](double v) {
            if (m_creating || m_syncing) return;
            beginEdit();
            write(v / dim);
            refreshStopwatches();
            emitEdited();          // ← já emitido aqui
        });
```

Os três setters novos repetem as duas chamadas:

```cpp
[this, clip](double v) {
    beginEdit();                 // ← duplicado
    clip->chromaKeySimilarity = v / 100.0;
    emitEdited();                // ← duplicado:_modified() emitido 2×
},
```

`beginEdit()` é idempotente (`m_undoPushed` + `m_undoTimer`, linhas 181-187),
então o undo não é duplicado. Mas `emitEdited()` → `emit modified()` **é**
emitido duas vezes, o que dispara duas vezes cada um dos consumidores em
`MainWindow.cpp:379-386`: `m_preview->refreshView()`, `m_mixer->refresh()`,
`m_mesa->refresh()` e `setModified()`.

**Consequência:** cada ajuste de slider dispara dois ciclos completos de
re-render de preview, mixer e mesa — o dobro do trabalho desnecessário, e o
indicador de "modificado" na janela é atualizado duas vezes.

**Correção sugerida:** remover `beginEdit()`/`emitEdited()` dos três lambdas
escritores, seguindo o padrão dos demais spinboxes (ex.: linha 539-541).

> Este achado é independente de B-01 e permanece mesmo após a correção da escala.

---

## 6. Bugs de Severidade Baixa

### B-11 — Guarda `m_dragMode == None` inalcançável (código morto)

**Arquivo:** `src/ui/TimelineDrag.cpp:1076`, `TimelineDrag.cpp:1082-1100`
**Severidade:** Baixa (manutenção)

```cpp
if (m_dragMode == MoveClip) {          // linha 1076
    ...
int row;
bool audio;
int vrow;
int cvrow;
// Durante um arraste ativo (MoveClip/Trim/etc), os hit-tests de volume
// são ignorados ...
if (m_dragMode == None) {              // linha 1090 — DENTRO do bloco MoveClip
    if (m_showVolLines && clipVolAt(e->pos(), cvrow) != nullptr) { ... return; }
    if (volRowAt(e->pos(), vrow) >= 0) { ... }
}
```

`m_dragMode` já é `MoveClip` na linha 1076, portanto a condição da linha 1090
nunca é verdadeira e o bloco inteiro é **código morto**.

**Contexto:** esse hit-test de volume também estava dentro do `if (m_dragMode ==
MoveClip)` na versão em `HEAD`. Ele fazia o arraste "congelar" quando o cursor
cruzava uma linha de volume durante um `MoveClip` (o `return` saía antes de
atualizar a posição). A correção do travamento **é funcionalmente correta**,
mas foi obtida por exclusão implícita em vez de remoção: as variáveis `vrow` e
`cvrow` (linhas 1084-1085) permanecem declaradas e usadas apenas em código
inalcançável, e a indentação irregular das declarações na linha 1082 sugere
que o bloco foi inserido no lugar errado originalmente.

**Correção sugerida:** remover o bloco morto e as variáveis `vrow`/`cvrow`,
realignando as declarações de `row`/`audio` e a indentação. Se o comportamento
desejado for outro (ignorar hit-test de volume durante **qualquer** arraste,
não só `MoveClip`), mover o teste para fora do `if (m_dragMode == MoveClip)`.

---

### B-12 — Guia de alinhamento não é limpa ao sair das faixas

**Arquivo:** `src/ui/TimelineDrag.cpp:1137-1177`
**Severidade:** Baixa (glitch visual)

`m_snapLineX` só é atribuído dentro de `if (m_dragMode == MoveClip)` /
`TrimLeft` / `TrimRight` (`TimelineDrag.cpp:1139`, `1159`, `1175`) e é limpo
apenas no `mouseReleaseEvent` (`TimelineDrag.cpp:1376`).

O corpo do `MoveClip` está dentro de `if (rowFromY(...))` (linha 1113). Se o
mouse mover para fora da área das faixas (por exemplo, sobre a régua), o bloco
não é executado e `m_snapLineX` **retém o valor anterior** — a linha branca
fica congelada na posição antiga enquanto o cursor se afasta.

**Correção sugerida:** zerar `m_snapLineX = -1.0` logo após a entrada no
`mouseMoveEvent` (antes do `if (rowFromY(...))`), ou adicionar
`m_snapLineX = -1.0;` no `mousePressEvent` para garantir estado limpo no início
de cada arraste.

---

### B-13 — Regressão de qualidade: faixa suave default muito estreita

**Arquivo:** `src/ui/PreviewWidget.cpp:3524-3525`, `3546-3551`
**Severidade:** Baixa (qualidade visual / comportamento)

Código novo:

```cpp
const double coreRange = sim * 255.0;
const double fullRange = coreRange * (1.0 + soft * 3.0);
...
if (dist <= coreRange) alpha = 0.0;                       // corte duro
else { const double t = (dist - coreRange) / (fullRange - coreRange);
       const double s = t * t * (3.0 - 2.0 * t);          // smoothstep
       alpha = s * 255.0; }
```

Código anterior (`HEAD:src/ui/PreviewWidget.cpp:3524-3529`):

```cpp
const double range = sim * 255.0;
...
if (dist < range) dst[si + 3] = (uchar)clamp((int)(dist / range * 255.0), 0, 255);
```

**Análise numérica com os defaults (`sim=0.15`, `soft=0.10`):**

| | Antes | Depois |
|---|---|---|
| `coreRange` | — | 38.25 |
| `fullRange` | 38.25 | 49.73 |
| Largura da rampa de transição | 38.25 (15% da escala) | **11.48 (4.5%)** |
| `alpha` em `dist = 38.25` | 255 (opaco) | **0 (totalmente transparente)** |
| Curva | linear | smoothstep (concentra a transição no meio) |

**Consequências:**
1. Um pixel que antes era **opaco** (alpha 255) agora é **totalmente
   transparente** — a mudança é semanticamente agressiva, não apenas estética.
2. A transição visível é ~3.3× mais estreita, e o smoothstep a concentra ainda
   mais, produzindo um recorte quase duro com os defaults. Bordas com franja
   de chroma KEY "cortam" em vez de esmaecer.
3. Projetos existentes salvos com `similarity` calibrado para o algoritmo
   antigo serão re-renderizados com corte mais agressivo e borda mais dura.

Isto **não é um bug de lógica** — é uma mudança de algoritmo possivelmente
intencional. Mas o valor default de `softness = 0.10` foi escolhido sem
compensar o fato de que a rampa default é de apenas ~11 unidades em 255.

**Correção sugerida:** reconsiderar o mapeamento, por exemplo
`fullRange = coreRange + soft * 255.0` (faixa de borda independente da
similaridade) em vez do fator multiplicativo `1 + soft*3`. Com `soft=0.10` isso
daria uma rampa de 25.5 unidades com curva razoável, e `soft=1.0` daria
`coreRange + 255`. Alternativamente, elevar o default de `softness` para ~0.25.

---

### B-14 — Constantes de loop recalculadas por pixel

**Arquivo:** `src/ui/PreviewWidget.cpp:3532-3534`
**Severidade:** Baixa (desempenho)

```cpp
for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
        ...
        const double coreSq = coreRange * coreRange;   // ← invariante do loop
        const double fullSq = fullRange * fullRange;   // ← invariante do loop
```

`coreSq` e `fullSq` dependem apenas de `sim` e `soft`, que são constantes
durante o quadro. Devem ser calculadas uma vez antes dos laços (junto de
`coreRange`/`fullRange`, na linha 3524-3525). Em 1080p são ~2.07 milhões de
multiplicações redundantes por quadro, por clipe.

Note que o compilador provavelmente já faz *loop-invariant code motion*, então
o impacto real deve ser pequeno. Ainda assim, é uma inconsistência de estilo
com o resto do arquivo — `keyLum` (linha 3565), igualmente invariante, está
**dentro** do `if` mais interno.

---

### B-15 — Testes de serialização sem os novos campos

**Arquivo:** `tests/tst_serialization.cpp:78-80`, `239`
**Severidade:** Baixa (cobertura)

O teste monta um `Clip` com `chromaKey`, `chromaKeyColor` e
`chromaKeySimilarity`, verifica esses três no round-trip, e **não**
exercita `chromaKeySoftness` nem `chromaKeySpillSuppress`.

A serialização em `Project.cpp:229-230,379-380` está correta, mas
`clipattrs.h` está incompleta (B-05) — e nenhum teste cobriria isso.

**Correção sugerida:** adicionar os dois campos ao fixture e às asserções de
`tests/tst_serialization.cpp`, e criar um caso de teste para
`clipToJson`/`clipFromJson` de `clipattrs.h`.

---

## 7. Achado Preexistente (não introduzido por estas alterações)

### B-16 — Branches de Ripple inalcançáveis

**Arquivo:** `src/ui/TimelineDrag.cpp:1153`, `1171`, `1184-1196`
**Severidade:** Info

```cpp
} else if (m_dragMode == TrimLeft) {              // linha 1153 — pega TODOS os TrimLeft
    ...
} else if (m_dragMode == TrimRight) {             // linha 1171 — pega TODOS os TrimRight
    ...
} else if (m_dragMode == TrimLeft && m_tool == ToolRipple) {   // linha 1184 — INALCANÇÁVEL
    rippleTrimLeft(clip, m_dragOrigIn + delta);
} else if (m_dragMode == TrimRight && m_tool == ToolRipple) {  // linha 1191 — INALCANÇÁVEL
    rippleTrimRight(clip, newDur);
}
```

Os testes em 1184 e 1191 verificam `m_dragMode == TrimLeft/TrimRight`, condição
já consumida pelas ramificações anteriores. O **Ripple Edit nunca executa**:
`rippleTrimLeft()`/`rippleTrimRight()` são código morto.

Isso já existia em `HEAD` e **não** foi introduzido pelas alterações atuais.
Está listado por ser um bug real e de alto impacto funcional no mesmo arquivo
tocado.

**Correção sugerida:** mover os testes de ripple para **antes** dos ramos
genéricos (ou aninhá-los dentro deles, verificando `m_tool == ToolRipple`).
Como o ripple altera os clipes subsequentes, é preciso confirmar a intenção
com o usuário antes de alterar a ordem — a mudança é mais invasiva do que
aparenta.

---

## 8. Limitações da Análise

A verificação foi **estática**. Não foi possível compilar nem executar os
testes neste ambiente:

```
$ make -C build -j$(nproc)
/bin/sh: linha 1: make: comando não encontrado
```

`make`, `cmake`, `ninja` e `g++` não estão disponíveis no `PATH`. O
`CMakeCache.txt` existente referencia `/usr/bin/c++` e `/usr/bin/make`, ambos
ausentes.

**Consequências:**
- B-01, B-02, B-05 e B-15 foram confirmados por **análise de fluxo de dados**
  e aritmética, não por execução — a confiança é alta, mas um teste
  automatizado deve ser adicionado junto com as correções.
- B-03, B-04, B-07, B-11 e B-12 foram confirmados por leitura do fluxo de
  controle e verificação cruzada de chamadas.
- B-08, B-09, B-13 e B-14 são julgamentos sobre qualidade/algoritmo e podem
  requerer decisão de produto.

---

## 9. Ordem de Correção Sugerida

✅ Todas as correções foram aplicadas (ver seção 2.1). Ordem executada:

1. **B-01** — corrigir a escala dos três spinboxes (quebrava o recurso inteiro).
2. **B-02** — adicionar os dois campos ao snapshot do preview.
3. **B-04** — corrigir o crescimento da faixa de scroll.
4. **B-03** — remover a dupla invalidação.
5. **B-10** — remover `beginEdit()`/`emitEdited()` duplicados.
6. **B-05** — completar a serialização de presets.
7. **B-08**, **B-09** — generalizar e corrigir o algoritmo de spill.
8. **B-07**, **B-12** — corrigir a guia de alinhamento.
9. **B-11** — remover código morto.
10. **B-06** — derivar `blend` do export da softness.
11. **B-13** — tolerância absoluta da borda.
12. **B-14** — mover constantes para fora do laço.
13. **B-15** — ampliar a cobertura de testes.
14. **B-16** — reordenar os branches de Ripple.
15. **B-17** — criar o indicador de trim no estilo Premiere.

### Pendente de validação

Nenhuma das correções foi **compilada ou testada** (ver seção 8). Antes de
considerar fechado, é preciso:

- [ ] `cmake --build build -j$(nproc)` e `ctest --test-dir build`.
- [ ] `tst_serialization` (os dois casos novos de preset).
- [ ] Manual: spinboxes de Similaridade/Suavidade/Spill (B-01), preview do
      clipe do topo vs. camada inferior (B-02), arraste de `TrimRight` no
      último clipe além do limite (B-04), Ripple Edit na ferramenta Ripple
      (B-16 — é a primeira vez que essa ferramenta executa).
- [ ] Recalibrar `chromaKeySimilarity` em projetos existentes: o mapeamento da
      borda mudou (B-13) e o alpha agora chega a 0 dentro do core.

---

## 10. Cobertura por Arquivo

| Arquivo | Achados |
|---|---|
| `src/ui/ClipPropertiesWidget.cpp` | B-01, B-10 |
| `src/ui/PreviewWidget.cpp` | B-02, B-08, B-09, B-13, B-14 |
| `src/ui/PreviewWidget.h` | B-02 |
| `src/ui/TimelineWidget.cpp` | B-03 |
| `src/ui/TimelineWidget.h` | B-03 |
| `src/ui/TimelineDrag.cpp` | B-04, B-07, B-11, B-12, B-16, B-17 |
| `src/clipattrs.h` | B-05 |
| `src/colombina/export/ProjectExporter.cpp` | B-06 |
| `tests/tst_serialization.cpp` | B-15 |
| `src/ui/ExpressWidget.cpp` | sem achados (conversão correta) |
| `src/colombina/models/Project.cpp` / `.h` | sem achados (serialização correta) |
| `src/ui/TimelinePaint.cpp` | B-17 |
