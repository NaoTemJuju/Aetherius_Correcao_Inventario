---
titulo: Correção da renderização da prévia 3D no MeridianUI
tipo: decisao
status: ativo
decisao: proposto
substitui: null
responsaveis: [Akaston]
atualizado: 2026-11-25
tags: [inventario, meridian, renderizacao, arquitetura, ui-core]
relacionados: [../02-sistemas/correcao-inventario/README.md, ../02-sistemas/ui-core/README.md]
---

# ADR-0038 — Correção da renderização da prévia 3D no MeridianUI

## Resumo
Corrigir os artefatos visuais do inventário 3D (malhas brancas, itens sem textura em cache, vidro escuro e paleta de cor alterada) diretamente no pipeline D3D11 do `MeridianUI` (`MeridianUI.dll`), entregue como mod separado **`AETHERIUS - CORREÇÃO INVENTARIO`**, em vez de editar individualmente os arquivos `.nif` dos mods da load order.

## Contexto
Itens de múltiplos provedores (`Skyrim - Meshes0/1.bsa`, `ccBGSSSE001-Fish.bsa`, *SMIM*, *Particle Patch*, *EmbersXD Torch Edit*, *Awesome Potions Simplified*, *Simple Wearable Lanterns*) apresentavam formas brancas sólidas, ausência de textura ou paleta/iluminação distorcida no inventário do Aetherius.

A investigação nos `.nif` reais e no código do `MeridianUI 1.5.0` demonstrou que o defeito não estava nos modelos nem no pós-processamento do mundo, mas em lacunas estruturais do `SkyrimNifExtractor`, `NifPreviewRenderer`, `NifMaterial.hlsl`, `NifLighting.h` e `RenderHost.cpp`.

## Opções consideradas
| Opção | Prós | Contras |
|---|---|---|
| Editar `.nif` de cada item afetado | Não requer recompilar C++ | Frágil e incompleto: afetaria centenas de NIFs de mods/CC, quebraria o visual dos itens no mundo 3D e não resolveria a paleta de cor, `GlowMap`, `VertexColors` nem a dupla pré-multiplicação (`alpha²`) |
| Corrigir o renderizador no `MeridianUI` e entregar como mod separado `AETHERIUS - CORREÇÃO INVENTARIO` | Resolve todos os itens vanilla, CC e de mods de uma vez; preserva 100% dos NIFs e do visual no mundo; reversível desativando o mod no MO2; integrável via patch cirúrgico no `aetherius-client` | Requer compilar `MeridianUI.dll` com MSVC 14.44 / CEF 152.0.6 e validar em jogo |

## Decisão
- Implementar todas as correções no pipeline `NifPreview` do `MeridianUI` (suporte a `BSEffectShaderProperty`, descarte de shapes sem shader/refração/dummies ENB Light, `VF_COLORS`, `GlowMap`/`OwnEmit`, ordenação opaco→transparente, culling correto em transparentes, fallback `normalSample.a` para `environmentMask`, composição premultiplied em `RenderHost` e iluminação neutra em `NifLighting.h`).
- Distribuir como mod autônomo **`AETHERIUS - CORREÇÃO INVENTARIO`** contendo a `MeridianUI.dll` compilada + patch cirúrgico + documentação no padrão do Acervo.
