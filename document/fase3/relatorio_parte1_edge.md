# CardioIA — Fase 3 · Parte 1
## Relatório: armazenamento e processamento local (Edge Computing)

**Grupo 66** · Gabriel Coppola · Tutor: Leonardo Ruiz
Código: `src/fase3/parte1_edge/` (`sketch.ino`, `diagram.json`, `libraries.txt`)

---

### 1. Cenário

O protótipo simula um **wearable de monitoramento cardíaco domiciliar** para pacientes
recém-saídos de internação cardiológica. É nesse período que mais ocorrem reinternações,
e é também quando o paciente fica longe da estrutura do hospital. O dispositivo precisa
continuar medindo e alertando **mesmo sem internet**: o roteador cai, o paciente sai de
casa, o celular fica sem bateria. A rede é tratada como algo que **pode faltar**, e o
dispositivo é projetado para funcionar nessa condição.

### 2. Hardware simulado (Wokwi)

| Componente | Pino | Papel |
|---|---|---|
| DHT22 | GPIO 15 | Temperatura (simulando um sensor de pele) e umidade |
| Botão vermelho | GPIO 4 | Cada aperto é um batimento; o ESP32 calcula o BPM |
| Chave deslizante | GPIO 19 | Variável booleana de conectividade Wi-Fi |
| LED vermelho | GPIO 26 | Alerta clínico local |
| LED verde | GPIO 27 | Dispositivo online |

### 3. Fluxo de funcionamento

```
 a cada 2 s
 ┌──────────┐   ┌──────────────────┐   ┌──────────────┐   chave ON   ┌──────────────────┐
 │  coleta  │──▶│ avaliação local  │──▶│ fila SPIFFS  │─────────────▶│ envio "nuvem"    │
 │ DHT22+BPM│   │ (limiares, LED)  │   │ /fila.csv    │              │ Serial.println   │
 └──────────┘   └──────────────────┘   └──────────────┘              └────────┬─────────┘
       ▲                                       ▲   chave OFF: a fila só cresce │ fila vazia
       │ interrupção do botão                  └───────────────────────────────┘ → arquivo apagado
```

1. **Batimentos.** O botão gera uma interrupção (`attachInterrupt`, borda de descida).
   Assim nenhum aperto se perde enquanto o loop grava no SPIFFS. A rotina descarta
   pulsos com menos de 250 ms do anterior (período refratário, o que limita a leitura
   a 240 bpm e filtra ruído) e guarda os últimos 5 intervalos entre batimentos (IBI).
   O BPM é `60000 / média dos IBIs`. Sem batimento por 5 s, o BPM vai a zero, o que
   indica sensor solto ou ausência de pulso.
2. **Coleta.** A cada 2 s (o período mínimo do DHT22), o firmware monta uma amostra com
   número sequencial, instante, temperatura, umidade, BPM e alertas.
3. **Processamento na borda.** Os limiares definidos pelo grupo são avaliados no
   próprio ESP32, e o **LED vermelho acende sem depender da rede**:

   | Alerta | Regra |
   |---|---|
   | Taquicardia | BPM > 120 |
   | Bradicardia | BPM < 50 |
   | Sem pulso | nenhum batimento em 5 s |
   | Febre | temperatura > 38,0 °C |
   | Hipotermia | temperatura < 35,0 °C |
   | Falha do sensor | o DHT22 não respondeu |

   Os alertas são gravados como bits de um byte, porque uma amostra pode ter vários ao
   mesmo tempo (taquicardia e febre, por exemplo).
4. **Armazenamento.** Toda amostra é anexada a `/fila.csv`, **conectado ou não**. Esse é
   o padrão *store-and-forward*. Existe um único caminho de dados, então o envio é sempre
   o mesmo código, testado em todas as execuções, e não um caminho de exceção que só roda
   quando a rede cai.
5. **Sincronização.** Com a chave em "conectado", o firmware lê a fila a partir do ponto
   em que parou e envia cada amostra pelo `Serial.println` em JSON, no mesmo formato que
   a Parte 2 publica via MQTT. O envio acontece em lotes de 20 por volta do loop, para que
   uma sincronização longa não atrase a coleta. Quando a fila esvazia, **o arquivo local
   é apagado**.

### 4. Lógica de resiliência

**Coleta contínua offline.** Com a chave em "desconectado", nada muda na coleta: as
amostras continuam entrando na fila, e o log mostra a fila crescendo
(`rede: OFFLINE | fila: 37`). Ao reconectar, o firmware informa quantas amostras vai
sincronizar e envia todas, em ordem.

**Envio sem reescrever o arquivo.** O firmware guarda um *offset*, a posição em bytes da
próxima linha a enviar. Cada lote começa nesse ponto, e o arquivo só é apagado quando o
offset chega ao fim. Se o arquivo fosse reescrito a cada lote, sincronizar uma noite
inteira custaria milhares de regravações na flash. O offset só avança depois que o envio
é confirmado. Na Parte 2, se o broker recusar a mensagem, a próxima tentativa recomeça
da linha que falhou.

**Estratégia de armazenamento limitado.** A capacidade foi dimensionada pelo modelo de
negócio. A pior janela offline que o produto precisa cobrir é **uma noite de sono**:

> 8 h × 3.600 s ÷ 2 s por amostra = **14.400 amostras**
> 14.400 × ~30 bytes por linha ≈ **430 KB**. Cabe com folga na partição SPIFFS padrão do
> ESP32 (~1,4 MB), inclusive com a cópia temporária usada na compactação.

Ao atingir o limite, o firmware **não para de coletar e não descarta a amostra nova**.
Ele compacta a fila: descarta de uma vez **10% das amostras normais mais antigas** e
**preserva as que tiveram alerta**. A lógica clínica é que uma noite com 1.440 leituras
normais a menos ainda conta a mesma história, mas um episódio de taquicardia perdido não
pode ser recuperado. Só se a fila inteira for de alertas o firmware descarta alertas
antigos. O descarte é feito em lote porque compactar a cada amostra reescreveria 430 KB
a cada 2 s.

**Monitor Serial como registro alternativo.** Se o SPIFFS não montar, cada leitura é
impressa com o prefixo `[REGISTRO-SERIAL]`. É a alternativa sugerida no enunciado para
os simuladores.

### 5. Limitação do simulador

No Wokwi o SPIFFS funciona **durante** a simulação. Por isso dá para observar a fila
crescer, ser enviada e o arquivo ser apagado. Mas ele é volátil: ao parar a simulação,
tudo se perde. Em um ESP32 físico, a função `carregarFilaExistente()` recupera a fila
após um reinício, recontando as amostras pendentes e continuando a numeração.

### 6. Como demonstrar

1. Inicie a simulação com a chave em "desconectado" e aperte o botão a ~1 vez/s:
   `BPM≈60`, LED vermelho apagado, fila crescendo.
2. Aperte mais rápido (2+ vezes/s): o log mostra `TAQUICARDIA` e o LED vermelho acende.
3. Clique no DHT22 e suba a temperatura acima de 38 °C: o log mostra `FEBRE`.
4. Mude a chave para "conectado": as linhas `[NUVEM] {...}` saem em ordem, com `atraso_s`
   crescente para as mais antigas, seguidas de
   `[FILA] sincronizacao concluida - arquivo local apagado`.
5. Para ver a compactação sem esperar 8 h, altere `MAX_AMOSTRAS` para `20`.

### 7. O que mudaria em um produto real

- **Desgaste da flash.** Gravar a cada 2 s gera 43.200 escritas por dia. O próximo
  passo é acumular ~30 amostras em RAM e gravar em bloco, aceitando perder no máximo
  1 minuto se a bateria acabar.
- **Relógio.** `millis()` zera a cada reinício. Em hardware real, a amostra deve levar
  o horário de um RTC ou do NTP sincronizado. A Parte 2 já reconstrói o horário via NTP.
- **Entrega "pelo menos uma vez".** Após um reinício, uma amostra pode ser reenviada. O
  campo `seq` permite que a nuvem descarte duplicatas.
