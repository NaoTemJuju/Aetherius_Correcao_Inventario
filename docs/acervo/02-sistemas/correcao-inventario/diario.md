---
titulo: Diário — Correção da prévia 3D do inventário
tipo: historico
status: ativo
responsaveis: [Akaston]
atualizado: 2026-11-25
tags: [diario, inventario, meridian, renderizacao]
relacionados: [README.md]
---

# Diário — Correção da prévia 3D do inventário

## Resumo
Registro cronológico da investigação, implementação e validação da correção do renderizador 3D do inventário (`AETHERIUS - CORREÇÃO INVENTARIO`).

## 2026-11-25 — Investigação completa dos NIFs e correção no MeridianUI 1.5.0
- **Feito:**
  - Inspeção do `MeridianUI.log` e extração/análise estrutural dos `.nif` reais vencedores do perfil `AETHERIUS - GRAFICO - QUALIDADE (ALQUIMIA)` (loose files e BSAs): `SalmonRoe01.nif`, `Eggs.nif` (*Particle Patch*), `DragonFlyBlue.nif`, `FishAnglerLarvae01Inv.nif` (*CC Fishing*), `Glowdust01.nif` (*SMIM*), `Torch.nif` (*EmbersXD Torch Edit*), `SimpleWearableLanterns\Iron\GO.nif` e `PotionHealth*.nif` (*Awesome Potions Simplified*).
  - Identificadas e corrigidas todas as 8 causas raízes no pipeline `NifPreview` do `MeridianUI` (sobre commit `5707877` + `changes.patch` oficial do Aetherius):
    1. Descarte de shapes sem `BSShaderProperty` (`SalmonRoe01:10`), `EditorMarker`, `kRefraction`/`kTempRefraction` (`HeatRefraction:0`) e dummies ENB Light (`ppLight`/`pp_black.dds`).
    2. Suporte completo a `BSEffectShaderProperty` (textura difusa, paleta grayscale, `baseColor`/`baseColorScale`, `falloff`, `VertexAlpha`, blending aditivo `dst == kOne`).
    3. Decodificação e interpolação de `VF_COLORS` (`VA_COLOR`, RGBA8).
    4. Suporte a `GlowMap` (slot 2 `_g.dds`), `OwnEmit` (`emissiveColor`/`emissiveMult`) e recuperação estendida de `NiSourceTexture` em modelos em cache (`Lanterna de Ferro`).
    5. Renderização em duas passadas (opacos → transparentes/aditivos), `CullClockwise()` para transparentes single-sided e inversão de normal por `SV_IsFrontFace`.
    6. Fallback de `environmentMask` para `normalSample.a` quando `_m.dds` está ausente.
    7. Composição de `NativeSurfaceMenu` em `RenderHost.cpp` com `AlphaBlend()` (eliminando dupla pré-multiplicação `alpha²` e habilitando aditivo sobre o fundo do inventário).
    8. Calibração neutra de estúdio em `NifLighting.h` (`Neutral`, `baseExposureStops = 0.0f`).
- **Testes:**
  - Build Release concluído com MSVC 14.44.35207, Windows SDK 10.0.26100.0 (`fxc.exe`), CEF 152.0.6 e CRT `/MT` (zero erros e zero warnings C++ em `/W4`).
  - ABI verificada via `dumpbin /EXPORTS` e `/DEPENDENTS`: idêntica à `MeridianUI.dll` do `AETHERIUS - UI CORE`.
  - CTest: **9/9 aprovados** (`NifPreviewGpuTests`, `NifPreviewGpuUnsupportedSharingTests`, `PackedGeometryDecoderTests`, `NifSceneCompositionTests`, `NifWeightInterpolationTests`, `NifTexturePathTests`, `NifCameraMathTests`, `NifLightingTests`, `NifPreviewRendererArchitectureTests`).
  - GPU física real (`NifPreviewGpuTests.exe --unsupported-sharing --hardware`): aprovado com `debugQueue=1` (zero erros do D3D11 Debug Layer), incluindo regressões de efeito aditivo (`BSEffectShaderProperty`), fallback seguro de textura de efeito ausente e `VertexColors`.
  - Artefato: `MeridianUI/MeridianUI.dll` (`2.424.320` bytes, SHA-256 `3036a6715f615ca51e8644c57601feb2eb8a83111cd8bb6a1a85f662d504a5e1`).
- **Próximo:** ativar `AETHERIUS - CORREÇÃO INVENTARIO` no MO2 acima de `AETHERIUS - UI CORE` e validar em jogo todos os itens do checklist (Ovas de Salmão, Ovos de Chaurus, Libélula Azul, Larva de Pescador, Poeira Lucente, Tocha, Lanterna de Ferro e Poção de Cura).

## 2026-11-25 — v1.1.0: Calibração de cor, normal maps `BC7_UNORM_SRGB` e reflexo de vidro contra o inventário vanilla
- **Diagnóstico empírico (comparação direta com inventário vanilla):**
  - Após validar que a v1.0 eliminou todas as formas brancas e o vidro manchado, a comparação lado a lado com o inventário vanilla mostrou que a corda (`Twine`) e a rolha (`Cork`) das poções ainda ficavam esbranquiçadas em vez de marrom-dourado rico.
  - Inspeção direta dos headers DXGI e decodificação via DirectXTex (`probe_dds`) nas texturas do *Awesome Potions Simplified* revelou a causa exata:
    1. `cork_n.dds` e `twine_n.dds` usam `DXGI_FORMAT_BC7_UNORM_SRGB (99)`. Com `DDS_LOADER_DEFAULT` decodificam para normal neutro `(0.48, 0.48, 0.92)`; com `DDS_LOADER_IGNORE_SRGB` (usado pelo MeridianUI), eram lidas como `(0.72, 0.72, 0.96)`, inclinando todas as normais 45° em direção à luz principal.
    2. `cork.dds`, `twine.dds`, `twine3.dds` e `purpleliquid.dds` também são `BC7_UNORM_SRGB (99)` (enquanto `redliquid.dds` é `BC1_UNORM (71)`). O Skyrim SE carrega respeitando o formato do arquivo (`DDS_LOADER_DEFAULT`) e sombreia sem aplicar `LinearToSrgb()` extra; forçar `DDS_LOADER_FORCE_SRGB` + `LinearToSrgb()` lavava o marrom de rolha/corda para bege esbranquiçado e triplicava o brilho das sombras.
- **Feito:**
  - `NifPreviewRenderer.h/.cpp`: `LoadTextureResource` agora respeita o formato DXGI nativo do arquivo DDS (`DDS_LOADER_DEFAULT`) por padrão tanto em difusas quanto em normal maps.
  - `NifMaterial.hlsl`: introduzido `EncodeSkyrimDisplay` (`LinearToSrgb(ToneMapACES(SrgbToLinear(exposed) * 1.18))`) sem dupla conversão gamma, vertex colors sem conversão dupla, e separação de `bodyDisplay * alpha + reflDisplay` em materiais `MATERIAL_PREMULTIPLIED_ALPHA` para preservar o brilho especular vertical no vidro.
  - `NifLighting.h`: ajuste fino do preset `Neutral` (`ambientTop=(0.36,0.35,0.33)`, `ambientBottom=(0.16,0.15,0.15)`, `lightColor=((0.92,0.89,0.84), (0.30,0.31,0.33), (0.22,0.22,0.23))`, `baseExposureStops=0.0`).
- **Testes:**
  - Build Release limpo (0 erros, 0 warnings); ABI idêntica via `dumpbin /EXPORTS` e `/DEPENDENTS`.
  - CTest: **9/9 aprovados** (`0.34s`).
  - GPU física real (`NifPreviewGpuTests.exe --unsupported-sharing --hardware`): aprovado com `DXVK=0 debugQueue=1`.
  - Artefatos v1.1.0:
    - `MeridianUI/MeridianUI.dll`: `2.427.392` bytes, SHA-256 `2bf2e99905bb5c71a4c0c4809e75f785d62525e73156f5817d9abd5cd9c2ea67`.
    - `patch/meridian-inventory-preview-fix.patch`: `67.416` bytes, SHA-256 `8cfb41519085a926c735a3a2704c17c55b8e4d2af42d247de919525ee7c0cb10`.

## 2026-11-25 — v1.2.0: Correção do `LightingPreset::Bright` (usado pelo Bridge), vidro cristalino, `kEnvmapLightFade` e animações de shader em tempo real
- **Diagnóstico empírico (análise da v1.1 + inspeção de `AetheriusUIBridge` e controllers NIF):**
  1. `AetheriusUIBridge` (`client/ui-core/native/src/main.cpp:644`) solicita explicitamente `LightingPreset::Bright` — que ainda tinha `ambientTop=(0.45,0.48,0.54)`, fill azul `(0.62,0.70,0.84)` e **`baseExposureStops = +0.45` (`1.37×`)**, superexpondo todos os itens do inventário!
  2. Medição via DirectXTex (`probe_dds2`) provou que `plainglasstileblue.dds` (`BC3_UNORM`) e `redliquid_g.dds` (`BC1_UNORM`) precisam de `DDS_LOADER_FORCE_SRGB` (em `DEFAULT`, o vidro ficava 8,6× claro/leitoso e `redliquid_g.dds` vazava manchas roxas/magenta), enquanto normal maps (`cork_n.dds`/`twine_n.dds` em `BC7_UNORM_SRGB` e `plainglasstile01_n.dds`/`liquid_n.dds` em `BC3/BC1_UNORM`) precisam de `DDS_LOADER_DEFAULT`.
  3. Na v1.1, somar `reflDisplay` sem multiplicar por `alpha` no bloco `MATERIAL_PREMULTIPLIED_ALPHA` + não aplicar `kEnvmapLightFade` + `specStrength=5.0` nas 3 luzes deixou o vidro esbranquiçado/estourado.
  4. Inspeção dos NIFs reais revelou controllers de animação ignorados pelo `NifPreviewRenderer`: `BSLightingShaderPropertyFloatController` (`var=22` V Offset em loop 20s no líquido das poções do *Awesome Potions Simplified*), `BSEffectShaderPropertyFloatController` (`var=6/7/8` nos wisps de `glowdust01.nif` e em `GlowAddMeshB` de `torch.nif`).
- **Feito:**
  - `NifLighting.h`: calibrados `LightingPreset::Bright` (`ambientTop=(0.18,0.17,0.15)`, `ambientBottom=(0.06,0.055,0.05)`, `key=(0.76,0.70,0.62)`, `baseExposureStops=0.0f`) e `Neutral` em espaço linear quente-neutro.
  - `NifPreviewRenderer.cpp`: separação exata de loader DDS (`FORCE_SRGB` para cor/glow/envmap, `DEFAULT` para normal maps, `IGNORE_SRGB` para máscaras), avaliação de `PreviewFloatTrack` a cada frame com relógio monotônico quando o item possui animações.
  - `SkyrimNifExtractor.cpp` + `NifPreviewMesh.h`: extração somente leitura de `BSLightingShaderPropertyFloatController`, `BSEffectShaderPropertyFloatController` e `NiAlphaController` (`U/V Offset`, `U/V Scale`, `Alpha`, `EmissiveMultiple`, `SpecularStrength`, `EnvironmentScale`, `Glossiness`, `Falloff`), leitura de `kEnvmapLightFade`, clamp de `specularPower` até `512.0` e aplicação dinâmica de UV via shader.
  - `NifMaterial.hlsl`: pipeline linear puro (`LinearToSrgb(ToneMapACES(...))`), `gUvTransform`, `MATERIAL_ENVMAP_LIGHT_FADE`, especular `×0.35`, cubemap `×0.32`, e `color * alpha` exato em `MATERIAL_PREMULTIPLIED_ALPHA`.
- **Testes:**
  - Build Release limpo (0 erros, 0 warnings); ABI idêntica via `dumpbin`.
  - CTest: **9/9 aprovados** (`0.78s`), incluindo regressão de `EvaluateFloatTrack` (UV scroll em loop).
  - GPU física real (`NifPreviewGpuTests.exe --unsupported-sharing --hardware`): aprovado com `DXVK=0 debugQueue=1`.
  - Artefatos v1.2.0:
    - `MeridianUI/MeridianUI.dll`: `2.435.072` bytes, SHA-256 `53948a0ae2954dcfa106a0c9b948fd24d910b73c527768a309ee2169dffd68ff`.
    - `patch/meridian-inventory-preview-fix.patch`: `93.552` bytes, SHA-256 `6cd8dc255d18227769b72c862c2a5685b292f3efb7991a8fbcef23aad9659f00`.

## 2026-11-25 — v1.3.0: Equilíbrio para itens escuros (Adaga/Elmo/Sopa), vidro vítreo com Fresnel e fim do pisca-pisca
- **Diagnóstico empírico (prints reais da v1.2 + `MeridianUI.log`):**
  1. Itens escuros (`Adaga de Ferro`, `Elmo Daédrico`, `Hot Cabbage Potato Soup`) ficaram próximos do preto porque a redução agressiva de `Bright` (`ambientBottom=0.06`, `fill=0.16`) somada a `environment × 0.32 × envLightFade` e ao *toe* do `ToneMapACES` (que esmaga valores `< 0.05` linear em até 70%) apagou metais escuros e madeira escura.
  2. O vidro das poções (`Poção de Respiração Aquática`) ficou parecendo fumê escuro por ter `diffuse ≈ 0.015` linear e `environment` quase zerado.
  3. O pisca-pisca a cada ~5–6s ocorria porque sincronizações periódicas reenviavam `NifViewAPIController::LoadModel` (`Status::Loading`), fazendo `NativeSurfaceMenu::Prepare()` limpar `m_shaderResourceView` para `nullptr` durante o recarregamento.
- **Feito:**
  - `NifLighting.h`: `Bright` elevado com equilíbrio quente-neutro (`ambientTop=(0.34,0.33,0.31)`, `ambientBottom=(0.16,0.155,0.15)`, `lightColor=((0.92,0.87,0.80),(0.38,0.38,0.39),(0.30,0.30,0.32))`, `baseExposureStops=0.0`).
  - `NifMaterial.hlsl`: transição suave Reinhard→ACES (`lerp(reinhard, aces, saturate(exposed * 2.5))`) para não esmagar sombras/metais escuros preservando a coloração das poções; `specular × 0.85`, `environment × 0.80` com piso `envLightFade >= 0.45`; Fresnel vítreo e destaque especular limpo em `MATERIAL_PREMULTIPLIED_ALPHA`.
  - `NativeSurfaceMenu.cpp`: mantém o último frame válido durante `Status::Loading` (fim do pisca-pisca).
  - `NifPreviewRenderer.h/.cpp`: throttle de 30 FPS (`33ms`) em ticks puramente de animação (0ms em câmera/resize).
- **Testes:**
  - Build Release limpo (0 erros, 0 warnings); ABI idêntica via `dumpbin`.
  - CTest: **9/9 aprovados** (`0.50s`); GPU física real aprovada (`DXVK=0 debugQueue=1`).
  - Artefatos v1.3.0:
    - `MeridianUI/MeridianUI.dll`: `2.439.168` bytes, SHA-256 `ed467ce2aa4fa4052995bc7caf0f5ce36af2a538233636d796c798c04ebb8d38`.
    - `patch/meridian-inventory-preview-fix.patch`: `97.357` bytes, SHA-256 `ffd1f6a556434fa1b514152c450ec91bd2f6088d18295b9e993f2c72ca6b149e`.
- **Bloqueios:** nenhum.
- **Nota sobre Acervo Aetherius:** este ambiente não realizou push direto no repositório `pablo-rodrigues1/AETHERIUS-ACERVO-DE-INFORMACOES`; os arquivos `.md` prontos para publicação estão em `docs/acervo/` e o resumo de tabelas em `docs/ATUALIZACOES-ACERVO.md`.
