---
titulo: Prévia 3D dos itens no inventário
tipo: guia-staff
status: ativo
responsaveis: [Akaston]
atualizado: 2026-11-25
tags: [inventario, visual, guia-staff, itens]
relacionados: [../../02-sistemas/correcao-inventario/README.md]
---

# Prévia 3D dos itens no inventário

## O que mudou
Foi criada a correção **`AETHERIUS - CORREÇÃO INVENTARIO`** para consertar a aparência dos itens exibidos em 3D no lado direito da tela de inventário.

Antes dessa correção, vários itens apareciam com defeitos visuais no inventário (mesmo estando normais no mundo):
- **Ovas de Salmão** apareciam como um bloco retangular branco;
- **Ovos de Chaurus**, **Larva de Pescador** e **Poeira Lucente** apareciam cercados por discos, esferas ou cones brancos;
- **Libélula Azul** tinha as asas como retângulos brancos;
- **Tochas** ficavam com a ponta branca onde deveria estar a chama;
- **Lanternas de Ferro** ficavam totalmente brancas;
- **Poções** e outros itens ficavam com o vidro escurecido/cinza e cores alteradas em relação ao jogo.

## O que a correção faz
- Faz o visualizador 3D do inventário entender efeitos translúcidos e luminosos (asas, chamas, brilhos de poções e ingredientes) e ignorar blocos invisíveis usados apenas por luzes/refração do mundo.
- Recupera automaticamente a textura de itens já carregados em memória (como a Lanterna de Ferro).
- Ajusta a iluminação da prévia para uma luz neutra fiel às cores originais dos itens.

## Impacto para jogadores e Staff
- **Apenas visual no cliente:** não altera peso, valor, receita, dano, efeitos nem dados do servidor.
- **Reversível:** pode ser ativado ou desativado como mod independente no Mod Organizer 2 sem modificar nenhum outro mod.
