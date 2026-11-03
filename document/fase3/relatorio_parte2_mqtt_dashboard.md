# CardioIA — Fase 3 · Parte 2
## Relatório: comunicação MQTT e dashboard Node-RED (Fog/Cloud Computing)

**Grupo 89** · Gabriel Coppola · Tutor: Leonardo Ruiz
Código: `src/fase3/parte2_mqtt/` (ESP32) e `src/fase3/node-red/flows.json` (dashboard)

---

### 1. Arquitetura

```
   EDGE (ESP32 / Wokwi)                 CLOUD                    FOG (Node-RED)                 USUÁRIO
 ┌──────────────────────┐  publish   ┌──────────────┐ subscribe ┌──────────────────────┐     ┌───────────────┐
 │ DHT22 + botão (BPM)  │──────────▶ │ broker MQTT  │──────────▶│ reavalia limiares    │────▶│ dashboard /ui │
 │ alertas locais (LED) │  .../vitais│ HiveMQ       │           │ separa ao vivo x     │     │ gráfico, gauge│
 │ fila SPIFFS offline  │  .../status│ (Last Will)  │           │ recuperado da fila   │     │ alerta, toast │
 └──────────────────────┘            └──────────────┘           └──────────────────────┘     └───────────────┘
```

Cada camada tem uma responsabilidade que as outras não cobrem:

- A **borda** (ESP32) coleta, alerta localmente e **guarda tudo** enquanto não houver
  rede. É a única camada que funciona com a internet caída.
- O **broker** desacopla quem produz de quem consome. O ESP32 não sabe quantos
  dashboards existem, e um dashboard novo pode assinar os tópicos sem mudar o firmware.
- O **fog** (Node-RED) concentra a regra clínica e a apresentação. Ajustar um limiar ali
  não exige regravar centenas de dispositivos.

### 2. Por que MQTT

MQTT é um protocolo publish/subscribe sobre TCP feito para dispositivos limitados. O
cabeçalho mínimo tem 2 bytes, a conexão é mantida aberta (sem o handshake por mensagem
que o HTTP exige), e o protocolo oferece dois recursos que este projeto usa diretamente:

- **Mensagem retida.** O broker guarda a última mensagem do tópico de status e a entrega
  a quem assinar depois. Um dashboard aberto agora já sabe se o dispositivo está online.
- **Last Will and Testament (LWT).** Na conexão, o ESP32 registra no broker a mensagem
  `offline`. Se a conexão cair sem um `DISCONNECT`, o **broker** publica essa mensagem em
  nome do dispositivo. É a única forma de saber que um aparelho sumiu, porque um aparelho
  sem rede não consegue avisar que ficou sem rede.

### 3. Tópicos e mensagens

| Tópico | Conteúdo | QoS | Retido |
|---|---|---|---|
| `cardioia/g89/p001/vitais` | 1 leitura por mensagem (JSON) | 0 | não |
| `cardioia/g89/p001/status` | `online` (publicado pelo ESP32) ou `offline` (Last Will) | 1 | sim |

A hierarquia `projeto/grupo/paciente/tipo` permite assinar com curingas, por exemplo
`cardioia/g89/+/status` para ver o status de todos os pacientes do grupo. O paciente é
identificado por um **código** (`p001`), nunca por nome ou documento.

Exemplo de payload em `.../vitais`:

```json
{"paciente":"p001","seq":4182,"ts":1793710455,"temp":38.4,"umid":55.0,"bpm":134,
 "alertas":["TAQUICARDIA","FEBRE"],"atrasado":false,"pendentes":0}
```

| Campo | Significado |
|---|---|
| `seq` | número sequencial da amostra; permite detectar lacunas e descartar duplicatas |
| `ts` | horário da **coleta** (epoch UTC). É reconstruído no envio como "agora (NTP) − idade da amostra". `0` se o NTP ainda não sincronizou |
| `temp`, `umid` | `null` quando o DHT22 falha, em vez de um zero que pareceria hipotermia |
| `bpm` | média dos últimos 5 intervalos entre batimentos; `0` = sem pulso |
| `alertas` | alertas avaliados na borda |
| `atrasado` | `true` se a amostra ficou na fila offline (idade > 2 ciclos de leitura) |
| `pendentes` | quantas amostras ainda estão na fila do ESP32 |

**Sobre o QoS.** A biblioteca PubSubClient publica apenas com QoS 0, isto é, sem
confirmação do broker. A garantia de entrega deste projeto **não depende do QoS**: ela
vem da fila local. Uma amostra só sai da fila quando a publicação é aceita pela conexão
TCP, e qualquer queda devolve o envio para a fila. O status usa QoS 1 e é retido porque
é o estado que um dashboard recém-aberto precisa receber.

### 4. Ciclo de comunicação no firmware

1. **Wi-Fi.** O ESP32 conecta à rede `Wokwi-GUEST` (no canal 6, que acelera a conexão no
   simulador) e reconecta sozinho se a rede cair.
2. **Relógio.** Com o Wi-Fi ativo, o NTP (`pool.ntp.org`) é iniciado uma única vez.
3. **Broker.** O firmware chama `mqtt.connect()` registrando o Last Will
   (`.../status` = `offline`, QoS 1, retido) e, se a conexão der certo, publica `online`
   retido. As tentativas são espaçadas a cada 5 s para não travar o loop.
4. **Publicação.** A cada volta do loop, o firmware envia até 10 amostras da fila a cada
   100 ms (~100 msg/s). Em operação normal a fila tem uma amostra e o envio acontece em
   menos de 100 ms após a coleta. Depois de uma noite offline, as 14.400 amostras chegam
   ao broker em ~2,5 min.
5. **Queda simulada.** A chave deslizante representa a cobertura de rede do paciente. Ao
   desligá-la, o firmware fecha o socket TCP **sem enviar DISCONNECT**, exatamente como
   numa queda real. O broker detecta o fechamento e publica o Last Will `offline`. O
   dashboard fica cinza enquanto a coleta continua e a fila cresce.
6. **Retorno.** Ao religar a chave, o ESP32 reconecta, publica `online` e esvazia a fila.
   As amostras antigas saem com `atrasado: true`.

### 5. Configuração do broker

O padrão do código e do flow é o **broker público da HiveMQ** (`broker.hivemq.com:1883`).
Ele não exige cadastro e funciona tanto no Wokwi quanto no Node-RED local, por isso é
adequado à demonstração. **Ele não é adequado a dados reais**: qualquer pessoa que
conheça o tópico pode ler e publicar.

Para usar o **HiveMQ Cloud** (plano gratuito):

1. Crie um cluster em console.hivemq.cloud e, em *Access Management*, um usuário e senha.
2. No `sketch.ino`, ajuste `#define USAR_TLS 1`, `MQTT_HOST` (o host do cluster),
   `MQTT_USUARIO` e `MQTT_SENHA`. A porta passa a ser 8883 (MQTT sobre TLS).
3. No Node-RED, edite o nó de configuração do broker com o mesmo host, porta 8883,
   *Use TLS* marcado e as credenciais na aba *Security*.

### 6. Dashboard no Node-RED

**Pré-requisitos.** Node-RED 3.x ou superior e a paleta `node-red-dashboard`
(*Menu → Manage palette → Install*). Importe `src/fase3/node-red/flows.json`
(*Menu → Import*), clique em *Deploy* e abra `http://localhost:1880/ui`.

**Nós do flow**

| Nó | Tipo | Função |
|---|---|---|
| vitais / status | `mqtt in` | Assinam os dois tópicos; `vitais` já converte o JSON |
| Avaliar sinais (fog) | `function` (7 saídas) | Reavalia limiares, separa ao vivo de recuperado, decide quando notificar |
| Status do dispositivo | `function` (2 saídas) | Traduz `online`/`offline` e acinzenta o indicador |
| Gráfico BPM / temperatura | `ui_chart` | Linha dos últimos 10 min |
| Medidor temperatura / BPM | `ui_gauge` | Faixas verde/amarela/vermelha (37,5 e 38 °C; 100 e 120 bpm) |
| Indicador de alerta | `ui_template` | "LED virtual" verde, vermelho ou cinza com a descrição do alerta |
| Notificação de alerta | `ui_toast` | Pop-up no canto da tela |
| Resumo da fila offline | `ui_text` | Progresso e resultado da sincronização |

**Lógica da função "Avaliar sinais".** Os limiares (BPM > 120 ou < 50, temperatura
> 38 °C ou < 35 °C, BPM 0 = sem pulso) são **reavaliados no fog**, sem confiar nos
alertas calculados pelo dispositivo. Isso centraliza a regra clínica: mudar um limiar é
mudar uma linha no Node-RED, e não regravar o firmware de todos os pacientes. O alerta da
borda (LED físico) continua existindo para o caso de não haver rede.

Duas decisões evitam que o dashboard engane quem o observa:

1. **Amostras atrasadas não mexem no estado atual.** Uma leitura de 3 h atrás que chega
   na sincronização não pode acender o indicador vermelho agora, nem mover o gauge. Ela
   alimenta só o painel *Resiliência offline*, que mostra o progresso
   (`Sincronizando: 1.200 recuperadas (3 com alerta), 13.200 ainda na fila`) e, ao final,
   o resumo com o último alerta ocorrido offline e o horário em que aconteceu. É essa
   informação que o médico precisa: houve um episódio de taquicardia às 02:13 enquanto o
   aparelho estava sem rede.
2. **A notificação dispara só na transição para alerta.** Com leituras a cada 2 s, um
   alerta contínuo geraria 30 pop-ups por minuto, e um alarme que toca sem parar acaba
   sendo ignorado. O estado anterior fica no contexto do flow e é compartilhado com a
   função de status, de modo que um alerta logo após o dispositivo voltar do offline
   notifica de novo.

**Layout (aba "CardioIA - Monitoramento")**

1. *Paciente p001 – estado atual*: status do dispositivo e indicador de alerta.
2. *Medidores*: gauges de temperatura e frequência cardíaca, lado a lado.
3. *Tendência*: gráficos de BPM e temperatura.
4. *Resiliência offline*: resumo da última sincronização.

### 7. Validação

- A lógica das duas funções foi testada com mensagens simuladas cobrindo leitura normal,
  taquicardia, febre com taquicardia, falha de sensor, dispositivo offline, sincronização
  de amostras atrasadas e payload inválido. Todos os casos produziram as saídas esperadas.
- O flow foi carregado em um Node-RED 5 com `node-red-dashboard` 3.6 sem erros, e um
  teste ponta a ponta com um broker MQTT local, publicando no mesmo formato do ESP32,
  confirmou o caminho tópico → função → gráfico, gauge, indicador e notificação.

### 8. Segurança, eficiência e boas práticas em IoT médico

**Segurança e privacidade.** Dados de saúde são **dados pessoais sensíveis** pela LGPD
(art. 5º, II). O que este protótipo já faz:

- Identifica o paciente por pseudônimo (`p001`); a tabela que liga o código à pessoa
  fica fora do fluxo IoT.
- Minimiza dados: o payload carrega só sinais vitais, sem nome, localização ou
  identificador do aparelho.
- Prevê TLS e autenticação (`USAR_TLS`, usuário e senha) para o HiveMQ Cloud.

O que um produto real exigiria além disso:

- TLS com a **CA fixada** no firmware, já que o `setInsecure()` do protótipo aceita
  qualquer certificado.
- Credencial **por dispositivo** e ACL no broker, para que `p001` só publique em
  `.../p001/#`.
- Autenticação no dashboard (`httpNodeAuth` no `settings.js` do Node-RED).
- Registro de auditoria de quem acessou cada dado.

**Eficiência.** Cada mensagem tem ~200 bytes de payload, cerca de 280 bytes com os
cabeçalhos MQTT/TCP/IP. São 43.200 mensagens por dia, ou **~12 MB/dia por paciente**,
compatível com redes móveis. Se for preciso reduzir, os caminhos são agrupar amostras
por mensagem ou usar um formato binário (CBOR) no lugar de JSON. Na borda, a interrupção
para o batimento e o envio em lotes mantêm o loop responsivo. Em hardware real, o
*modem sleep* do Wi-Fi entre leituras reduziria o consumo de bateria.

**Confiabilidade.** A entrega é "pelo menos uma vez": a fila garante que nada se perde,
e o `seq` permite que a nuvem descarte duplicatas. O Last Will garante que o silêncio de
um dispositivo **aparece** no dashboard em vez de passar despercebido.

### 9. Evidências

Os prints da aplicação estão em `assets/fase3/`:

- `wokwi_circuito.png`: circuito no Wokwi com o Monitor Serial mostrando leituras e publicações MQTT
- `nodered_flow.png`: o flow no editor do Node-RED
- `dashboard_normal.png`: dashboard com sinais dentro dos limites
- `dashboard_alerta.png`: dashboard em alerta (taquicardia e/ou febre), com a notificação
- `dashboard_offline.png`: dispositivo offline (indicador cinza) e o resumo da sincronização após reconectar
