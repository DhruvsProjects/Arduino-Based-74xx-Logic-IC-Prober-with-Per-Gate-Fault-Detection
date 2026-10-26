// ============================================================
//  IC PROBER v6 — Consensus Full-Chip Probing
//  Arduino Uno R3 + SSD1306 OLED (128x64, I2C on A4/A5)
//
//  Every gate in a layout is probed once and its function is
//  detected independently, then a majority vote decides the
//  layout. Two or more agreeing gates confirms it. If no layout
//  ever reaches consensus, the first layout that produced one
//  valid gate is used: Gate 1 is assumed good and the remaining
//  gates are reported as faulty.
//
//  Display is laid out for dual-colour panels: rows 0-15 are
//  yellow, 16-63 blue, with a physical seam between segments.
//  Nothing straddles row 16.
//
//  WIRING:
//    IC pin  1 -> D2      IC pin  8 -> D8
//    IC pin  2 -> D3      IC pin  9 -> D9
//    IC pin  3 -> D4      IC pin 10 -> D11
//    IC pin  4 -> D5      IC pin 11 -> A1
//    IC pin  5 -> D6      IC pin 12 -> A2
//    IC pin  6 -> D7      IC pin 13 -> A3
//    IC pin  7 -> GND (hardwire)
//    IC pin 14 -> VCC (hardwire)
//    D10 -> BUTTON (active LOW, internal pullup)
//    A4  -> OLED SDA
//    A5  -> OLED SCL
//    A0, D12, D13 unused. 220 ohm series resistors recommended.
// ============================================================

#include <U8g2lib.h>
#include <Wire.h>

U8G2_SSD1306_128X64_NONAME_1_SW_I2C u8g2(
  U8G2_R0, SCL, SDA, U8X8_PIN_NONE
);

#define BTN_PIN 10

// ============================================================
// PIN MAPPING: IC pin (1-14) -> Arduino pin
// Pin 7 = GND and Pin 14 = VCC are hardwired (-1)
// ============================================================
const int8_t icPin[15] = {
  -1,               // [0]  unused
   2,  3,  4,       // IC 1-3   -> D2, D3, D4
   5,  6,  7,       // IC 4-6   -> D5, D6, D7
  -1,               // IC 7     = GND
   8,  9,           // IC 8-9   -> D8, D9
  11, A1, A2, A3,   // IC 10-13 -> D11, A1, A2, A3
  -1                // IC 14    = VCC
};

// ============================================================
// GATE FUNCTION ENUM
// ============================================================
enum GateFunc : uint8_t {
  FN_AND,
  FN_NAND,
  FN_OR,
  FN_NOR,
  FN_XOR,
  FN_XNOR,
  FN_NOT,
  FN_BUF,
  FN_NONE
};

// ============================================================
// GATE FAULT ENUM
// ============================================================
enum GateFault : uint8_t {
  GF_OK,
  GF_STUCK_LOW,
  GF_STUCK_HIGH,
  GF_WRONG_FUNCTION,
  GF_NO_RESPONSE
};

// ============================================================
// FUNCTION NAMES
// ============================================================
const char S0[] PROGMEM = "AND";
const char S1[] PROGMEM = "NAND";
const char S2[] PROGMEM = "OR";
const char S3[] PROGMEM = "NOR";
const char S4[] PROGMEM = "XOR";
const char S5[] PROGMEM = "XNOR";
const char S6[] PROGMEM = "NOT";
const char S7[] PROGMEM = "BUF";
const char S8[] PROGMEM = "?";

const char *const FNAMES[] PROGMEM = {
  S0, S1, S2, S3, S4, S5, S6, S7, S8
};

void getFName(GateFunc f, char *buf, uint8_t sz) {
  strncpy_P(
    buf,
    (const char *)pgm_read_ptr(&FNAMES[f]),
    sz - 1
  );
  buf[sz - 1] = '\0';
}

// ============================================================
// IC PINOUT LAYOUT DATABASE
//
//   byte 0 : numGates
//   byte 1 : inputsPerGate (N)
//   then per gate: N input pins, 1 output pin
// ============================================================

#define NUM_LAYOUTS 6

// Layout 0: 7400 NAND / 7408 AND / 7432 OR / 7486 XOR
const uint8_t L0[] PROGMEM = {
  4, 2,
  1,  2,  3,
  4,  5,  6,
  9, 10,  8,
 12, 13, 11
};

// Layout 1: 7402 NOR
const uint8_t L1[] PROGMEM = {
  4, 2,
  2,  3,  1,
  5,  6,  4,
  8,  9, 10,
 11, 12, 13
};

// Layout 2: 7404 NOT / 7407 BUF
const uint8_t L2[] PROGMEM = {
  6, 1,
  1,  2,
  3,  4,
  5,  6,
  9,  8,
 11, 10,
 13, 12
};

// Layout 3: 7410 NAND / 7411 AND / 7427 NOR
const uint8_t L3[] PROGMEM = {
  3, 3,
  1,  2, 13, 12,
  3,  4,  5,  6,
  9, 10, 11,  8
};

// Layout 4: 7420 NAND / 7421 AND
const uint8_t L4[] PROGMEM = {
  2, 4,
  1,  2,  4,  5,  6,
  9, 10, 12, 13,  8
};

// Layout 5: 7430 NAND
const uint8_t L5[] PROGMEM = {
  1, 8,
  1, 2, 3, 4, 5, 6, 11, 12, 8
};

const uint8_t *const LAYOUTS[NUM_LAYOUTS] PROGMEM = {
  L0, L1, L2, L3, L4, L5
};

// ============================================================
// LAYOUT NAMES
// ============================================================
const char N0[] PROGMEM = "Quad 2-In";
const char N1[] PROGMEM = "Quad 2-In Rev";
const char N2[] PROGMEM = "Hex 1-In";
const char N3[] PROGMEM = "Triple 3-In";
const char N4[] PROGMEM = "Dual 4-In";
const char N5[] PROGMEM = "Single 8-In";

const char *const LNAMES[NUM_LAYOUTS] PROGMEM = {
  N0, N1, N2, N3, N4, N5
};

// ============================================================
// IC NUMBER DATABASE
// ============================================================
struct ICEntry {
  uint8_t  lay;
  uint8_t  func;
  uint16_t num;
};

const ICEntry ICDB[] PROGMEM = {
  {0, FN_NAND, 7400},
  {1, FN_NOR,  7402},
  {2, FN_NOT,  7404},
  {2, FN_BUF,  7407},
  {0, FN_AND,  7408},
  {3, FN_NAND, 7410},
  {3, FN_AND,  7411},
  {4, FN_NAND, 7420},
  {4, FN_AND,  7421},
  {3, FN_NOR,  7427},
  {5, FN_NAND, 7430},
  {0, FN_OR,   7432},
  {0, FN_XOR,  7486}
};

#define NUM_ICS (sizeof(ICDB) / sizeof(ICDB[0]))

uint16_t lookupIC(uint8_t layIdx, GateFunc fn) {

  for (uint8_t i = 0; i < NUM_ICS; i++) {

    if (
      pgm_read_byte(&ICDB[i].lay) == layIdx &&
      pgm_read_byte(&ICDB[i].func) == (uint8_t)fn
    ) {
      return pgm_read_word(&ICDB[i].num);
    }
  }

  return 0;
}

// ============================================================
// RESULT STATE
// ============================================================
#define MAX_TT 16
#define MAX_GATES 6

GateFunc resFunc = FN_NONE;

uint8_t  resLay = 0;
uint16_t resIC  = 0;

bool     resVerify = false;
uint8_t  resVotes  = 0;

uint8_t resNIn    = 0;
uint8_t resNGates = 0;

uint8_t resTT[MAX_TT];
uint8_t resNTT = 0;

uint8_t resInPins[8];
uint8_t resOutPin = 0;

bool resFound = false;

// ============================================================
// GATEWISE STATE
// ============================================================
GateFault gateFault[MAX_GATES];
GateFunc  gateDetectedFunc[MAX_GATES];
uint8_t   gateFailPattern[MAX_GATES];
bool      gateHasFailPattern[MAX_GATES];

// Raw observations for every gate of the layout under test
uint8_t gObs[MAX_GATES][MAX_TT];
uint8_t gNObs[MAX_GATES];

// ============================================================
// CLEAR GATE RESULTS
// ============================================================
void clearGateResults() {

  for (uint8_t i = 0; i < MAX_GATES; i++) {

    gateFault[i]          = GF_NO_RESPONSE;
    gateDetectedFunc[i]   = FN_NONE;
    gateFailPattern[i]    = 0;
    gateHasFailPattern[i] = false;
    gNObs[i]              = 0;
  }
}

// ============================================================
// LOW LEVEL PIN CONTROL
// ============================================================
void releaseAll() {

  for (uint8_t p = 1; p <= 13; p++) {

    if (icPin[p] >= 0)
      pinMode(icPin[p], INPUT);
  }

  delay(2);
}

void drivePin(uint8_t ic, uint8_t v) {

  int8_t a = icPin[ic];

  if (a < 0)
    return;

  pinMode(a, OUTPUT);
  digitalWrite(a, v);
}

uint8_t readPin(uint8_t ic) {

  int8_t a = icPin[ic];

  if (a < 0)
    return 0;

  pinMode(a, INPUT_PULLUP);
  delayMicroseconds(200);

  return digitalRead(a);
}

// ============================================================
// EXPECTED OUTPUT
// ============================================================
uint8_t expected(GateFunc fn, uint8_t nIn, uint8_t bits) {

  uint8_t r;

  switch (fn) {

    case FN_AND:
    case FN_NAND:

      r = 1;

      for (uint8_t i = 0; i < nIn; i++)
        r &= (bits >> i) & 1;

      return (fn == FN_NAND) ? (r ^ 1) : r;

    case FN_OR:
    case FN_NOR:

      r = 0;

      for (uint8_t i = 0; i < nIn; i++)
        r |= (bits >> i) & 1;

      return (fn == FN_NOR) ? (r ^ 1) : r;

    case FN_XOR:
    case FN_XNOR:

      r = 0;

      for (uint8_t i = 0; i < nIn; i++)
        r ^= (bits >> i) & 1;

      return (fn == FN_XNOR) ? (r ^ 1) : r;

    case FN_NOT:
      return (bits & 1) ^ 1;

    case FN_BUF:
      return bits & 1;

    default:
      return 0;
  }
}

// ============================================================
// TRUTH TABLE MATCH (1-4 inputs)
// ============================================================
GateFunc matchTT(uint8_t obs[], uint8_t nIn, uint8_t nC) {

  const GateFunc all[] = {
    FN_AND, FN_NAND, FN_OR, FN_NOR,
    FN_XOR, FN_XNOR, FN_NOT, FN_BUF
  };

  uint8_t lo;
  uint8_t hi;

  if (nIn == 1) {
    lo = 6;
    hi = 8;
  } else {
    lo = 0;
    hi = 6;
  }

  for (uint8_t f = lo; f < hi; f++) {

    bool ok = true;

    for (uint8_t c = 0; c < nC && ok; c++) {

      if (expected(all[f], nIn, c) != obs[c])
        ok = false;
    }

    if (ok)
      return all[f];
  }

  return FN_NONE;
}

// ============================================================
// 8-INPUT MATCH (strategic patterns)
// ============================================================
GateFunc match8in(uint8_t obs[], uint8_t nIn) {

  bool a0_1 = (obs[0] == 1);
  bool a1_0 = (obs[1] == 0);
  bool a0_0 = (obs[0] == 0);
  bool a1_1 = (obs[1] == 1);

  bool oneLow_all1 = true;
  bool oneLow_all0 = true;

  for (uint8_t k = 0; k < nIn; k++) {

    if (obs[2 + k] != 1)
      oneLow_all1 = false;

    if (obs[2 + k] != 0)
      oneLow_all0 = false;
  }

  if (a0_1 && a1_0 && oneLow_all1)
    return FN_NAND;

  if (a0_1 && a1_0 && oneLow_all0)
    return FN_NOR;

  if (a0_0 && a1_1 && oneLow_all0)
    return FN_AND;

  if (a0_0 && a1_1 && oneLow_all1)
    return FN_OR;

  return FN_NONE;
}

// ============================================================
// DETECT FUNCTION (dispatch on input count)
// ============================================================
GateFunc detectFunc(uint8_t obs[], uint8_t nIn, uint8_t nObs) {

  if (nIn <= 4)
    return matchTT(obs, nIn, nObs);

  return match8in(obs, nIn);
}

// ============================================================
// PROBE ONE GATE
// ============================================================
uint8_t probeGate(
  const uint8_t *layPgm,
  uint8_t gateIdx,
  uint8_t nIn,
  uint8_t out[]
) {

  uint8_t  stride = nIn + 1;
  uint16_t off    = 2 + (uint16_t)gateIdx * stride;

  uint8_t inP[8];

  for (uint8_t i = 0; i < nIn; i++)
    inP[i] = pgm_read_byte(layPgm + off + i);

  uint8_t outP = pgm_read_byte(layPgm + off + nIn);

  if (gateIdx == 0) {

    for (uint8_t i = 0; i < nIn; i++)
      resInPins[i] = inP[i];

    resOutPin = outP;
  }

  releaseAll();

  // ---- 1-4 input gates: exhaustive ----
  if (nIn <= 4) {

    uint8_t nC = 1 << nIn;

    for (uint8_t c = 0; c < nC; c++) {

      for (uint8_t i = 0; i < nIn; i++)
        drivePin(inP[i], (c >> i) & 1);

      delay(1);

      out[c] = readPin(outP);
    }

    releaseAll();

    return nC;
  }

  // ---- 8 input gate: strategic patterns ----

  for (uint8_t i = 0; i < nIn; i++)
    drivePin(inP[i], 0);

  delay(1);
  out[0] = readPin(outP);
  releaseAll();

  for (uint8_t i = 0; i < nIn; i++)
    drivePin(inP[i], 1);

  delay(1);
  out[1] = readPin(outP);
  releaseAll();

  for (uint8_t k = 0; k < nIn; k++) {

    for (uint8_t i = 0; i < nIn; i++)
      drivePin(inP[i], (i == k) ? 0 : 1);

    delay(1);
    out[2 + k] = readPin(outP);
    releaseAll();
  }

  return 2 + nIn;
}

// ============================================================
// PROBE EVERY GATE IN A LAYOUT
// ============================================================
void probeAllGates(
  const uint8_t *lay,
  uint8_t nGates,
  uint8_t nIn
) {

  for (uint8_t g = 0; g < nGates && g < MAX_GATES; g++) {

    gNObs[g] = probeGate(lay, g, nIn, gObs[g]);

    gateDetectedFunc[g] =
      detectFunc(gObs[g], nIn, gNObs[g]);
  }
}

// ============================================================
// MAJORITY VOTE ACROSS GATES
//
// Returns the most commonly detected function and writes the
// number of agreeing gates into *bestCount.
// Ties break in favour of Gate 1.
// ============================================================
GateFunc voteBest(uint8_t nGates, uint8_t *bestCount) {

  uint8_t counts[8];

  for (uint8_t f = 0; f < 8; f++)
    counts[f] = 0;

  for (uint8_t g = 0; g < nGates && g < MAX_GATES; g++) {

    GateFunc f = gateDetectedFunc[g];

    if (f != FN_NONE)
      counts[f]++;
  }

  uint8_t bi = 0;
  uint8_t bc = 0;

  for (uint8_t f = 0; f < 8; f++) {

    if (counts[f] > bc) {
      bc = counts[f];
      bi = f;
    }
  }

  GateFunc g0 = gateDetectedFunc[0];

  if (g0 != FN_NONE && counts[g0] == bc)
    bi = g0;

  *bestCount = bc;

  return (bc == 0) ? FN_NONE : (GateFunc)bi;
}

// ============================================================
// CLASSIFY GATE FAULT (1-4 inputs)
// ============================================================
GateFault classifyGateFault(
  uint8_t obs[],
  uint8_t nObs,
  uint8_t nIn,
  GateFunc expectedFn,
  uint8_t *failPattern
) {

  bool allLow  = true;
  bool allHigh = true;

  for (uint8_t i = 0; i < nObs; i++) {

    if (obs[i] != 0)
      allLow = false;

    if (obs[i] != 1)
      allHigh = false;
  }

  if (allLow) {
    *failPattern = 0;
    return GF_STUCK_LOW;
  }

  if (allHigh) {
    *failPattern = 0;
    return GF_STUCK_HIGH;
  }

  for (uint8_t c = 0; c < nObs; c++) {

    if (obs[c] != expected(expectedFn, nIn, c)) {
      *failPattern = c;
      return GF_WRONG_FUNCTION;
    }
  }

  return GF_OK;
}

// ============================================================
// CLASSIFY GATE FAULT (8 inputs)
// ============================================================
GateFault classify8GateFault(
  uint8_t obs[],
  uint8_t nIn,
  GateFunc expectedFn,
  uint8_t *failPattern
) {

  bool allLow  = true;
  bool allHigh = true;

  for (uint8_t i = 0; i < 2 + nIn; i++) {

    if (obs[i] != 0)
      allLow = false;

    if (obs[i] != 1)
      allHigh = false;
  }

  if (allLow) {
    *failPattern = 0;
    return GF_STUCK_LOW;
  }

  if (allHigh) {
    *failPattern = 0;
    return GF_STUCK_HIGH;
  }

  if (match8in(obs, nIn) == expectedFn)
    return GF_OK;

  for (uint8_t p = 0; p < 2 + nIn; p++) {

    uint8_t expectedValue;

    if (p == 0) {

      expectedValue = expected(expectedFn, nIn, 0);

    } else if (p == 1) {

      expectedValue = expected(expectedFn, nIn, 255);

    } else {

      uint8_t bits = 255;
      bits &= ~(1 << (p - 2));

      expectedValue = expected(expectedFn, nIn, bits);
    }

    if (obs[p] != expectedValue) {
      *failPattern = p;
      return GF_WRONG_FUNCTION;
    }
  }

  *failPattern = 0;
  return GF_WRONG_FUNCTION;
}

// ============================================================
// CLASSIFY ALL GATES AGAINST THE ACCEPTED FUNCTION
// ============================================================
void classifyAllGates(uint8_t nGates, uint8_t nIn, GateFunc fn) {

  for (uint8_t g = 0; g < nGates && g < MAX_GATES; g++) {

    uint8_t   failPattern = 0;
    GateFault fault;

    if (nIn <= 4) {

      fault = classifyGateFault(
        gObs[g], gNObs[g], nIn, fn, &failPattern
      );

    } else {

      fault = classify8GateFault(
        gObs[g], nIn, fn, &failPattern
      );
    }

    gateFault[g]          = fault;
    gateFailPattern[g]    = failPattern;
    gateHasFailPattern[g] = (fault == GF_WRONG_FUNCTION);
  }
}

// ============================================================
// COMMIT A LAYOUT AS THE IDENTIFIED IC
// ============================================================
void finishIdentification(
  uint8_t li,
  uint8_t nGates,
  uint8_t nIn,
  GateFunc fn,
  uint8_t votes
) {

  resFound  = true;
  resFunc   = fn;
  resLay    = li;
  resNIn    = nIn;
  resNGates = nGates;
  resVotes  = votes;
  resVerify = (votes >= 2);

  resNTT = gNObs[0];
  memcpy(resTT, gObs[0], gNObs[0]);

  resIC = lookupIC(li, fn);

  classifyAllGates(nGates, nIn, fn);
}

// ============================================================
// SERIAL HELPERS
// ============================================================
void serialPrint2(uint8_t n) {

  if (n < 10)
    Serial.print(' ');

  Serial.print(n);
}

void serialPrintPinList(
  const uint8_t *layPgm,
  uint8_t gateIdx,
  uint8_t nIn
) {

  uint8_t  stride = nIn + 1;
  uint16_t off    = 2 + (uint16_t)gateIdx * stride;

  Serial.print(F("P"));

  for (uint8_t i = 0; i < nIn; i++) {

    if (i)
      Serial.print(F(",P"));

    Serial.print(pgm_read_byte(layPgm + off + i));
  }

  Serial.print(F(" -> P"));
  Serial.print(pgm_read_byte(layPgm + off + nIn));
}

void printFailPattern(uint8_t pattern, uint8_t nIn) {

  Serial.print(F("      Failing input: "));

  for (uint8_t i = 0; i < nIn; i++)
    Serial.print((pattern >> i) & 1);

  Serial.println();
}

// ============================================================
// PIN ROLE
// ============================================================
char pinRole(uint8_t p) {

  if (p == 7)
    return 'G';

  if (p == 14)
    return 'V';

  if (!resFound)
    return '?';

  const uint8_t *lay =
    (const uint8_t *)pgm_read_ptr(&LAYOUTS[resLay]);

  uint8_t stride = resNIn + 1;

  for (uint8_t g = 0; g < resNGates; g++) {

    uint16_t off = 2 + (uint16_t)g * stride;

    for (uint8_t i = 0; i < resNIn; i++) {

      if (pgm_read_byte(lay + off + i) == p)
        return 'I';
    }

    if (pgm_read_byte(lay + off + resNIn) == p)
      return 'O';
  }

  return '-';
}

// ============================================================
// BUILD A COMPACT PIN STRING FOR ONE GATE
//
// Produces e.g.  "1,2>3"  or  "1,2,13>12"
// Buffer must hold at least 28 bytes for the 8-input case.
// ============================================================
void buildPinStr(uint8_t gateIdx, char *buf) {

  buf[0] = '\0';

  if (!resFound)
    return;

  const uint8_t *lay =
    (const uint8_t *)pgm_read_ptr(&LAYOUTS[resLay]);

  uint8_t  stride = resNIn + 1;
  uint16_t off    = 2 + (uint16_t)gateIdx * stride;

  char num[4];
  uint8_t pos = 0;

  for (uint8_t i = 0; i < resNIn; i++) {

    if (i)
      buf[pos++] = ',';

    itoa(pgm_read_byte(lay + off + i), num, 10);

    for (uint8_t k = 0; num[k]; k++)
      buf[pos++] = num[k];
  }

  buf[pos++] = '>';

  itoa(pgm_read_byte(lay + off + resNIn), num, 10);

  for (uint8_t k = 0; num[k]; k++)
    buf[pos++] = num[k];

  buf[pos] = '\0';
}

// ============================================================
// DIP DIAGRAM (SERIAL)
// ============================================================
void printDIPdiagram() {

  char role[15];

  for (uint8_t p = 1; p <= 14; p++)
    role[p] = pinRole(p);

  Serial.println();
  Serial.println(F("         DIP-14 PINOUT"));
  Serial.println(F("        ______________"));
  Serial.println(F("       |    notch     |"));

  for (uint8_t i = 1; i <= 7; i++) {

    uint8_t rp = 15 - i;

    char lc = role[i];
    char rc = role[rp];

    switch (lc) {
      case 'I': Serial.print(F(" IN"));  break;
      case 'O': Serial.print(F("OUT"));  break;
      case 'G': Serial.print(F("GND"));  break;
      case '-': Serial.print(F(" NC"));  break;
      default:  Serial.print(F("  ?"));  break;
    }

    Serial.print(' ');
    serialPrint2(i);
    Serial.print(F(" |             | "));
    serialPrint2(rp);
    Serial.print(' ');

    switch (rc) {
      case 'I': Serial.println(F("IN"));  break;
      case 'O': Serial.println(F("OUT")); break;
      case 'V': Serial.println(F("VCC")); break;
      case '-': Serial.println(F("NC"));  break;
      default:  Serial.println(F("?"));   break;
    }
  }

  Serial.println(F("       |______________|"));
}

// ============================================================
// SERIAL RESULT
// ============================================================
void printResult() {

  char fname[8];
  char lname[16];

  getFName(resFunc, fname, sizeof(fname));

  strncpy_P(
    lname,
    (const char *)pgm_read_ptr(&LNAMES[resLay]),
    15
  );
  lname[15] = '\0';

  Serial.println();
  Serial.println(F("  ================================="));
  Serial.println(F("       IC IDENTIFICATION"));
  Serial.println(F("  ================================="));

  Serial.print(F("  IC:       "));

  if (resIC > 0)
    Serial.println(resIC);
  else
    Serial.println(F("Unknown"));

  Serial.print(F("  Function: "));
  Serial.println(fname);

  Serial.print(F("  Layout:   "));
  Serial.println(lname);

  Serial.print(F("  Inputs:   "));
  Serial.println(resNIn);

  Serial.print(F("  Gates:    "));
  Serial.println(resNGates);

  Serial.print(F("  Verified: "));

  if (resVerify) {

    Serial.print(F("YES ("));
    Serial.print(resVotes);
    Serial.print('/');
    Serial.print(resNGates);
    Serial.println(F(" gates agree)"));

  } else if (resNGates == 1) {

    Serial.println(F("-- (single gate)"));

  } else {

    Serial.println(F("NO - assumed from Gate 1"));
  }

  Serial.println(F("  ================================="));

  // ---- Gate health ----
  Serial.println();
  Serial.println(F("        GATE HEALTH"));
  Serial.println(F("        ----------"));

  const uint8_t *lay =
    (const uint8_t *)pgm_read_ptr(&LAYOUTS[resLay]);

  for (uint8_t g = 0; g < resNGates; g++) {

    Serial.print(F("  Gate "));
    Serial.print(g + 1);
    Serial.print(F("  "));

    serialPrintPinList(lay, g, resNIn);
    Serial.println();

    Serial.print(F("          "));

    switch (gateFault[g]) {

      case GF_OK:
        Serial.println(F("OK"));
        break;

      case GF_STUCK_LOW:
        Serial.println(F("FAULT: STUCK LOW"));
        break;

      case GF_STUCK_HIGH:
        Serial.println(F("FAULT: STUCK HIGH"));
        break;

      case GF_WRONG_FUNCTION:
        Serial.println(F("FAULT: WRONG FUNCTION"));
        break;

      default:
        Serial.println(F("FAULT: NO RESPONSE"));
        break;
    }

    if (
      gateFault[g] == GF_WRONG_FUNCTION &&
      gateHasFailPattern[g]
    ) {

      printFailPattern(gateFailPattern[g], resNIn);

      char detected[8];
      getFName(gateDetectedFunc[g], detected, sizeof(detected));

      Serial.print(F("      Detected: "));
      Serial.println(detected);

      Serial.print(F("      Expected: "));
      Serial.println(fname);
    }
  }

  // ---- Truth table for gate 1 ----
  if (resNIn <= 4) {

    Serial.println();
    Serial.println(F("        GATE 1 TRUTH TABLE"));

    Serial.print(F("        "));

    for (uint8_t i = 0; i < resNIn; i++) {
      Serial.print((char)('A' + i));
      Serial.print(' ');
    }

    Serial.println(F("| OUT"));
    Serial.println(F("        ----------------"));

    for (uint8_t c = 0; c < resNTT; c++) {

      Serial.print(F("        "));

      for (uint8_t i = 0; i < resNIn; i++) {
        Serial.print((c >> i) & 1);
        Serial.print(' ');
      }

      Serial.print(F("|  "));
      Serial.println(resTT[c]);
    }
  }

  printDIPdiagram();

  Serial.println();
}

void printAccepted() {

  Serial.println();
  Serial.println(F("  ========================================"));
  Serial.println(F("              TEST COMPLETE"));
  Serial.println(F("  ========================================"));
}

// ============================================================
// DISPLAY LAYOUT
//
// Dual-colour 128x64 panels render rows 0-15 yellow and rows
// 16-63 blue, with a physical seam between the segments.
// Nothing may straddle row 16: the header fills the yellow
// band exactly and all content lives below it.
// ============================================================
#define HDR_H 16       // yellow band height

void drawStrCentered(uint8_t y, const char *s, uint8_t charW) {

  uint8_t w = strlen(s) * charW;

  u8g2.drawStr((uint8_t)((128 - w) / 2), y, s);
}

void drawStrRight(uint8_t xRight, uint8_t y,
                  const char *s, uint8_t charW) {

  uint8_t w = strlen(s) * charW;

  u8g2.drawStr(xRight - w, y, s);
}

// Filled header occupying exactly the yellow band
void drawHeader(const char *title) {

  u8g2.drawBox(0, 0, 128, HDR_H);

  u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_6x10_tf);

  drawStrCentered(12, title, 6);

  u8g2.setDrawColor(1);
}

// Inverted footer bar, rows 53-63
void drawFooter(const char *msg) {

  u8g2.drawBox(0, 53, 128, 11);

  u8g2.setDrawColor(0);
  u8g2.setFont(u8g2_font_5x7_tf);

  drawStrCentered(61, msg, 5);

  u8g2.setDrawColor(1);
}

// Two-letter status code per gate
const char *faultCode(GateFault f) {

  switch (f) {
    case GF_OK:             return "OK";
    case GF_STUCK_LOW:      return "SL";
    case GF_STUCK_HIGH:     return "SH";
    case GF_WRONG_FUNCTION: return "FN";
    default:                return "NR";
  }
}

// ============================================================
// OLED IDLE
// ============================================================
void drawIdle() {

  u8g2.firstPage();

  do {

    drawHeader("IC PROBER v6");

    u8g2.drawRFrame(46, 24, 36, 24, 2);

    u8g2.drawCircle(
      64, 24, 3,
      U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT
    );

    for (uint8_t i = 0; i < 7; i++) {
      u8g2.drawHLine(41, 26 + i * 3, 5);
      u8g2.drawHLine(82, 26 + i * 3, 5);
    }

    u8g2.setFont(u8g2_font_5x7_tf);

    u8g2.drawStr(6,  32, "14-pin");
    u8g2.drawStr(6,  43, "DIP");
    u8g2.drawStr(96, 32, "74xx");
    u8g2.drawStr(96, 43, "gate");

    drawFooter("PRESS BTN TO TEST");

  } while (u8g2.nextPage());
}

// ============================================================
// OLED BUSY
// ============================================================
void drawBusy() {

  u8g2.firstPage();

  do {

    drawHeader("SCANNING");

    u8g2.setFont(u8g2_font_5x7_tf);
    drawStrCentered(30, "Probing all gates", 5);

    u8g2.drawRFrame(14, 36, 100, 10, 2);

    for (uint8_t i = 0; i < 6; i++)
      u8g2.drawBox(18 + i * 16, 39, 12, 4);

    drawStrCentered(58, "6 layouts / all gates", 5);

  } while (u8g2.nextPage());
}

// ============================================================
// OLED NO MATCH
// ============================================================
void drawNoMatch() {

  u8g2.firstPage();

  do {

    drawHeader("NO MATCH");

    u8g2.drawLine(54, 22, 74, 42);
    u8g2.drawLine(55, 22, 75, 42);
    u8g2.drawLine(74, 22, 54, 42);
    u8g2.drawLine(75, 22, 55, 42);

    u8g2.setFont(u8g2_font_5x7_tf);
    drawStrCentered(51, "Check wiring & orient.", 5);

    drawFooter("PRESS BTN TO RETRY");

  } while (u8g2.nextPage());
}

// ============================================================
// OLED DIP CHIP
// ============================================================
void drawDIPchip(
  uint8_t chipX,
  uint8_t chipY,
  uint8_t chipW,
  uint8_t chipH
) {

  u8g2.drawRFrame(chipX, chipY, chipW, chipH, 2);

  uint8_t notchX = chipX + chipW / 2;

  u8g2.drawCircle(
    notchX, chipY, 3,
    U8G2_DRAW_LOWER_LEFT | U8G2_DRAW_LOWER_RIGHT
  );

  uint8_t pinSpacing = (chipH - 4) / 6;
  uint8_t stubLen    = 4;

  char buf[3];

  u8g2.setFont(u8g2_font_4x6_tf);

  for (uint8_t i = 0; i < 7; i++) {

    uint8_t py = chipY + 2 + i * pinSpacing;

    // ---- Left pin (1..7) ----
    uint8_t lp = i + 1;

    u8g2.drawHLine(chipX - stubLen, py, stubLen);

    char lr = pinRole(lp);

    if (lr == 'I')
      u8g2.drawStr(chipX - stubLen - 5, py + 3, "I");
    else if (lr == 'O')
      u8g2.drawStr(chipX - stubLen - 5, py + 3, "O");
    else if (lr == 'G')
      u8g2.drawStr(chipX - stubLen - 5, py + 3, "G");
    else if (lr == '-')
      u8g2.drawStr(chipX - stubLen - 5, py + 3, "-");

    itoa(lp, buf, 10);
    u8g2.drawStr(chipX - stubLen - 14, py + 3, buf);

    // ---- Right pin (14..8) ----
    uint8_t rp = 14 - i;

    u8g2.drawHLine(chipX + chipW, py, stubLen);

    char rr = pinRole(rp);
    uint8_t rx = chipX + chipW + stubLen + 2;

    if (rr == 'I')
      u8g2.drawStr(rx, py + 3, "I");
    else if (rr == 'O')
      u8g2.drawStr(rx, py + 3, "O");
    else if (rr == 'V')
      u8g2.drawStr(rx, py + 3, "V");
    else if (rr == '-')
      u8g2.drawStr(rx, py + 3, "-");

    itoa(rp, buf, 10);
    u8g2.drawStr(rx + 6, py + 3, buf);
  }
}

// ============================================================
// OLED RESULT
//
//   rows  0-15 : yellow header, function name
//   rows 17-23 : IC number + verification badge
//   row     26 : rule
//   rows 29-52 : one line per gate, with its pin numbers
//   rows 53-63 : inverted summary bar
//
// Gate lines read:  G1 1,2>3        OK
// meaning gate 1 uses IC pins 1 and 2 as inputs, pin 3 as
// output, and passed. Codes: OK, SL stuck low, SH stuck high,
// FN wrong function, NR no response.
// ============================================================
void drawResult() {

  char fname[8];
  getFName(resFunc, fname, sizeof(fname));

  // ---- Header title ----
  char title[18];

  if (resNIn <= 2) {

    strcpy(title, fname);

  } else {

    title[0] = '0' + resNIn;
    title[1] = '-';
    title[2] = 'I';
    title[3] = 'n';
    title[4] = ' ';
    title[5] = '\0';

    strcat(title, fname);
  }

  // ---- IC number ----
  char icStr[12];

  strcpy(icStr, "IC:");

  if (resIC > 0)
    itoa(resIC, icStr + 3, 10);
  else
    strcpy(icStr + 3, "----");

  // ---- Verification badge ----
  char badge[10];

  if (resNGates == 1) {

    strcpy(badge, "1 GATE");

  } else if (resVerify) {

    badge[0] = '0' + resVotes;
    badge[1] = '/';
    badge[2] = '0' + resNGates;
    badge[3] = '\0';

    strcat(badge, " OK");

  } else {

    strcpy(badge, "UNVERIF");
  }

  // ---- Fault tally ----
  uint8_t nBad = 0;

  for (uint8_t g = 0; g < resNGates; g++) {

    if (gateFault[g] != GF_OK)
      nBad++;
  }

  // ---- Pick the largest font the gate lines will fit in ----
  //
  // 6x10 gives 21 chars and needs 9 px per row.
  // 5x7  gives 25 chars and needs 8 px per row.
  // 4x6  gives 32 chars and needs 7 px per row.
  //
  // Longest line is "G1 " + pins + gap + 2-char code.

  uint8_t maxLen = 0;

  for (uint8_t g = 0; g < resNGates && g < MAX_GATES; g++) {

    char pins[28];
    buildPinStr(g, pins);

    uint8_t len = 3 + strlen(pins) + 1 + 2;

    if (len > maxLen)
      maxLen = len;
  }

  uint8_t rows   = resNGates;
  uint8_t charW  = 4;
  uint8_t rowH   = 7;
  bool    bigFnt = false;   // 6x10
  bool    midFnt = false;   // 5x7

  // Two columns only when the lines are short and there are
  // enough gates to justify it.
  bool twoCol = (maxLen <= 13 && resNGates >= 4);

  if (twoCol)
    rows = (resNGates + 1) / 2;

  uint8_t avail = 56 - 29;   // 27 px of body height

  uint8_t effLen = twoCol ? maxLen : maxLen;

  if (
    effLen * 6 <= (twoCol ? 62 : 126) &&
    rows * 10 <= avail + 3
  ) {

    bigFnt = true;
    charW  = 6;
    rowH   = 10;

  } else if (
    effLen * 5 <= (twoCol ? 62 : 126) &&
    rows * 8 <= avail + 3
  ) {

    midFnt = true;
    charW  = 5;
    rowH   = 8;
  }

  // Vertically centre the block in the body area
  uint8_t blockH = rows * rowH;
  uint8_t top    = 29 + ((avail + 3) - blockH) / 2;

  u8g2.firstPage();

  do {

    drawHeader(title);

    u8g2.setFont(u8g2_font_5x7_tf);
    u8g2.drawStr(2, 23, icStr);
    drawStrRight(126, 23, badge, 5);

    u8g2.drawHLine(0, 26, 128);

    if (bigFnt)
      u8g2.setFont(u8g2_font_6x10_tf);
    else if (midFnt)
      u8g2.setFont(u8g2_font_5x7_tf);
    else
      u8g2.setFont(u8g2_font_4x6_tf);

    for (uint8_t g = 0; g < resNGates && g < MAX_GATES; g++) {

      char pins[28];
      buildPinStr(g, pins);

      char line[34];

      line[0] = 'G';
      line[1] = '1' + g;
      line[2] = ' ';
      line[3] = '\0';

      strcat(line, pins);

      const char *code = faultCode(gateFault[g]);
      bool bad = (gateFault[g] != GF_OK);

      uint8_t x, y, colW;

      if (twoCol) {

        x    = (g % 2) ? 66 : 2;
        y    = top + (g / 2) * rowH + (rowH - 2);
        colW = 60;

      } else {

        x    = 2;
        y    = top + g * rowH + (rowH - 2);
        colW = 124;
      }

      // Faulty rows render inverted for visibility
      if (bad) {
        u8g2.drawBox(x - 1, y - (rowH - 2), colW, rowH);
        u8g2.setDrawColor(0);
      }

      u8g2.drawStr(x, y, line);
      drawStrRight(x + colW - 2, y, code, charW);

      if (bad)
        u8g2.setDrawColor(1);
    }

    // ---- Compact status line ----
    u8g2.setFont(u8g2_font_4x6_tf);

    if (nBad == 0) {

      drawStrCentered(63, "ALL OK", 4);

    } else {

      char status[10];

      status[0] = '0' + nBad;
      status[1] = '/';
      status[2] = '0' + resNGates;
      status[3] = '\0';

      strcat(status, " BAD");

      // Small inverted tag so faults still catch the eye
      uint8_t w = strlen(status) * 4;
      uint8_t x = (128 - w) / 2;

      u8g2.drawBox(x - 3, 57, w + 6, 7);
      u8g2.setDrawColor(0);
      u8g2.drawStr(x, 63, status);
      u8g2.setDrawColor(1);
    }

  } while (u8g2.nextPage());
}
// ============================================================
// MAIN PROBE — CONSENSUS ALGORITHM
// ============================================================
void runProbe() {

  resFound  = false;
  resFunc   = FN_NONE;
  resIC     = 0;
  resVerify = false;
  resVotes  = 0;

  clearGateResults();

  Serial.println();
  Serial.println(F("=========================================="));
  Serial.println(F(" IC PROBER v6 - CONSENSUS FULL CHIP TEST"));
  Serial.println(F("=========================================="));

  // Fallback: first layout where exactly one gate read valid
  bool     haveFallback = false;
  uint8_t  fbLay        = 0;
  GateFunc fbFn         = FN_NONE;

  for (uint8_t li = 0; li < NUM_LAYOUTS; li++) {

    const uint8_t *lay =
      (const uint8_t *)pgm_read_ptr(&LAYOUTS[li]);

    uint8_t nGates = pgm_read_byte(lay);
    uint8_t nIn    = pgm_read_byte(lay + 1);

    char lname[16];
    strncpy_P(
      lname,
      (const char *)pgm_read_ptr(&LNAMES[li]),
      15
    );
    lname[15] = '\0';

    Serial.println();
    Serial.print(F("[Layout "));
    Serial.print(li + 1);
    Serial.print('/');
    Serial.print(NUM_LAYOUTS);
    Serial.print(F("] "));
    Serial.println(lname);

    // ---- Probe every gate once ----
    probeAllGates(lay, nGates, nIn);

    for (uint8_t g = 0; g < nGates && g < MAX_GATES; g++) {

      char dn[8];
      getFName(gateDetectedFunc[g], dn, sizeof(dn));

      Serial.print(F("  Gate "));
      Serial.print(g + 1);
      Serial.print(F(": "));
      serialPrintPinList(lay, g, nIn);
      Serial.print(F("  -> "));
      Serial.println(dn);
    }

    // ---- Vote ----
    uint8_t  votes;
    GateFunc fn = voteBest(nGates, &votes);

    if (fn == FN_NONE) {
      Serial.println(F("  -> No gate produced a valid table."));
      continue;
    }

    char fname[8];
    getFName(fn, fname, sizeof(fname));

    // ---- Single-gate IC: nothing to cross-check ----
    if (nGates == 1) {

      Serial.print(F("  -> "));
      Serial.print(fname);
      Serial.println(F(" (single gate, no cross-check)"));

      finishIdentification(li, nGates, nIn, fn, votes);

      printAccepted();
      printResult();
      drawResult();
      return;
    }

    // ---- Consensus reached ----
    if (votes >= 2) {

      Serial.print(F("  -> Consensus: "));
      Serial.print(fname);
      Serial.print(F("  ("));
      Serial.print(votes);
      Serial.print('/');
      Serial.print(nGates);
      Serial.println(F(" gates agree)"));

      finishIdentification(li, nGates, nIn, fn, votes);

      printAccepted();
      printResult();
      drawResult();
      return;
    }

    // ---- Only one gate matched: hold and keep searching ----
    Serial.print(F("  -> Only 1/"));
    Serial.print(nGates);
    Serial.println(F(" gates matched. Holding as fallback."));

    if (!haveFallback) {

      haveFallback = true;
      fbLay        = li;

      fbFn = (gateDetectedFunc[0] != FN_NONE)
               ? gateDetectedFunc[0]
               : fn;
    }
  }

  // ==========================================================
  // NO CONSENSUS ANYWHERE - FALL BACK TO GATE 1
  // ==========================================================
  if (haveFallback) {

    const uint8_t *lay =
      (const uint8_t *)pgm_read_ptr(&LAYOUTS[fbLay]);

    uint8_t nGates = pgm_read_byte(lay);
    uint8_t nIn    = pgm_read_byte(lay + 1);

    // gObs was overwritten by later layouts - re-probe
    probeAllGates(lay, nGates, nIn);

    finishIdentification(fbLay, nGates, nIn, fbFn, 1);

    char fname[8];
    getFName(fbFn, fname, sizeof(fname));

    Serial.println();
    Serial.println(F("  ----------------------------------------"));
    Serial.println(F("        UNVERIFIED IDENTIFICATION"));
    Serial.println(F("  ----------------------------------------"));
    Serial.println(F("  No layout reached consensus."));
    Serial.print(  F("  Assuming Gate 1 is good: "));
    Serial.println(fname);
    Serial.println(F("  Remaining gates flagged as faulty."));

    printResult();
    drawResult();
    return;
  }

  // ==========================================================
  // NOTHING AT ALL
  // ==========================================================
  Serial.println();
  Serial.println(F("  No IC identified on any layout."));
  Serial.println(F("  Check wiring, IC orientation, and power."));

  resFound = false;
  drawNoMatch();
}

// ============================================================
// SETUP
// ============================================================
void setup() {

  pinMode(BTN_PIN, INPUT_PULLUP);

  Serial.begin(115200);

  u8g2.begin();

  releaseAll();
  clearGateResults();
  drawIdle();

  Serial.println(F("IC Prober v6 - Consensus Gate Testing ready."));
  Serial.println(F("Press button to probe."));
}

// ============================================================
// LOOP
// ============================================================
void loop() {

  if (digitalRead(BTN_PIN) == LOW || Serial.available()) {

    if (Serial.available()) {

      while (Serial.available())
        Serial.read();

    } else {

      delay(30);
      while (digitalRead(BTN_PIN) == LOW);
    }

    drawBusy();
    runProbe();
  }

  delay(40);
}