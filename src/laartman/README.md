# Laartman — motor de codecs do Pierrot

Dividido em **camadas por estabilidade**: o que muda muito fica fácil (raiz);
o que é congelado fica fundo. A regra é simples:

> Camada de fora **nunca** inclui camada de dentro por atalho — só através da
> camada imediatamente superior.

```
src/laartman/
├── Decoder.h / Decoder.cpp     L3 · fachada PÚBLICA — o único header que o
│                                resto do app deve incluir. É o contrato
│                                congelado do plano v0.4: probe/open/frame/
│                                seekAudio/readPcm. Aqui mexeremos MUITO
│                                (novos formatos, opções, erro tipado).
│
├── core/                        L1+L2 · motor de decode — CONGELADO.
│   ├── FFmpegDecoder.*           Estado da sessão de decode, hold/loop,
│   │                             seek inteligente, VAAPI, VFR, sws.
│   ├── FFmpegDecoderPriv.h       Helpers internos (NUNCA incluir de fora).
│   └── ErrorConceal.h            Classificação Ok/Pending/Eof/Error/Cancelled.
│
└── io/                          L0 · demux/leitura — CONGELADO (mais fundo).
    ├── Demuxer.*                 Abre arquivo, descobre streams, lê pacotes
    │                             (contexto do áudio e das imagens).
    └── PacketReader.*            Thread de leitura à frente do vídeo, buffer
                                  limitado, contexto de demux PRÓPRIO.
```

## Regras das camadas

| Regra | Por quê |
|---|---|
| Consumidores incluem **só** `laartman/Decoder.h` | A superfície pública é pequena e estável; trocar o motor não quebra quem está acima. |
| `core/` e `io/` não se conhecem por atalho | `core` inclui `io` via `laartman/io/…`; `io` não inclui `core`. |
| Nada de `laartman/core/…` fora deste diretório (exceto as exceções abaixo) | É camada congelada: se um consumidor precisa de algo dela, o certo é crescer a fachada. |
| Estado mutável só existe em `core/` (L2), privado | Invariante 2 do relatório v0.4: nada de posse/estado de pipeline no header público. |

## Consumidores (estado em 2026-10-21)

**14 arquivos usam a fachada** `laartman/Decoder.h` (timeline, media pool,
proxy, bench, source monitor, trimmer, conform de áudio, exportadores,
preview…). A fachada estendeu com `frameCacheIndex`, `usesHardware`,
`audioPeaks` + `MediaInfo.audioChannels` — só metadados, sem posse.

**9 exceções justificadas** ainda incluem `laartman/core/FFmpegDecoder.h`:

| Arquivo | Razão |
|---|---|
| `SourcePool`, `FramePipeline`, `MesaRenderer`, `FrameWorker` | O pool (L4) devolve o **motor** por design — infraestrutura de posse vive ACIMA da fachada |
| `MediaCache`, `UiDecode`, `PantaloneWidget`, `PivotWidget` | usam gestão de memória do motor (liberação de buffers pós-decode) que o teste de invariantes **proíbe** na fachada |
| `OfxPluginManager`… (nenhum) | — |

O teste `facadeHeaderExposesNoOwnership` (CI) garante que posse/política
nunca entrem na fachada; crescer a fachada por demanda é o caminho certo —
exatamente como `frameCacheIndex`/`audioPeaks` entraram.

## O que já é política (vive ACIMA, no Pierrot/Colombina)

Pool de decoders (`SourcePool`), prefetch/lookahead (`FramePipeline`), cache
de quadros (`FrameCache`), escolha de proxy/qualidade, métricas — tudo em
`src/colombina/ffmpeg/` (L4–L5). O Laartman só decodifica um frame num tempo.

Relatórios: `Arquivos/Relatorios/Relatorio-Reestruturacao-Laartman-v0.4.md`,
`Relatorio-Estabilidade-Laartman-2026-10-20.md`,
`Relatorio-Correcoes-2026-10-21.md`.
