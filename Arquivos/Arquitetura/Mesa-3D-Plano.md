# Plano — Mesa 3D estilo After Effects Classic (2010–2018)

Documento de planejamento (2026-10-02). Status: **plano realista**,
não implementado. Detalhe no `ROADMAP.md` (seção “Mesa 3D — plano realista”).

## Referência

**After Effects Classic 3D (2010–2018)** — renderer de camadas 3D com luzes
e malhas simples, **sem** Cinema 4D, **sem** ray-trace, **sem** simulação.
O mesmo espírito do Pierrot: composição em CPU, foco em motion graphics.

## O que era o AE Classic 3D (nosso alvo)

| Recurso | Pierrot v1 3D |
|---|---|
| 3D layer (XYZ pos, XYZ rot) | `mesaZ` + rot XYZ + KFs |
| 3D camera (pos, POI, zoom/FOV) | `camZ/fov/pitch/yaw` + KFs |
| Lights parallel/point (lambert) | Luz simples |
| Accepts Lights | flag `acceptsLights` |
| Depth sort | ordenar por Z |
| Malha simples | glTF/OBJ estático |
| Renderer CPU | QPainter + projeção |

## Escopo v1

| Entra | Não entra |
|---|---|
| Z + rot XYZ por camada | C4D renderer |
| Câmera FOV/Z/pitch/yaw/POI | Ray-trace |
| Depth sort | Simulação |
| Luz parallel/point | PBR / node materials |
| acceptsLights | Skinning / mocap |
| glTF/OBJ estático | GPU obrigatória |
| Export PNG sequence | Competir com AE em VFX |

## Fases (detalhe)

### Fase 0 — Fundação (0.8) — ✅ implementada 2026-10-02

- `Math3D.h`: Vec3, Mat4, perspective, lookAt, rotateXYZ
- `tst_math3d`
- Campos: `mesa3d`, `camZ/fov/pitch/yaw/roll`, `camPoi*`, `mesaZ`, `mesaRotX/Y`, `mesaAcceptsLights`
- Serialização `.Blanc`
- Flag `mesa3d` lida no `MesaRenderer` (render ainda 2D até a Fase 1)

**Aceite:** projetos antigos = defaults 2D; `tst_math3d` + serialização.

### Fase 1 — Câmera/layers Classic 3D (0.8)

- Projeção perspectiva quando `mesa3d`
- Câmera POI (lookAt)
- Layer Z + rotX/Y no painter
- Depth sort por Z
- UI: sliders Z/FOV/pitch/yaw/POI
- Graph Editor: props 3D
- Preview + export paridade

**Aceite:** parallax com Z; FOV animado; export = preview.

### Fase 2 — Luzes (0.9)

- Luz parallel/point + KFs
- Painel de luzes na Mesa
- `acceptsLights` por camada
- Lambert CPU

**Aceite:** camada com luz reage ao ângulo; flag false = flat.

### Fase 3 — Malhas estáticas (0.9)

- glTF 2.0 + OBJ estáticos (sem skin/anim)
- Camada `meshPath`
- Raster software (tris + textura + Z)
- Luz/câmera aplicam
- Export PNG sequence

**Aceite:** glTF simples na Mesa; export usa o raster.

### Fase 4 — Polish (1.0)

- Gizmos XYZ, pré-comp 3D, docs, benchmark, testes

**Aceite:** cena 3D montável sem ler código.

## Sequência

```
0 → 1 (0.8)  →  2 → 3 (0.9)  →  4 (1.0)
```

## Se um dia precisar de AE “de verdade”

Plugin OFX ou pré-render em Blender → clipe na timeline.
