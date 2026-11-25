---
titulo: Correção da prévia 3D do inventário
tipo: sistema
status: ativo
fase: em-teste
responsaveis: [Akaston]
repositorio: https://github.com/NaoTemJuju/Aetherius_Correcao_Inventario
depende_de: [../ui-core/README.md]
atualizado: 2026-11-25
tags: [inventario, meridian, renderizacao, shader, nif, ui-core]
relacionados: [diario.md, ../ui-core/README.md, ../../03-decisoes/ADR-0038-correcao-renderizacao-preview-inventario.md, ../../07-guia-da-staff/itens/previa-3d-inventario.md]
---

# Correção da prévia 3D do inventário

## Resumo
Correção isolada no pipeline de renderização 3D de inventário do **MeridianUI 1.5.0** (`SkyrimNifExtractor`, `PackedGeometryDecoder`, `NifPreviewRenderer`, `NifMaterial.hlsl`, `NifLighting.h`, `RenderHost`), distribuída como mod separado **`AETHERIUS - CORREÇÃO INVENTARIO`** para eliminar malhas brancas de efeitos/helpers de mundo, recuperar texturas em cache e restaurar a fidelidade de cores, brilho e transparência dos itens no inventário.

## Onde está o código
- Pacote local: `AETHERIUS - CORREÇÃO INVENTARIO/`
- Patch cirúrgico sobre MeridianUI 1.5.0 + `changes.patch` oficial: `patch/meridian-inventory-preview-fix.patch`
- Pontos de entrada alterados:
  - `src/UIPlatform/Render/SkyrimNifExtractor.cpp`: suporte a `BSEffectShaderProperty`, `GlowMap` (slot 2), `OwnEmit`, `VF_COLORS`, `kEnvmapLightFade`, clamp de `specularPower` até `512.0`, recuperação estendida de `NiSourceTexture` em cache, descarte de shapes sem shader / `EditorMarker` / `kRefraction` / dummies ENB Light, e extração somente leitura de `BSLightingShaderPropertyFloatController`, `BSEffectShaderPropertyFloatController` e `NiAlphaController`.
  - `src/UIPlatform/Render/PackedGeometryDecoder.h` e `NifWeightInterpolation.h`: decodificação e interpolação de `VF_COLORS` (`VA_COLOR`, RGBA8).
  - `src/UIPlatform/Render/NifPreviewMesh.h`, `NifPreviewRenderer.h` e `NifPreviewRenderer.cpp`: passagem dupla (opacos → transparentes/aditivos), blend state aditivo dedicado, culling de backfaces em transparentes single-sided, fallback seguro para texturas de efeito, avaliação em tempo real de `PreviewFloatTrack` (animação contínua de UV/alpha/emissive/specular) e separação correta de espaço de cor (`DDS_LOADER_FORCE_SRGB` para texturas de cor, `DDS_LOADER_DEFAULT` para normal maps, `DDS_LOADER_IGNORE_SRGB` para máscaras).
  - `src/UIPlatform/Render/Shaders/NifMaterial.hlsl`: caminho dedicado para `BSEffectShaderProperty` (paleta grayscale, falloff, vertex alpha, aditivo), `SV_IsFrontFace`, `VertexColors` (sRGB→linear), `GlowMap`/`OwnEmit`, `gUvTransform` dinâmico, `MATERIAL_ENVMAP_LIGHT_FADE`, especular/cubemap calibrados ao `BSLightingShader` e pipeline linear puro (`LinearToSrgb(ToneMapACES(...))`) com `color * alpha` em `MATERIAL_PREMULTIPLIED_ALPHA`.
  - `src/UIPlatform/Render/NifLighting.h`: calibração quente-neutra em espaço linear para `LightingPreset::Bright` (usado pelo `AetheriusUIBridge.dll`) e `Neutral`.
  - `src/UIPlatform/Menus/NativeSurfaceMenu.cpp`: preserva o último frame renderizado durante `Status::Loading`, eliminando o pisca-pisca causado por recarregamentos periódicos da sincronização de inventário.
  - `src/UIPlatform/Render/RenderHost.cpp`: composição de `NativeSurfaceMenu` com `AlphaBlend()` (premultiplied) preservando `NonPremultiplied()` para os browsers CEF.

## Escopo
- Inclui: correção completa de visualização e animação de ingredientes, poções, tochas, lanternas, comidas, armas e armaduras na prévia 3D do inventário, sem modificar nenhum `.nif` ou mod de textura da load order.
- Não inclui: alterações de gameplay, servidor, banco de dados ou layout HTML/CSS do `AetheriusUI_Core`.

## Parâmetros e fórmulas
| Parâmetro | Valor | Descrição |
|---|---|---|
| `Bright.ambientTop` *(usado no inventário)* | `(0.34, 0.33, 0.31)` | Luz ambiente superior quente-neutra balanceada para itens escuros e claros |
| `Bright.ambientBottom` | `(0.16, 0.155, 0.15)` | Piso de sombra que mantém metais escuros e tigelas de madeira legíveis |
| `Bright.lightColor[0..2]` | `(0.92,0.87,0.80)` / `(0.38,0.38,0.39)` / `(0.30,0.30,0.32)` | Key quente + fill/rim neutros que recortam armas e armaduras sem dominante azulada |
| `Bright.baseExposureStops` | `0.0` (`1.0×`) | Elimina a superexposição `+0.45 stops` (`1.37×`) que lavava rolha e corda |
| Curva de tonemap | `lerp(Reinhard, ACES, saturate(exposed * 2.5))` | Evita que o *toe* do ACES esmague metais escuros (`< 0.05` linear) mantendo highlights ACES em poções |
| Loader DDS por tipo | Cor: `FORCE_SRGB` · Normal: `DEFAULT` · Máscara: `IGNORE_SRGB` | Lineariza texturas de cor (`plainglasstileblue.dds`, `redliquid_g.dds`) e respeita `BC7_UNORM_SRGB` em normal maps (`cork_n.dds`, `twine_n.dds`) |
| Specular / EnvMap / Vidro | Specular `×0.85`, EnvMap `×0.80 × lerp(0.45, 1.0, fade)`, Fresnel vítreo `lerp(0.45, 1.0, (1-NdotV)²)` | Devolve brilho metálico real em Adaga/Elmo e transparência vítrea com reflexo vertical nas poções |
| Animação de shader | `gUvTransform = (uScale, vScale, uOffset, vOffset)` via `EvaluateFloatTrack` (throttle 30 FPS) | Reproduz em tempo real o movimento do líquido em poções, wisps da Poeira Lucente e brilho da Tocha sem sobrecarregar o `FrameTransport` |
| Blend de `NativeSurfaceMenu` | `CommonStates::AlphaBlend()` | Elimina dupla pré-multiplicação (`alpha²`) e permite brilho aditivo sobre o fundo |

## Dependências
| Depende de | Por quê |
|---|---|
| [Aetherius UI Core](../ui-core/README.md) | Fornece `MeridianUIPlugin.dll`, runtime CEF 152.0.6 e frontend `aetheriusui/` |
| MeridianUI 1.5.0 (`5707877`) | Base nativa do renderizador D3D11 |

## Decisões relacionadas
- [ADR-0038](../../03-decisoes/ADR-0038-correcao-renderizacao-preview-inventario.md): corrigir no renderizador MeridianUI em vez de editar NIFs individuais dos mods.

## Diário
Ver [`diario.md`](diario.md).
