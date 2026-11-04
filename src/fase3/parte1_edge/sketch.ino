/*
 * CardioIA - Fase 3 | Parte 1: Armazenamento e processamento local (Edge Computing)
 * Grupo 66
 *
 * Simula um wearable cardiaco no ESP32:
 *   - DHT22 ............. temperatura (simulando sensor de pele) e umidade
 *   - botao vermelho .... cada aperto = 1 batimento; o ESP32 calcula o BPM
 *   - chave deslizante .. simula a conectividade Wi-Fi (variavel booleana)
 *   - LED vermelho ...... alerta clinico local (funciona mesmo sem rede)
 *   - LED verde ......... dispositivo "online"
 *
 * Fluxo (store-and-forward):
 *   leitura -> avaliacao de alertas na borda -> fila local (SPIFFS)
 *           -> [se conectado] envio para a "nuvem" via Serial.println
 *           -> arquivo local apagado quando a fila esvazia
 *
 * Toda leitura passa pela fila, conectado ou nao. Assim existe um unico caminho
 * de dados e nenhuma amostra depende de a rede estar disponivel no instante da
 * coleta. Se o SPIFFS nao montar, o Monitor Serial vira o registro offline.
 */

#include <Arduino.h>
#include <FS.h>
#include <SPIFFS.h>
#include <DHTesp.h>

// ----------------------------------------------------------------------- pinos
constexpr uint8_t PINO_DHT        = 15;
constexpr uint8_t PINO_BATIMENTO  = 4;
constexpr uint8_t PINO_CONEXAO    = 19;
constexpr uint8_t PINO_LED_ALERTA = 26;
constexpr uint8_t PINO_LED_ONLINE = 27;

// ------------------------------------------------------------------ amostragem
// 2 s e o periodo minimo de leitura do DHT22.
constexpr uint32_t INTERVALO_LEITURA_MS = 2000;

// ------------------------------------------------------ armazenamento limitado
// Modelo de negocio: monitoramento domiciliar de paciente pos-alta cardiologica.
// A maior janela sem conexao que o produto precisa cobrir e uma noite inteira
// (roteador desligado, paciente dormindo fora de casa):
//   8 h x 3600 s / 2 s = 14.400 amostras  (~30 bytes cada = ~430 KB no SPIFFS)
// Ao atingir o limite, descarta-se em lote 10% das amostras NORMAIS mais antigas.
// Amostras com alerta so sao descartadas se a fila inteira for de alertas.
// Para ver a compactacao no simulador, reduza MAX_AMOSTRAS para 20.
constexpr uint32_t MAX_AMOSTRAS  = 14400;
constexpr uint32_t DESCARTE_LOTE = MAX_AMOSTRAS / 10;

// Envio em lotes para nao travar a coleta durante uma sincronizacao longa.
constexpr uint16_t LOTE_ENVIO = 20;

// ------------------------------------------------------ deteccao de batimento
// 250 ms de periodo refratario = teto de 240 bpm; tambem absorve ruido do botao.
constexpr uint32_t REFRATARIO_MS = 250;
// Sem batimento por 5 s, o BPM vai a zero (sem pulso / sensor solto).
constexpr uint32_t SEM_PULSO_MS  = 5000;
// O BPM e a media dos ultimos 5 intervalos entre batimentos (IBI).
constexpr uint8_t  JANELA_IBI    = 5;

// ---------------------------------------------- limiares definidos pelo grupo
constexpr uint16_t BPM_TAQUICARDIA = 120;
constexpr uint16_t BPM_BRADICARDIA = 50;
constexpr float    TEMP_FEBRE      = 38.0f;
constexpr float    TEMP_HIPOTERMIA = 35.0f;

// Alertas sao bits: uma amostra pode ter varios ao mesmo tempo.
enum Alerta : uint8_t {
  ALERTA_TAQUICARDIA = 1 << 0,
  ALERTA_BRADICARDIA = 1 << 1,
  ALERTA_SEM_PULSO   = 1 << 2,
  ALERTA_FEBRE       = 1 << 3,
  ALERTA_HIPOTERMIA  = 1 << 4,
  ALERTA_FALHA_DHT   = 1 << 5,
};

struct NomeAlerta {
  uint8_t bit;
  const char* nome;
};

const NomeAlerta NOMES_ALERTA[] = {
  {ALERTA_TAQUICARDIA, "TAQUICARDIA"},
  {ALERTA_BRADICARDIA, "BRADICARDIA"},
  {ALERTA_SEM_PULSO,   "SEM_PULSO"},
  {ALERTA_FEBRE,       "FEBRE"},
  {ALERTA_HIPOTERMIA,  "HIPOTERMIA"},
  {ALERTA_FALHA_DHT,   "FALHA_SENSOR_TEMP"},
};

struct Amostra {
  uint32_t seq;          // numero sequencial: permite deduplicar na nuvem
  uint32_t instanteMs;   // millis() da coleta
  float    temperatura;
  float    umidade;
  uint16_t bpm;
  uint8_t  alertas;
};

const char* ARQ_FILA = "/fila.csv";
const char* ARQ_TEMP = "/fila.tmp";

DHTesp dht;

bool     fsDisponivel    = false;
uint32_t pendentes       = 0;   // amostras na fila ainda nao enviadas
uint32_t offsetEnvio     = 0;   // posicao (bytes) da proxima linha a enviar
uint32_t proximoSeq      = 1;
uint32_t ultimaLeituraMs = 0;
bool     onlineAnterior  = false;

// ------------------------------------------------- batimentos (interrupcao)
portMUX_TYPE muxBatimento = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t ultimoBatimentoMs = 0;
volatile uint32_t ibis[JANELA_IBI];
volatile uint8_t  idxIbi = 0;
volatile uint8_t  qtdIbi = 0;

// Usar interrupcao (e nao leitura no loop) garante que nenhum aperto se perca
// enquanto o loop esta ocupado gravando no SPIFFS ou sincronizando a fila.
void IRAM_ATTR aoDetectarBatimento() {
  uint32_t agora = millis();
  portENTER_CRITICAL_ISR(&muxBatimento);
  uint32_t ibi = agora - ultimoBatimentoMs;
  if (ultimoBatimentoMs == 0 || ibi >= REFRATARIO_MS) {
    if (ultimoBatimentoMs != 0 && ibi < SEM_PULSO_MS) {
      ibis[idxIbi] = ibi;
      idxIbi = (idxIbi + 1) % JANELA_IBI;
      if (qtdIbi < JANELA_IBI) qtdIbi++;
    } else {
      // Pausa longa: o ritmo anterior nao representa mais o paciente.
      qtdIbi = 0;
      idxIbi = 0;
    }
    ultimoBatimentoMs = agora;
  }
  portEXIT_CRITICAL_ISR(&muxBatimento);
}

uint16_t calcularBpm() {
  portENTER_CRITICAL(&muxBatimento);
  uint32_t ultimo = ultimoBatimentoMs;
  uint8_t  qtd    = qtdIbi;
  uint32_t soma   = 0;
  for (uint8_t i = 0; i < qtd; i++) soma += ibis[i];
  portEXIT_CRITICAL(&muxBatimento);

  if (ultimo == 0 || millis() - ultimo > SEM_PULSO_MS || qtd < 2) return 0;
  return (uint16_t)(60000UL * qtd / soma);
}

// ------------------------------------------------------- processamento local
uint8_t avaliarAlertas(float temperatura, uint16_t bpm, bool dhtOk) {
  uint8_t alertas = 0;

  if (bpm == 0)                    alertas |= ALERTA_SEM_PULSO;
  else if (bpm > BPM_TAQUICARDIA)  alertas |= ALERTA_TAQUICARDIA;
  else if (bpm < BPM_BRADICARDIA)  alertas |= ALERTA_BRADICARDIA;

  if (!dhtOk)                              alertas |= ALERTA_FALHA_DHT;
  else if (temperatura > TEMP_FEBRE)       alertas |= ALERTA_FEBRE;
  else if (temperatura < TEMP_HIPOTERMIA)  alertas |= ALERTA_HIPOTERMIA;

  return alertas;
}

String descreverAlertas(uint8_t alertas) {
  if (alertas == 0) return "nenhum";
  String texto;
  for (const NomeAlerta& n : NOMES_ALERTA) {
    if (alertas & n.bit) {
      if (texto.length()) texto += ", ";
      texto += n.nome;
    }
  }
  return texto;
}

Amostra coletar() {
  TempAndHumidity th = dht.getTempAndHumidity();
  bool dhtOk = dht.getStatus() == DHTesp::ERROR_NONE;

  Amostra a;
  a.seq         = proximoSeq++;
  a.instanteMs  = millis();
  a.temperatura = dhtOk ? th.temperature : 0.0f;
  a.umidade     = dhtOk ? th.humidity : 0.0f;
  a.bpm         = calcularBpm();
  a.alertas     = avaliarAlertas(a.temperatura, a.bpm, dhtOk);
  return a;
}

// --------------------------------------------------------------- serializacao
// Formato da linha no SPIFFS: seq,instante_ms,temperatura,umidade,bpm,alertas
String paraCsv(const Amostra& a) {
  char linha[64];
  snprintf(linha, sizeof(linha), "%lu,%lu,%.1f,%.1f,%u,%u",
           (unsigned long)a.seq, (unsigned long)a.instanteMs,
           a.temperatura, a.umidade, a.bpm, a.alertas);
  return String(linha);
}

bool deCsv(const String& linha, Amostra& a) {
  unsigned long seq, instante;
  float temperatura, umidade;
  unsigned bpm, alertas;
  if (sscanf(linha.c_str(), "%lu,%lu,%f,%f,%u,%u",
             &seq, &instante, &temperatura, &umidade, &bpm, &alertas) != 6) {
    return false;
  }
  a.seq = seq;
  a.instanteMs = instante;
  a.temperatura = temperatura;
  a.umidade = umidade;
  a.bpm = bpm;
  a.alertas = alertas;
  return true;
}

uint8_t alertasDaLinha(const String& linha) {
  return (uint8_t)linha.substring(linha.lastIndexOf(',') + 1).toInt();
}

// Mesmo formato JSON que a Parte 2 publica no MQTT.
String paraJson(const Amostra& a) {
  String alertas = "[";
  for (const NomeAlerta& n : NOMES_ALERTA) {
    if (a.alertas & n.bit) {
      if (alertas.length() > 1) alertas += ',';
      alertas += '"';
      alertas += n.nome;
      alertas += '"';
    }
  }
  alertas += ']';

  char temperatura[8] = "null";
  char umidade[8]     = "null";
  if (!(a.alertas & ALERTA_FALHA_DHT)) {
    snprintf(temperatura, sizeof(temperatura), "%.1f", a.temperatura);
    snprintf(umidade, sizeof(umidade), "%.1f", a.umidade);
  }

  char json[224];
  snprintf(json, sizeof(json),
           "{\"paciente\":\"p001\",\"seq\":%lu,\"atraso_s\":%lu,"
           "\"temp\":%s,\"umid\":%s,\"bpm\":%u,\"alertas\":%s}",
           (unsigned long)a.seq,
           (unsigned long)((millis() - a.instanteMs) / 1000),
           temperatura, umidade, a.bpm, alertas.c_str());
  return String(json);
}

// ---------------------------------------------------- fila local (SPIFFS)
void carregarFilaExistente() {
  File f = SPIFFS.open(ARQ_FILA, FILE_READ);
  if (!f) return;
  uint32_t maiorSeq = 0;
  while (f.available()) {
    String linha = f.readStringUntil('\n');
    Amostra a;
    if (linha.length() && deCsv(linha, a)) {
      pendentes++;
      if (a.seq > maiorSeq) maiorSeq = a.seq;
    }
  }
  f.close();
  proximoSeq = maiorSeq + 1;
  if (pendentes) {
    Serial.printf("[FILA] %lu amostras de uma execucao anterior aguardando envio\n",
                  (unsigned long)pendentes);
  }
}

// Reescreve a fila sem as amostras mais antigas. Retorna quantas descartou.
uint32_t compactarFila(bool preservarAlertas) {
  File origem  = SPIFFS.open(ARQ_FILA, FILE_READ);
  File destino = SPIFFS.open(ARQ_TEMP, FILE_WRITE);
  if (!origem || !destino) return 0;

  origem.seek(offsetEnvio);   // o que ja foi enviado nao entra na copia
  uint32_t descartadas = 0;
  uint32_t mantidas    = 0;
  while (origem.available()) {
    String linha = origem.readStringUntil('\n');
    if (linha.length() == 0) continue;
    bool descartar = descartadas < DESCARTE_LOTE &&
                     (!preservarAlertas || alertasDaLinha(linha) == 0);
    if (descartar) {
      descartadas++;
      continue;
    }
    destino.print(linha);
    destino.print('\n');
    mantidas++;
  }
  origem.close();
  destino.close();

  SPIFFS.remove(ARQ_FILA);
  SPIFFS.rename(ARQ_TEMP, ARQ_FILA);
  offsetEnvio = 0;
  pendentes   = mantidas;
  return descartadas;
}

void abrirEspacoNaFila() {
  uint32_t descartadas = compactarFila(true);
  if (descartadas == 0) descartadas = compactarFila(false);  // fila so de alertas
  Serial.printf("[FILA] limite de %lu amostras atingido: %lu normais mais antigas "
                "descartadas, alertas preservados\n",
                (unsigned long)MAX_AMOSTRAS, (unsigned long)descartadas);
}

void armazenar(const Amostra& a) {
  String linha = paraCsv(a);

  if (!fsDisponivel) {
    Serial.println("[REGISTRO-SERIAL] " + linha);
    return;
  }

  if (pendentes >= MAX_AMOSTRAS) abrirEspacoNaFila();

  File f = SPIFFS.open(ARQ_FILA, FILE_APPEND);
  if (!f) {
    Serial.println("[REGISTRO-SERIAL] " + linha);
    return;
  }
  // print + '\n' (e nao println) para nao gravar '\r' no arquivo.
  f.print(linha);
  f.print('\n');
  f.close();
  pendentes++;
}

// ------------------------------------------------------ envio para a nuvem
// Nesta parte a "nuvem" e o Monitor Serial, como pede o enunciado.
bool enviarParaNuvem(const Amostra& a) {
  Serial.print("[NUVEM] ");
  Serial.println(paraJson(a));
  return true;
}

void sincronizarFila() {
  if (!fsDisponivel || pendentes == 0) return;

  File f = SPIFFS.open(ARQ_FILA, FILE_READ);
  if (!f) {
    pendentes = 0;
    offsetEnvio = 0;
    return;
  }

  // Envia a partir de onde parou: a fila e lida sequencialmente e so e apagada
  // quando esvazia, sem reescrever o arquivo a cada lote.
  f.seek(offsetEnvio);
  uint16_t enviadas = 0;
  while (enviadas < LOTE_ENVIO && f.available()) {
    String linha = f.readStringUntil('\n');
    if (linha.length() == 0) {
      offsetEnvio = f.position();
      continue;
    }
    Amostra a;
    if (deCsv(linha, a) && !enviarParaNuvem(a)) break;  // falhou: tenta de novo depois
    offsetEnvio = f.position();
    if (pendentes) pendentes--;
    enviadas++;
  }
  bool esvaziou = !f.available();
  f.close();

  if (esvaziou) {
    SPIFFS.remove(ARQ_FILA);
    offsetEnvio = 0;
    pendentes   = 0;
    Serial.println("[FILA] sincronizacao concluida - arquivo local apagado");
  }
}

// ------------------------------------------------------------------- logs
void registrarLeitura(const Amostra& a, bool online) {
  Serial.printf("[LEITURA #%lu] T=%.1fC U=%.1f%% BPM=%u | alertas: %s | rede: %s | fila: %lu\n",
                (unsigned long)a.seq, a.temperatura, a.umidade, a.bpm,
                descreverAlertas(a.alertas).c_str(),
                online ? "ONLINE" : "OFFLINE", (unsigned long)pendentes);
}

void registrarMudancaDeRede(bool online) {
  if (online) {
    Serial.printf("[REDE] conectado - sincronizando %lu amostras pendentes\n",
                  (unsigned long)pendentes);
  } else {
    Serial.println("[REDE] conexao perdida - a coleta continua e os dados vao para a fila local");
  }
}

// ------------------------------------------------------------------ setup
void setup() {
  Serial.begin(115200);

  pinMode(PINO_BATIMENTO, INPUT_PULLUP);
  pinMode(PINO_CONEXAO, INPUT);          // a chave liga o pino a 3V3 ou a GND
  pinMode(PINO_LED_ALERTA, OUTPUT);
  pinMode(PINO_LED_ONLINE, OUTPUT);
  attachInterrupt(digitalPinToInterrupt(PINO_BATIMENTO), aoDetectarBatimento, FALLING);

  dht.setup(PINO_DHT, DHTesp::DHT22);

  fsDisponivel = SPIFFS.begin(true);     // true = formata na primeira execucao
  if (fsDisponivel) {
    carregarFilaExistente();
  } else {
    Serial.println("[FS] SPIFFS indisponivel - o Monitor Serial sera o registro offline");
  }

  Serial.println();
  Serial.println("==============================================================");
  Serial.println(" CardioIA - Fase 3 | Parte 1: Edge Computing (Grupo 66)");
  Serial.printf(" Leitura a cada %lu ms | fila maxima: %lu amostras\n",
                (unsigned long)INTERVALO_LEITURA_MS, (unsigned long)MAX_AMOSTRAS);
  Serial.println(" Aperte o botao no ritmo do coracao | chave = conectividade");
  Serial.println("==============================================================");

  onlineAnterior = digitalRead(PINO_CONEXAO) == HIGH;
  registrarMudancaDeRede(onlineAnterior);
}

// ------------------------------------------------------------------- loop
void loop() {
  bool online = digitalRead(PINO_CONEXAO) == HIGH;   // a "variavel booleana" de Wi-Fi
  digitalWrite(PINO_LED_ONLINE, online);
  if (online != onlineAnterior) {
    registrarMudancaDeRede(online);
    onlineAnterior = online;
  }

  if (millis() - ultimaLeituraMs >= INTERVALO_LEITURA_MS) {
    ultimaLeituraMs = millis();
    Amostra a = coletar();
    digitalWrite(PINO_LED_ALERTA, a.alertas ? HIGH : LOW);   // alerta local, sem depender da rede
    armazenar(a);
    registrarLeitura(a, online);
  }

  if (online) sincronizarFila();
}
