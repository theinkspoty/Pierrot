# Features futuras — Pierrot Video Editor

> Ideias e roadmap de funcionalidades para versões futuras.  
> Não são promessas de implementação, apenas referência de direção.

---

## 1. Performance e estabilidade

| Feature | Prioridade | Descrição |
|---------|-----------|-----------|
| Prefetch de vídeo no corte | Alta | Pré-carrega o primeiro frame do próximo clip quando o atual está nos últimos 0.5s (elimina o travamento no corte). |
| Prefetch de áudio no corte | Alta | Abre o decoder de áudio do próximo clip antecipadamente para evitar silêncio na transição. |
| Cache de frames decodificados | Média | Mantém os últimos N frames decodificados em memória para scrub instantâneo sem re-decoder. |
| Decode em background thread dedicada | Média | Thread separada para preview, sem disputa com MediaCache/thumbnails. |
| Medição de frame time | Baixa | Exibe ms por frame no preview para diagnosticar gargalos. |

---

## 2. Edição de vídeo

| Feature | Prioridade | Descrição |
|---------|-----------|-----------|
| Transições entre clips | Alta | Crossfade, fade to black, wipe, slide — aplicáveis no corte entre dois clips. |
| Keyframes de efeitos visuais | Alta | Brightness, contrast, saturation, blur, grayscale com keyframes ao longo do tempo. |
| Chroma key avançado | Média | Ajuste de similarity, smoothness, spill suppression; pré-visualização em tempo real. |
| Máscaras por clip | Média | Máscara de elipse/retângulo/gradiente para ocultar ou destacar partes do vídeo. |
| Estabilização de vídeo | Baixa | Detecção de movimento e aplicação de transformação para estabilizar clipes trepidantes. |
| Slow motion / fast motion com curve | Baixa | Speed ramp com curva de velocidade (não apenas linear). |
| Auto-cut (detecção de cenas) | Baixa | Detecta mudanças de cena automáticas e sugere cortes. |

---

## 3. Edição de áudio

| Feature | Prioridade | Descrição |
|---------|-----------|-----------|
| Edição de waveform | Alta | Corte, cópia e colagem diretamente na waveform da timeline (ferramenta razor visual). |
| Fade in/out por clave | Alta | Fade de áudio por curva (não apenas linear) com keyframes de volume. |
| Noise gate / compressor | Média | Efeitos de áudio adicionais: gate, compressor, reverb simples. |
| Mix de áudio multi-faixa com pan | Média | Controle de pan estéreo por clip e por faixa. |
| Detecção automática de silêncio | Baixa | Identifica trechos silenciosos e sugere cortes. |
| Export de áudio separado | Baixa | Opção de exportar apenas a trilha de áudio mixada (WAV/FLAC). |

---

## 4. Efeitos e composição

| Feature | Prioridade | Descrição |
|---------|-----------|-----------|
| Blend modes reais | Alta | Implementar composição real dos 12 blend modes (não apenas no export, mas no preview). |
| Camadas de efeito (stack) | Alta | Permitir múltiplos efeitos por clip em ordem configurável. |
| Text overlay com estilos | Alta | Fontes customizadas, tamanho, cor, sombra, stroke, animação de texto. |
| Picture-in-picture | Média | Inserir um vídeo dentro de outro com redimensionamento e posicionamento. |
| Split screen | Média | Dividir a tela em 2, 3 ou 4 partes com vídeos diferentes. |
| Green screen / chroma key em tempo real | Média | Pré-visualização do chroma key no preview (atualmente só no export). |

---

## 5. Exportação

| Feature | Prioridade | Descrição |
|---------|-----------|-----------|
| Presets de export | Alta | Perfis pré-configurados (YouTube, Instagram, TikTok, 4K, etc.). |
| Export de sequência de imagens | Média | Exportar frames individuais como PNG/JPEG sequence. |
| Export por região / range | Média | Exportar apenas uma seção da timeline (in/out). |
| Codecs adicionais | Média | H.265/HEVC, AV1, ProRes (depende de ffmpeg build). |
| Export de thumbnail customizada | Baixa | Permitir escolher o frame que será a thumbnail do arquivo exportado. |
| Batch export | Baixa | Exportar múltiplos projetos ou múltiplos ranges de uma vez. |

---

## 6. UI / UX

| Feature | Prioridade | Descrição |
|---------|-----------|-----------|
| Snap inteligente | Alta | Snap a markers, outros clips, playhead — com toggle visual de onde está encaixando. |
| Navegação por atalhos | Alta | Atalhos de teclado para zoom, scroll, split, delete, undo/redo (inspirado em Vegas/Premiere). |
| Mini-map da timeline | Média | Visão geral da timeline com viewport visível, para navegação rápida em projetos longos. |
| Split view (antes/depois) | Média | Comparar dois pontos da timeline lado a lado no preview. |
| Themes / skin | Baixa | Tema escuro/claro, cores customizadas da timeline. |
| Layout de painéis customizável | Baixa | Salvar/restaurar arranjos de docks diferentes para edição vs. exportação. |

---

## 7. Gerenciamento de projeto

| Feature | Prioridade | Descrição |
|---------|-----------|-----------|
| Proxies automáticos | Alta | Criar versões de baixa resolução para scrubbing fluido, trocar para full-res no export. |
| Auto-save configurável | Alta | Intervalo de auto-save customizável, backup de versões anteriores. |
| Histórico de undo/redo ilimitado | Média | Atualmente limitado a 60 snapshots; permitir mais ou usar diff incremental. |
| Import de projetos | Média | Suporte a importar timelines de outros editores (via XML/EDL/AAF se possível). |
| Metadata de mídia | Baixa | Exibir codec, bitrate, corespace, HDR info na pool de mídia. |

---

## 8. Colaboração e arquivo

| Feature | Prioridade | Descrição |
|---------|-----------|-----------|
| Referências de arquivo relativas | Alta | Projetos salvam caminhos relativos para facilitar portabilidade e colaboração. |
| Detecção de arquivos ausentes | Alta | Marcar clips com arquivo ausente, sugerir localizar manualmente. |
| Versionamento de projeto | Baixa | Integração com git para versionar `.ovp` (diff visual de mudanças na timeline). |
| Export de EDL/XML | Baixa | Exportar decisões de edição para trocar com outros NLEs. |

---

## 9. Compatibilidade

| Feature | Prioridade | Descrição |
|---------|-----------|-----------|
| Suporte a mais formatos de entrada | Média | MKV experimental atual; expandir para MOV, AVI, MXF com melhor feedback de codec. |
| Suporte a HDR | Baixa | Detectar e preservar HDR10/Dolby Vision no preview e export (requer Qt + ffmpeg com suporte). |
| Suporte a VR/360 | Baixa | Preview e export de vídeo equiretangular 360°. |
| Suporte a legendas | Baixa | Importar SRT/ASS, exibir e editar legendas na timeline, queimar no export. |

---

## 10. Experimentais

| Feature | Prioridade | Descrição |
|---------|-----------|-----------|
| AI-powered features | Baixa | Auto-caption, detecção de objetos, remoção de fundo automática (requer modelo ML). |
| GPU acceleration | Baixa | Decode via CUDA/NVDEC / VAAPI / VideoToolbox para preview e export mais rápidos. |
| Plugin system | Baixa | Arquitetura de plugins para efeitos customizados em C++ ou Lua/Python. |

---

## 11. Workflow e produtividade

| Feature | Prioridade | Descrição |
|---------|-----------|-----------|
| Templates de projeto | Alta | Salvar configurações de projeto (resolução, fps, faixas padrão) como template para reuso. |
| Clipes "inteligentes" | Média | Clipes que se adaptam automaticamente à duração do gap (auto-fill com stretch/loop). |
| Histórico visual de undo | Média | Painel visual mostrando estados anteriores da timeline (thumbnails) para undo/redo rápido. |
| Copy/paste de atributos | Média | Copiar efeitos/keyframes de um clip e colar em outro sem copiar o clip inteiro. |
| Marcadores de capitulo | Média | Marcadores com nome/cor que viram capítulos no export (ex: MP4 chapters). |
| Timeline "snap" visual | Média | Linhas guia que aparecem durante o drag mostrando onde o clip vai encaixar. |

---

## 12. Áudio avançado

| Feature | Prioridade | Descrição |
|---------|-----------|-----------|
| Visualização de spectrum | Alta | Espectrograma ou barômetro de frequência na waveform para identificar frequências problemáticas. |
| Sync automático A/V | Alta | Detectar e corrigir automaticamente o offset entre áudio e vídeo de um mesmo clipe. |
| Mixer de áudio dedicado | Média | Painel separado com faders, pan, solo/mute por faixa — estilo mixer de estúdio. |
| Normalização de loudness | Média | Normalizar clipes para LUFS alvo (ex: -14 LUFS para streaming). |
| Extração de áudio | Baixa | Extrair trilha de áudio de um clipe de vídeo para edição separada. |

---

## 13. Color grading e cor

| Feature | Prioridade | Descrição |
|---------|-----------|-----------|
| Correção de cor básica | Alta | White balance, tint, exposure, contrast, highlights/shadows. |
| LUTs (Look-Up Tables) | Média | Aplicar LUTs .cube/.3dl para looks cinematográficos. |
| Curvas de cor | Média | Curvas RGB separadas (RGB parade) para ajuste fino. |
| Scopes | Média | Waveform, vectorscope, histogram no preview para monitorar a cor. |
| Match de cor entre clips | Baixa | Ajustar automaticamente o balanço de cor de um clip para combinar com outro. |

---

## 14. Motion graphics e título

| Feature | Prioridade | Descrição |
|---------|-----------|-----------|
| Animações de texto | Alta | Fade, slide, typewriter, bounce — keyframes de posição/opacidade do texto. |
| Estilos de texto predefinidos | Média | Presets de título (lower third, banner, subtitle) com fontes/cores configuráveis. |
| Shape layers | Média | Retângulos, elipses, linhas com preenchimento, stroke, shadow — animáveis. |
| Animação de clip | Média | Keyframes de posição, escala, rotação, anchor point — já existe no modelo mas sem preview fluido. |

---

## 15. Importação e mídia

| Feature | Prioridade | Descrição |
|---------|-----------|-----------|
| Import de pasta inteira | Alta | Importar todos os arquivos de uma pasta mantendo a ordem alfabética. |
| Detecção de duplicatas | Média | Identificar arquivos duplicados (mesmo hash) na importação. |
| Proxy automático | Média | Criar proxies de baixa resolução automaticamente na importação para projetos pesados. |
| Thumbnails personalizadas | Baixa | Permitir escolher um frame customizado como thumbnail na pool de mídia. |
| Suporte a image sequence | Baixa | Importar sequências de imagens (PNG/JPEG) como clipes de vídeo. |

---

## 16. Exportação avançada

| Feature | Prioridade | Descrição |
|---------|-----------|-----------|
| Queue de exportação | Alta | Fila de múltiplos exports com diferentes settings (ex: YouTube + Instagram). |
| Metadata no export | Média | Injetar metadata customizada (título, descrição, tags) no arquivo exportado. |
| Export com proxy | Média | Opção de exportar usando os proxies de baixa resolução para teste rápido. |
| Watermark overlay | Baixa | Adicionar watermark (imagem/logo) com posição/opacidade configuráveis. |
| Export de projeto para NLE | Baixa | Exportar timeline para formatos inter-câmbio (EDL, XML para Premiere/DaVinci). |

---

## 17. Acessibilidade e internacionalização

| Feature | Prioridade | Descrição |
|---------|-----------|-----------|
| Interface em múltiplos idiomas | Média | Tradução completa da UI (atualmente em pt-BR). |
| Suporte a screen readers | Baixa | Acessibilidade para leitores de tela (Qt accessibility). |
| Atalhos customizáveis | Baixa | Permitir remapear atalhos de teclado via config. |

---

## 18. Arquitetura e manutenibilidade

| Feature | Prioridade | Descrição |
|---------|-----------|-----------|
| Sistema de undo/redo incremental | Alta | Diff incremental ao invés de snapshot JSON completo — muito mais rápido e menor consumo de memória. |
| Testes automatizados | Média | Unit tests para Project, TimelineWidget, PreviewWidget — cobrir bugs já encontrados. |
| Logging estruturado | Média | Sistema de logs com categorias (video, audio, export, ui) e níveis configuráveis. |
| Profile de memória | Baixa | Ferramenta interna para medir consumo de memória do MediaCache, decoder, pixmaps. |

---

## 19. Integração e automação

| Feature | Prioridade | Descrição |
|---------|-----------|-----------|
| Scripting / macro | Média | Automatizar tarefas repetitivas via scripts simples (Python/Lua ou Qt Script). |
| Integração com ffmpeg avançada | Baixa | Permitir usuário adicionar filtros ffmpeg customizados via string. |
| HTTP streaming input | Baixa | Abrir vídeos via URL/HTTP sem download completo (ffmpeg suporta). |

