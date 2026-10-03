# Arquitetura

Documentos que descrevem **como o Pierrot funciona por dentro**: estrutura do
código, responsabilidades dos módulos, pipelines de dados e decisões de
design. São descritivos e de referência — não são auditorias nem listas de
problemas.

Para análises e diagnósticos (performance medida, bugs, comparações de
recursos), ver [`../Relatorios/`](../Relatorios/).

## Documentos

| Documento | Assunto | Atualizado |
|---|---|---|
| [`KERNEL.md`](KERNEL.md) | Núcleo `src/colombina` (models, ffmpeg, render, export, ofx) — LOC por módulo, caches, thread-safety, estado da GPU, comparação com o Vegas Pro | 2026-10-02 (v0.7 alpha) |
| [`Mesa-3D-Plano.md`](Mesa-3D-Plano.md) | Plano Mesa 3D estilo AE Classic 2010–2018 (fases 0–4) | 2026-10-02 |
| [`Texto-Fontes-Plano.md`](Texto-Fontes-Plano.md) | Plano texto/fontes estilo AE (Character + Text Animator) | 2026-10-02 |

## Relatórios (fora de Arquitetura)

Ver também `../Relatorios/`:
- `Relatorio-Bibliotecas-Efeitos-Video.md` — OFX/frei0r (hosts open source)
- `Relatorio-Efeitos-Nativos.md` — inventário nativo vs Premiere + fila P0–P2

## Convenção

- Nomeie `Arquitetura-<Area>.md` ao criar um novo (ex.: `Arquitetura-Timeline.md`).
- Comece com "números" (LOC, arquivos) e depois responsabilidades por módulo —
  o `KERNEL.md` é o modelo de estrutura.
- Registre a versão do projeto no cabeçalho; números de LOC envelhecem rápido.
- Se um achado virar backlog, mova-o para o `ROADMAP.md` (seção 4.5) em vez de
  deixá-lo acumulado aqui.
