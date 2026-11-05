// ESP32 - painel 4x5 (20 botões + LEDs), enviando os toques por Bluetooth Low Energy.
// A página do instrumento (Chrome/Edge) conecta direto ao ESP32 via Web Bluetooth.
// Baseado no sketch "Professor de Melodias" (mesmo hardware: teclado 4x5 e LEDs com 2x 74HC595).
//
// Bibliotecas: "Keypad" (Mark Stanley / Alexander Brevig) pelo Gerenciador de Bibliotecas.
// A biblioteca BLE já vem com o core do ESP32 (Arduino core 3.x).

#include <Keypad.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// Estes dois códigos precisam ser IGUAIS aos do index.html
#define SERVICO_UUID "9a1c0001-5b7e-4c3a-9d2e-6f1a7b3c8d10"
#define CARACT_UUID  "9a1c0002-5b7e-4c3a-9d2e-6f1a7b3c8d10"

// ------------------- TECLADO 4 linhas x 5 colunas (igual ao sketch original) -------------------
const byte LINHAS = 4, COLUNAS = 5;
char teclas[LINHAS][COLUNAS] = {
  {'1','2','3','4','5'},
  {'6','7','8','9','A'},
  {'B','C','D','E','F'},
  {'G','H','I','J','K'}
};
byte pinosLinhas[LINHAS]   = {25, 26, 27, 14};
byte pinosColunas[COLUNAS] = {18, 5, 17, 16, 4};
Keypad teclado = Keypad(makeKeymap(teclas), pinosLinhas, pinosColunas, LINHAS, COLUNAS);

// Tecla -> número do botão (0 a 19), contando da esquerda p/ direita, de cima p/ baixo
int teclaParaIndice(char k) {
  if (k >= '1' && k <= '9') return k - '1';        // 1..9 -> 0..8
  if (k >= 'A' && k <= 'K') return 9 + (k - 'A');  // A..K -> 9..19
  return -1;
}

// ------------------- LEDs (2x 74HC595, igual ao sketch original) -------------------
const bool USAR_LEDS = true;            // false = desliga o acendimento dos LEDs
const int dataPin      = 32;
const int clockPin     = 33;
const int latchPinCols = 23;            // SR1 - colunas
const int latchPinRows = 22;            // SR2 - linhas
const uint8_t COL_OFFSET = 1;
const uint8_t ROW_OFFSET = 1;
unsigned long apagarLedEm = 0;

void clearAllLeds() {
  digitalWrite(latchPinCols, LOW);
  shiftOut(dataPin, clockPin, MSBFIRST, (uint8_t)(0xFF << COL_OFFSET));
  digitalWrite(latchPinCols, HIGH);
  digitalWrite(latchPinRows, LOW);
  shiftOut(dataPin, clockPin, MSBFIRST, (uint8_t)0x00);
  digitalWrite(latchPinRows, HIGH);
}

void setLed(int row, int col) {
  if (row < 0 || row >= LINHAS || col < 0 || col >= COLUNAS) return;
  digitalWrite(latchPinCols, LOW);
  byte colData = 0xFF;
  colData &= ~(1 << col);               // coluna em LOW acende
  shiftOut(dataPin, clockPin, MSBFIRST, (byte)(colData << COL_OFFSET));
  digitalWrite(latchPinCols, HIGH);
  digitalWrite(latchPinRows, LOW);
  shiftOut(dataPin, clockPin, MSBFIRST, (byte)(1 << (row + ROW_OFFSET)));
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

void aoApertar(int indice) {
  Serial.printf("botao %d%s\n", indice + 1, conectado ? "" : " (sem conexao)");
  if (USAR_LEDS) {
    setLed(indice / COLUNAS, indice % COLUNAS);
    apagarLedEm = millis() + 250;
  }
  if (!conectado) return;
  uint8_t b = indice;
  caracteristica->setValue(&b, 1);      // 1 byte: número do botão (0 a 19)
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
  if (apagarLedEm && millis() >= apagarLedEm) {
    clearAllLeds();
    apagarLedEm = 0;
  }
}
