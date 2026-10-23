# Relatório — Arquitetura de Motion Graphics do Pierrot

Data: 2026-10-22
Deriva de: `Arquivos/Relatorios/Relatorio-Arquitetura-Motion-Graphics-Laartman-v4.1.md`
Status: **proposta** (nenhum código alterado por este documento)
Escopo: adaptar a proposta de motor de Motion Graphics/VFX à arquitetura real do
Pierrot, definir o modelo de dados, os pontos de injeção, as fases de
implementação e os critérios de teste.

---

## 1. Decisão executiva

O relatório v4.1 está correto nos pilares (camadas, keyframes, vetores,
mídia), mas não pode ser aplicado literalmente. O Pierrot já tem uma base
própria que deve ser **estendida**, não substituída:

| Tema | Decisão |
|---|---|
| UI | Continua **Qt Widgets**. Dear ImGui não entra. |
| Render 2D | Continua **QPainter/QImage** no `MesaRenderer`. Skia/Cairo ficam para um backend futuro, atrás de interface. |
| Keyframes | Reutilizar `Keyframe`/`kfValue` (`src/colombina/models/Project.h:95-230`). Não criar um segundo motor de curvas. |
| Codecs | **Laartman permanece congelado** como camada de decode (`src/laartman/README.md`). Motion Graphics não mora nele. |
| Efeitos | Reutilizar hosts **OFX** e **frei0r** existentes. Não incorporar Natron. |
| Novo módulo | `src/colombina/motion/` para modelo animável genérico, shapes, hierarquia e avaliação. |
| Primeiro MVP | **Shape layers** na Mesa + serialização + render QPainter + testes. |
| Dependências novas | Nenhuma na fase 1. Skia/OpenCV só depois de necessidade medida. |

O objetivo não é “virar After Effects”, e sim fechar os buracos que hoje
impedem composições de motion graphics no Pierrot: formas vetoriais
animáveis, hierarquia de camadas, propriedades genéricas e animação de texto.

---

## 2. Auditoria do relatório v4.1

### 2.1 Bibliotecas

| Recomendação v4.1 | Realidade do Pierrot | Decisão |
|---|---|---|
| Skia como espinha dorsal | `MesaRenderer` usa QPainter/QImage e já entrega 2D, 3D leve, câmera, blend e motion blur (`src/colombina/render/MesaRenderer.cpp:871`). | **Adiar**. QPainter cobre o MVP. Criar `RenderBackend` só quando houver gargalo medido. |
| Cairo como alternativa CPU | QPainter já é o backend CPU. | **Não adotar**. Duplicaria rasterização sem ganho. |
| Natron/OFX | Já existe host OFX (`src/colombina/ofx/`) e frei0r (`src/colombina/frei0r/`). | **Reutilizar**. Natron serve como referência de grafo, não como dependência. |
| OpenCV para matting/tracking | Chroma key e máscaras já existem; tracking ainda não é requisito. | **Adiar**. Só entra quando houver um caso de uso concreto. |
| FFmpeg | Já é o coração via Laartman. | **Manter**. Nenhuma mudança de arquitetura. |
| Dear ImGui | UI inteira é Qt Widgets. | **Rejeitar**. Misturar dois toolkits aumentaria build, temas, i18n e acessibilidade. |
| Qt GUI/QML | Qt Widgets já é o toolkit. | **Manter**. |

### 2.2 Keyframes e curvas Bézier

O relatório descreve o básico (localizar A/B, normalizar, Bézier cúbico). O
Pierrot já tem mais que isso:

- `KfLinear`, `KfStep`, `KfSmooth` (Catmull-Rom/Hermite), `KfBezier` manual,
  `KfElastic`, `KfBounce` e `KfBack` (`Project.h:95-230`).
- Handles independentes de entrada/saída e resolução paramétrica por Newton.
- Editor gráfico com ferramentas Select/Add/Curve
  (`src/ui/motion/GraphEditorWidget.h:34`).
- Presets de animação (`src/ui/motion/AnimPresetDock.h`).

O buraco não é a interpolação: é a **falta de propriedades animáveis
genéricas**. Hoje cada propriedade é um campo `kf*` fixo e o Graph Editor tem
um `enum GraphProp` fechado (`GraphEditorWidget.h:34-61`). Shape layers e
text animators não cabem nesse modelo sem campos novos para cada coisa.

### 2.3 Composição

O relatório fala em “DAG”. Para edição estilo AE, a estrutura correta é uma
**árvore de camadas com pré-composições**, não um DAG arbitrário:

- Mesa atual: `MesaComposition::trackIds` é uma lista plana
  (`Project.h:274-286`).
- Cada camada é uma `Track` com transform, opacidade, blend e 3D
  (`Project.h:856-897`).
- Não existe `parentId`, não existe precomp aninhada e não existe hierarquia
  de transform.

Portanto: implementar **tree + precomp**, com detecção de ciclo. DAG fica
fora de escopo.

### 2.4 O que já existe e deve ser preservado

| Área | Implementação atual |
|---|---|
| Coleção de keyframes | `Project.h:95-230`, testes em `tests/tst_keyframes.cpp` |
| Mesa (canvas, câmera, 3D, motion blur) | `Project.h:274-342`, `MesaRenderer.cpp` |
| Camadas com transform/blend | `Project.h:856-897`, `MesaRenderer::paintStack` |
| Máscaras animáveis | `Project.h:344-371` |
| Texto/títulos | `TextStyle` (`Project.h:250`), `textrender` (`src/colombina/render/TextRender.h`) |
| OFX/frei0r | `src/colombina/ofx/`, `src/colombina/frei0r/` |
| Cache de composição | `Project::revision` (`Project.h:945`) + cache do `MesaRenderer` |
| Serialização versionada | `schemaVersion = 2` (`Project.h:1130`), `Project::toJson/fromJson` |

---

## 3. Lacunas reais

1. **Shape layers**: não há camada de forma vetorial (retângulo, elipse,
   polígono/estrela, caminho Bézier) com fill/stroke independentes.
2. **Animação de caminho**: máscaras têm keyframes de centro/raio/rotação,
   mas não há animação de vértices, trim path ou offset.
3. **Hierarquia**: não existe parent/child entre camadas nem herança de
   transform.
4. **Precomp**: a Mesa não é uma sequência aninhável; também falta um “clipe
   Mesa” na timeline.
5. **Propriedades genéricas**: cada novo parâmetro exige campo `kf*`, código
   no Graph Editor, serialização e undo.
6. **Text animators**: texto é um bloco único; não há seletor por
   caractere/palavra/linha nem propriedades animáveis por range.
7. **Render backend**: tudo é QPainter/CPU; não há interface para trocar o
   rasterizador.
8. **Presets de motion graphics**: os 48 presets atuais animam transform de
   clipe, não formas/texto.

---

## 4. Arquitetura alvo

```
┌──────────────────────────────────────────────────────────────────┐
│ UI — Qt Widgets                                                   │
│ TimelineWidget · MesaWidget · GraphEditorWidget · AnimPresetDock  │
└───────────────────────────────┬──────────────────────────────────┘
                                │ edita modelo / pede render
┌───────────────────────────────▼──────────────────────────────────┐
│ colombina/motion  (NOVO)                                          │
│ LayerTree · AnimatableProperty · ShapeLayer · TextAnimator        │
│ MotionEvaluator (puro, const, thread-safe)                        │
└───────────────────────────────┬──────────────────────────────────┘
                                │ lê modelo
┌───────────────────────────────▼──────────────────────────────────┐
│ colombina/models                                                  │
│ Project · MesaComposition · Track · Clip · Mask · Keyframe        │
└───────────────────────────────┬──────────────────────────────────┘
                                │ render
┌───────────────────────────────▼──────────────────────────────────┐
│ colombina/render                                                  │
│ MesaRenderer (QPainter hoje) · RenderBackend (interface futura)   │
└───────────────────────────────┬──────────────────────────────────┘
                                │ pixels
┌───────────────────────────────▼──────────────────────────────────┐
│ colombina/export + colombina/ffmpeg + laartman/Decoder            │
│ ProjectExporter · LainkaRenderer · encode/decode                  │
└──────────────────────────────────────────────────────────────────┘
```

Regras:

- `colombina/motion/` não inclui `src/ui/` nem `src/laartman/`.
- A avaliação de propriedades é **pura** (`const`), sem alocação por frame no
  caminho quente e sem estado global.
- `MesaRenderer` continua sendo o único lugar que conhece QPainter.
- Preview e export continuam usando o mesmo renderer (paridade).
- O cache de composição continua invalidando por `Project::revision`.

---

## 5. Modelo de dados proposto

### 5.1 Propriedade animável genérica

Introduzir em `src/colombina/motion/`:

```cpp
template <typename T>
struct Animatable {
    T base{};
    QVector<Keyframe> keys;   // reutiliza o Keyframe existente
    T at(double t) const;     // usa kfValue para escalares
};
```

Para o MVP, apenas escalares precisam de interpolação genérica. Vetores,
cores e caminhos podem ser decompostos em escalares ou receber overloads
específicos depois.

**Estratégia incremental obrigatória**: não migrar os campos `kf*` existentes
de uma vez. Criar adaptadores:

```cpp
Animatable<double> opacityAdapter(Track& layer); // lê/escreve kfMesaOpacity
```

Assim o Graph Editor pode ganhar uma lista dinâmica de propriedades sem
quebrar o schema atual. A migração dos campos antigos fica para quando o
modelo novo estiver estável.

### 5.2 Shape layer

Adicionar ao modelo uma camada de forma **sem depender de mídia**:

```cpp
enum class ShapeKind { Rect, Ellipse, Path };

struct ShapeStyle {
    QColor fill{255, 255, 255};
    QColor stroke{0, 0, 0, 0};
    double strokeWidth = 0.0;
    Qt::PenJoinStyle join = Qt::MiterJoin;
    Qt::PenCapStyle cap = Qt::FlatCap;
};

struct ShapeItem {
    ShapeKind kind = ShapeKind::Rect;
    QPainterPath path;          // para Path; para Rect/Ellipse, geometria paramétrica
    ShapeStyle style;
    Animatable<double> trimStart; // 0..1
    Animatable<double> trimEnd;   // 0..1
    Animatable<double> offset;    // rotação/deslocamento do trim
};

struct ShapeLayer {
    QVector<ShapeItem> shapes;
    bool enabled = true;
};
```

Ponto de injeção no modelo: `Track::shape` + `Track::hasShape()`, análogo a
`Track::meshPath` (`Project.h:868`). Serialização em `trackToJson`/
`trackFromJson` com defaults retrocompatíveis; subir `kSchemaVersion` para 3.

Render: novo caminho em `MesaRenderer` antes/depois de `drawTrackLayer`:

- `drawShapeLayer(QPainter&, const Track&, double relTime)`
- aplica o mesmo transform da camada (`prepareLayer`/`drawTrackImage`)
- `QPainterPath::toSubpathPolygons`/`pointAtPercent` para trim
- fill/stroke com as cores do modelo

UI mínima:

- `MesaCanvasOps.cpp:336` já tem “Nova camada”; adicionar “Forma”.
- Seleção e edição de geometria no `MesaWidget`.
- `GraphEditorWidget` passa a listar `trimStart`/`trimEnd`/`posiçao` da forma.

### 5.3 Hierarquia e precomp

Adicionar a `Track`:

```cpp
QString parentId;      // vazio = raiz da Mesa
QString precompMesaId; // vazio = camada normal; senão, renderiza outra Mesa
```

- Resolver transform em ordem pai → filho, com detecção de ciclo.
- `MesaRenderer::paintStack` passa a percorrer a árvore, não só
  `mesa.trackIds`.
- Precomp: renderizar a Mesa filha num `QImage` e desenhá-la como camada,
  respeitando transform/opacidade/blend.
- Cache: a chave da precomp precisa incluir a revisão e o tempo da Mesa
  filha.
- Fora de escopo no MVP: DAG, expressões, parenting de propriedades.

### 5.4 Text animators

Adicionar em `src/colombina/motion/`:

```cpp
enum class TextSelector { All, Characters, Words, Lines };

struct TextAnimator {
    TextSelector selector = TextSelector::Characters;
    Animatable<double> start;   // 0..1 do range
    Animatable<double> end;     // 0..1
    Animatable<double> offset;  // deslocamento no tempo
    Animatable<double> positionX;
    Animatable<double> positionY;
    Animatable<double> scale;
    Animatable<double> rotation;
    Animatable<double> opacity;
};
```

O layout já existe em `textrender::layout()`. O renderer passa a desenhar
cada glifo/palavra/linha com o transform acumulado do animator. O texto
continua sendo `Clip::isText` (`Project.h:388`) no MVP; a diferença é que o
desenho deixa de ser um bloco único.

### 5.5 Serialização e undo

- Subir `kSchemaVersion` para 3 quando o primeiro campo novo for persistido.
- Campos ausentes = defaults (shape vazio, parent vazio, precomp vazia,
  animators vazios).
- Nenhum campo existente muda de significado.
- Toda edição continua passando por `Project::touch()` para invalidar cache.
- Undo/redo: o modelo novo precisa entrar no mesmo mecanismo dos campos
  atuais; não criar um segundo snapshot.

---

## 6. Plano por fases

### Fase 0 — Documento e decisões (esta entrega)

**Entrega**: arquitetura adaptada, não-goals, pontos de injeção, fases.
**Critério de saída**: revisão aprovada pelo mantenedor.

### Fase 1 — Núcleo de propriedades animáveis

**Objetivo**: provar o modelo genérico sem tocar no schema nem no render.

**Arquivos previstos**:

- `src/colombina/motion/AnimatableProperty.h`
- `src/colombina/motion/MotionEvaluator.h/.cpp`
- `tests/tst_motion_properties.cpp`

**Testes**:

- curvas lineares, step, smooth, bezier, elastic, bounce e back via
  `Animatable<double>`;
- valores antes/depois do range;
- chave vazia retorna base;
- avaliação sem alocação no caminho quente (contador de alocações no teste).

**Critério de saída**: teste passa; nenhum campo existente é removido.

### Fase 2 — Shape layers (MVP)

**Objetivo**: formas geométricas sólidas (estilo shape layer do AE), animáveis
pelas curvas, editadas por um segundo viewer (**Dottore**) dockável ao lado do
Preview — sem passar pela Mesa.

> **Direção revisada (Dottore).** A primeira tentativa plugou as formas na Mesa
> (`Track::shape`), mas a Mesa é o compositor de multicamadas: obrigar o motion
> a viver lá é pouco prático. O Dottore reaproveita o gerador de mídia que já
> existe: `generator == "shape"` em `MediaItem` é renderizado por
> `generatorFrame()` — a MESMA função usada por Preview, Pantalone, Timeline e
> exportação — logo preview e export batem de graça, sem caminho duplicado e
> sem depender da Mesa. A forma entra na timeline como qualquer clipe sólido e
> é animada pelas keyframes normais (transform/opacidade) + Editor de Curvas.
>
> Implementado: campos `shape*` em `MediaItem` + serialização; caso `"shape"`
> em `generatorFrame()` (fill/stroke/roundness/elipse); criação "Forma →
> Retângulo/Elipse" na Central de Mídias; `DottoreWidget` (viewer + gizmo de
> mover/escalar/girar + painel de fill/stroke/dimensões) dockado à direita do
> Preview; `tests/tst_shape_generator.cpp` + round-trip em `tst_serialization`.

**Arquivos**:

- `src/colombina/models/Project.h` (`MediaItem::shape*`), `Project.cpp`
  (serialização condicional quando `generator == "shape"`)
- `src/colombina/generators.h` (caso `"shape"`)
- `src/ui/panels/MediaPoolWidget.cpp` (criação)
- `src/ui/motion/DottoreWidget.h/.cpp` (viewer + gizmo + propriedades)
- `src/MainWindow.cpp` (dock ao lado do Preview)
- `tests/tst_shape_generator.cpp`, `tests/tst_serialization.cpp`

**Testes**:

- retângulo/elipse renderizados em `QImage` offscreen com amostragem de pixels;
- fill e stroke independentes; cantos arredondados recortam o canto;
- serialização round-trip `.Blanc` preserva os campos de forma;
- projeto antigo (sem os campos) carrega com os defaults (retângulo 50%).

**Critério de saída**: forma aparece no preview e no export com o mesmo
resultado; testes de render e serialização passam.

### Fase 3 — Hierarquia e precomp

**Objetivo**: parent/child de camadas e Mesa aninhada.

**Arquivos previstos**:

- `Project.h/.cpp` (`parentId`, `precompMesaId`)
- `src/colombina/motion/LayerTree.h/.cpp` (ordem, ciclo, transform mundial)
- `MesaRenderer.cpp` (paint recursivo + cache)
- `MesaWidget`/`TimelineWidget` (UI de parenting)
- `tests/tst_motion_layers.cpp`

**Testes**:

- filho herda transform do pai;
- ciclo é rejeitado;
- precomp renderiza e respeita opacidade/blend;
- cache invalida ao editar a Mesa filha;
- preview ↔ export com a mesma árvore.

**Critério de saída**: uma composição com 2 níveis de precomp renderiza
corretamente e é serializada sem perda.

### Fase 4 — Text animators e presets

**Objetivo**: animação por caractere/palavra/linha e presets de motion
graphics.

**Arquivos previstos**:

- `src/colombina/motion/TextAnimator.h`
- integração com `textrender` e `MesaRenderer`
- `src/ui/motion/AnimPresetDock.cpp` (presets de texto/forma)
- `tests/tst_motion_text.cpp`

**Testes**:

- seletor por caractere/palavra/linha;
- range start/end e offset;
- opacidade/posição/escala por glifo;
- preset aplicado a um clipe de texto gera keyframes esperados;
- export parity.

**Critério de saída**: texto animado por range sem quebrar texto estático.

### Fase 5 — Performance e backend (opcional)

**Objetivo**: medir e, se necessário, trocar o rasterizador.

- Criar `RenderBackend` com implementação atual QPainter.
- Perfilar `MesaRenderer` com 10+ camadas e shapes.
- Só então avaliar Skia/GPU.
- OpenCV apenas para tracking/matting, com caso de uso aprovado.

**Critério de saída**: decisão documentada com números, não por moda.

---

## 7. Estratégia de testes

| Teste | Escopo |
|---|---|
| `tst_keyframes` (existente) | regressão da interpolação atual |
| `tst_motion_properties` (novo) | `Animatable<T>` e avaliador |
| `tst_motion_shape` (novo) | render offscreen de formas + serialização |
| `tst_motion_layers` (novo) | parenting, ciclo, precomp |
| `tst_motion_text` (novo) | seletores e animators |
| `tst_serialization` (existente) | schema 3, defaults e round-trip |
| `tst_exporter` / `tst_export_pipeline` | paridade preview ↔ export |

Regras:

- Testes de render usam `QImage` offscreen e amostragem de pixels; evitar
  golden images grandes no início.
- Nenhum teste depende de GPU, áudio ou display.
- Toda fase entra no `CMakeLists.txt` como alvo próprio e no `ctest`.

---

## 8. Riscos e não-objetivos

### Riscos

1. **Explosão de schema**: adicionar campos sem migração versionada quebra
   projetos. Mitigação: defaults + `schemaVersion` + teste de round-trip.
2. **Duplicação de keyframes**: criar um segundo motor de curvas. Mitigação:
   reutilizar `Keyframe`/`kfValue`; adaptadores para campos antigos.
3. **Thread safety**: `MesaRenderer` é usado por UI e worker. Mitigação:
   avaliador puro/const; nada de estado global.
4. **Performance QPainter**: shapes complexos e muitas camadas. Mitigação:
   perfil antes de trocar backend; cache por revisão.
5. **Escopo**: tentar fazer DAG, expressões, GPU e tracking no mesmo ciclo.
   Mitigação: fases com critério de saída.

### Não-objetivos (nesta arquitetura)

- Dear ImGui.
- Natron como dependência.
- Skia/Cairo no MVP.
- OpenCV no MVP.
- DAG arbitrário.
- Expressões estilo AE.
- GPU compositor.
- Substituir `MesaRenderer` ou `Laartman`.

---

## 9. Próximo passo recomendado

Começar pela **Fase 1** (núcleo de propriedades animáveis), porque:

- não altera schema;
- não mexe no render;
- cria a base para shapes e text animators;
- é testável de forma pura e rápida;
- reduz o risco de retrabalho nas fases seguintes.

Sequência sugerida de PRs:

1. `motion/AnimatableProperty.h` + `MotionEvaluator` + testes.
2. `Track::shape` + serialização + `ShapeRenderer` + testes.
3. `parentId`/`precompMesaId` + árvore + testes.
4. `TextAnimator` + presets + testes.
5. Perfil de performance e decisão sobre backend.

Cada PR deve manter preview e export usando o mesmo caminho de render.
