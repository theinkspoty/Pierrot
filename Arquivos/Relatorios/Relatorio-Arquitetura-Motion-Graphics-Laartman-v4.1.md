# Relatório — Arquitetura e Bibliotecas Open-Source para o Laartman v4.1

Data: 2026-10-20
Escopo: estruturar a arquitetura técnica e identificar as bibliotecas de código
aberto (GPL, LGPL e licenças compatíveis) necessárias para o desenvolvimento de
um motor de Motion Graphics e Efeitos Visuais (VFX) em C++, semelhante ao Adobe
After Effects, como evolução do Laartman no Pierrot.
Status: **proposta** (nenhum código alterado por este documento).

---

## 1. Introdução

Softwares no estilo After Effects são caracterizados por quatro pilares
primários:

1. Composição por Camadas (Layer-Based Composition) e Hierarquia de Nós.
2. Motor de Animação Paramétrica baseado em Linha de Tempo e Keyframes.
3. Renderização Vetorial 2D e Sistema de Manipulação de Matrizes.
4. Processamento e Codificação/Decodificação de Mídia (Vídeo/Áudio).

---

## 2. Bibliotecas recomendadas

### A. Motor de renderização vetorial e rasterização 2D

#### 1. Skia Graphics Engine

- **Licença:** BSD-style (compatível com GPL / uso em softwares livres e proprietários)
- **Linguagem:** C++
- **Função no programa:** espinha dorsal gráfica do sistema. O Skia lida com
  desenho de formas vetoriais, operações booleanas entre caminhos (path
  operations), gradientes, modos de mesclagem (blend modes), máscaras e clipping.
- **Recursos principais:**
  - Aceleração de hardware nativa via OpenGL, Vulkan e Metal.
  - Renderização precisa de texto (via HarfBuzz/FreeType embutido).
  - Suporte completo a curvas de Bézier cúbicas e quadráticas.
  - Suporte nativo a matrizes de transformação 3D/2D (SkMatrix, SkM44).

#### 2. Cairo Graphics

- **Licença:** LGPL v2.1 / MPL 1.1
- **Linguagem:** C
- **Função no programa:** alternativa de renderização 2D focada em execução via CPU.
- **Recursos principais:**
  - Excelente estabilidade em ambientes heterogêneos sem GPU dedicada.
  - Saída nativa para buffers de memória de pixels (Image Surfaces).

### B. Composição, processamento de imagem e efeitos (VFX)

#### 1. Natron / OFX (Open Effects Modules)

- **Licença:** GNU GPL v2+ (core do Natron) / BSD (OpenFX Header Standard)
- **Linguagem:** C++
- **Função no programa:** estudo da arquitetura do Grafo de Composição
  (Directed Acyclic Graph — DAG) e gerenciamento do pipeline de efeitos.
- **Recursos principais:**
  - Implementação do padrão industrial OpenFX (OFX), permitindo que o programa
    carregue plugins de efeitos de terceiros.
  - Gerenciamento de espaço de cor usando OpenColorIO (OCIO) para precisão de
    cor float de 32-bit (HDR).
  - Sistema de cache de memória de frames dinâmico.

#### 2. OpenCV (Open Source Computer Vision Library)

- **Licença:** Apache 2.0 (compatível com GPLv3)
- **Linguagem:** C++
- **Função no programa:** operações matemáticas pesadas sobre arrays de pixels,
  matting (remoção de fundo / chroma key), rastreamento de movimento (motion
  tracking) e análise de fluxo óptico (optical flow).

### C. Manipulação, decodificação e codificação de mídia

#### 1. FFmpeg (libavcodec, libavformat, libswscale, libavfilter)

- **Licença:** LGPL v2.1+ / GPL v2+ (dependendo dos módulos ativos na compilação)
- **Linguagem:** C (com wrappers C++ de alto nível)
- **Função no programa:** leitura e escrita do pipeline de vídeo — já é o
  coração atual do Laartman (`src/laartman/`).
- **Recursos principais:**
  - Decodificação assíncrona de arquivos de vídeo (MP4, MOV, ProRes, DNxHD)
    para buffers de memória em tempo real.
  - Codificação do projeto final renderizado para arquivos de saída.
  - Manipulação de canais alfa (transparência) para exportação em ProRes 4444
    ou WebM/VP9.

### D. Interface de usuário (GUI) e canvas interativo

#### 1. Dear ImGui

- **Licença:** MIT (compatível com GPL)
- **Linguagem:** C++
- **Função no programa:** renderização rápida de painéis de ferramentas,
  timeline, inspetor de propriedades e editores de curvas de keyframes. Padrão
  de mercado para ferramentas internas em gamedev e aplicações C++.

#### 2. Qt Framework (módulos Qt GUI e Qt QML)

- **Licença:** LGPL v3 / GPL v3
- **Linguagem:** C++
- **Função no programa:** opção para construção de uma interface de software
  comercial robusta com suporte nativo a drag-and-drop, gerenciamento de
  janelas complexas e multi-monitor. **Já é o toolkit do Pierrot.**

---

## 3. Estrutura da arquitetura

Para implementar um motor funcional, o código em C++ deve ser dividido nos
seguintes módulos lógicos principais:

```
+-----------------------------------------------------------------------------+
|                          INTERFACING & CONTROLS (GUI)                       |
|                   (Dear ImGui / Qt / Timeline UI / Viewport)                |
+-----------------------------------------------------------------------------+
                                      |
                                      v
+-----------------------------------------------------------------------------+
|                            TIMELINE & KEYFRAME ENGINE                       |
| - Avaliação de tempo (Current Frame t)                                      |
| - Interpolação de Curvas de Bézier (Easing Curves)                          |
| - Gestão de propriedades animáveis (Posição, Escala, Opacidade, Efeitos)    |
+-----------------------------------------------------------------------------+
                                      |
                                      v
+-----------------------------------------------------------------------------+
|                             COMPOSITION TREE (DAG)                          |
| - Estrutura hierárquica de Camadas (Layers) e Parent/Child Relationships    |
| - Aplicação de Transformações (Matrizes de Escala, Rotação e Translação)    |
| - Gerenciamento de Máscaras e Blend Modes                                   |
+-----------------------------------------------------------------------------+
                                      |
                                      v
+-----------------------------------------------------------------------------+
|                            RENDER PIPELINE (Skia/GPU)                       |
| - Rasterização do Grafo em Framebuffers                                     |
| - Aplicação de Shaders de Efeitos (Blurs, Glows, Distortions)               |
+-----------------------------------------------------------------------------+
                                      |
                                      v
+-----------------------------------------------------------------------------+
|                         EXPORT & CACHE ENGINE (FFmpeg)                      |
| - Armazenamento de RAM Cache para Playback em Tempo Real                    |
| - Envio de Pixels para o Encoder de Vídeo (H.264/ProRes/etc)                |
+-----------------------------------------------------------------------------+
```

---

## 4. Lógica do motor de keyframes e curvas Bézier (C++)

A alma do After Effects é a capacidade de interpolar propriedades ao longo do
tempo usando curvas de aceleração/desaceleração (Ease In / Ease Out).

Em C++, o cálculo de uma propriedade animada no tempo `t` envolve:

1. Localizar o Keyframe A (Tempo <= t) e Keyframe B (Tempo >= t).
2. Calcular o fator interpolador normalizado:

   ```
   factor = (t - KeyframeA.time) / (KeyframeB.time - KeyframeA.time)
   ```

3. Mapear o `factor` através de uma Curva de Bézier Cúbica com base nas
   tangentes definidas pelo usuário na linha do tempo:

   ```
   P(t) = (1-t)³·P0 + 3(1-t)²·t·P1 + 3(1-t)·t²·P2 + t³·P3
   ```

4. Utilizar o valor resultante para atualizar o parâmetro correspondente
   (ex.: `SkMatrix::preTranslate()` ou parâmetro de shader).

---

## 5. Recomendação de licenciamento para o programa

Se o objetivo é distribuir o programa sob uma licença aberta (Open Source):

- A combinação de código **GPLv3** é ideal, permitindo reutilizar livremente
  módulos do Natron, FFmpeg (compilado sob GPL) e de outras ferramentas abertas.

Se o objetivo é manter o código proprietário/fechado enquanto utiliza
bibliotecas:

- Utilizar a estratégia de vincular bibliotecas sob licença **LGPL** (como Cairo
  e versões LGPL do FFmpeg) como bibliotecas dinâmicas (`.dll` / `.so`) via
  linkagem dinâmica no C++.
- Utilizar bibliotecas com licenças permissivas (**MIT/BSD/Apache 2.0**) para o
  motor de renderização e interface (Skia, Dear ImGui, OpenCV).

---
