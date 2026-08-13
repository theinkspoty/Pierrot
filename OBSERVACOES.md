# Observações

## Interface
- [x] Toolbar principal (Novo/Abrir/Salvar/Desfazer/Refazer/Reproduzir/Exportar) removida — ações agora só nos menus; "Importar mídia" ganhou entrada no menu Arquivo (Ctrl+I segue ativo, além do botão Adicionar no painel Mídia).

## RAM
- [x] Cache de mídia (thumbnails/waveforms) reduzido: 512→256 thumbs, 24→16 waveforms, thumb 320→256px; liberação dos buffers decodificados (DPB) após thumbnails e pan/crop; cache é limpo ao trocar de projeto.
- [ ] Acompanhar uso em projeto grande (p.ex. 4K): o decoder do preview (FrameWorker) mantém buffers enquanto aberto — se ainda houver pico, avaliar liberar quando pausado.
