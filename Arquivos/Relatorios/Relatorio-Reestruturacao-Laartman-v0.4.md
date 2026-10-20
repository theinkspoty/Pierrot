# Relatório — Reestruturação do Laartman (v0.4)

Data: 2026-10-19
Escopo: reestruturar o motor de decodificação `Laartman` para um **núcleo
imutável** de decodificação, movendo toda a coordenação de acesso (pool,
posse, prefetch, lookahead, qualidade) para a camada do Pierrot.
Status: **proposta** (nenhum código alterado por este documento).

---

## 0. Tese

> O Laartman v0.4 **só decodifica**. Ele não sabe quantos decoders existem,
> quem os usa, nem por quê. Toda coordenação vive **acima** dele, no Pierrot.

O núcleo deixa de tentar coordenar acesso. Com isso, toda uma classe de falhas
de concorrência (leases disputados, "sem slot", frame vazio por bloqueio) deixa
de existir **por construção**, em vez de ser remediada caso a caso.

---

## 1. Objetivo

- **Congelar a responsabilidade:** o Laartman decodifica um frame num tempo e
  pronto. Nenhuma decisão de fluxo (quantos abertos, qual fonte, qual
  qualidade, quando prefetch) pertence a ele.
- **Estabilidade por desenho:** um núcleo que não conhece a timeline não pode
  ficar instável por causa da timeline.
- **Superfície mínima:** uma API pequena e estável, que não precise ser mexida
  quando o Pierrot evoluir.

---

## 2. Estado atual (inventário)

| Arquivo | LOC | Destino |
|---|---|---|
| `FFmpegDecoder.cpp` | 611 | L2 (split) |
| `FFmpegDecoderOpen.cpp` | 347 | L0 + L1 (split) |
| `FFmpegDecoderAudio.cpp` | 483 | L0 + L1 + L3-Áudio |
| `FFmpegDecoderProbe.cpp` | 88 | L3 (probe) |
| `FFmpegDecoderPriv.h` | 154 | helpers L0/L1 |
| `DecoderPool.cpp/.h` | 204 | **L4 (Pierrot)** |
| `FFmpegEncoder.cpp/.h` | 351 | **sai do Laartman** |
| **Total do núcleo** | ~2.238 | alvo ~1.400 |

Problema observado: o Laartman hoje acumula, além da decodificação, o
gerenciamento de posse (`busy`/lease), um pool com permissão de acesso e o
pipeline de lookahead. É nessa parte — alheia à decodificação — que se originam
as falhas de reprodução em projetos de grande escala.

---

## 3. As sete camadas

```
┌──────────────────── PIERROT · política (volátil) ────────────────────┐
│ L6  SESSÃO/UI      PreviewWidget, Mesa, workers, export               │
│ L5  ESCOLHA        proxy, qualidade, hw on/off                        │
│ L4  CACHE/PIPELINE SourcePool, prefetch, lookahead                    │
├──────────────────── FRONTEIRA CONGELADA · Laartman ──────────────────┤
│ L3  CONTEXTO       VideoContext, AudioContext, probe   ← API pública  │
│ L2  SESSÃO DECODE  frame/seek/hold/loop/cancel (privado)              │
│ L1  CODEC          libavcodec, sws, swr, VAAPI                        │
│ L0  DEMUX          libavformat, read_packet, packets_thread           │
└───────────────────────────────────────────────────────────────────────┘
```

Regra de ouro: **estado mutável só existe em L2, é privado, e nunca escapa.**
A fronteira congelada fica entre **L3 e L4**.

---

## 4. Desenho por camada

### L0 — Demux

| Arquivo | Função |
|---|---|
| `Demuxer.h/.cpp` | Um contexto de demux por tipo de mídia (vídeo e áudio separados) |
| `PacketReader.h/.cpp` | Thread dedicada de leitura de pacotes à frente |

Cada uma destas responsabilidades será implementada no v0.4:

- **Contextos separados de vídeo e áudio:** cada fluxo tem seu próprio contexto
  de demuxer. Isso elimina a dependência de um único contexto compartilhado com
  cadeia de mutex cruzada entre vídeo e áudio (hoje `m_mutex → m_audioMutex`).
  Vídeo e áudio passam a ler pacotes de forma independente, sem esperar um ao
  outro.
- **Thread de leitura à frente:** uma thread lê pacotes adiantada, com
  sincronização e buffer limitado. Separa I/O (disco) de compute (decode): se o
  disco engasga, o decode não para.
- **Detecção de taxa de quadros variável (VFR):** streams com timestamps
  irregulares passam a ter a matemática de tempo ajustada. Hoje o motor assume
  taxa constante, o que gera drift em arquivos VFR.
- **Correção de DTS negativo / fora de ordem:** arquivos reais com timestamps
  quebrados são corrigidos sem perder ordem de pacotes.

### L1 — Codec

| Arquivo | Função |
|---|---|
| `VideoCodec.h/.cpp` | send/receive de vídeo + conversão para RGBA + VAAPI |
| `AudioCodec.h/.cpp` | send/receive de áudio + resample para S16 |
| `HwDevice.h/.cpp` | device de aceleração compartilhado + auto-cura |
| `ErrorConceal.h/.cpp` | ocultação de erro (último quadro bom) |

- **Aceleração por hardware como dependência injetável:** hoje o device VAAPI é
  resolvido dentro do `open`. No v0.4, a aceleração vira um `HwDevice` que a
  camada de política escolhe e injeta. O núcleo recebe a decisão, não a toma.
- **Ocultação de erro (último quadro bom):** quando um pacote chega corrompido,
  o motor entrega o último quadro válido em vez de devolver vazio ou quebrar.
  Isso é robustez de decodificação e pertence ao núcleo.
- **Caches segregados por tipo:** a conversão de cor/escala e o resample de
  áudio são mantidos separados e reutilizados, sem misturar estado de vídeo e de
  áudio.

### L2 — Sessão de decode (o estado, agora privado)

| Arquivo | Função |
|---|---|
| `DecodeSession.h/.cpp` | posição, convenção de hold, loop, cancelamento |
| `SeekPolicy.h/.cpp` | lógica de busca (hoje embutida no `frameAt`) |

- O estado mutável que hoje vaza (`m_lastPtsSec`, `m_lastFrame`, posse,
  convenção de hold, cancelamento) passa a ser **privado da sessão** e
  serializado pelo mutex interno. Ninguém de fora o vê nem o disputa.
- **O cache de frames sai daqui:** hoje um LRU de 120 quadros mora dentro do
  decoder. No v0.4, o cache é política de L4. O núcleo não promete cache.
- **A busca inteligente** (decodificar do keyframe até o alvo; re-seek só em
  saltos grandes; embrulho de loop) permanece aqui — é decodificação pura.
- **O lookahead de frame sai daqui:** hoje o motor decodifica um quadro à frente
  "escondido". No v0.4 isso vira política explícita em L4.

### L3 — Contexto (API congelada)

| Arquivo | Função |
|---|---|
| `Decoder.h/.cpp` | fachada pública (o único header que o Pierrot vê) |
| `VideoContext.h/.cpp` | `open/frame/close` = L0 + L1 + L2 |
| `AudioContext.h/.cpp` | `seekAudio/readPcm` |
| `Probe.cpp` | `probe()` / detecção de imagem animada |

API pública (congelada):

```cpp
struct MediaInfo {
    double duration;
    int width, height;
    double fps;
    bool hasVideo, hasAudio;
};

class Decoder {
public:
    static MediaInfo probe(const QString& path);
    bool   open(const QString& path, const DecodeOptions& opt);
    void   close();
    QImage frame(double tAbsoluteSec, int maxWidth,
                 const std::atomic<bool>* cancel = nullptr);
    bool   seekAudio(double tAbsoluteSec);
    int    readPcm(void* out, int maxBytes);
    bool   audioFinished() const;
    double fps() const;
};
```

O que **não** existe mais no header público: `acquire`, `release`, `busy`,
`setHardwareDecodeAllowed`, `releaseBuffers`. Nenhuma noção de posse.

### L4 — Cache/Pipeline (Pierrot)

| Arquivo | Função |
|---|---|
| `SourcePool.h/.cpp` | mantém decoders abertos (ex-`DecoderPool`, **sem `busy`**) |
| `FramePipeline.h/.cpp` | lookahead e prefetch (ex-`FrameWorker`) |
| `FrameCache.h/.cpp` | cache de frames (ex-LRU que sai do núcleo) |

`SourcePool`:

```cpp
std::shared_ptr<Decoder> get(const QString& path);  // SEMPRE devolve um decoder
```

- Sem `busy`. Se não houver slot, o pool despeja o decodificador mais frio
  (LRU) ou abre além do orçamento; **nunca devolve `nullptr` por "ocupado"**.
- Isso elimina as falhas de pool **por construção**, porque não existe caminho
  que recuse acesso a uma fonte.

### L5 — Escolha (Pierrot)

| Arquivo | Função |
|---|---|
| `FramePolicy.h/.cpp` | escolha de proxy vs original, `maxWidth`, `hwAllowed` |
| `ProxyChooser.h/.cpp` | gestão de proxies (refino do atual `ProxyManager`) |

Toda decisão de "qual fonte, qual qualidade, aceleração ligada ou não" fica
aqui. É política pura, separada do núcleo.

### L6 — Sessão/UI

`PreviewWidget`, `MesaRenderer` e os workers passam a chamar apenas a
`FramePipeline`. Sem `acquire`, sem `release`, sem posse de decodificador.

---

## 5. Árvore de arquivos final

```
src/laartman/                     NÚCLEO CONGELADO
  Decoder.h/.cpp                  L3 fachada (único header público)
  VideoContext.h/.cpp             L3
  AudioContext.h/.cpp             L3
  Probe.cpp                       L3
  DecodeSession.h/.cpp            L2
  SeekPolicy.h/.cpp               L2
  VideoCodec.h/.cpp               L1
  AudioCodec.h/.cpp               L1
  HwDevice.h/.cpp                 L1
  ErrorConceal.h/.cpp             L1
  Demuxer.h/.cpp                  L0
  PacketReader.h/.cpp             L0
  DecoderPriv.h                   helpers
  (Encoder → movido para fora do Laartman)

src/colombina/ffmpeg/             L4 + L5 POLÍTICA
  SourcePool.h/.cpp               ex-DecoderPool (sem busy)
  FramePipeline.h/.cpp            ex-lookahead/prefetch
  FrameCache.h/.cpp               ex-LRU
  FramePolicy.h/.cpp              proxy/qualidade/hw
  MediaCache.cpp                  (existente)
  ProxyManager.*                  (existente)
  AudioConformCache.*             (existente)

src/ui/monitor/                   L6 CONSUMO
  FrameWorker.h/.cpp              só chama FramePipeline
  BgPrefetchWorker.h/.cpp         incorporado ao FramePipeline
```

Testes novos:

```
tests/tst_decoder_api.cpp         contrato de Decoder (frame/seek/loop)
tests/tst_sourcepool.cpp          get() nunca nullptr; LRU; sem busy
tests/tst_framepipeline.cpp       lookahead/prefetch
(tst_decoderpool.cpp → reescrito como tst_sourcepool.cpp)
```

---

## 6. Fases de migração

| Fase | Entrega | Toca núcleo? |
|---|---|---|
| **0** | `Decoder.h` como fachada fina sobre o motor atual | mínimo |
| **1** | `SourcePool` sem `busy` (L4) | não |
| **2** | `FramePipeline` recebe lookahead/prefetch (L4) | não |
| **3** | `Demuxer` separa vídeo e áudio (L0) | sim |
| **4** | `PacketReader` + VFR + DTS (L0) | sim |
| **5** | `ErrorConceal` último quadro bom (L1) | sim |
| **6** | `FrameCache` sai do decode e vai para L4 | sim |
| **7** | Remoção do `DecoderPool` antigo; mover Encoder | — |
| **8** | Aceitação: sessão de referência = 0 falhas, 0 drops | — |

As fases 0–1 entregam o conserto urgente **sem reescrever o núcleo**. As fases
3–6 são o ganho estrutural. Cada fase mantém a `main` compilando e é reversível
de forma independente.

---

## 7. Critérios de aceitação

1. **Zero falhas de pool** na sessão de referência (as 754 observadas → 0).
2. **Zero drops** em projeto multi-faixa e multi-tipo.
3. **Paridade de frame:** mesmo `(path, t, maxWidth)` produz pixels idênticos
   antes e depois.
4. **`Decoder` sem estado de posse:** nenhuma API de lease/pool no header.
5. **`SourcePool::get()` nunca devolve `nullptr`** (teste unitário).
6. **Latência não pior** que a versão anterior (lookahead preservado, agora em
   L4).
7. **CI:** todos os testes passam; ASan/UBSan limpos em `tst_concurrency`.

---

## 8. Melhorias adicionais propostas

Itens de alto retorno que valem entrar durante a reestruturação:

1. **Índice de busca persistido:** mapear os keyframes do arquivo e guardá-los em
   cache (`~/.cache/pierrot/seekidx/<hash>.bin`). O corte frio deixa de pagar a
   decodificação descartada desde o keyframe — hoje o custo dominante de abrir
   uma fonte.
2. **Leitura de pacotes com buffer limitado:** a thread de leitura à frente
   opera com teto (ex.: 32 pacotes), desacoplando disco de decode sem estourar
   memória.
3. **Erro como tipo, não como imagem vazia:** hoje "sem slot", "falha de decode"
   e "fim de arquivo" viram todos `QImage()` nulo. No v0.4, `frame()` devolve um
   resultado com status. O Pierrot distingue "aguarde" de "erro" de "acabou" e
   para de tratar tudo como vazio.
4. **Orçamento de memória global para o cache:** o `FrameCache` de L4 passa a ter
   um teto em megabytes (em vez de 120 imagens fixas por decoder), escalando por
   resolução. Resolve o consumo elevado em 4K.
5. **Instrumentação fora do núcleo:** as contagens de seek/open/work hoje são
   estáticas globais dentro do decoder. No v0.4 viram um `DecoderMetrics` em L4.
   O núcleo fica limpo; a política mede.
6. **`probe()` barato e cacheado:** evitar reabrir o arquivo para obter
   metadados na importação, com cache por `(path, mtime)`.
7. **Teste de paridade preview↔export automático:** estender o teste de pipeline
   de exportação para comparar o frame decodificado com o frame gerado pelo
   caminho de exportação, num conjunto fixo de arquivos. Vira a garantia de
   paridade.
8. **Interface abstrata de leitura:** `Decoder` implementa uma interface mínima
   de leitura, deixando aberta a possibilidade de trocar a implementação sem
   tocar no Pierrot.

---

## 9. Invariantes do núcleo

Um motor que não precisa ser mexido não é o que tem menos bugs: é o que **não
pode** ter certas classes de bug, porque o desenho as torna impossíveis. As
propriedades abaixo são o que transforma o Laartman de "estável por sorte" em
"estável por construção".

1. **Determinismo.** Para o mesmo `(path, t, maxWidth)`, o frame devolvido é
   sempre idêntico, independentemente do histórico de chamadas. Se o motor usar
   posição interna para acelerar, essa posição é **cache**, não verdade — e o
   teste prova a igualdade (mesmo `t` decodificado por três caminhos: fresco,
   após seek para a frente, após seek para trás → pixel-idêntico).
2. **Encapsulamento total.** Nenhum estado mutável escapa da sessão. O header
   público não contém `acquire`, `release`, `busy` nem qualquer noção de posse;
   o CI quebra se alguém reintroduzir.
3. **Falha visível.** Erro é tipo, não silêncio: `frame()` devolve um resultado
   com status (`Ok | Pending | Error | Eof | Cancelled`). Nenhuma condição vira
   "imagem vazia" ambígua.
4. **Caminho quente previsível.** Decodificar uma posição vizinha não aloca de
   forma imprevisível, não trava em mutex global e não faz syscall. O perfil de
   latência é plano (p99 próximo do p50), sem picos.
5. **Cancelamento contratado.** Todo `frame` aceita cancelamento por padrão, e o
   motor garante que um cancel não deixa estado sujo: o próximo pedido funciona.
6. **Idempotência.** `close()` duas vezes não quebra; `open` num arquivo já
   aberto não reabre; `frame` após `close` devolve erro, nunca quebra.
7. **Orçamento de memória.** O consumo do motor é O(orçamento), não O(tempo de
   uso). Arquivos longos não degradam a latência ao longo do tempo.
8. **Hardware testável.** O device de aceleração é injetável, permitindo testar
   o caminho de fallback sem GPU.

Relação entre classe de falha e invariante que a elimina:

| Classe de falha | Invariante |
|---|---|
| "frame errado" | Determinismo (1) |
| "não se sustenta" | Encapsulamento, sem posse (2) |
| "engasgo aleatório" | Caminho quente previsível (4) |
| "quebra às vezes" | Idempotência + cancelamento + falha visível (3, 5, 6) |
| "piora com o tempo" | Orçamento de memória (7) |
| "só funciona na máquina certa" | Hardware testável (8) |

Estes invariantes têm prioridade sobre qualquer otimização. Otimizar um motor
imprevisível apenas o torna imprevisível e mais rápido.

---

## 10. Dependências candidatas (compatíveis com GPL)

Critério: uma dependência só entra se resolve algo que **a camada precisa** —
não por conveniência. No núcleo (L0–L3), cada dependência é um ponto versionado
e uma superfície de bug, então o número é mantido mínimo. Onde a biblioteca é
política, ela vive em L4–L6.

### No núcleo (decodificação)

| Biblioteca | Licença | Resultado |
|---|---|---|
| **dav1d** | BSD-2 | Decode de AV1 substancialmente mais rápido. Entra quando o AV1 aparecer nos projetos. |
| **libraw** | LGPL-2.1 / CDDL | Decode de RAW de câmera (CR2/NEF/ARW). Fecha a lacuna de formatos RAW. |
| **libjpeg-turbo** | BSD / IJG | Decode de JPEG mais rápido (stills, miniaturas, importação). |
| **libavif** | BSD | Decode de AVIF, quando houver fontes nesse formato. |
| **zita-resampler** | GPL-2+ | Reamostragem de áudio de alta qualidade. |

### Na política (L4–L6)

| Biblioteca | Licença | Resultado |
|---|---|---|
| **Abseil** | Apache-2.0 | Fornece `StatusOr` (o modelo pronto para o erro tipado do invariante 3) e tabelas hash planas O(1) para o `SourcePool`. |
| **libebur128** | MIT | Medição de loudness (LUFS / EBU R128) para o áudio. |
| **mimalloc** | MIT | Alocador com menos fragmentação e picos menores no caminho quente de imagens. |
| **Tracy** | BSD-3 | Perfilador de frames em tempo real: mostra onde o motor engasga, em vez de adivinhar. |
| **DPF** | ISC | Host de plugins de áudio (VST/LV2) sem licença restritiva. |
| **miniaudio** | MIT / domínio público | I/O e mixagem de áudio leves. |

### Critério de rejeição

Não entram: bibliotecas sob **AGPL** ou licenças comerciais que contaminem o
GPL-3 do projeto (ex.: frameworks de plugins com licenciamento dual
AGPL/comercial), nem bibliotecas que tragam um **framework de controle** próprio
e engulam o desenho do núcleo (grafo de nós, modelo de propriedades string).

### Ordem de adoção

1. **Abseil** — potencializa o erro tipado e o `SourcePool`; entra junto com a
   reestruturação.
2. **mimalloc** — ganho de previsibilidade sem alterar lógica.
3. **Tracy** — instrumentação para medir antes/depois.
4. **dav1d / libraw / libjpeg-turbo** — conforme a demanda real de formatos.
5. **libebur128 / DPF / miniaudio** — quando a frente de áudio entrar.

Nenhuma biblioteca substitui os invariantes da seção 9. Elas apenas os
potencializam **depois** que o desenho está correto.

---

## 11. Riscos e mitigações

| Risco | Mitigação |
|---|---|
| Regressão de paridade no split de L0/L1 | `tst_decoder_api` + frames de referência antes/depois |
| Dois contextos de demux = custo de abertura dobrado | medir; mesmo arquivo, cache de probe |
| Concorrência na thread de pacotes | `tst_concurrency` + TSan no CI |
| Fase 3 quebra o loop de GIF | teste de referência do GIF |
| Lookahead em L4 altera o timing | comparar latência p99 antes/depois |

---

## 12. Cronograma sugerido

| Bloco | Fases | Foco |
|---|---|---|
| Semana 1 | 0 + 1 | Conserto urgente das falhas de pool |
| Semana 2 | 2 + 6 | Pipeline e cache na política; núcleo enxuga |
| Semana 3–4 | 3 + 4 | Demux separado + leitura à frente + VFR/DTS |
| Semana 5 | 5 + 7 + 8 | Ocultação de erro, limpeza, aceitação |
| Bônus | 8.x | Índice de busca, orçamento de memória, erro tipado |

---

## 13. Primeiro passo concreto

A Fase 0 + 1 é onde o valor aparece imediatamente e o risco é mínimo:

1. `Decoder.h` — fachada fina (não altera comportamento; estabelece a API).
2. `SourcePool` — derivado do pool atual, **removido o `busy`**, `get()`
   sempre devolve um decoder.
3. Migrar `FrameWorker` e `BgPrefetchWorker` para o `SourcePool`.
4. Rodar a sessão de referência (150 s) → esperado: **0 falhas**.

---

## 14. Resumo

O Laartman v0.4 é um motor de decodificação **imutável**: decodifica um frame
num tempo absoluto e não coordena nada além disso. Pool, posse, prefetch,
lookahead e escolha de qualidade passam a viver no Pierrot, onde são política
ajustável sem tocar no núcleo. O resultado é um núcleo pequeno, estável e
congelável — e uma classe inteira de falhas que deixa de existir por desenho.
