// ESP32 - painel 5x5 (25 botões + LEDs), enviando os toques por Bluetooth Low Energy.
// A página do instrumento (Chrome/Edge) conecta direto ao ESP32 via Web Bluetooth.
// Pinos e LEDs copiados do sketch "Professor de Melodias" que já funcionava neste hardware.
//
// Biblioteca: "Keypad" (Mark Stanley / Alexander Brevig) pelo Gerenciador de Bibliotecas.
// A biblioteca BLE já vem com o core do ESP32 (Arduino core 3.x).

#include <Keypad.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// Estes dois códigos precisam ser IGUAIS aos do index.html
#define SERVICO_UUID "9a1c0001-5b7e-4c3a-9d2e-6f1a7b3c8d10"
#define CARACT_UUID  "9a1c0002-5b7e-4c3a-9d2e-6f1a7b3c8d10"   // ESP32 -> página: botão apertado
#define LEDS_UUID    "9a1c0003-5b7e-4c3a-9d2e-6f1a7b3c8d10"   // página -> ESP32: acender LED

// ------------------- TECLADO 5 linhas x 5 colunas -------------------
const byte LINHAS = 5, COLUNAS = 5;
char teclas[LINHAS][COLUNAS] = {
  {'1','2','3','4','5'},
  {'6','7','8','9','A'},
  {'B','C','D','E','F'},
  {'G','H','I','J','K'},
  {'L','M','N','O','P'}
};
byte pinosLinhas[LINHAS]   = {12, 14, 27, 26, 25};
byte pinosColunas[COLUNAS] = {19, 18, 5, 17, 16};
Keypad teclado = Keypad(makeKeymap(teclas), pinosLinhas, pinosColunas, LINHAS, COLUNAS);

// Tecla -> número do botão (0 a 24), da esquerda p/ direita, de cima p/ baixo
int teclaParaIndice(char k) {
  if (k >= '1' && k <= '9') return k - '1';        // 1..9 -> 0..8
  if (k >= 'A' && k <= 'P') return 9 + (k - 'A');  // A..P -> 9..24
  return -1;
}

// ------------------- LEDs (2x 74HC595) -------------------
const bool USAR_LEDS = true;            // false = não acende os LEDs
const int dataPin      = 32;
const int clockPin     = 33;
const int latchPinCols = 23;            // SR1 - colunas
const int latchPinRows = 22;            // SR2 - linhas
const uint8_t COL_OFFSET = 1;
const uint8_t ROW_OFFSET = 1;
unsigned long apagarLedEm = 0;

void clearAllLeds() {
  digitalWrite(latchPinCols, LOW);
  shiftOut(dataPin, clockPin, MSBFIRST, 0xFF);   // todas as colunas HIGH
  digitalWrite(latchPinCols, HIGH);
  digitalWrite(latchPinRows, LOW);
  shiftOut(dataPin, clockPin, MSBFIRST, 0x00);   // todas as linhas LOW
  digitalWrite(latchPinRows, HIGH);
}

void setLed(int row, int col) {
  if (row < 0 || row >= LINHAS || col < 0 || col >= COLUNAS) return;
  byte colData = 0xFF;
  byte rowData = 0x00;
  colData &= ~(1 << (col + COL_OFFSET));         // coluna ativa em LOW
  rowData |=  (1 << (row + ROW_OFFSET));         // linha ativa em HIGH
  digitalWrite(latchPinCols, LOW);
  shiftOut(dataPin, clockPin, MSBFIRST, colData);
  digitalWrite(latchPinCols, HIGH);
  digitalWrite(latchPinRows, LOW);
  shiftOut(dataPin, clockPin, MSBFIRST, rowData);
  digitalWrite(latchPinRows, HIGH);
}

// ------------------- BLUETOOTH -------------------
BLECharacteristic* caracteristica;
bool conectado = false;

class AoConectar : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    conectado = true;
    Serial.println("Computador conectado");
  }
  void onDisconnect(BLEServer*) override {
    conectado = false;
    Serial.println("Desconectado - voltando a procurar");
    BLEDevice::startAdvertising();      // permite reconectar sem reiniciar o ESP32
  }
};

// A página manda 1 byte: 0..24 acende o LED desse botão, 255 apaga.
// Enquanto a página controla os LEDs (exercícios), o ESP32 não acende o LED ao apertar.
volatile int comandoLed = -1;           // -1 = nenhum comando novo
bool ledsRemoto = false;

class AoReceberLed : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    auto v = c->getValue();
    if (v.length() > 0) comandoLed = (uint8_t)v[0];
  }
};

void aoApertar(int indice) {
  Serial.printf("botao %d%s\n", indice + 1, conectado ? "" : " (sem conexao)");
  if (USAR_LEDS && !ledsRemoto) {
    setLed(indice / COLUNAS, indice % COLUNAS);
    apagarLedEm = millis() + 250;
  }
  if (!conectado) return;
  uint8_t b = indice;
  caracteristica->setValue(&b, 1);      // 1 byte: número do botão (0 a 24)
  caracteristica->notify();
}

void setup() {
  Serial.begin(115200);
  pinMode(dataPin, OUTPUT);
  pinMode(clockPin, OUTPUT);
  pinMode(latchPinCols, OUTPUT);
  pinMode(latchPinRows, OUTPUT);
  clearAllLeds();

  BLEDevice::init("MeuInstrumento");
  BLEServer* servidor = BLEDevice::createServer();
  servidor->setCallbacks(new AoConectar());
  BLEService* servico = servidor->createService(SERVICO_UUID);
  caracteristica = servico->createCharacteristic(CARACT_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  caracteristica->addDescriptor(new BLE2902());
  BLECharacteristic* leds = servico->createCharacteristic(
      LEDS_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  leds->setCallbacks(new AoReceberLed());
  servico->start();

  BLEAdvertising* anuncio = BLEDevice::getAdvertising();
  anuncio->addServiceUUID(SERVICO_UUID);
  anuncio->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("Pronto. Abra a página e toque em 'Conectar o painel'.");
}

void loop() {
  if (teclado.getKeys()) {              // aceita várias teclas ao mesmo tempo
    for (int i = 0; i < LIST_MAX; i++) {
      if (teclado.key[i].stateChanged && teclado.key[i].kstate == PRESSED) {
        int idx = teclaParaIndice(teclado.key[i].kchar);
        if (idx >= 0) aoApertar(idx);
      }
    }
  }
  if (comandoLed != -1) {
    int c = comandoLed;
    comandoLed = -1;
    if (c >= 0 && c < LINHAS * COLUNAS) {
      ledsRemoto = true;
      apagarLedEm = 0;
      setLed(c / COLUNAS, c % COLUNAS);
    } else if (c == 255) {
      ledsRemoto = false;
      apagarLedEm = 0;
      clearAllLeds();
    }
  }
  if (apagarLedEm && millis() >= apagarLedEm) {
    clearAllLeds();
    apagarLedEm = 0;
  }
}
