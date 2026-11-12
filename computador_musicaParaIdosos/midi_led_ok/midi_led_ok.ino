#include <Keypad.h>
#include <Control_Surface.h>

BluetoothMIDI_Interface midi;

// ------------------- CONFIGURAÇÃO DO TECLADO --------------------------
const byte ROWS = 4, COLS = 5;

char keys[ROWS][COLS] = {
  {'1','2','3','4','5'},
  {'6','7','8','9','A'},
  {'B','C','D','E','F'},
  {'G','H','I','J','K'}
};

byte rowPins[ROWS] = {25, 26, 27, 14};
byte colPins[COLS] = {18, 5, 17, 16, 4};

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

enum PlayStyle { LEGATO, STACCATO, MARCATO };
PlayStyle playStyle = STACCATO; // Estilo padrão

// Estado da demonstração
enum DemoState { PLAYING_NOTE, GAP_BETWEEN_NOTES };
DemoState demoState = PLAYING_NOTE;
unsigned long noteStartTime = 0;
unsigned long gapStartTime = 0;

// Nomes das notas (para debug)
const char* noteNames[ROWS][COLS] = {
  {"C4","C#4","D4","D#4","E4"},
  {"F4","F#4","G4","G#4","A4"},
  {"A#4","B4","C5","C#5","D5"},
  {"D#5","E5","F5","F#5","G5"}
};


// ------------------- SHIFT REGISTERS --------------------------
const int dataPin      = 32;
const int clockPin     = 33;
const int latchPinCols = 23;  // SR1 - colunas
const int latchPinRows = 22;  // SR2 - linhas

// Deslocamento de bits em cada SR
const uint8_t COL_OFFSET = 1;  // ajuste conforme seu circuito
const uint8_t ROW_OFFSET = 1;

bool ledMatrix[ROWS][COLS] = {false};

// ------------------- MELODIAS --------------------------
struct Note {
  byte index;    // 0..19 (0 a 19 para 4x5)
  int  duration; // ms
};

// Melodia 1: "Dó Ré Mi "
Note melody1[] = {
  {0, 500}, {2, 500}, {4, 500}, {5, 500}, {7,500},
  {9, 500}, {11, 500}, {12, 500}, {14, 500}, {16,500},
  {17, 500}, {19, 1500}
};

// Melodia 2: "Parabéns pra Você"
Note melody2[] = {
  {0,300},{0,300},{2,600},{0,600},{5,600},{4,1200},
  {0,300},{0,300},{2,600},{0,600},{7,600},{5,1200}
};

// Melodia 3: "Brilha Brilha Estrelinha"
Note melody3[] = {
  {0,400},{0,400},{7,400},{7,400},{9,400},{9,400},{7,800},
  {5,400},{5,400},{4,400},{4,400},{2,400},{2,400},{0,800}
};

// Melodia 4: "Ode to Joy"
Note melody4[] = {
  {4,400},{4,400},{5,400},{7,400},{7,400},{5,400},{4,400},
  {2,400},{0,400},{0,400},{2,400},{4,400},{4,600},{2,200},{2,800}
};

// ------------------- VARIÁVEIS DO JOGO --------------------------
enum GameMode { MENU, LEARNING, PLAYING, FREE_PLAY };
GameMode currentMode = MENU;

Note*  currentMelody   = nullptr;
int    melodySize     = 0;
int    currentNoteIndex = 0;
int    score          = 0;
int    mistakes       = 0;
bool   waitingForInput = false;
bool   demonstrating   = false;
int    expectedNote    = -1;
unsigned long lastActionTime = 0;

// ------------------- ESTADOS NÃO BLOQUEANTES --------------------------
enum FeedbackState { NONE, CORRECT, INCORRECT, CELEBRATION };
FeedbackState feedbackState = NONE;
FeedbackState feedbackMode = NONE;
unsigned long feedbackStart = 0;
int feedbackStep = 0;
int feedbackIndex = 0;
const int FEEDBACK_DURATION = 400;  // ms
const int CELEBRATION_STEP_MS = 30; // ms por LED
const int CELEBRATION_STEP = 30;
int celebrationTotalSteps = 3 * ROWS * COLS;



// ------------------- FUNÇÕES AUXILIARES --------------------------
int charToIndex(char k) {
  if (k >= '1' && k <= '9') return k - '1';
  if (k >= 'A' && k <= 'K') return 9 + (k - 'A'); // A=9, B=10, ..., K=19
  return -1;
}

// ------------------- SETUP --------------------------
void setup() {
  Serial.begin(115200);
  pinMode(dataPin,      OUTPUT);
  pinMode(clockPin,     OUTPUT);
  pinMode(latchPinCols, OUTPUT);
  pinMode(latchPinRows, OUTPUT);
  clearAllLeds();
  showMenu();
  Control_Surface.begin();

}

// ------------------- LOOP PRINCIPAL --------------------------
void loop() {

  Control_Surface.loop();

  char key = keypad.getKey();
  if (key != NO_KEY) handleKeyPress(key);

  // Atualiza modos do jogo
  switch (currentMode) {
    case MENU:      break;
    case LEARNING:  runLearningMode();  break;
    case PLAYING:   runPlayingMode();   break;
    case FREE_PLAY: runFreePlay();      break;
  }
  // Gerencia feedback não bloqueante
  handleFeedback();

// ------------------- ATUALIZA FEEDBACK VISUAL --------------------------
if (feedbackMode != NONE) {
  unsigned long now = millis();

  if (feedbackMode == CORRECT || feedbackMode == INCORRECT) {
    if (now - feedbackStart < FEEDBACK_DURATION) {
      if (millis() % 100 < 50) {
        // Pisca LEDs verdes (índices 5 a 9)
        for (int i = 0; i < 20; i++) {
          ledMatrix[i/COLS][i%COLS] = (feedbackMode == CORRECT);
        }
      } else {
        clearAllLeds();
      }
    } else {
      clearAllLeds();
      feedbackMode = NONE;
      currentNoteIndex++;
      delay(200); // Pequeno delay OK aqui
      nextNote();
    }
  }

  else if (feedbackMode == CELEBRATION) {
    int step = (now - feedbackStart) / CELEBRATION_STEP;
    int totalSteps = 3 * ROWS * COLS;

    if (step < totalSteps) {
      int i = step % (ROWS * COLS);
      clearAllLeds();
      ledMatrix[i/COLS][i%COLS] = true;
    } else {
      clearAllLeds();
      feedbackMode = NONE;
      returnToMenu();
    }
  }
}

}

// Pisca Leds em Sequencia
void piscaLeds(){
  static unsigned long lastLedTime = 0;
  static int ledRow = 0, ledCol = 0;
  int i;
  for (i = 0; i < 20; i++){
  if (millis() - lastLedTime > 500) {
    clearAllLeds();
    setLed(ledRow, ledCol, true);
    ledCol++;
    if (ledCol >= 5) {
      ledCol = 0;
      ledRow++;
    }
    if (ledRow >= 4) {
      ledRow = 0;
      ledCol = 0;
    }
    lastLedTime = millis();
  }
  delay(10); // Evita sobrecarga
  }
}

void setLed(int row, int col, bool on) {
  if (row >= 0 && row < 4 && col >= 0 && col < 5) {
    // Ativa ou desativa o LED na posição (row, col)
    if (on) {
      digitalWrite(latchPinCols, LOW);
      byte colData = 0xFF;
      colData &= ~(1 << col);  // coluna = LOW para acender
      byte shiftedCol = colData << COL_OFFSET;
      shiftOut(dataPin, clockPin, MSBFIRST, shiftedCol);
      digitalWrite(latchPinCols, HIGH);

      digitalWrite(latchPinRows, LOW);
      byte rowData = 1 << (row + ROW_OFFSET);
      shiftOut(dataPin, clockPin, MSBFIRST, rowData);
      digitalWrite(latchPinRows, HIGH);
    } else {
      clearAllLeds();
    }
  }
}

// ------------------- MANUSEIO DE TECLAS --------------------------
void handleKeyPress(char key) {
  int idx = charToIndex(key);

  if (currentMode == MENU) {
    switch (key) {
      case '1': startMelody(melody1, sizeof(melody1)/sizeof(Note), "Do Re Mi"); break;
      case '2': startMelody(melody2, sizeof(melody2)/sizeof(Note), "Parabens pra Voce"); break;
      case '3': startMelody(melody3, sizeof(melody3)/sizeof(Note), "Brilha Estrelinha"); break;
      case '4': startMelody(melody4, sizeof(melody4)/sizeof(Note), "Ode to Joy"); break;
      case '5': startFreePlay(); break;
      case '6': 
        // Muda para o próximo estilo
        playStyle = static_cast<PlayStyle>((playStyle + 1) % 3);
        Serial.print("Estilo alterado para: ");
        Serial.println(getStyleName());
        showMenu(); // Atualiza o menu
        break;
    }
  }
  
  else if (currentMode == LEARNING || currentMode == PLAYING) {
    if (waitingForInput && idx >= 0) {
      checkUserInput(idx);
    }
    if (key == 'K') returnToMenu();
  }
  else if (currentMode == FREE_PLAY) {
    if (idx >= 0) {
      int row = idx / COLS;
      int col = idx % COLS;
      flashLed(row, col, 100);
      Serial.print("Nota: ");
      Serial.println(noteNames[row][col]);
    }
    if (key == 'K') returnToMenu();
  }
}

// ------------------- MENU --------------------------
void showMenu() {
  Serial.println("\n=== PROFESSOR DE MELODIAS ===");
  Serial.println("ESCOLHA UMA OPCAO:");
  Serial.println("1 - Do Re Mi ");
  Serial.println("2 - Parabens pra Voce");
  Serial.println("3 - Brilha Estrelinha");
  Serial.println("4 - Ode to Joy");
  Serial.println("5 - Modo Livre");
  Serial.println("6 - Estilo: " + getStyleName());
  Serial.println("Pressione uma tecla...\n");
}

String getStyleName() {
  switch(playStyle) {
    case LEGATO: return "Legato";
    case STACCATO: return "Staccato";
    case MARCATO: return "Marcato";
    default: return "Desconhecido";
  }
}
// ------------------- INICIA A MELODIA --------------------------
void startMelody(Note* melody, int size, const char* name) {
  currentMelody     = melody;
  melodySize        = size;
  currentNoteIndex  = 0;
  score             = 0;
  mistakes          = 0;
  waitingForInput   = false;
  demonstrating     = true;
  currentMode       = LEARNING;
  demoState         = PLAYING_NOTE; // Inicia no estado correto

  Serial.println("\n=====================================");
  Serial.print("APRENDENDO: ");
  Serial.println(name);
  Serial.println("Demonstração...");
  Serial.println("Pressione K para voltar ao menu\n");

  clearAllLeds();
  
  // MOSTRA A PRIMEIRA NOTA IMEDIATAMENTE
  if (melodySize > 0) {
    Note firstNote = currentMelody[0];
    int row = firstNote.index / COLS;
    int col = firstNote.index % COLS;
    setLed(row, col, true);
    Serial.print("-> ");
    Serial.print(noteNames[row][col]);
    Serial.print(" (Tecla ");
    Serial.print(keys[row][col]);
    Serial.println(")");
    noteStartTime = millis();
  }
}
// ------------------- MODO DEMONSTRAÇÃO --------------------------
void runLearningMode() {
  if (!demonstrating) return;
  unsigned long currentTime = millis();

  // Se já mostramos todas as notas
  if (currentNoteIndex >= melodySize) {
    clearAllLeds();
    demonstrating = false;
    currentNoteIndex = 0;
    currentMode = PLAYING;
    Serial.println("\nSua vez! Repita a sequência:");
    nextNote();
    return;
  }

  Note currentNote = currentMelody[currentNoteIndex];
  int gapTime;
  switch(playStyle) {
    case LEGATO: gapTime = 20; break;
    case STACCATO: gapTime = 150; break;
    case MARCATO: gapTime = 250; break;
  }
  switch (demoState) {
    case PLAYING_NOTE:
      // Verifica se já passou o tempo da nota atual
      if (currentTime - noteStartTime >= currentNote.duration) {
        clearAllLeds();
        
        // Verifica se há próxima nota e se é igual à atual
        bool isSameAsNext = false;
        if (currentNoteIndex + 1 < melodySize) {
          Note nextNote = currentMelody[currentNoteIndex + 1];
          isSameAsNext = (currentNote.index == nextNote.index);
        }
        
        // Se for igual à próxima, entra no estado de GAP
        if (isSameAsNext) {
          demoState = GAP_BETWEEN_NOTES;
          gapStartTime = currentTime;
        } else {
          // Vai direto para a próxima nota
          currentNoteIndex++;
          if (currentNoteIndex < melodySize) {
            Note nextNote = currentMelody[currentNoteIndex];
            int row = nextNote.index / COLS;
            int col = nextNote.index % COLS;
            setLed(row, col, true);
            Serial.print("-> ");
            Serial.print(noteNames[row][col]);
            Serial.print(" (Tecla ");
            Serial.print(keys[row][col]);
            Serial.println(")");
            noteStartTime = currentTime;
          }
        }
      }
      break;
      
    case GAP_BETWEEN_NOTES:
      // Espera um breve intervalo antes de acender novamente
      if (currentTime - gapStartTime >= gapTime) {
        demoState = PLAYING_NOTE;
        currentNoteIndex++;
        if (currentNoteIndex < melodySize) {
          Note nextNote = currentMelody[currentNoteIndex];
          int row = nextNote.index / COLS;
          int col = nextNote.index % COLS;
          setLed(row, col, true);
          Serial.print("-> ");
          Serial.print(noteNames[row][col]);
          Serial.print(" (Tecla ");
          Serial.print(keys[row][col]);
          Serial.println(")");
          noteStartTime = currentTime;
        }
      }
      break;
  }
}
// ------------------- MODO JOGANDO --------------------------
void runPlayingMode() {
 /* if (waitingForInput) {
    static unsigned long lastBlink = 0;
    static bool blinkState = false;
    if (millis() - lastBlink > 300) {
      int row = expectedNote / COLS;
      int col = expectedNote % COLS;
      ledMatrix[row][col] = blinkState;
      blinkState = !blinkState;
      lastBlink = millis();
    }
  }*/
}

// ------------------- AVANÇA PARA A PRÓXIMA NOTA --------------------------
void nextNote() {
  if (currentNoteIndex < melodySize) {
    Note note = currentMelody[currentNoteIndex];
    expectedNote = note.index;
    waitingForInput = true;
    int row = expectedNote / COLS;
    int col = expectedNote % COLS;
    //ledMatrix[row][col] = true;
    setLed(row, col, true);
    Serial.print("Toque: ");
    Serial.print(noteNames[row][col]);
    Serial.println();
  }
  else {
    endMelody();
  }
}

// ------------------- CHECA INPUT DO ALUNO --------------------------
void checkUserInput(int pressedIndex) {
  clearAllLeds();
  waitingForInput = false;

  if (pressedIndex == expectedNote) {
    Serial.println("✓ Correto!");
    score++;
    feedbackMode = CORRECT;
  } else {
    Serial.println("✗ Errado!");
    mistakes++;
    feedbackMode = INCORRECT;
  }
  feedbackStart = millis();
  feedbackIndex = 0;
}

// ------------------- FIM DA MELODIA --------------------------
void endMelody() {
  piscaLeds();
  Serial.println("\n=== MELODIA COMPLETA! ===");
  Serial.print("Acertos: "); Serial.print(score);
  Serial.print(" | Erros: "); Serial.println(mistakes);
  float acc = (float)score / (score + mistakes) * 100;
  Serial.print("Precisão: "); Serial.print(acc); Serial.println("%");

  if (acc == 100) {
    Serial.println("PERFEITO!");
    feedbackState = CELEBRATION;
    feedbackStart = millis();
    feedbackStep = 0;
  } else {
    delay(1000);
    returnToMenu();
  }
}

// ------------------- MANAGE FEEDBACK VISUAL (não bloqueante) --------------------------
void handleFeedback() {
  if (feedbackState == NONE) return;
  unsigned long now = millis();

  if (feedbackState == CORRECT || feedbackState == INCORRECT) {
    if (now - feedbackStart < FEEDBACK_DURATION) {
      bool on = (now % 150 < 75); // Pisca rápido
      for (int i = 0; i < ROWS * COLS; i++) {
        int r = i / COLS, c = i % COLS;
        if (feedbackState == CORRECT && i >= 0 && i < 20) {
          ledMatrix[r][c] = on;
        } else if (feedbackState == INCORRECT && i == 0) {
          ledMatrix[r][c] = on;
        }
      }
    } else {
      clearAllLeds();
      feedbackState = NONE;
      nextNote(); // Continua a sequência
    }
  }

  else if (feedbackState == CELEBRATION) {
    int step = (now - feedbackStart) / CELEBRATION_STEP_MS;
    if (step < celebrationTotalSteps) {
      int i = step % (ROWS * COLS);
      clearAllLeds();
      int r = i / COLS, c = i % COLS;
      ledMatrix[r][c] = true;
    } else {
      clearAllLeds();
      feedbackState = NONE;
      returnToMenu();
    }
  }
}

// ------------------- MODO LIVRE --------------------------
void startFreePlay() {
  currentMode = FREE_PLAY;
  Serial.println("\n=== MODO LIVRE ===");
  Serial.println("Toque à vontade!");
  Serial.println("Pressione P para voltar ao menu\n");
}

void runFreePlay() {
  // Tudo em handleKeyPress
}

// ------------------- FLASH DE LED --------------------------
void flashLed(int row, int col, int duration) {
  ledMatrix[row][col] = true;
//  updateLedMatrix();
  delay(duration);
  ledMatrix[row][col] = false;
}

// ------------------- ATUALIZAÇÃO DA MATRIZ NO 74HC595 --------------------------
/*void updateLedMatrix() {
  static unsigned long lastUpdate = 0;
  static byte currentRow = 0;
  if (millis() - lastUpdate < 2) return;
  lastUpdate = millis();

  // Colunas: 0 = ativa (LOW), 1 = desativada (HIGH)
  byte colData = 0xFF;
  for (byte c = 0; c < COLS; c++) {
    if (ledMatrix[currentRow][c]) {
      colData &= ~(1 << c); // coluna em LOW
    }
  }
  uint8_t shiftedCol = colData << COL_OFFSET;
  digitalWrite(latchPinCols, LOW);
  shiftOut(dataPin, clockPin, MSBFIRST, shiftedCol);
  digitalWrite(latchPinCols, HIGH);

  // Linhas: apenas uma ativa (HIGH)
  uint8_t rowData = 1 << (currentRow + ROW_OFFSET);
  digitalWrite(latchPinRows, LOW);
  shiftOut(dataPin, clockPin, MSBFIRST, rowData);
  digitalWrite(latchPinRows, HIGH);

  // Próxima linha
  currentRow++;
  if (currentRow >= ROWS) currentRow = 0;
}*/
/*
// ------------------- LIMPA TODOS OS LEDs --------------------------
void clearAllLeds() {
  digitalWrite(latchPinCols, LOW);
  shiftOut(dataPin, clockPin, MSBFIRST, (uint8_t)(0xFF << COL_OFFSET));
  digitalWrite(latchPinCols, HIGH);

  digitalWrite(latchPinRows, LOW);
  shiftOut(dataPin, clockPin, MSBFIRST, 0x00);
  digitalWrite(latchPinRows, HIGH);

  memset(ledMatrix, 0, sizeof(ledMatrix));
}*/

// ------------------- FUNÇÕES AUXILIARES --------------------------
void clearAllLeds() {
  digitalWrite(latchPinCols, LOW);
  shiftOut(dataPin, clockPin, MSBFIRST, (uint8_t)(0xFF << COL_OFFSET));
  digitalWrite(latchPinCols, HIGH);

  digitalWrite(latchPinRows, LOW);
  shiftOut(dataPin, clockPin, MSBFIRST, (uint8_t)0x00);
  digitalWrite(latchPinRows, HIGH);
  memset(ledMatrix, 0, sizeof(ledMatrix));
}

// ------------------- VOLTAR AO MENU --------------------------
void returnToMenu() {
  currentMode = MENU;
  feedbackState = NONE;
  clearAllLeds();
  showMenu();
}