# FIAP - Faculdade de Informática e Administração Paulista

<p align="center">
<a href= "https://www.fiap.com.br/"><img src="assets/logo-fiap.png" alt="FIAP - Faculdade de Informática e Admnistração Paulista" border="0" width=40% height=40%></a>
</p>

<br>

# CardioIA

## Grupo 89

## 👨‍🎓 Integrantes:
- <a href="https://github.com/gabrielcoppola">Gabriel Coppola</a>

## 👩‍🏫 Professores:
### Tutor(a)
- <a href="https://github.com/Leoruiz197">Leonardo Ruiz</a>


## 📜 Descrição

O **CardioIA** é uma plataforma acadêmica que simula o ecossistema de uma cardiologia
moderna. O projeto evolui ao longo das fases do curso, e cada fase fica na sua própria
pasta dentro de `src/`, `document/` e `assets/`.

| Fase | Tema | Onde está |
|------|------|-----------|
| **3** | [Monitoramento contínuo com IoT: Edge, Fog e Cloud](#fase-3--monitoramento-contínuo-iot-edge-fog-e-cloud) | `src/fase3/` |
| 2 | [Diagnóstico automatizado: IA no estetoscópio digital](#fase-2--diagnóstico-automatizado-ia-no-estetoscópio-digital) | `src/fase2/` |


---

## Fase 3 — Monitoramento contínuo (IoT, Edge, Fog e Cloud)

🔗 **Wokwi, Parte 1 (Edge):** `[COLE AQUI O LINK DO WOKWI — PARTE 1]`
🔗 **Wokwi, Parte 2 (MQTT):** `[COLE AQUI O LINK DO WOKWI — PARTE 2]`

A Fase 3 simula um **wearable cardíaco para monitoramento domiciliar pós-alta** e
percorre o caminho completo do dado em IoT aplicado à saúde:
**captura → processamento → transmissão → visualização → alerta**.

**Parte 1 — Edge Computing.** Um ESP32 no Wokwi lê um **DHT22** (temperatura e umidade)
e um **botão de batimento**: cada aperto é um batimento, e o ESP32 calcula o BPM pela
média dos últimos 5 intervalos entre batimentos. Uma chave deslizante simula a
conectividade Wi-Fi. Toda leitura é avaliada contra os limiares clínicos **no próprio
dispositivo**, e o LED vermelho acende mesmo sem rede. Depois, a leitura vai para uma fila
em **SPIFFS** (*store-and-forward*). Com a chave em "conectado", a fila é enviada pelo
`Serial.println` e o arquivo é apagado. Sem conexão, a coleta continua normalmente.

A capacidade da fila foi dimensionada pelo modelo de negócio: cobrir **uma noite inteira
offline**, ou seja, 8 h × 1 amostra a cada 2 s = **14.400 amostras (~430 KB)**. Ao atingir
o limite, o firmware descarta em lote 10% das amostras **normais** mais antigas e
**preserva as que tiveram alerta**: um episódio de taquicardia perdido não pode ser
recuperado depois.

**Parte 2 — Fog/Cloud.** O mesmo firmware passa a publicar em um broker **MQTT**
(HiveMQ). O dispositivo registra um **Last Will**: quando a cobertura cai, o broker publica
`offline` em nome dele, e o dashboard mostra que o paciente parou de ser monitorado em vez
de simplesmente parar de atualizar. No **Node-RED**, uma função de *fog* reavalia os
limiares e alimenta gráficos de BPM e temperatura, gauges, um LED virtual de alerta e
notificações. As leituras recuperadas da fila offline são separadas das leituras ao vivo:
uma amostra de 3 h atrás não acende o alerta agora. Ela aparece num painel próprio, que
informa, por exemplo, que houve taquicardia às 02:13 enquanto o aparelho estava sem rede.

| Limiar (definido pelo grupo) | Alerta |
|---|---|
| BPM > 120 | Taquicardia |
| BPM < 50 | Bradicardia |
| nenhum batimento em 5 s | Sem pulso |
| Temperatura > 38,0 °C | Febre |
| Temperatura < 35,0 °C | Hipotermia |

📄 **Relatórios:**
[Parte 1 — fluxo e resiliência offline](document/fase3/relatorio_parte1_edge.md) ·
[Parte 2 — MQTT e dashboard](document/fase3/relatorio_parte2_mqtt_dashboard.md)

### 🔧 Como executar a Fase 3

**Parte 1 — Wokwi (Edge)**

1. Em [wokwi.com](https://wokwi.com), crie um projeto **ESP32** e substitua `sketch.ino`
   e `diagram.json` pelos arquivos de `src/fase3/parte1_edge/`. Adicione também o
   `libraries.txt` (ou instale a biblioteca *DHT sensor library for ESPx* pela aba
   *Library Manager*).
2. Inicie a simulação e aperte o botão vermelho no ritmo de um coração (~1 vez/s = 60 bpm).
3. Mude a chave deslizante para alternar entre offline (a fila cresce) e online (a fila é
   enviada e o arquivo é apagado). Clique no DHT22 para alterar a temperatura.
4. Para ver a compactação da fila sem esperar 8 h, reduza `MAX_AMOSTRAS` para `20`.

**Parte 2 — Wokwi (MQTT) + Node-RED**

1. Crie outro projeto ESP32 no Wokwi com os arquivos de `src/fase3/parte2_mqtt/`
   (bibliotecas: *DHT sensor library for ESPx* e *PubSubClient*). O ESP32 usa a rede
   `Wokwi-GUEST` e o broker público `broker.hivemq.com:1883`.
2. Instale o Node-RED (`npm install -g node-red`) e rode `node-red`.
3. Em `http://localhost:1880`, abra *Menu → Manage palette → Install*, instale
   `node-red-dashboard` e depois importe `src/fase3/node-red/flows.json`
   (*Menu → Import*). Clique em **Deploy**.
4. Abra `http://localhost:1880/ui` e inicie a simulação no Wokwi.

> Para usar o **HiveMQ Cloud** (com TLS e autenticação) no lugar do broker público, veja a
> seção 5 do [relatório da Parte 2](document/fase3/relatorio_parte2_mqtt_dashboard.md#5-configuração-do-broker).
> O broker público serve para a demonstração, mas qualquer pessoa pode ler os tópicos:
> não use com dados reais.

**Tópicos MQTT**

| Tópico | Conteúdo |
|---|---|
| `cardioia/g89/p001/vitais` | JSON por leitura: `seq`, `ts`, `temp`, `umid`, `bpm`, `alertas`, `atrasado`, `pendentes` |
| `cardioia/g89/p001/status` | `online` / `offline` (retido; `offline` vem do Last Will) |

### 📊 Evidências

Os prints do circuito no Wokwi, do flow e do dashboard (estado normal, em alerta e
offline) estão em [`assets/fase3/`](assets/fase3/), listados na seção 9 do relatório da
Parte 2.


---

## Fase 2 — Diagnóstico automatizado (IA no estetoscópio digital)

🎥 **Vídeo de demonstração:** [https://youtu.be/xgy_ljhVqZA](https://youtu.be/xgy_ljhVqZA)

A **Fase 2 — Diagnóstico Automatizado** transforma o relato de um paciente,
escrito em texto livre, em duas respostas: **o que ele pode ter** e **se é grave**.

O projeto foi construído em duas abordagens deliberadamente diferentes, para que o
contraste entre elas seja ele próprio o aprendizado.

**Parte 1 — Extração de sintomas (abordagem simbólica).** Dez relatos escritos como um
paciente escreveria alimentam um extrator que identifica sintomas e propõe diagnósticos.
A base de conhecimento tem três camadas: 10 frases de entrada, um mapa com 63 regras no
formato `sintoma_1 · sintoma_2 · doença · peso · urgência`, e uma tabela de 162 expressões
coloquiais mapeadas para sintomas canônicos. Essa terceira camada é o que faz o sistema
funcionar com texto real — ninguém escreve "apresento palpitação", escrevem "sinto o
coração disparar". Uma regra só "fecha" quando os dois sintomas aparecem (peso × 2,0);
casamento parcial vale pouco (peso × 0,5), para que "dor no peito" sozinha não puxe um
infarto. O desempate é pelo número de regras fechadas, e a urgência adotada é sempre a
mais alta entre as regras acionadas — em triagem, erra-se para o lado seguro. Resultado:
100% de cobertura nos 10 relatos. O sistema distingue angina estável de infarto no mesmo
relato (o que desempata é a melhora com repouso) e exibe os diagnósticos diferenciais com
a evidência que sustentou cada um.

**Parte 2 — Classificador de risco (abordagem estatística).** Uma base de 205 frases
rotuladas como alto ou baixo risco é vetorizada com TF-IDF (unigramas e bigramas) e
usada para treinar três modelos. O Naive Bayes venceu, com 86,5% de acurácia e 80,8% de
recall em alto risco. A escolha do modelo foi feita por **recall**, não por acurácia,
porque os dois erros não custam a mesma coisa: um alarme falso gasta tempo de equipe;
um caso grave liberado vai para a fila comum.

O achado central da fase está na análise crítica: o modelo acerta, em parte, **pelo
motivo errado**. Ele aprendeu vocabulário e intensificadores, não fisiopatologia. Isso
está documentado com evidência no notebook e resumido na seção de resultados abaixo.

A Parte 1 sempre sabe explicar por que decidiu; a Parte 2 acerta mais, mas precisa ser
auditada para que se descubra por quê. É essa tensão — explicabilidade contra desempenho —
que define boa parte das decisões de IA aplicada à saúde.

### 🔧 Como executar a Fase 2

**Pré-requisitos**

| Item | Versão testada |
|------|----------------|
| Python | 3.11+ |
| pandas | 3.0 |
| scikit-learn | 1.9 |
| matplotlib | 3.11 |
| Jupyter Notebook | qualquer versão recente |

**Instalação**

```bash
git clone https://github.com/gabrielcoppola/cardio-ia.git cardioia-fiap
cd cardioia-fiap
pip install -r config/requirements.txt
```

**Parte 1: extração de sintomas**

```bash
python src/fase2/parte1_extracao/extrator_sintomas.py

# salvando o resultado em CSV
python src/fase2/parte1_extracao/extrator_sintomas.py --csv document/fase2/other/diagnosticos.csv

# analisando uma frase avulsa
python src/fase2/parte1_extracao/extrator_sintomas.py --frase "sinto dor no peito ao subir escadas"
```

O script localiza a pasta `src/fase2/dados` sozinho, subindo a árvore de diretórios — pode
ser executado da raiz do repositório ou de dentro de qualquer subpasta.

**Parte 2: classificador de risco**

```bash
jupyter notebook src/fase2/parte2_classificador/classificador_risco.ipynb
```

Execute as células na ordem (**Kernel → Restart & Run All**). Sem Jupyter instalado, abra
`document/fase2/classificador_risco.html` no navegador: o notebook está salvo com todas as
saídas e gráficos.

**Tudo de uma vez**

```bash
bash scripts/fase2_executar_tudo.sh
```

### 📊 Resultados e análise crítica

#### Parte 1 — exemplo de saída

```
PACIENTE 01
Relato: "Há dois dias sinto uma dor forte no peito que aperta quando subo escadas
         e melhora se eu paro para descansar."

Sintomas extraídos:
  - melhora com repouso   (do trecho: "paro para descansar")
  - dor no peito          (do trecho: "dor forte no peito")
  - subir escadas         (do trecho: "subo escadas")
  - aperto no torax       (do trecho: "aperta")

Hipótese principal: Angina Estavel        Pontuação: 17.5   Confiança: alta
  Conduta ... URGENTE - avaliação médica em até 24h
  Evidência . aperto no torax + melhora com repouso;
              dor no peito + subir escadas; dor no peito + melhora com repouso

  Diagnósticos diferenciais:
    - Infarto Agudo do Miocardio      8.0 pts
    - Angina Instavel                 3.0 pts
```

Cobertura de **100%**. Triagem resultante: 2 emergências, 5 urgentes, 3 de rotina.

#### Parte 2 — desempenho

| Modelo | Acurácia | Recall alto risco |
|--------|----------|-------------------|
| **Naive Bayes** (escolhido) | **86,5%** | **80,8%** |
| Regressão Logística | 84,6% | 76,9% |
| Árvore de Decisão | 73,1% | 61,5% |

Validação cruzada (5 divisões): **80,0% (±6,4%)** — a faixa honesta de desempenho.

<p align="center">
<img src="assets/fase2/matriz_confusao.png" alt="Matriz de confusão" width=45%>
</p>

Cinco casos graves passaram como baixo risco. Em triagem real isso é inaceitável sem
revisão humana — daí a zona cinzenta implementada na função `triar()` do notebook.

#### O achado: o modelo acerta pelo motivo errado

<p align="center">
<img src="assets/fase2/termos_decisivos.png" alt="Termos mais decisivos para o classificador" width=62%>
</p>

Os termos que mais empurram para **baixo risco** não são clínicos: são
*"leve"*, *"pouco"*, *"quando"*, *"quero"*. O modelo aprendeu a **intensidade do
advérbio**, não a gravidade do quadro. Do outro lado, "peito" aparece em 40,6% das frases
graves contra 11,5% das leves — um atalho lexical clássico.

Os erros no conjunto de teste confirmam, nas duas direções:

| Erro | Frase | O que revela |
|------|-------|--------------|
| falso **positivo** | *"mancha vermelha no braço sem dor nem febre"* | "braço" é termo forte da classe grave (vem de "irradia para o braço esquerdo") — e aqui não significa nada |
| falso **positivo** | *"pressão 12 por 8 na consulta de rotina"* | escalada pela palavra "pressão", ignorando que o valor é normal |
| falso **negativo** | *"ganhei quatro quilos em cinco dias e estou inchado"* | retenção hídrica — sinal clássico de IC descompensada, sem o vocabulário dominante |
| falso **negativo** | *"tive dois desmaios essa semana sem motivo aparente"* | síncope de repetição, urgente por definição |
| falso **negativo** | *"meu pulso está muito lento e estou com sono o dia todo"* | bradicardia sintomática |

#### Um segundo achado, metodológico

Escrevemos seis frases adversariais **de propósito** para derrubar o modelo — e ele
**passou nas seis**. Foram os erros reais do conjunto de teste que expuseram o problema,
sem esforço nenhum.

A lição ficou registrada no notebook em vez de ser apagada: quem escreve o teste do
próprio modelo tende a testar o viés que **imagina** ter, não o que o modelo **tem**.
Olhar os erros é mais honesto do que prever onde eles estariam.

#### O limiar de decisão é uma decisão clínica, não estatística

| Limiar | Acurácia | Recall alto risco | Casos graves perdidos | Alarmes falsos |
|--------|----------|-------------------|----------------------|----------------|
| 0,50 | 86,5% | 80,8% | **5** | 2 |
| 0,40 | 76,9% | 88,5% | 3 | 9 |
| 0,35 | 75,0% | 96,2% | **1** | 12 |
| 0,25 | 57,7% | 100,0% | **0** | 22 |

Cada linha é uma política de triagem diferente. Escolher uma é decidir quantos alarmes
falsos o serviço aguenta para não perder um infarto — conversa com o corpo clínico, não
com o time de dados sozinho.

#### Ligação com a Fase 1 e próximos passos

Na Fase 1 o achado foi que a coluna `target` do dataset estava **invertida** em relação à
convenção do UCI. Aqui, que o modelo acerta em parte pelo vocabulário. Mesmo tipo de
problema: **o número bonito na tela não prova que o sistema entendeu a questão.**

| Limitação atual | Para onde vai |
|-----------------|---------------|
| Atalho lexical | ampliar a base com dor torácica benigna e quadros graves sem dor |
| TF-IDF não entende contexto | *embeddings* em português (BERTimbau) |
| Sem explicabilidade | SHAP/LIME mostrando ao profissional qual trecho pesou |
| 1 em 5 casos graves perdidos | limiar calibrado com o corpo clínico + revisão humana |
| Métrica agregada esconde desigualdade | reportar desempenho **estratificado** por perfil |


---

## 📁 Estrutura de pastas

Dentre os arquivos e pastas presentes na raiz do projeto, definem-se:

- <b>.github</b>: Nesta pasta ficarão os arquivos de configuração específicos do GitHub que ajudam a gerenciar e automatizar processos no repositório.

- <b>assets</b>: aqui estão os arquivos relacionados a elementos não-estruturados deste repositório, como imagens. Contém o logo da FIAP e uma subpasta por fase: em `fase2/`, os gráficos gerados pelo notebook; em `fase3/`, os prints do Wokwi, do flow e do dashboard.

- <b>config</b>: Posicione aqui arquivos de configuração que são usados para definir parâmetros e ajustes do projeto. Contém o `requirements.txt` com as versões das bibliotecas Python.

- <b>document</b>: aqui estão todos os documentos do projeto que as atividades poderão pedir, em uma subpasta por fase. Em `fase3/`, os relatórios das Partes 1 e 2. Em `fase2/`, o notebook renderizado (`classificador_risco.html`, abre sem Jupyter), o roteiro do vídeo e, na subpasta "other", a saída completa da Parte 1 e o resultado em formato tabular.

- <b>scripts</b>: Posicione aqui scripts auxiliares para tarefas específicas do seu projeto. Contém o `fase2_executar_tudo.sh`, que roda as duas partes da Fase 2 de ponta a ponta.

- <b>src</b>: Todo o código fonte criado para o desenvolvimento do projeto ao longo das 7 fases, em uma subpasta por fase. Em `fase3/`: o firmware do ESP32 de cada parte (com o circuito do Wokwi) e o flow do Node-RED. Em `fase2/`: `dados/` (as quatro bases), `parte1_extracao/` (extrator de sintomas) e `parte2_classificador/` (notebook do classificador).

- <b>README.md</b>: arquivo que serve como guia e explicação geral sobre o projeto (o mesmo que você está lendo agora).

```
cardioia-fiap/
├── .github/
├── assets/
│   ├── logo-fiap.png
│   ├── fase2/
│   │   ├── matriz_confusao.png
│   │   └── termos_decisivos.png
│   └── fase3/                          prints do Wokwi, do flow e do dashboard
├── config/
│   └── requirements.txt
├── document/
│   ├── fase2/
│   │   ├── ROTEIRO_VIDEO.md
│   │   ├── classificador_risco.html
│   │   └── other/
│   │       ├── diagnosticos.csv
│   │       └── relatorio_parte1.txt
│   └── fase3/
│       ├── relatorio_parte1_edge.md
│       └── relatorio_parte2_mqtt_dashboard.md
├── scripts/
│   └── fase2_executar_tudo.sh
├── src/
│   ├── fase2/
│   │   ├── dados/
│   │   │   ├── frases_pacientes.txt    Parte 1 — 10 relatos de pacientes
│   │   │   ├── mapa_conhecimento.csv   Parte 1 — ontologia (63 regras)
│   │   │   ├── sinonimos.csv           Parte 1 — 162 expressões → sintoma canônico
│   │   │   └── frases_risco.csv        Parte 2 — 205 frases rotuladas
│   │   ├── parte1_extracao/
│   │   │   └── extrator_sintomas.py
│   │   └── parte2_classificador/
│   │       └── classificador_risco.ipynb
│   └── fase3/
│       ├── parte1_edge/                ESP32: coleta, alertas locais e fila offline
│       │   ├── sketch.ino
│       │   ├── diagram.json
│       │   └── libraries.txt
│       ├── parte2_mqtt/                ESP32: + Wi-Fi, MQTT, Last Will e NTP
│       │   ├── sketch.ino
│       │   ├── diagram.json
│       │   └── libraries.txt
│       └── node-red/
│           └── flows.json              dashboard: gráficos, gauges, alerta e notificação
├── .gitattributes
├── .gitignore
└── README.md
```


## 🗃 Histórico de lançamentos

* 0.3.0 - 03/11/2026
    * Fase 3 completa: wearable ESP32 com fila offline em SPIFFS (Parte 1) e MQTT + dashboard Node-RED (Parte 2)
    * Repositório renomeado para `cardioia-fiap` e reorganizado com uma subpasta por fase
* 0.2.0 - 06/10/2026
    * Reorganização do repositório no template oficial FIAP
    * Gráficos exportados para `assets` e notebook renderizado em `document`
* 0.1.0 - 06/10/2026
    * Fase 2 completa: extrator de sintomas (Parte 1) e classificador de risco (Parte 2)
    * Mapa de conhecimento com 63 regras e 162 expressões mapeadas
    * Base de 205 frases rotuladas, TF-IDF e comparação de três modelos
    * Análise de viés, teste adversarial e calibração de limiar


## 📋 Licença

<img style="height:22px!important;margin-left:3px;vertical-align:text-bottom;" src="https://mirrors.creativecommons.org/presskit/icons/cc.svg?ref=chooser-v1"><img style="height:22px!important;margin-left:3px;vertical-align:text-bottom;" src="https://mirrors.creativecommons.org/presskit/icons/by.svg?ref=chooser-v1"><p xmlns:cc="http://creativecommons.org/ns#" xmlns:dct="http://purl.org/dc/terms/"><a property="dct:title" rel="cc:attributionURL" href="https://github.com/agodoi/template">MODELO GIT FIAP</a> por <a rel="cc:attributionURL dct:creator" property="cc:attributionName" href="https://fiap.com.br">Fiap</a> está licenciado sobre <a href="http://creativecommons.org/licenses/by/4.0/?ref=chooser-v1" target="_blank" rel="license noopener noreferrer" style="display:inline-block;">Attribution 4.0 International</a>.</p>
