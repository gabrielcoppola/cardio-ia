/*
 * CardioIA - Fase 3 | Parte 2: Transmissao para a nuvem via MQTT (Fog/Cloud)
 * Grupo 89
 *
 * Evolucao da Parte 1: o mesmo wearable (DHT22 + botao de batimento + fila
 * local em SPIFFS) agora publica as leituras em um broker MQTT, consumidas por
 * um dashboard Node-RED.
 *
 * Topicos (prefixo cardioia/g89/p001):
 *   .../vitais  JSON com uma leitura por mensagem (QoS 0)
 *   .../status  "online" | "offline" (retido; "offline" vem do Last Will)
 *
 * Conectividade:
 *   A chave deslizante continua simulando a cobertura de rede do paciente.
 *   Chave desligada -> o socket MQTT e derrubado sem DISCONNECT, como numa queda
 *   real; o broker entao publica o Last Will "offline" e o dashboard fica cinza.
 *   Durante a queda as leituras vao para a fila; na volta, sao publicadas com
 *   "atrasado": true para o dashboard nao confundi-las com o estado atual.
 *
 * Broker:
 *   Padrao: broker publico da HiveMQ (sem TLS, sem senha). Serve para a
 *   demonstracao, mas qualquer pessoa pode ler os topicos: nunca use com dados reais.
 *   Para o HiveMQ Cloud: USAR_TLS 1, porta 8883, usuario e senha do cluster.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <FS.h>
#include <SPIFFS.h>
#include <DHTesp.h>
#include <time.h>

#define USAR_TLS 0
#if USAR_TLS
#include <WiFiClientSecure.h>
#endif

// ------------------------------------------------------------------ rede
const char* WIFI_SSID  = "Wokwi-GUEST";
const char* WIFI_SENHA = "";

const char*    MQTT_HOST    = "broker.hivemq.com";
const uint16_t MQTT_PORTA   = USAR_TLS ? 8883 : 1883;
const char*    MQTT_USUARIO = "";
const char*    MQTT_SENHA   = "";

// O paciente e identificado por um codigo, nunca por nome ou documento.
const char* ID_PACIENTE   = "p001";
const char* TOPICO_VITAIS = "cardioia/g89/p001/vitais";
const char* TOPICO_STATUS = "cardioia/g89/p001/status";

constexpr uint32_t INTERVALO_RECONEXAO_MS = 5000;

// ----------------------------------------------------------------------- pinos
constexpr uint8_t PINO_DHT        = 15;
constexpr uint8_t PINO_BATIMENTO  = 4;
constexpr uint8_t PINO_CONEXAO    = 19;
constexpr uint8_t PINO_LED_ALERTA = 26;
constexpr uint8_t PINO_LED_ONLINE = 27;

// ------------------------------------------------------------------ amostragem
constexpr uint32_t INTERVALO_LEITURA_MS = 2000;

// ------------------------------------------------------ armazenamento limitado
// Mesmo dimensionamento da Parte 1: 8 h offline x 1 amostra / 2 s = 14.400.
constexpr uint32_t MAX_AMOSTRAS  = 14400;
constexpr uint32_t DESCARTE_LOTE = MAX_AMOSTRAS / 10;

// Ritmo da sincronizacao: ate 10 mensagens a cada 100 ms (~100 msg/s), para
// esvaziar uma noite de fila em ~2,5 min sem afogar broker nem dashboard.
constexpr uint16_t LOTE_ENVIO          = 10;
constexpr uint32_t INTERVALO_LOTE_MS   = 100;

// ------------------------------------------------------ deteccao de batimento
constexpr uint32_t REFRATARIO_MS = 250;
constexpr uint32_t SEM_PULSO_MS  = 5000;
constexpr uint8_t  JANELA_IBI    = 5;

// ---------------------------------------------- limiares definidos pelo grupo
constexpr uint16_t BPM_TAQUICARDIA = 120;
constexpr uint16_t BPM_BRADICARDIA = 50;
constexpr float    TEMP_FEBRE      = 38.0f;
constexpr float    TEMP_HIPOTERMIA = 35.0f;

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
  uint32_t seq;
  uint32_t instanteMs;
  float    temperatura;
  float    umidade;
  uint16_t bpm;
  uint8_t  alertas;
};

const char* ARQ_FILA = "/fila.csv";
const char* ARQ_TEMP = "/fila.tmp";

#if USAR_TLS
WiFiClientSecure rede;
#else
WiFiClient rede;
#endif
PubSubClient mqtt(rede);
DHTesp dht;

String   idCliente;
bool     fsDisponivel        = false;
bool     ntpIniciado         = false;
uint32_t pendentes           = 0;
uint32_t offsetEnvio         = 0;
uint32_t proximoSeq          = 1;
uint32_t ultimaLeituraMs     = 0;
uint32_t ultimoLoteMs        = 0;
uint32_t ultimaTentativaMqtt = 0;
bool     onlineAnterior      = false;

// ------------------------------------------------- batimentos (interrupcao)
portMUX_TYPE muxBatimento = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t ultimoBatimentoMs = 0;
volatile uint32_t ibis[JANELA_IBI];
volatile uint8_t  idxIbi = 0;
volatile uint8_t  qtdIbi = 0;

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
// Os alertas continuam sendo avaliados na borda: o LED do wearable acende
// mesmo sem rede. O Node-RED reavalia os limiares do lado da nuvem.
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

// O horario da coleta e reconstruido no envio: agora (NTP) menos a idade da
// amostra. Sem NTP sincronizado, "ts" vai 0 e o Node-RED usa o horario de chegada.
String paraJson(const Amostra& a) {
  uint32_t idadeMs = millis() - a.instanteMs;
  time_t agora = time(nullptr);
  unsigned long ts = agora > 1700000000 ? (unsigned long)(agora - idadeMs / 1000) : 0;
  bool atrasado = idadeMs > 2 * INTERVALO_LEITURA_MS;

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

  char json[288];
  snprintf(json, sizeof(json),
           "{\"paciente\":\"%s\",\"seq\":%lu,\"ts\":%lu,\"temp\":%s,\"umid\":%s,"
           "\"bpm\":%u,\"alertas\":%s,\"atrasado\":%s,\"pendentes\":%lu}",
           ID_PACIENTE, (unsigned long)a.seq, ts, temperatura, umidade, a.bpm,
           alertas.c_str(), atrasado ? "true" : "false",
           (unsigned long)(pendentes ? pendentes - 1 : 0));
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

uint32_t compactarFila(bool preservarAlertas) {
  File origem  = SPIFFS.open(ARQ_FILA, FILE_READ);
  File destino = SPIFFS.open(ARQ_TEMP, FILE_WRITE);
  if (!origem || !destino) return 0;

  origem.seek(offsetEnvio);
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
  if (descartadas == 0) descartadas = compactarFila(false);
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
  f.print(linha);
  f.print('\n');
  f.close();
  pendentes++;
}

// ------------------------------------------------------------------- MQTT
bool enviarParaNuvem(const Amostra& a) {
  String json = paraJson(a);
  if (!mqtt.publish(TOPICO_VITAIS, json.c_str())) return false;
  Serial.println("[MQTT] -> " + json);
  return true;
}

void sincronizarFila() {
  if (!fsDisponivel || pendentes == 0) return;
  if (millis() - ultimoLoteMs < INTERVALO_LOTE_MS) return;
  ultimoLoteMs = millis();

  File f = SPIFFS.open(ARQ_FILA, FILE_READ);
  if (!f) {
    pendentes = 0;
    offsetEnvio = 0;
    return;
  }

  // offsetEnvio so avanca depois que o broker aceitou a mensagem: se a conexao
  // cair no meio do lote, a proxima tentativa recomeca da linha que falhou.
  f.seek(offsetEnvio);
  uint16_t enviadas = 0;
  while (enviadas < LOTE_ENVIO && f.available()) {
    String linha = f.readStringUntil('\n');
    if (linha.length() == 0) {
      offsetEnvio = f.position();
      continue;
    }
    Amostra a;
    if (deCsv(linha, a) && !enviarParaNuvem(a)) break;
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
  }
}

void conectarMqtt() {
  if (millis() - ultimaTentativaMqtt < INTERVALO_RECONEXAO_MS && ultimaTentativaMqtt != 0) return;
  ultimaTentativaMqtt = millis();

  Serial.printf("[MQTT] conectando a %s:%u ... ", MQTT_HOST, MQTT_PORTA);
  // Last Will: se o dispositivo sumir sem se despedir, o broker publica
  // "offline" (retido) no topico de status em nome dele.
  bool ok = mqtt.connect(idCliente.c_str(),
                         MQTT_USUARIO[0] ? MQTT_USUARIO : nullptr,
                         MQTT_SENHA[0] ? MQTT_SENHA : nullptr,
                         TOPICO_STATUS, 1, true, "offline");
  if (!ok) {
    Serial.printf("falhou (estado %d), nova tentativa em %lu s\n",
                  mqtt.state(), (unsigned long)(INTERVALO_RECONEXAO_MS / 1000));
    return;
  }
  Serial.println("ok");
  mqtt.publish(TOPICO_STATUS, "online", true);
}

// Retorna true quando ha caminho ate o broker.
bool manterConexao(bool coberturaSimulada) {
  if (!coberturaSimulada) {
    // Queda simulada: fecha o socket sem DISCONNECT para o broker disparar o Last Will.
    if (mqtt.connected()) rede.stop();
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) return false;   // o ESP32 reconecta sozinho

  if (!ntpIniciado) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    ntpIniciado = true;
  }
  if (!mqtt.connected()) conectarMqtt();
  mqtt.loop();
  return mqtt.connected();
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
    Serial.printf("[REDE] online - sincronizando %lu amostras pendentes\n",
                  (unsigned long)pendentes);
  } else {
    Serial.println("[REDE] offline - a coleta continua e os dados vao para a fila local");
  }
}

// ------------------------------------------------------------------ setup
void setup() {
  Serial.begin(115200);

  pinMode(PINO_BATIMENTO, INPUT_PULLUP);
  pinMode(PINO_CONEXAO, INPUT);
  pinMode(PINO_LED_ALERTA, OUTPUT);
  pinMode(PINO_LED_ONLINE, OUTPUT);
  attachInterrupt(digitalPinToInterrupt(PINO_BATIMENTO), aoDetectarBatimento, FALLING);

  dht.setup(PINO_DHT, DHTesp::DHT22);

  fsDisponivel = SPIFFS.begin(true);
  if (fsDisponivel) {
    carregarFilaExistente();
  } else {
    Serial.println("[FS] SPIFFS indisponivel - o Monitor Serial sera o registro offline");
  }

  Serial.println();
  Serial.println("==============================================================");
  Serial.println(" CardioIA - Fase 3 | Parte 2: MQTT + Node-RED (Grupo 89)");
  Serial.printf(" Broker: %s:%u | topico: %s\n", MQTT_HOST, MQTT_PORTA, TOPICO_VITAIS);
  Serial.println(" Aperte o botao no ritmo do coracao | chave = cobertura de rede");
  Serial.println("==============================================================");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_SENHA, 6);   // canal 6 acelera a conexao no Wokwi
  Serial.printf("[WIFI] conectando a %s\n", WIFI_SSID);

#if USAR_TLS
  // Aceita qualquer certificado: suficiente para o simulador. Em producao,
  // fixe a CA do broker com rede.setCACert(...).
  rede.setInsecure();
#endif
  idCliente = "cardioia-g89-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  mqtt.setServer(MQTT_HOST, MQTT_PORTA);
  mqtt.setBufferSize(512);
  mqtt.setSocketTimeout(3);
}

// ------------------------------------------------------------------- loop
void loop() {
  bool cobertura = digitalRead(PINO_CONEXAO) == HIGH;
  bool online = manterConexao(cobertura);
  digitalWrite(PINO_LED_ONLINE, online);
  if (online != onlineAnterior) {
    registrarMudancaDeRede(online);
    onlineAnterior = online;
  }

  if (millis() - ultimaLeituraMs >= INTERVALO_LEITURA_MS) {
    ultimaLeituraMs = millis();
    Amostra a = coletar();
    digitalWrite(PINO_LED_ALERTA, a.alertas ? HIGH : LOW);
    armazenar(a);
    registrarLeitura(a, online);
  }

  if (online) sincronizarFila();
}
