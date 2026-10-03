# Plano — Texto e fontes estilo After Effects

Documento de planejamento (2026-10-02). Status: **plano**, não implementado.
Detalhe no `ROADMAP.md` (seção “Texto e fontes”).

## Objetivo

Painel **Character** + **Text Animator** do After Effects: fontes ricas,
tracking/leading, contorno/sombra, animação de texto (typewriter, slide…),
com paridade preview ↔ export.

## Estado atual

- `TextStyle`: fonte, tamanho, bold, fill, outline, fundo, x/y, align
- UI: dialog estático, sem preview ao vivo, sem animação
- Animação: só do **clipe** (opacity/transform)
- Fontes: só do sistema; export resolve TTF no disco
- TextResource (cópia unificada) ✅

## Fases

| Fase | Entrega | Versão |
|---|---|---|
| 0 | Character panel: tracking, leading, baseline, italic, weight, drop shadow + preview ao vivo | 0.8 |
| 1 | Text Animator: typewriter, fade words/chars, slide + UI + export | 0.8/0.9 |
| 2 | Fontes embutidas, styles por família, justify, polish | 0.9 |

## Fora de escopo

MOGRT, STT/legendas, texto 3D, edição por caractere no preview.
