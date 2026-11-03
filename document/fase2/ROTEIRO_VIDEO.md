# Roteiro do vídeo — CardioIA Fase 2

**Duração alvo:** 3min50 (o limite é 4min — deixe folga)
**Formato:** gravação de tela com narração
**YouTube:** subir como **"não listado"** e colar o link no topo do README

> Dica: grave em duas tomadas (Parte 1 e Parte 2) e junte. Fica muito mais fácil do que
> acertar quatro minutos seguidos. Deixe os dois terminais já abertos na pasta do projeto
> e o notebook já executado antes de começar a gravar.

---

## 0:00 – 0:25 | Abertura

**Mostre:** o README aberto no GitHub.

> "Oi, eu sou o Gabriel, grupo 58. Esta é a Fase 2 do CardioIA. O objetivo é transformar
> o relato de um paciente, escrito em texto livre, em duas respostas: **o que ele pode
> ter** e **se é grave**. A primeira parte usa um sistema de regras; a segunda, machine
> learning. Vou mostrar as duas rodando."

---

## 0:25 – 1:30 | Parte 1 — extração de sintomas

**Mostre:** `dados/frases_pacientes.txt`, depois `dados/mapa_conhecimento.csv` (role
rápido), depois `dados/sinonimos.csv`.

> "Tenho dez relatos escritos como um paciente escreveria. O mapa de conhecimento tem 63
> regras ligando pares de sintomas a doenças, com peso e nível de urgência. E essa
> terceira tabela é o que faz funcionar com texto real: ninguém escreve 'apresento
> palpitação', escrevem 'sinto o coração disparar'. São 162 expressões mapeadas."

**Rode:**
```bash
python src/extrator_sintomas.py
```

**Pare no Paciente 01** e deixe na tela.

> "Olha esse caso: dor no peito que aperta ao subir escada e **melhora quando ele para**.
> O sistema sugeriu angina estável, e deixou o infarto como diferencial logo abaixo. O que
> separou os dois foi justamente a melhora com repouso. E ele mostra a evidência — quais
> combinações de sintomas fecharam a regra. Num sistema de apoio à decisão, isso importa
> tanto quanto o diagnóstico."

**Role até o resumo final.**

> "Cobertura de 100%, e a triagem fica assim: duas emergências, cinco urgentes, três de
> rotina."

---

## 1:30 – 2:30 | Parte 2 — classificador de risco

**Mostre:** `dados/frases_risco.csv` rolando rápido, depois o notebook.

> "Agora a parte estatística. São 205 frases rotuladas como alto ou baixo risco. O
> pipeline é TF-IDF — que transforma texto em números — e um classificador."

**Mostre a tabela de comparação de modelos.**

> "Testei três modelos. O Naive Bayes ficou com 86,5% de acurácia. Mas repare que eu
> ordenei por **recall de alto risco**, não por acurácia — porque os dois erros não custam
> a mesma coisa. Um alarme falso gasta tempo da equipe. Um caso grave liberado pode custar
> a vida do paciente."

**Mostre a matriz de confusão.**

> "Aqui está o problema: cinco casos graves passaram como baixo risco."

---

## 2:30 – 3:25 | O achado — por que o modelo acerta

**Mostre:** a contagem da palavra "peito" e, logo em seguida, a lista de erros da seção 9.

> "Essa é a parte que eu acho mais interessante. A palavra 'peito' aparece em 40% das
> frases graves e em 11% das leves. Então levantei a hipótese de que o modelo tivesse
> aprendido vocabulário, e não medicina."

**Aponte dois erros específicos na tela.**

> "Olha o que ele errou. Classificou 'mancha vermelha no **braço** sem dor nem febre' como
> alto risco — porque 'braço' vem de 'irradia para o braço esquerdo'. E liberou 'ganhei
> quatro quilos em cinco dias e estou inchado', que é retenção hídrica, sinal clássico de
> insuficiência cardíaca descompensada. O modelo acerta, em parte, pelo motivo errado."

**Mostre a seção 8 — o teste adversarial com 6/6.**

> "E tem um detalhe que eu deixei no notebook de propósito: eu escrevi seis frases
> adversariais para derrubar o modelo, e ele passou em todas as seis. Quem expôs o viés
> foram os erros reais. Ou seja: quem escreve o teste do próprio modelo testa o viés que
> **imagina** ter, não o que ele tem."

---

## 3:25 – 3:50 | Fechamento

**Mostre:** a tabela de limiares.

> "Por último, o limiar. O corte de 0,5 é convenção estatística, não decisão clínica.
> Se eu baixo para 0,35, o recall vai para 96% e sobra só um caso perdido — ao custo de
> doze alarmes falsos. Cada linha dessa tabela é uma política de triagem diferente, e
> escolher uma é conversa com o corpo clínico, não com o time de dados sozinho."

> "É isso. Na Fase 1 o achado foi um rótulo invertido no dataset; aqui, um modelo que
> acerta pelo motivo errado. Mesmo tipo de problema: o número bonito na tela não prova que
> o sistema entendeu a questão. Obrigado."

---

## Checklist antes de gravar

- [ ] Notebook já executado, com todos os gráficos visíveis
- [ ] Terminal com fonte grande (o avaliador vai assistir no celular)
- [ ] `python src/extrator_sintomas.py` testado e rodando sem erro
- [ ] Microfone testado — áudio ruim derruba mais nota que slide feio
- [ ] Gravação de no máximo 4 minutos

## Checklist depois de gravar

- [ ] Upload no YouTube como **não listado** (não é "privado" — privado o avaliador não abre)
- [ ] Link colado no topo do `README.md`
- [ ] `git add . && git commit && git push`
- [ ] Abrir o repositório numa janela anônima e conferir que o vídeo abre dali
- [ ] Entregar o link do repositório na plataforma da FIAP
