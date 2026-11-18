
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP280.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_timer.h>

constexpr uint8_t SDA_PIN = 21;
constexpr uint8_t SCL_PIN = 22;

Adafruit_AHTX0 aht;
Adafruit_BMP280 bmp;
bool ahtOK = false;
bool bmpOK = false;

// =======================
// UUIDs GATT
// =======================
#define SERVICE_UUID        "7c9e0001-6e4f-4c3b-8b9a-123456789001"
#define TEMP_CHAR_UUID      "7c9e0002-6e4f-4c3b-8b9a-123456789002"
#define HUM_CHAR_UUID       "7c9e0003-6e4f-4c3b-8b9a-123456789003"
#define PRESS_CHAR_UUID     "7c9e0004-6e4f-4c3b-8b9a-123456789004"
#define STATUS_CHAR_UUID    "7c9e0005-6e4f-4c3b-8b9a-123456789005"
#define INTERVAL_CHAR_UUID  "7c9e0006-6e4f-4c3b-8b9a-123456789006"
// Novas características do registro (datalogger)
#define TIME_CHAR_UUID      "7c9e0007-6e4f-4c3b-8b9a-123456789007"  // READ/WRITE: hora (unix, texto)
#define LOGCTRL_CHAR_UUID   "7c9e0008-6e4f-4c3b-8b9a-123456789008"  // READ: info | WRITE: GET/STOP/CLEAR
#define LOGDATA_CHAR_UUID   "7c9e0009-6e4f-4c3b-8b9a-123456789009"  // NOTIFY: dados binários

BLEServer* pServer = nullptr;
BLECharacteristic* pTempChar = nullptr;
BLECharacteristic* pHumChar = nullptr;
BLECharacteristic* pPressChar = nullptr;
BLECharacteristic* pStatusChar = nullptr;
BLECharacteristic* pIntervalChar = nullptr;
BLECharacteristic* pTimeChar = nullptr;
BLECharacteristic* pLogCtrlChar = nullptr;
BLECharacteristic* pLogDataChar = nullptr;

bool deviceConnected = false;
bool oldDeviceConnected = false;

// intervalo de aquisição em milissegundos
volatile unsigned long intervaloMs = 2000;
unsigned long ultimoEnvio = 0;

// Limites didáticos
const unsigned long INTERVALO_MIN_MS = 500;
const unsigned long INTERVALO_MAX_MS = 60000;

// =======================
// Registro na memória interna (LittleFS)
// =======================
// A cada LOG_PERIODO_S segundos uma leitura é gravada na flash, mesmo sem
// ninguém conectado. A memória é dividida em dois arquivos: quando o atual
// enche, o mais antigo é apagado e os dois trocam de papel. Assim sempre
// ficam guardados os registros mais recentes.
const uint32_t LOG_PERIODO_S = 300;  // 5 minutos

// Cada registro ocupa 16 bytes. O ESP32 não tem relógio com bateria, então
// cada registro guarda "sessão (boot) + segundos desde que ligou". Quando o
// celular ou o PC informam a hora, é gravado um registro de sincronização
// daquela sessão, e na hora de gerar o CSV os horários são reconstruídos.
struct __attribute__((packed)) Registro {
  uint32_t seg;    // segundos desde que o ESP32 ligou
  uint16_t boot;   // número da sessão (incrementa a cada vez que liga)
  uint8_t  tipo;   // 0 = leitura | 1 = sincronização de hora
  uint8_t  flags;  // bit0 = AHT ok | bit1 = BMP ok
  int16_t  temp;   // temperatura em centésimos de °C
  uint16_t umid;   // umidade em centésimos de %
  uint32_t valor;  // tipo 0: pressão em centésimos de hPa | tipo 1: hora unix
};
static_assert(sizeof(Registro) == 16, "Registro deve ter 16 bytes");

const char* ARQ[2] = {"/log_a.bin", "/log_b.bin"};
Preferences prefs;
bool fsOK = false;
uint8_t arqAtual = 0;          // índice do arquivo que está recebendo dados
size_t limiteArquivo = 0;      // tamanho máximo de cada arquivo (bytes)
uint16_t bootId = 0;
int64_t epochNoBoot = -1;      // hora unix no instante em que ligou (-1 = desconhecida)
uint32_t proximoLogSeg = 10;   // primeiro registro 10 s depois de ligar

// Transferência pelo BLE
bool enviando = false;
volatile bool pedidoGet = false, pedidoStop = false, pedidoClear = false;
volatile bool falhaNotify = false;
volatile uint32_t pedidoHora = 0;
File arqEnvio;
uint8_t etapaEnvio = 0;        // 0 = arquivo antigo, 1 = arquivo atual
size_t restanteEtapa[2] = {0, 0};
uint16_t seqEnvio = 0;
uint32_t totalEnvio = 0, enviadosEnvio = 0;

// Comandos pela porta serial (USB)
String linhaSerial;

uint32_t uptimeSeg() {
  return (uint32_t)(esp_timer_get_time() / 1000000ULL);  // não estoura em 49 dias como millis()
}

// =======================
// Callbacks BLE
// =======================
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    deviceConnected = true;
    Serial.println("Cliente BLE conectado.");
  }

  void onDisconnect(BLEServer* pServer) override {
    deviceConnected = false;
    pedidoStop = true;
    Serial.println("Cliente BLE desconectado.");
  }
};

class IntervalCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    // getValue() retorna std::string no core 2.x e String no core 3.x;
    // .c_str() funciona nas duas versões.
    String value = String(pCharacteristic->getValue().c_str());

    if (value.length() == 0) return;

    String texto = value;
    texto.trim();

    unsigned long novoIntervalo = texto.toInt();

    if (novoIntervalo >= INTERVALO_MIN_MS &&
        novoIntervalo <= INTERVALO_MAX_MS) {

      intervaloMs = novoIntervalo;

      Serial.print("Novo intervalo de aquisicao: ");
      Serial.print(intervaloMs);
      Serial.println(" ms");

      String retorno = String(intervaloMs);
      pIntervalChar->setValue(retorno.c_str());

    } else {
      Serial.print("Valor invalido recebido: ");
      Serial.println(texto);

      String retorno = String(intervaloMs);
      pIntervalChar->setValue(retorno.c_str());
    }
  }
};

// Recebe a hora do celular (texto com o tempo unix em segundos)
class TimeCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    String texto = String(pCharacteristic->getValue().c_str());
    texto.trim();
    pedidoHora = (uint32_t)strtoul(texto.c_str(), nullptr, 10);  // tratado no loop()
  }
};

// Comandos do registro: GET (enviar tudo), STOP (cancelar), CLEAR (apagar)
class LogCtrlCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    String cmd = String(pCharacteristic->getValue().c_str());
    cmd.trim();
    cmd.toUpperCase();
    if (cmd == "GET") pedidoGet = true;
    else if (cmd == "STOP") pedidoStop = true;
    else if (cmd == "CLEAR") pedidoClear = true;
  }
};

// Detecta quando a fila do Bluetooth está cheia, para reenviar o pacote
class LogDataCallbacks : public BLECharacteristicCallbacks {
  void onStatus(BLECharacteristic* pCharacteristic, Status s, uint32_t code) override {
    if (s == Status::ERROR_GATT) falhaNotify = true;
  }
};

// =======================
// Sensores
// =======================
void iniciarSensores() {
  Wire.begin(SDA_PIN, SCL_PIN);

  ahtOK = aht.begin();
  Serial.println(ahtOK ? "AHT20 detectado." : "ERRO: AHT20 nao encontrado.");

  bmpOK = bmp.begin(0x76);
  if (!bmpOK) bmpOK = bmp.begin(0x77);

  if (bmpOK) {
    Serial.println("BMP280 detectado.");

    bmp.setSampling(
      Adafruit_BMP280::MODE_NORMAL,
      Adafruit_BMP280::SAMPLING_X2,
      Adafruit_BMP280::SAMPLING_X16,
      Adafruit_BMP280::FILTER_X16,
      Adafruit_BMP280::STANDBY_MS_500
    );
  } else {
    Serial.println("ERRO: BMP280 nao encontrado.");
  }
}

void lerSensores(float& temperatura, float& umidade, float& pressao) {
  temperatura = NAN;
  umidade = NAN;
  pressao = NAN;

  if (ahtOK) {
    sensors_event_t humidity;
    sensors_event_t temp;

    aht.getEvent(&humidity, &temp);

    temperatura = temp.temperature;
    umidade = humidity.relative_humidity;
  }

  if (bmpOK) {
    pressao = bmp.readPressure() / 100.0F;
  }
}

// =======================
// Registro (LittleFS)
// =======================
uint8_t arqAntigo() { return arqAtual ^ 1; }

size_t tamanhoArquivo(uint8_t i) {
  if (!fsOK || !LittleFS.exists(ARQ[i])) return 0;
  File f = LittleFS.open(ARQ[i], FILE_READ);
  size_t s = f ? f.size() : 0;
  f.close();
  return s;
}

void atualizarInfoLog() {
  if (!pLogCtrlChar) return;
  size_t total = tamanhoArquivo(0) + tamanhoArquivo(1);
  String info = String("registros=") + (total / sizeof(Registro)) +
                ";bytes=" + total +
                ";capacidade=" + (fsOK ? (2 * limiteArquivo / sizeof(Registro)) : 0) +
                ";periodo=" + LOG_PERIODO_S +
                ";boot=" + bootId +
                ";hora=" + (epochNoBoot >= 0 ? "OK" : "NAO") +
                ";memoria=" + (fsOK ? "OK" : "FALHA");
  pLogCtrlChar->setValue(info.c_str());
}

void atualizarHoraBLE() {
  if (!pTimeChar) return;
  String h = epochNoBoot >= 0 ? String((uint32_t)(epochNoBoot + uptimeSeg())) : String("0");
  pTimeChar->setValue(h.c_str());
}

bool anexarRegistro(const Registro& r) {
  File f = LittleFS.open(ARQ[arqAtual], FILE_APPEND);
  if (!f) return false;
  size_t n = f.write((const uint8_t*)&r, sizeof(r));
  f.close();
  return n == sizeof(r);
}

Registro registroSync() {
  Registro r = {};
  r.seg = uptimeSeg();
  r.boot = bootId;
  r.tipo = 1;
  r.valor = (uint32_t)(epochNoBoot + r.seg);
  return r;
}

void gravarRegistro(const Registro& r) {
  if (!fsOK) return;

  // Arquivo atual cheio: apaga o mais antigo e passa a gravar nele
  // (durante uma transferência não troca de arquivo, para não apagar o que está sendo lido)
  if (!enviando && tamanhoArquivo(arqAtual) + sizeof(Registro) > limiteArquivo) {
    arqAtual = arqAntigo();
    LittleFS.remove(ARQ[arqAtual]);
    prefs.putUChar("arq", arqAtual);
    Serial.println("Registro: arquivo cheio, o mais antigo foi descartado.");
    // a nova metade precisa saber a hora desta sessão
    if (epochNoBoot >= 0 && r.tipo == 0) anexarRegistro(registroSync());
  }

  if (!anexarRegistro(r)) Serial.println("ERRO: falha ao gravar registro.");
  atualizarInfoLog();
}

void gravarLeitura() {
  float t, h, p;
  lerSensores(t, h, p);

  Registro r = {};
  r.seg = uptimeSeg();
  r.boot = bootId;
  r.tipo = 0;
  r.flags = (ahtOK ? 1 : 0) | (bmpOK ? 2 : 0);
  r.temp = isnan(t) ? 0 : (int16_t)lroundf(t * 100.0f);
  r.umid = isnan(h) ? 0 : (uint16_t)lroundf(h * 100.0f);
  r.valor = isnan(p) ? 0 : (uint32_t)lroundf(p * 100.0f);
  gravarRegistro(r);
}

void definirHora(uint32_t epoch) {
  if (epoch < 1700000000UL) {  // antes de 2023: valor inválido
    Serial.println("Hora invalida recebida.");
    return;
  }
  int64_t novo = (int64_t)epoch - uptimeSeg();
  bool mudou = epochNoBoot < 0 || llabs(novo - epochNoBoot) > 5;
  epochNoBoot = novo;
  if (mudou) {
    gravarRegistro(registroSync());
    Serial.print("Hora sincronizada: ");
    Serial.println(epoch);
  }
  atualizarHoraBLE();
}

void apagarRegistros() {
  if (!fsOK) return;
  if (enviando) { arqEnvio.close(); enviando = false; }
  LittleFS.remove(ARQ[0]);
  LittleFS.remove(ARQ[1]);
  arqAtual = 0;
  prefs.putUChar("arq", 0);
  if (epochNoBoot >= 0) anexarRegistro(registroSync());
  atualizarInfoLog();
  Serial.println("Registros apagados.");
}

void iniciarRegistro() {
  prefs.begin("estacao", false);
  bootId = prefs.getUShort("boot", 0) + 1;
  prefs.putUShort("boot", bootId);
  arqAtual = prefs.getUChar("arq", 0) & 1;

  fsOK = LittleFS.begin(true);  // formata na primeira vez
  if (!fsOK) {
    Serial.println("ERRO: memoria interna (LittleFS) indisponivel.");
    return;
  }
  // cada arquivo usa ~45% da memória (o resto fica para o sistema de arquivos)
  limiteArquivo = (LittleFS.totalBytes() * 45 / 100) / sizeof(Registro) * sizeof(Registro);

  size_t total = tamanhoArquivo(0) + tamanhoArquivo(1);
  Serial.printf("Registro: sessao %u, %u registros guardados, capacidade ~%u registros.\n",
                bootId, (unsigned)(total / sizeof(Registro)),
                (unsigned)(2 * limiteArquivo / sizeof(Registro)));
}

// =======================
// Transferência pelo BLE
// =======================
// Pacote = [seq (2 bytes, little-endian)] + dados. O seq conta de 0 a 0xFFEF e recomeça.
// seq 0xFFFE = início (dados: total de bytes, uint32)
// seq 0xFFFF = fim    (dados: total enviado, uint32)
void notificarPacote(uint16_t seq, const uint8_t* dados, size_t n) {
  uint8_t buf[2 + 512];
  buf[0] = seq & 0xFF;
  buf[1] = seq >> 8;
  memcpy(buf + 2, dados, n);
  pLogDataChar->setValue(buf, 2 + n);
  pLogDataChar->notify();
}

void iniciarEnvio() {
  if (!fsOK || !deviceConnected) return;
  if (enviando) arqEnvio.close();
  restanteEtapa[0] = tamanhoArquivo(arqAntigo());
  restanteEtapa[1] = tamanhoArquivo(arqAtual);
  totalEnvio = restanteEtapa[0] + restanteEtapa[1];
  enviadosEnvio = 0;
  seqEnvio = 0;
  etapaEnvio = 0;
  enviando = true;

  uint8_t ini[4];
  memcpy(ini, &totalEnvio, 4);
  notificarPacote(0xFFFE, ini, 4);
  delay(20);
  Serial.printf("Enviando registro pelo BLE: %u bytes.\n", (unsigned)totalEnvio);
}

void finalizarEnvio(bool completo) {
  if (arqEnvio) arqEnvio.close();
  enviando = false;
  if (completo && deviceConnected) {
    uint8_t fim[4];
    memcpy(fim, &enviadosEnvio, 4);
    notificarPacote(0xFFFF, fim, 4);
    Serial.println("Envio pelo BLE concluido.");
  } else {
    Serial.println("Envio pelo BLE interrompido.");
  }
}

void continuarEnvio() {
  uint16_t mtu = pServer->getPeerMTU(pServer->getConnId());
  size_t carga = (mtu > 5) ? (size_t)(mtu - 5) : 18;  // 3 de cabeçalho ATT + 2 do seq
  if (carga > 496) carga = 496;

  for (int k = 0; k < 4 && enviando; k++) {
    // passa para o próximo arquivo quando o atual termina
    while (etapaEnvio < 2 && restanteEtapa[etapaEnvio] == 0) {
      if (arqEnvio) arqEnvio.close();
      etapaEnvio++;
    }
    if (etapaEnvio >= 2) { finalizarEnvio(true); return; }

    if (!arqEnvio) {
      arqEnvio = LittleFS.open(ARQ[etapaEnvio == 0 ? arqAntigo() : arqAtual], FILE_READ);
      if (!arqEnvio) { finalizarEnvio(false); return; }
    }

    uint8_t buf[496];
    size_t n = min(carga, restanteEtapa[etapaEnvio]);
    size_t pos = arqEnvio.position();
    n = arqEnvio.read(buf, n);
    if (n == 0) { restanteEtapa[etapaEnvio] = 0; continue; }

    falhaNotify = false;
    notificarPacote(seqEnvio, buf, n);
    if (falhaNotify) {          // fila cheia: volta e tenta de novo depois
      arqEnvio.seek(pos);
      delay(30);
      return;
    }
    seqEnvio = (seqEnvio + 1) % 0xFFF0;  // nunca chega aos códigos 0xFFFE/0xFFFF
    enviadosEnvio += n;
    restanteEtapa[etapaEnvio] -= n;
    delay(8);
  }
}

// =======================
// Comandos pela porta serial (usados pelo script do PC)
// =======================
// T<unix>  -> ajusta a hora
// DUMP     -> envia todos os registros em binário
// INFO     -> mostra o resumo do registro
// CLEAR    -> apaga os registros
void despejarSerial() {
  size_t tam[2] = {tamanhoArquivo(arqAntigo()), tamanhoArquivo(arqAtual)};
  uint8_t ordem[2] = {arqAntigo(), arqAtual};
  Serial.printf("\n#DUMP %u\n", (unsigned)(tam[0] + tam[1]));
  Serial.flush();
  uint8_t buf[512];
  for (int i = 0; i < 2; i++) {
    if (tam[i] == 0) continue;
    File f = LittleFS.open(ARQ[ordem[i]], FILE_READ);
    size_t falta = tam[i];
    while (f && falta > 0) {
      size_t n = f.read(buf, min(sizeof(buf), falta));
      if (n == 0) break;
      Serial.write(buf, n);
      falta -= n;
    }
    // completa com zeros se algo falhar, para manter o tamanho anunciado
    memset(buf, 0, sizeof(buf));
    while (falta > 0) { size_t n = min(sizeof(buf), falta); Serial.write(buf, n); falta -= n; }
    f.close();
  }
  Serial.flush();
  Serial.println("\n#FIM");
}

void tratarComandoSerial(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;
  if (cmd[0] == 'T' || cmd[0] == 't') {
    definirHora((uint32_t)strtoul(cmd.c_str() + 1, nullptr, 10));
  } else if (cmd.equalsIgnoreCase("DUMP")) {
    despejarSerial();
  } else if (cmd.equalsIgnoreCase("CLEAR")) {
    apagarRegistros();
  } else if (cmd.equalsIgnoreCase("INFO")) {
    atualizarInfoLog();
    Serial.print("#INFO ");
    Serial.println(pLogCtrlChar ? pLogCtrlChar->getValue().c_str() : "");
  }
}

void lerSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (linhaSerial.length()) tratarComandoSerial(linhaSerial);
      linhaSerial = "";
    } else if (linhaSerial.length() < 40) {
      linhaSerial += c;
    }
  }
}

// =======================
// BLE
// =======================
void iniciarBLE() {
  BLEDevice::init("ESP32_Estacao_Ambiental");
  BLEDevice::setMTU(517);  // pacotes grandes deixam a transferência do registro mais rápida

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  // 40 handles: o padrão (15) não comporta todas as características
  BLEService* pService = pServer->createService(BLEUUID(SERVICE_UUID), 40);

  pTempChar = pService->createCharacteristic(
    TEMP_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY
  );

  pHumChar = pService->createCharacteristic(
    HUM_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY
  );

  pPressChar = pService->createCharacteristic(
    PRESS_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY
  );

  pStatusChar = pService->createCharacteristic(
    STATUS_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ
  );

  // Característica de intervalo: leitura + escrita
  pIntervalChar = pService->createCharacteristic(
    INTERVAL_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_WRITE
  );

  // Registro: hora, controle e dados
  pTimeChar = pService->createCharacteristic(
    TIME_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_WRITE
  );

  pLogCtrlChar = pService->createCharacteristic(
    LOGCTRL_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_WRITE
  );

  pLogDataChar = pService->createCharacteristic(
    LOGDATA_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );

  // CCCD para notificações
  pTempChar->addDescriptor(new BLE2902());
  pHumChar->addDescriptor(new BLE2902());
  pPressChar->addDescriptor(new BLE2902());
  pLogDataChar->addDescriptor(new BLE2902());

  // Callbacks WRITE
  pIntervalChar->setCallbacks(new IntervalCallbacks());
  pTimeChar->setCallbacks(new TimeCallbacks());
  pLogCtrlChar->setCallbacks(new LogCtrlCallbacks());
  pLogDataChar->setCallbacks(new LogDataCallbacks());

  // Valores iniciais
  pTempChar->setValue("0.0");
  pHumChar->setValue("0.0");
  pPressChar->setValue("0.0");

  String statusInicial = String("AHT20=") + (ahtOK ? "OK" : "FALHA") +
                         ";BMP280=" + (bmpOK ? "OK" : "FALHA");
  pStatusChar->setValue(statusInicial.c_str());

  String intervaloInicial = String(intervaloMs);
  pIntervalChar->setValue(intervaloInicial.c_str());

  atualizarHoraBLE();
  atualizarInfoLog();

  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);

  BLEDevice::startAdvertising();

  Serial.println("Servidor BLE iniciado.");
  Serial.println("Nome: ESP32_Estacao_Ambiental");
  Serial.print("Intervalo inicial: ");
  Serial.print(intervaloMs);
  Serial.println(" ms");
}

// =======================
// Atualiza sensores + BLE
// =======================
void atualizarDadosBLE() {
  float temperatura, umidade, pressao;
  lerSensores(temperatura, umidade, pressao);

  char buffer[20];

  if (!isnan(temperatura)) {
    snprintf(buffer, sizeof(buffer), "%.2f", temperatura);
    pTempChar->setValue(buffer);
    if (deviceConnected) pTempChar->notify();
  }

  if (!isnan(umidade)) {
    snprintf(buffer, sizeof(buffer), "%.2f", umidade);
    pHumChar->setValue(buffer);
    if (deviceConnected) pHumChar->notify();
  }

  if (!isnan(pressao)) {
    snprintf(buffer, sizeof(buffer), "%.2f", pressao);
    pPressChar->setValue(buffer);
    if (deviceConnected) pPressChar->notify();
  }

  String statusAtual = String("AHT20=") + (ahtOK ? "OK" : "FALHA") +
                       ";BMP280=" + (bmpOK ? "OK" : "FALHA");
  pStatusChar->setValue(statusAtual.c_str());
  atualizarHoraBLE();

  Serial.print("T=");
  Serial.print(temperatura, 2);
  Serial.print(" C | RH=");
  Serial.print(umidade, 2);
  Serial.print(" % | P=");
  Serial.print(pressao, 2);
  Serial.print(" hPa | intervalo=");
  Serial.print(intervaloMs);
  Serial.println(" ms");
}

// =======================
// Setup
// =======================
void setup() {
  Serial.begin(115200);
  delay(800);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" Estacao Ambiental BLE - ESP32");
  Serial.println(" AHT20 + BMP280 + WRITE de intervalo");
  Serial.println(" + registro a cada 5 min na memoria");
  Serial.println("========================================");

  iniciarRegistro();
  iniciarSensores();
  iniciarBLE();
}

// =======================
// Loop
// =======================
void loop() {
  lerSerial();

  // pedidos vindos dos callbacks BLE (tratados aqui, fora da tarefa do Bluetooth)
  if (pedidoHora && !enviando) { uint32_t h = pedidoHora; pedidoHora = 0; definirHora(h); }
  if (pedidoClear) { pedidoClear = false; apagarRegistros(); }
  if (pedidoStop) { pedidoStop = false; if (enviando) finalizarEnvio(false); }
  if (pedidoGet) { pedidoGet = false; iniciarEnvio(); }

  if (enviando) {
    continuarEnvio();
  } else if (uptimeSeg() >= proximoLogSeg) {
    // (durante uma transferência o registro espera ela terminar)
    gravarLeitura();
    proximoLogSeg += LOG_PERIODO_S;
    if (proximoLogSeg <= uptimeSeg()) proximoLogSeg = uptimeSeg() + LOG_PERIODO_S;
  }

  if (millis() - ultimoEnvio >= intervaloMs) {
    ultimoEnvio = millis();
    atualizarDadosBLE();
  }

  if (!deviceConnected && oldDeviceConnected) {
    delay(500);
    pServer->startAdvertising();
    Serial.println("Advertising reiniciado.");
    oldDeviceConnected = deviceConnected;
  }

  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }

  delay(enviando ? 0 : 10);
}
