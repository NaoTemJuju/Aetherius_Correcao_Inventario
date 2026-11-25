# Instruções para agentes de IA

<!-- ACERVO-AETHERIUS:INICIO -->
## Documentação obrigatória: Acervo Aetherius
Este projeto documenta seu desenvolvimento em:
https://github.com/pablo-rodrigues1/AETHERIUS-ACERVO-DE-INFORMACOES

Regras (completas em REGRAS.md do acervo, seção 10):
1. Antes de criar ou alterar um sistema, leia INDICE.md, o sistema afetado, seus dependentes e os ADRs vigentes no acervo.
2. Envie atualizações continuamente por commit direto na `main` do acervo (`git pull --rebase` antes; mensagem `docs(<sistema>): <resumo>`):
   no início do sistema, a cada decisão relevante, mudança de fórmula/parâmetro, entrega, bloqueio e ao fim de cada sessão.
3. Atualize: `02-sistemas/<sistema>/diario.md`, o quadro de status em `02-sistemas/README.md`, o painel `07-guia-da-staff/progresso.md` e, quando couber, README do sistema, ADRs, CHANGELOG e INDICE.
4. Se a mudança afetar o que a Staff precisa saber, atualize também `07-guia-da-staff/` em linguagem simples para leigos.
5. Apenas arquivos .md, seguindo os modelos de `_modelos/`.
6. Se este repositório for isolado, garanta que o sistema no acervo tenha `repositorio: <URL deste repo>` e manteinha aqui um link de volta para a documentação.
7. Se o sistema já existir no acervo, explique ao dev quem atua nele e em que fase está, e peça confirmação antes de continuar.
8. Se não conseguir ler ou enviar ao acervo, AVISE O DEV imediatamente e entregue os .md prontos.
9. Nunca envie segredos, IPs de produção ou dados de jogadores.
<!-- ACERVO-AETHERIUS:FIM -->
