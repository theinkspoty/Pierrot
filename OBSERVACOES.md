# Observações

## Interface
- [x] Toolbar principal (Novo/Abrir/Salvar/Desfazer/Refazer/Reproduzir/Exportar) removida — ações agora só nos menus; "Importar mídia" ganhou entrada no menu Arquivo (Ctrl+I segue ativo, além do botão Adicionar no painel Mídia).

## Planejamento: Novo Editor de Curvas (estilo Premiere)

- [ ] Reconstruir o painel "Editor de Curvas" (GraphEditorWidget) inspirado no Adobe Premiere Pro:
  - [ ] Layout com painel de propriedades à esquerda (lista de curvas: posição, escala, rotação, opacidade, volume, etc.) e área de gráfico à direita.
  - [ ] Curvas desenhadas como splines suaves (interpolação de Bezier) ao invés de poligonais retas entre keyframes.
  - [ ] Handles de alça (tangentes) editáveis para cada keyframe, permitindo controle de entrada/saída.
  - [ ] Suporte a diferentes tipos de interpolação: Linear, Bezier, Auto Bezier, Hold.
  - [ ] Zoom e pan independentes nos eixos X (tempo) e Y (valor).
  - [ ] Grid adaptativo com marcações de tempo e valores.
  - [ ] Seleção múltipla de keyframes (box marquee e clique + Shift).
  - [ ] Move, copy/paste e delete de keyframes via teclado e mouse.
  - [ ] Sincronização com o playhead: destaca keyframes vizinhos e permite scrub visual.
  - [ ] Indicação visual de curvas fixadas (pin) e modo de edição relativa/absoluta.
  - [ ] Atalhos de teclado consistentes com NLEs (ex.: K/L para velocidade de reprodução, Shift+drag para snap).
  - [ ] Persistência dos tipos de interpolação e handles no modelo de dados (Project/Clip).
  - [ ] Atualização em tempo real do preview quando curvas são alteradas.
  - [ ] Performance: cache de spline calculada e redraw incremental apenas da área modificada.

