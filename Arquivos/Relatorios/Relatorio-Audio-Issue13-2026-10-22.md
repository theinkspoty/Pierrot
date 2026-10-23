# Relatório — Áudio do Pierrot e o issue #13 ("Nem todos os Áudios são tocados")

Data: 2026-10-22 · Repo: theinkspoty/Pierrot · Branch `main` sincronizada com `origin/main`
(Arch: `01bf4db`). Fix de áudio: commit `36d74d3`.

Complementa `Relatorio-Audio.md` (arquitetura geral do pipeline), que está
desatualizado no ponto do conform — ver §3.

---

## 1. O que o issue diz

**[#13 — Nem todos os Áudios são tocados](https://github.com/theinkspoty/Pierrot/issues/13)**
· autor **Gumduckk** · aberto em **22/10/2026** · **aberto**, sem comentários,
sem assignee/label/milestone.

Três reclamações no mesmo corpo:

| # | Relato (resumo) |
|---|---|
| A | Áudios não tocam quando há um ou mais áudios "um atrás do outro", principalmente SFX que precisam tocar logo em seguida, num curto intervalo. |
| B | O frame do vídeo é renderizado **depois** do áudio, com certo delay: ele ouve o áudio e, alguns milésimos depois, o quadro aparece — "falando de um buraco (vazio) para o vídeo". |
| C | Persistem travamentos em clipes pequenos (vários clipes curtos seguidos), "de clipe para clipe". Teste/reclamação dele sobre o **laartman** atual. |

---

## 2. Diagnóstico

### 2.1 Reclamação A — SFX em sequência ficam mudos (Causa-raiz confirmada)

O conform (`AudioConformCache`) é servido por **um único worker** que escolhia o
chunk com o **menor frame de mídia ainda não decodificado**
(`workerLoop`, pré-fix). Com SFX curtos em fila, esse critério é
anti-temporal: o worker ficava preso terminando janelas antigas de clipes já
passados enquanto o clipe que ia tocar agora não tinha PCM. Dois agravantes:

1. **Pedidos maiores que o clipe**: `updateSources` pedia janelas fixas de
   **4 s** (ativa) e **6 s** (warm) por fonte, e `startAudio` pedia mais **3 s**
   por leitura. Um SFX de 0,2 s enfileirava ~6 s de decode inútil, competindo
   pela mesma sessão do worker.
2. **Sem urgência**: `request(cache, start, len)` não dizia *quando* aquele
   trecho precisava tocar.

Efeito ouvido: buraco de silêncio exatamente nos cortes — o áudio seguinte chega
sem conteúdo ("só se ouve o de trás").

### 2.2 Reclamação B — áudio uns milissegundos à frente do vídeo

Isso **não é defeito do mix**, é consequência do design do relógio:

- O **áudio é o relógio-mestre**. `tickImpl` corrige o relógio de parede com o
  consumo real do sink (`PlaybackEngine.cpp:274-345`): slew limitado
  (~6 frames/s de correção, `PlaybackEngine.cpp:341-342`) e tolerância de
  400 ms pós-ancoragem (`:307-312`).
- O sink é puxado por `QAudioSink` e **enche buffer à frente do playhead**
  (`startAudio`/warm, `PreviewWidget.cpp:1535-1540`) — deliberadamente: segurar
  o início até o PCM estar pronto (`waitReadyBeforeSink`) custava até 150 ms de
  UI e foi removido (Fix 9, 2026-10-17). O trade-off aceito é "1-2 frames de
  silêncio no warm-up frio" **e** áudio que toca ligeiramente antes do quadro.
- O vídeo é async (FrameWorker + pipeline de prefetch): quando o decode/atualiza
  atrasa, o quadro chega depois do que já está no ar no sink → o "buraco" que o
  autor descreve.

Ou seja: **B é sintoma do caminho de apresentação de vídeo**, não do pipeline de
áudio. É o mesmo fenômeno dos issues **#12** (preview congela/reaproveita frame)
e **#11** (travamentos em clipes cortados pequenos).

### 2.3 Reclamação C — travamento entre clipes pequenos

Cruzado com o item A: com dezenas de SFX curtos, o worker rodava em
sessões curtas de 0,75 s em rodízio (`AudioConformCache.cpp:415`) num backlog
inchado por pedidos que passavam do fim do clipe — a disputa atrasava o
próximo clipe e o quadro "trava" na transição. O fix limita o backlog ao
material real do clipe, o que **alivia C**, mas C também depende do caminho de
vídeo (issue #11, ainda aberto).

---

## 3. O que já foi corrigido — commit `36d74d3` (22/10, 14:10)

`fix(audio): prioriza conform pela timeline e limita pedidos ao clipe`
· 5 arquivos, +174/−28 · **já em `origin/main`**.

### 3.1 Urgência no pedido (`dueSec`)

- `AudioConformCache::request(cache, start, len, dueSec)` — `dueSec` = instante
  da **timeline** em que o trecho é necessário (`AudioConformCache.h:106-112`).
- Metadata por chunk: `requestDueSec`, `lastRequestMs`, `lastServedMs`
  (`AudioConformCache.h:69-71`). Pedidos no mesmo tick (≤50 ms) ficam com o
  **menor** due — ativo (playhead) vence warm (início do clipe)
  (`AudioConformCache.cpp:96-98`).

### 3.2 Prioridade no worker (`workerLoop`, `:313-390`)

1. Pedidos **frescos** primeiro (renovados nos últimos `kFreshRequestMs = 1000 ms`,
   `:30`) — janelas velhas de trecho já reproduzido só entram se não há nada atual.
2. Entre frescos: menor `dueSec` (o que toca mais cedo na timeline).
3. Empate: servido há mais tempo (rodízio entre fontes simultâneas).
4. Último critério: menor frame de mídia (o critério antigo, agora só como
   desempate).
5. Contagem de sessão (`pending`) passa a considerar só os frescos (`:414`) —
   backlog antigo não encolhe a sessão do áudio que está tocando.

### 3.3 Pedidos limitados ao material do clipe

Novo campo `Source::readEnd` (`AudioMixer.h:688`) = frame de mídia do fim do
clipe. Os três pontos de pedido agora limitam o comprimento:

| Onde | Antes | Agora | Ref. |
|---|---|---|---|
| Fonte ativa (`updateSources`) | 4 s fixos | `min(4, fimDoClipe − mediaPos + 0,1)`, due = `anchorSec` | `AudioMixer.h:219-222` |
| Warm | 6 s fixos | `min(6, fimDoClipe − mediaPos + 0,1)`, due = `clipPos` | `AudioMixer.h:255-258` |
| Leitura em curso (`readData`) | 3 s fixos | `min(3, fimDoClipe − readSec)`, due = 0 (é o que toca agora) | `AudioMixer.h:483-486` |
| `warmPlayhead` | 6 s fixos | `min(6, restanteDoClipe × speed + 0,1)`, due = `playhead` | `PreviewWidget.cpp:2734-2739` |

### 3.4 Lookahead no start do play

`startAudio` agora inclui as fontes de **warm** no primeiro `updateSources`
(`PreviewWidget.cpp:1535-1540`): se o play começa colado num corte, o sink já
enche o buffer com o clipe curto que começa logo — sem ele, o SFX era pulado.

### 3.5 Testes

`tests/tst_audio_conform.cpp` ganhou `requestRecordsUrgency` e
`requestKeepsSmallestDueWithinSameTick` (menor due vence no mesmo tick;
pedido separado no tempo substitui a urgência).

**Verificado nesta máquina:**

```
./build/tst_audio_conform → Totals: 26 passed, 0 failed, 0 skipped (121 ms)
binário build: 22/10 16:28 · fonte do fix: 22/10 13:48 → build cobre o fix
```

---

## 4. Status

| Item | Estado |
|---|---|
| Causa A (silêncio em SFX em sequência) | **Corrigido** em `36d74d3`, em `origin/main`, testes verdes |
| C (travamento em clipes pequenos) | **Aliviado** pelo mesmo fix; dependência restante = caminho de vídeo (issue #11, aberto) |
| B (áudio antes do quadro) | **Comportamento de projeto** (áudio é relógio-mestre + buffer à frente); não atacado por este fix; relacionado a #12 |
| Issue #13 | **Ainda aberto**, sem comentário — o autor não sabe que a causa A já foi corrigida |
| Regressão de escopo | Nenhuma: só toca em pedidos de conform, mixer e play — a cadeia de DSP/export (`Relatorio-Audio.md`) não mudou |

## 5. Recomendações

1. **Responder no issue #13** e pedir re-teste numa build a partir de `01bf4db`;
   fechar (ou estreitar para só "B") após o retorno do Gumduckk.
2. Ao fechar, deixar explícito que B (delay do quadro) e C viram acompanhamento
   nos issues **#12** e **#11**, para o issue não virar mistura de três assuntos.
3. Antes de fechar: teste manual com ~20 SFX de 0,2-0,5 s encadeados e com
   "tocar da agulha" (Enter) colado num corte — foi exatamente o cenário que o
   fix cobre e só ele valida o fim do buraco.
4. Para B (se persistir como reclamação): medir o atraso real sink↔exibição com
   `playDbg()` (`PlaybackEngine.cpp:422-435`) antes de mexer em qualquer relógio.
