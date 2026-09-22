# Plano de atualização do núcleo (kernel `src/colombina`)

Plano de trabalho para a próxima passada de melhoria no núcleo do Pierrot.
Baseado no relatório `Arquivos/Relatorios/KERNEL.md` e no backlog registrado no
ROADMAP.md (seção 4.5). Ordenado por prioridade de impacto/recompensa.
Criado em 2026-09-22.

## Prêmios da fila

- **P0 — Versionamento de schema `.Blanc`** (`format_version` no JSON)
  - Objetivo: abrir projeto de versão futura avisa em vez de desserializar errado.
  - Arquivos: `src/colombina/models/Project.cpp` (toJson/fromJson).
  - Aceite: `.Blanc` sem `format_version` abre como hoje; com versão maior que a
    suportada, abre com aviso explícito e sem mutar o arquivo.
- **P1 — Indexação O(1) por id**
  - Objetivo: eliminar varreduras lineares chamadas por quadro no render.
  - Arquivos: `src/colombina/models/Project.h` (`findMedia`, `findMesa`,
    `findMesaForTrack`, `findGroup`), `MesaRenderer`, `PreviewWidget`.
  - Aceite: ids indexados em `QHash`; nenhuma mudança de FPS em projeto grande;
    ctest verde.
- **P2 — Load `.Blanc` assíncrono e defensivo**
  - Objetivo: `fromJson` fora da UI thread (o save já é async) + parser que
    nunca crasha com entradas malformadas.
  - Arquivos: `src/colombina/models/Project.cpp/.h`, `MainWindow` (open),
    novo teste de fuzz do parser.
  - Aceite: abrir projeto 4K não trava a UI; corpus de JSONs malformados não
    crasha nem trava.

## Fila de performance (a partir do relatório KERNEL.md)

- **P3 — GPU na composição** — `MesaRenderer` 100% CPU `QPainter`. Maior gap
  vs. Vegas (OpenCL/CUDA). Maior investimento; melhor recompensa em 4K multi.
- **P4 — Smart render** — `stream copy` nos trechos intactos do export
  (depende de cortes simples no `ProjectExporter`).

## Higiene (contínua)

- **P5 — Sanitizers (ASan/UBSan) no CI** + `-Wall -Wextra` no default.
- **P6 — Cobertura do kernel >50%** (gcov/lcov no CI) — meta para aceitar novo
  teste/feature.

## Regra

Nenhum item entra na linha de frente enquanto o "núcleo de foco" do ROADMAP
(confiança: CI, anti-crash, métricas) estiver aberto — exceto se o usuário
decidir retirar um dos itens acima antes.