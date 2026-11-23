// TamaPoke - tamagotchi pixel art inspirado en la gen 1
// para Waveshare ESP32-S3-Touch-AMOLED-1.75
//
// Librerias (Library Manager o repo de Waveshare):
//   - "GFX Library for Arduino" (moononournation), con soporte CO5300 QSPI
//   - "SensorLib" (Lewis He), driver tactil CST9217
//
// Placa: ESP32S3 Dev Module | Flash 16MB | PSRAM: OPI PSRAM | USB CDC On Boot: Enabled
//
// Los sprites y la tabla de especies se generan con tools/sprites.py (emit).

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include "Arduino_GFX_Library.h"
#include "TouchDrvCSTXXX.hpp"
#include "pin_config.h"
#include "species.h"
#include "dex.h"
#include "pet.h"
#include "sdmon.h"
#include "rtcbat.h"
#include "i18n.h"
#include "audio.h"
#include "battle.h"
#include "time_utils.h"
#include "dayphase.h"
#include "imu.h"
#include "netota.h"
#include "gesture.h"
#include "gps.h"

// arcos de las barras de necesidades (ver drawBars): pixeles precalculados
struct ArcPix { uint16_t x, y; uint8_t t; };
struct ArcShape { float startDeg, endDeg; ArcPix *pix; uint16_t n; };

// Version del firmware. Subir este numero en cada release (y manifest.json para
// el instalador web). Se muestra en la pantalla de ajustes y por serie al arrancar.
#if defined(TAMAPOKE_LOCAL_TEST) && defined(TAMAPOKE_GEN3_RELEASE)
#error "TAMAPOKE_LOCAL_TEST and TAMAPOKE_GEN3_RELEASE are mutually exclusive"
#endif
#if DEX_COUNT > 251 && !defined(TAMAPOKE_LOCAL_TEST) && !defined(TAMAPOKE_GEN3_RELEASE)
#error "Gen-3 source requires an explicit local-test or release build profile"
#endif
#ifdef TAMAPOKE_LOCAL_TEST
#define FW_VERSION "1.36.2-hoenn-walk-local"
#elif defined(TAMAPOKE_GEN3_RELEASE)
#if __has_include("version_ci.h")
#include "version_ci.h"  // written by the release workflow: 1.37.<build number>
#else
#define FW_VERSION "1.36.2"
#endif
#else
#define FW_VERSION "1.35.3-soft-step"
#endif
#define HELP_PAGE_COUNT 8
#define HELP_NAV_Y 358
#define HELP_NAV_H 54
#define HELP_NAV_W 120
#define HELP_PREV_X 96
#define HELP_NEXT_X 250
#define HELP_LINE_COUNT 6

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *panel = new Arduino_CO5300(
  bus, LCD_RESET, 0 /*rotation*/, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
// Framebuffer completo en PSRAM: dibujamos todo y hacemos flush() (sin parpadeo)
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel);

TouchDrvCST92xx touch;
Pet pet;

// sprite animado de la SD para la especie actual (si existe el archivo)
SdMon mon;          // sprite B/N (respaldo y minijuego si no hay PMD)
PmdMon pmd;         // sprite PMD multi-accion (pantalla principal)
PmdMon evoPmd;      // forma anterior, solo durante el parpadeo de evolucion
PmdMon wildPmd;     // rival salvaje en la pantalla de combate
int16_t monFor = -2;
bool monShinyFor = false;

// comportamiento del bicho en pantalla
struct {
  uint8_t mode = 0;     // 0 idle, 1 paseo, 2 gesto one-shot
  uint8_t act = PMD_IDLE;
  uint32_t t0 = 0;      // inicio de la animacion en curso
  uint32_t until = 0;   // fin del estado actual
  float x = 233, targetX = 233;
} beh;
#define PET_GROUND 304  // linea de suelo de la mascota
PmdMon galleryPmd;  // sprite grande de la vista detalle de la galeria (PMD/TPK2, legal)

// galeria pokedex
bool galleryOpen = false;
bool galleryDirty = false;
int galleryPage = 0;        // 10 paginas de 16
int16_t galleryDetail = 0;  // dex en vista detalle, 0 = rejilla
uint8_t galleryFilter = 0;  // 0 todos, 1 criados, 2 capturados
uint8_t galleryRegion = 0;  // 0 all, 1 Kanto, 2 Johto, 3 Hoenn
// carrusel del detalle: girar el dedo alrededor del centro pasa de Pokemon
bool galleryRotating = false;   // el carrusel esta visible (dedo girando)
bool galRotHasAngle = false;
float galRotLastAngle = 0;
float galRotAccum = 0;          // radianes acumulados desde el ultimo paso
int16_t galRotDex = 0;          // Pokemon en el centro del carrusel
#define GAL_ROT_STEP 0.6f       // ~34 grados por Pokemon
#define GAL_ROT_START 0.35f     // ~20 grados para empezar (no confundir con un toque)

bool screenOff = false;       // pulsacion corta del boton PWR
bool cardOpen = false;        // ficha del bicho (deslizar vertical)
bool kbOpen = false;          // teclado para renombrar al bicho
char nameBuf[12] = "";
uint8_t nameLen = 0;
#define CARD_COUNT 9
#define BOX_ROWS 4
#define BOX_ROW0_Y 140
#define BOX_ROW_STEP 48
#define BOX_ROW_H 44
#define BOX_NAV_Y 338
#define BOX_NAV_H 46
#define BOX_SORT_X 163
#define BOX_SORT_Y 58
#define BOX_SORT_W 140
#define BOX_SORT_H 44
#define PROFILE_FRAME_Y 336
#define PROFILE_ARROW_W 64
#define PROFILE_ARROW_H 46
#define PROFILE_PREV_X 80
#define PROFILE_NEXT_X 322
uint8_t cardPage = 0;         // 0 perfil ... 7 expedicion, 8 pasos/trail
uint8_t boxPage = 0;
uint8_t boxSort = 0;          // 0 dex, 1 tipo, 2 criados primero
bool expeditionTrainChoiceOpen = false;
ExpeditionSite expeditionSiteChoice = EXP_SITE_COAST;
bool clockOpen = false;       // ajustes (deslizar abajo): menu y sus paginas
enum SetPage : uint8_t { SETP_MENU, SETP_TIME, SETP_SOUND, SETP_INFO, SETP_GPS };
uint8_t setPage = SETP_MENU;
int clockH = 12, clockM = 0;  // hora en edicion
bool powerSave = false;       // ahorro opcional: off por defecto
bool helpOpen = false;
// Wi-Fi y actualizaciones: capa sobre los ajustes (clockOpen sigue en true)
enum NetPage : uint8_t { NETP_WIFI, NETP_SCAN, NETP_PASS, NETP_UPDATE };
bool netOpen = false;
uint8_t netPage = NETP_WIFI;
bool netDirty = false;
uint32_t netSeenGen = 0;
int8_t netForgetIdx = -1;
uint8_t netListPage = 0, netScanPage = 0;
char pwSsid[NET_SSID_LEN + 1] = "";
char pwBuf[NET_PASS_LEN + 1] = "";
uint8_t pwLen = 0;
uint8_t pwMode = 0;  // 0 minusculas, 1 mayusculas, 2 simbolos
void openNet(uint8_t page);
void closeNet();
void renderNet();
void netTap(int16_t x, int16_t y);
void netService(uint32_t now);
uint8_t helpPage = 0;
bool uiDirty = true;
bool cardDirty = true;
bool clockDirty = true;
bool helpDirty = true;
bool keyboardDirty = true;
bool gameMenuDirty = true;
bool battleDirty = true;
bool starterDirty = true;


// escena de bano: espuma sobre el bicho y limpieza al reventar
uint32_t bathUntil = 0;
bool bathPending = false;
struct { int16_t x, y; uint8_t r, ph; } bubbles[14];
uint32_t feedMenuUntil = 0;   // selector de comida abierto hasta este millis
uint32_t nextAmbientSoundAt = 0;

// minijuego "toques": mantener la pokeball en el aire
bool gameOpen = false;
bool gameMenuOpen = false;
uint8_t gameMode = 0;  // 0 ball, 1 catch, 2 memo
uint32_t gameOverUntil = 0;
float ballX, ballY, ballVX, ballVY, gamePetX;
uint8_t gameScore, gameMisses;
float hitX, hitY;             // ultimo golpe (anillo de impacto)
uint32_t hitTime = 0;
uint32_t ballLastHitAt = 0;
bool gameNewHi = false;
uint8_t gameGain = 0;
uint32_t catchUntil = 0, catchTargetUntil = 0;
int16_t catchX = 0, catchY = 0;
uint8_t catchIcon = 0;
// INCLINA: una Pokeball rueda segun la inclinacion de la placa (acelerometro)
#define TILT_GAME_MS 30000
#define TILT_FIELD_R 182      // radio del campo (centro de la pantalla)
#define TILT_BALL_R 16
#define TILT_HOLES 3
#define TILT_SIGN_X 1.0f       // si en la placa la bola va al reves, cambiar el signo
#define TILT_SIGN_Y 1.0f
float tiltBX = 233, tiltBY = 233, tiltVX = 0, tiltVY = 0;
float tiltNX = 0, tiltNY = 0;  // "plano" = como se sujetaba la placa al empezar
int16_t tiltBerryX = 233, tiltBerryY = 160;
int16_t tiltHoleX[TILT_HOLES], tiltHoleY[TILT_HOLES];
uint8_t tiltHoleCount = 0;
uint32_t tiltUntil = 0, tiltSafeUntil = 0;
#define CATCH_TRAP 3
uint8_t memoSeq[14] = { 0 };
uint8_t memoLen = 0, memoShow = 0, memoInput = 0, memoRounds = 0;
uint32_t memoNextAt = 0;
uint32_t memoReadyUntil = 0;  // los 4 botones encendidos: va a empezar la secuencia
bool memoShowing = false;
int8_t memoActivePad = -1, memoFlashPad = -1, memoHintPad = -1;
bool memoFlashGood = false;
uint32_t memoFlashUntil = 0, memoFailUntil = 0, memoTurnUntil = 0;
int16_t cleanX[4] = { 0 }, cleanY[4] = { 0 };
bool cleanAlive[4] = { false };
int16_t cleanDirt[4] = { 0 }, cleanDirt0[4] = { 0 };  // cuanto falta frotar (px de recorrido)
uint32_t cleanBorn[4] = { 0 };
bool cleanRubHas = false;
int16_t cleanRubX = 0, cleanRubY = 0;
#define CLEAN_GAME_MS 25000
uint32_t cleanUntil = 0, cleanSpawnAt = 0;
uint8_t cleanActive = 0;
uint8_t typeEnemy = TYPE_GRASS;
uint8_t typeEnemy2 = TYPE_NONE;
int16_t typeEnemyDex = 1;
uint32_t typeDurationMs = 4200;
uint8_t typeChoice[3] = { TYPE_FIRE, TYPE_WATER, TYPE_GRASS };
uint8_t typeCorrect = 0;
uint32_t typeUntil = 0;

// saco de entrenamiento (entrena la fuerza)
bool sackOpen = false;
uint32_t sackUntil = 0, sackOverUntil = 0;
uint16_t sackHits = 0;
float sackShake = 0;
uint8_t sackGain = 0;
bool sackNewHi = false;

bool battleOpen = false;
bool battleResolved = false;
int16_t battleDex = 0;
uint8_t battleLevel = 1;
BattleStats battlePlayer = {};
BattleStats battleEnemy = {};
BattleResult battleResult = {};
BattleRuntime battleRun = {};
BattleTurnResult battleTurn = {};
BattleAction battleLastAction = BATTLE_ATTACK;
BattleReward battleReward = {};
char battleMsg[28] = "";
uint32_t battleAttackMenuUntil = 0;
bool battleCatchOffered = false;
bool battleCatchTried = false;
bool battleCatchDone = false;
bool battleCatchSuccess = false;
bool battleRespectCatch = false;
uint8_t battleCatchChance = 0;
bool battleLowHpWarned = false;

#define WILD_COOLDOWN_MS (20UL * 60UL * 1000UL)
#define WILD_PROMPT_MS 20000UL
#define WILD_CHECK_MS 60000UL
uint32_t wildPromptUntil = 0;
uint32_t nextWildEligible = 0;
uint32_t lastWildCheck = 0;
int16_t wildPromptDex = 0;
uint8_t wildPromptLevel = 1;
bool wildPromptShiny = false;
bool battleShiny = false;

#define PET_EVENT_COOLDOWN_MS (15UL * 60UL * 1000UL)
#define PET_EVENT_PROMPT_MS 18000UL
#define PET_EVENT_CHECK_MS 60000UL
uint32_t petEventUntil = 0;
uint32_t nextPetEventEligible = 0;
uint32_t lastPetEventCheck = 0;
uint8_t petEventType = PET_EVENT_BERRY;
uint32_t petEventFeedbackUntil = 0;
char petEventMsg[18] = "";
uint32_t morningUntil = 0;
uint32_t eggWiggleUntil = 0;

int sceneHour();
uint8_t currentDayPhase();
bool mainScreenReadyForShake();
void maybeOfferMorning(uint32_t now);
uint32_t pmdActTotalMs(const PmdAct &a);
void flushStampedFrame();
void startBattleWith(int16_t forcedDex, uint8_t forcedLevel, int8_t forcedShiny = -1);
void startBattle();

// las 9 especies con sprite propio en flash (respaldo sin SD): dex -> indice
int flashIdxForDex(int16_t dex) {
  static const int8_t IDX[10] = { -1, 3, 4, 5, 0, 1, 2, 6, 7, 8 };
  return (dex >= 1 && dex <= 9) ? IDX[dex] : -1;
}

#define CX 233  // centro de la pantalla redonda
#define CY 233
#define PET_CY 202  // centro vertical del sprite

static const uint16_t INK_K = 0x18C4;  // spriteColor('k')

void loadPowerSave() {
  Preferences p;
  p.begin("tamapoke", true);
  powerSave = p.getBool("psave", false);
  p.end();
}

void setPowerSave(bool on) {
  powerSave = on;
  Preferences p;
  p.begin("tamapoke", false);
  p.putBool("psave", powerSave);
  p.end();
}

// botones de icono siguiendo el arco inferior de la pantalla redonda
// (los exteriores van mas altos para no salirse del circulo)
struct Btn {
  int16_t cx, cy;
  const char *const *icon;
};
Btn buttons[4] = {
  { 130, 352, SPR_ICON_FOOD },   // comer
  { 197, 391, SPR_ICON_PLAY },   // jugar
  { 269, 391, SPR_ICON_LIGHT },  // luz
  { 336, 352, SPR_ICON_CLEAN },  // bano
};
#define BTN_HALF 34  // boton de 68x68 siguiendo la curva de abajo
#define FEED_MENU_Y 214  // selector de comida, sobre el bicho
#define BTN_HIT 36   // radio tactil (un poco mas generoso)

// grietas del huevo (pixeles 'k' sobre el sprite)
static const uint8_t CRACK1[][2] = { {15,8},{16,9},{15,10} };
static const uint8_t CRACK2[][2] = { {11,13},{12,14},{11,15},{20,12},{19,13},{20,14} };
// estrellas del modo noche
static const uint16_t STARS[][2] = { {120,140},{330,120},{370,210},{95,230},{280,90},{160,95} };

bool wasPressed = false;
// eleccion de inicial (primera partida): primero region, despues 3 starters.
static const int16_t STARTER_DEX[3][3] = {
  { 1, 4, 7 },       // Kanto
  { 152, 155, 158 }, // Johto
  { 252, 255, 258 }, // Hoenn
};
static const StrId STARTER_REGION_LABELS[3] = {
  S_RANK_KANTO, S_RANK_JOHTO, S_RANK_HOENN
};
uint8_t starterRegion = 0xFF;  // se elige en cada arranque si el starter sigue pendiente
#define STARTER_ROW_Y 110
#define STARTER_ROW_H 70
#define STARTER_ROW_GAP 8
// boton-CTA de evolucion (centrado, mitad de pantalla)
#define EVO_BTN_W 256
#define EVO_BTN_H 64
#define EVO_BTN_X (CX - EVO_BTN_W / 2)
#define EVO_BTN_Y 172
// boton-CTA de despedida (mas ancho: lleva el nombre + frase)
#define FAR_BTN_W 408
#define FAR_BTN_H 58
#define FAR_BTN_X (CX - FAR_BTN_W / 2)
#define FAR_BTN_Y 176
// el CST9217 avisa por el pin INT cuando hay datos tactiles; lo usamos para no
// leer el bus I2C mientras el chip esta dormido (esa lectura se colgaba ~1s)
volatile bool gTouchIrq = false;
void IRAM_ATTR touchIsr() { gTouchIrq = true; }
uint32_t lastRender = 0;
uint32_t lastLoopStart = 0;
uint32_t perfRenderLastMs = 0, perfRenderMaxMs = 0;
uint32_t perfLoopLastMs = 0, perfLoopMaxMs = 0;
// parte mas lenta del bucle en la ventana actual (diagnostico en Ajustes > Info)
const char *perfSlowName = "-";
uint32_t perfSlowMs = 0;
#define PERF_SECTION(name, code) do { uint32_t _pt = millis(); code; uint32_t _pd = millis() - _pt; \
  if (_pd > perfSlowMs) { perfSlowMs = _pd; perfSlowName = name; } } while (0)
uint32_t perfRenderCount = 0, perfRenderSkipCount = 0;
// proteccion del AMOLED: atenuado por inactividad
uint32_t lastInteract = 0;
uint8_t dimStage = 0;        // 0 despierto, 1 atenuado (90s), 2 casi apagado (5min)
bool swallowGesture = false; // el toque que despierta no acciona nada
uint32_t ignoreTouchUntil = 0;
uint32_t holdStart = 0;     // pulsacion larga sobre el bicho
uint32_t confirmUntil = 0;  // dialogo "soltar?" activo hasta este millis
uint8_t choiceKind = 0;     // 0 ninguno, 1 evolucion, 2 despedida, 3 objetivo de evolucion
uint32_t choiceUntil = 0;   // se cierra solo a este millis
int16_t tX0, tY0, tXl, tYl; // gesto en curso (inicio y ultima posicion)
uint32_t tStart = 0;
bool holdFired = false;

void markUiDirty() {
  uiDirty = true;
  starterDirty = true;
  cardDirty = true;
  clockDirty = true;
  helpDirty = true;
  keyboardDirty = true;
  gameMenuDirty = true;
  battleDirty = true;
  galleryDirty = true;
  netDirty = true;
}

void lockTouchBrief(uint16_t ms = 160) {
  uint32_t until = millis() + ms;
  if (until > ignoreTouchUntil) ignoreTouchUntil = until;
  wasPressed = false;
  swallowGesture = true;
}

const char *screenName() {
  if (pet.awaitingStarter()) return "starter";
  if (galleryOpen) return galleryDetail ? "gallery-detail" : "gallery-grid";
  if (gameOpen) return "game";
  if (sackOpen) return "sack";
  if (battleOpen) return battleResolved ? "battle-result" : "battle";
  if (kbOpen) return "keyboard";
  if (helpOpen) return "help";
  if (netOpen) return "net";
  if (clockOpen) return "settings";
  if (cardOpen) return "card";
  if (gameMenuOpen) return "game-menu";
  return screenOff ? "screen-off" : "main";
}

bool staticScreenClean() {
  if (pet.awaitingStarter()) return !starterDirty;
  if (galleryOpen && !galleryDetail) return !galleryDirty;
  if (battleOpen && battleResolved) return !battleDirty;
  if (kbOpen) return !keyboardDirty;
  if (helpOpen) return !helpDirty;
  if (netOpen) return !netDirty;
  if (clockOpen) return !clockDirty;
  if (cardOpen) return !cardDirty;
  if (gameMenuOpen) return !gameMenuDirty;
  return false;
}

void setup() {
  Serial.setRxBufferSize(8192);  // la transferencia a SD llega en bloques de 2 KB
  Serial.begin(115200);
  // CRITICO: sin esto, Serial.print BLOQUEA el juego cuando no hay un
  // monitor serie abierto en el host (el bufer TX del USB CDC se llena
  // y nadie lo vacia) -> con timeout 0 los mensajes se descartan
  Serial.setTxTimeoutMs(0);
  Serial.printf("TamaPoke fw v%s\n", FW_VERSION);
  otaBootGuard(FW_VERSION);  // crash loop after an update: back to the previous firmware
  loadLang();  // idioma guardado (ES por defecto)
  Wire.setBufferSize(2048);  // frases NMEA del GPS (-G): hasta ~2 KB por lectura
  Wire.begin(IIC_SDA, IIC_SCL);
  // CST9217 (tactil), AXP2101 (PMU) y PCF85063 (RTC) comparten este bus I2C.
  // Red de seguridad para PMU/RTC (SensorLib NO respeta este timeout en el
  // tactil; el cuelgue del tactil dormido se resuelve gateando por INT, ver
  // handleTouch).
  Wire.setTimeOut(50);

  // CRITICO: encender la alimentacion del panel (BLDO1=OLED VDD 3.3V) ANTES de
  // inicializar el display. Si el PMU se reseteo (drenaje total), este rail
  // queda OFF y la pantalla se ve negra aunque el resto de la placa funcione.
  pmuEnablePanel();

  // QSPI a 80MHz (por defecto 40): el flush del framebuffer es el cuello de
  // botella del fps (~56ms a 40MHz). Si el panel mostrara basura, bajar a 40M.
  if (!gfx->begin(80000000)) Serial.println("gfx->begin() fallo");
  panel->setBrightness(180);

  touch.setPins(TP_RESET, TP_INT);
  bool touchOk = false;
  for (int i = 0; i < 3 && !touchOk; i++) {  // a veces falla al primer intento
    touchOk = touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL);
    if (!touchOk) delay(150);
  }
  if (!touchOk) Serial.println("CST9217 no detectado");
  // begin() deja el chip en modo comando (lee la identidad y no sale);
  // hace falta un reset por hardware para que vuelva a reportar toques
  touch.reset();
  touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
  touch.setMirrorXY(true, true);  // el panel esta montado girado 180 grados
  // INT activo-bajo: salta cuando hay datos. Gatea las lecturas I2C (ver loop)
  pinMode(TP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TP_INT), touchIsr, FALLING);

  pet.begin();
  Serial.printf("SAVE %s spec=%d lv=%u wins=%u losses=%u streak=%u\n",
                pet.saveLoadedFromNvs ? "loaded" : "created",
                pet.speciesId, pet.level(), pet.battleWins, pet.battleLosses, pet.battleStreak);
  sdBegin();
  thumbs.load();

  // reloj real: aplica el tiempo que estuvo apagado
  rtcBegin();
  batBegin();
  pwrSetup();
  gpsBegin();  // solo placa -G: comprueba el GNSS y lo deja apagado (reset)
  uint32_t e = rtcEpoch();
  if (e == 0) {
    rtcSetEpoch(1767225600UL);  // RTC virgen: semilla (la hora absoluta da igual,
    e = rtcEpoch();             // solo importan las diferencias)
    Serial.println("RTC sin hora: sembrado, sin progresion offline esta vez");
  }
  pet.syncClock(e);
  loadPowerSave();
  netBegin(FW_VERSION);
  imuBegin();

  audioBegin();  // ES8311 + I2S + amplificador (suena un jingle de arranque)

  lastInteract = millis();
}

// carga/descarga el sprite de SD cuando cambia la especie
void ensureMon() {
  if (otaSdBusy()) return;  // la actualizacion esta escribiendo en la SD
  if (pet.speciesId == monFor && monShinyFor == pet.shiny && !sdDirty) return;
  sdDirty = false;
  monFor = pet.speciesId;
  monShinyFor = pet.shiny;
  mon.unload();
  pmd.unload();
  beh.x = beh.targetX = 233;
  beh.mode = 0;
  beh.until = 0;
  if (pet.speciesId >= 1 && pet.speciesId <= DEX_COUNT) {
    pmd.load(pet.speciesId, pet.shiny);          // principal: PMD
    if (!pmd.loaded) mon.load(pet.speciesId, pet.shiny);  // respaldo: B/N
  }
}

bool mainScreenReadyForAmbientSound() {
  if (audioMode() != SOUND_FULL || screenOff || dimStage > 0) return false;
  if (pet.awaitingStarter() || pet.isEgg() || pet.sleeping || pet.ceremony) return false;
  if (battleOpen || gameOpen || gameMenuOpen || sackOpen || cardOpen || galleryOpen || kbOpen || clockOpen || helpOpen) return false;
  if (feedMenuUntil || confirmUntil || choiceKind || bathUntil || wildPromptUntil || petEventUntil) return false;
  if (pet.evolving() || pet.wantEvolveButton() || pet.canRunawayNow() || pet.wantFarewellButton()) return false;
  return true;
}

void maybePlayAmbientSound(uint32_t now) {
  if (!mainScreenReadyForAmbientSound()) {
    nextAmbientSoundAt = now + 9000;
    return;
  }
  if (nextAmbientSoundAt == 0) nextAmbientSoundAt = now + 8000 + random(8000);
  if (deadlineActive(now, nextAmbientSoundAt)) return;
  uint8_t r = (uint8_t)random(3);
  sfxPlay(r == 0 ? SFX_HEART : (r == 1 ? SFX_EVENT_SPARKLE : SFX_MENU));
  nextAmbientSoundAt = now + 8000 + random(8000);
}

uint16_t renderIntervalMs() {
  if (screenOff) return 5000;
  if (battleOpen) return battleResolved ? 320 : 190;
  if (gameOpen && gameMode == 5) return 50;  // INCLINA: movimiento fluido
  if (gameOpen || sackOpen) return 115;
  if (galleryRotating) return 40;  // carrusel: animacion fluida mientras gira el dedo
  if (galleryOpen || cardOpen || kbOpen || clockOpen || helpOpen || gameMenuOpen) return powerSave ? 650 : 320;
  if (!powerSave) return 100;
  if (dimStage >= 2) return 650;
  if (dimStage >= 1) return 350;
  return 180;
}

bool lightSleepAllowed(uint32_t now) {
  if (netBusy()) return false;  // Wi-Fi encendido
  if (!powerSave || usbPresent() || audioBusy() || Serial.available()) return false;
  if (!screenOff && dimStage == 0) return false;
  if (!screenOff && now - lastInteract < 1500UL) return false;
  if (wasPressed || gTouchIrq || deadlineActive(now, ignoreTouchUntil)) return false;
  if (gameOpen || sackOpen || battleOpen || bathUntil) return false;
  if (pet.awaitingStarter() || feedMenuUntil || confirmUntil || choiceKind || wildPromptUntil || petEventUntil) return false;
  if (pet.evolving() || pet.ceremony || pet.eating() || pet.showHeart()) return false;
  if (galleryOpen || cardOpen || kbOpen || clockOpen || helpOpen || gameMenuOpen) return false;
  return true;
}

uint16_t lightSleepMs(uint32_t now) {
  if (!lightSleepAllowed(now)) return 0;
  uint16_t maxMs = screenOff ? 750 : (dimStage >= 2 ? 300 : 180);
  // Die Software-Schritterkennung braucht auch bei dunklem Display regelmaessige
  // Rohwerte. Kurze Light-Sleep-Intervalle erhalten sie ohne Dauer-Rendering.
  if (imuOk() && maxMs > 70) maxMs = 70;
  uint16_t ri = renderIntervalMs();
  uint32_t sinceRender = now - lastRender;
  if (sinceRender < ri) {
    uint32_t untilRender = ri - sinceRender;
    if (untilRender < maxMs) maxMs = untilRender;
  }
  if (maxMs < 40) return 0;
  return maxMs;
}

void maybeLightSleep(uint32_t now) {
  uint16_t ms = lightSleepMs(now);
  if (!ms) return;

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)TP_INT, 0);
  esp_light_sleep_start();

  if (digitalRead(TP_INT) == LOW || esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    gTouchIrq = true;
  }
}

void loop() {
  uint32_t now = millis();
  uint32_t loopStart = now;
  bool petChanged;
  PERF_SECTION("pet", petChanged = pet.update(now));
  if (petChanged && cardOpen && cardPage != 7) cardDirty = true;

  // avisa con un sonido cuando el bicho pasa a estar listo para evolucionar
  // (incluye el caso de cumplir al despertar). canEvolveNow es false durmiendo.
  static bool wasEvoReady = false;
  bool evoReady = pet.wantEvolveButton();
  if (evoReady && !wasEvoReady) sfxPlay(SFX_MEDAL);
  wasEvoReady = evoReady;
  // aviso sombrio cuando el bicho esta a punto de escaparse por abandono
  static bool wasRunReady = false;
  bool runReady = pet.canRunawayNow();
  if (runReady && !wasRunReady) sfxPlay(SFX_DENY);
  wasRunReady = runReady;

  PERF_SECTION("touch", handleTouch());
  PERF_SECTION("serial", handleSerial());
  PERF_SECTION("wifi", netService(now));
  bool gpsPage = clockOpen && setPage == SETP_GPS;
  PERF_SECTION("gps", gpsPoll(now, gpsPage));  // el GNSS solo funciona con su pagina abierta
  static uint32_t gpsSeen = 0;
  if (gpsPage && gpsGen() != gpsSeen) { gpsSeen = gpsGen(); clockDirty = true; }
  PERF_SECTION("sprite SD", ensureMon());
  setPowerCacheInterval(powerSave ? 10000UL : 2000UL);
  pet.ensureDailyGoals();
  maybeOfferWildEncounter(now);
  maybeOfferPetEvent(now);
  maybeOfferMorning(now);
  maybePlayAmbientSound(now);

  uint16_t imuMs = screenOff ? 70 : (dimStage ? 60 : 40);
  PERF_SECTION("imu", imuPoll(now, imuMs));
  uint32_t stepsBefore = pet.stepsToday;
  uint32_t stepDayBefore = pet.stepDay;
  uint16_t walked = imuTakeSteps();
  // Die Erkennung filtert bereits Einzelstoesse und verlangt Geh-Rhythmus.
  // Deshalb auch direkt nach dem Abziehen von USB zaehlen: Der gecachte
  // VBUS-Status kann sonst bis zu zehn Sekunden echte Schritte verwerfen.
  if (walked) {
    pet.applyWalk(walked, rtcEpoch());
    if (cardOpen && cardPage == 7) cardDirty = true;
  }
  if ((pet.stepsToday != stepsBefore || pet.stepDay != stepDayBefore) &&
      cardOpen && cardPage == 8) cardDirty = true;
  if (imuShakeEdge() && !screenOff) {
    if (pet.isEgg()) {
      eggWiggleUntil = now + 450;
      sfxPlay(SFX_TAP);
    } else if (mainScreenReadyForShake()) {
      lastInteract = now;
      if (pmd.has(PMD_POSE)) {
        beh.mode = 2;
        beh.act = PMD_POSE;
        beh.t0 = now;
        beh.until = now + pmdActTotalMs(pmd.acts[PMD_POSE]);
      }
      if (pet.applyShake()) {
        if (audioMode() == SOUND_FULL) speciesChirpPlay(pet.speciesId, true);
        else sfxPlay(SFX_HEART);
        snprintf(petEventMsg, sizeof(petEventMsg), "%s", T(S_HAPPY_FB));
        petEventFeedbackUntil = now + 1600;
      } else {
        sfxPlay(SFX_TAP);
      }
    }
  }

  // Die Expeditionskarte bleibt sonst als statischer Screen stehen. Ein
  // sekundenweises Dirty-Render ist nur aktiv, waehrend ihr Countdown sichtbar ist.
  static uint32_t lastExpeditionCardTick = 0;
  if (cardOpen && cardPage == 7 && now - lastExpeditionCardTick >= 1000UL) {
    lastExpeditionCardTick = now;
    cardDirty = true;
  }

  // pulsacion corta del PWR: pantalla on/off
  static uint32_t lastPwr = 0;
  if (now - lastPwr > 250) {
    lastPwr = now;
    if (pwrShortPressed()) {
      if (screenOff) {  // bloqueada: PWR despierta
        screenOff = false;
        lastInteract = now;
        ignoreTouchUntil = now + 900;
        swallowGesture = true;
        wasPressed = false;
        markUiDirty();
      } else if (goBack()) {  // en menus y ventanas: PWR = atras
        lastInteract = now;
        dimStage = 0;
        sfxPlay(SFX_TAP);
      } else {  // pantalla principal: PWR bloquea
        screenOff = true;
      }
    }
  }

  updateBrightness(now);

  // vuelca el autoguardado periodico SOLO con la pantalla atenuada/apagada o
  // durmiendo: la escritura a NVS congela ~1s ambos cores (caché de flash off),
  // y aqui no hay animacion que se corte ni dedo esperando respuesta. Con 90s
  // de inactividad la pantalla ya atenua, asi que se vuelca enseguida; el uso
  // activo persiste igual por los guardados de cada accion (comer/jugar/...).
  if (pet.savePending() && (screenOff || dimStage >= 1 || pet.sleeping)) {
    PERF_SECTION("save", pet.flushSave());
  }

  // anota la hora real cada 30 s (se persiste en cada save del juego)
  static uint32_t lastClock = 0;
  uint32_t clockPollMs = powerSave ? 60000UL : 30000UL;
  if (now - lastClock > clockPollMs) {
    lastClock = now;
    uint32_t e = rtcEpoch();
    if (e) pet.lastSeenEpoch = e;
  }

  static bool expeditionWasActive = false;
  bool expeditionActiveNow = pet.expeditionActive(pet.lastSeenEpoch);
  if (expeditionWasActive && !expeditionActiveNow && pet.expeditionEndEpoch) {
    sfxPlay(SFX_EXPEDITION_FOUND);
    if (cardOpen && cardPage == 7) cardDirty = true;
  }
  expeditionWasActive = expeditionActiveNow;

  // latido de salud cada 5 min (para el soak test; se descarta si no hay monitor)
  static uint32_t lastHealth = 0;
  uint32_t healthMs = powerSave ? 600000UL : 300000UL;
  if (now - lastHealth > healthMs) {
    lastHealth = now;
    Serial.printf("HEALTH up=%lus heap=%u min=%u rMax=%lums lMax=%lums screen=%s\n",
                  (unsigned long)(now / 1000), ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                  (unsigned long)perfRenderMaxMs, (unsigned long)perfLoopMaxMs, screenName());
    perfRenderMaxMs = 0;
    perfLoopMaxMs = 0;
    perfSlowMs = 0;
    perfSlowName = "-";
  }

  // Juego/combate usan intervalos conservadores para que el redibujado no pise
  // el envio DMA del frame anterior; las pantallas estaticas se saltan si no
  // estan "dirty".
  if (now - lastRender >= renderIntervalMs()) {
    lastRender = now;
    uint32_t rt0 = millis();
    PERF_SECTION("render", render());
    perfRenderLastMs = millis() - rt0;
    if (perfRenderLastMs > perfRenderMaxMs) perfRenderMaxMs = perfRenderLastMs;
    perfRenderCount++;
  }

  // el light sleep del ahorro de energia no es trabajo del bucle: se mide antes
  perfLoopLastMs = millis() - loopStart;
  if (perfLoopLastMs > perfLoopMaxMs) perfLoopMaxMs = perfLoopLastMs;
  maybeLightSleep(millis());
  lastLoopStart = loopStart;
}

// brillo segun sueno + inactividad (proteccion del AMOLED)
void updateBrightness(uint32_t now) {
  // los eventos visibles despiertan la pantalla solos
  if (pet.evolving() || pet.ceremony || pet.eating() || pet.showHeart()) {
    lastInteract = now;
  }
  uint32_t idle = now - lastInteract;
  dimStage = (idle > 300000) ? 2 : (idle > 90000) ? 1 : 0;
  // luz de noche solo en la pantalla del bicho dormido: en menus, fichas,
  // Pokedex y ajustes el brillo es el normal
  bool sleepDim = pet.sleeping && !strcmp(screenName(), "main");
  uint8_t target = sleepDim ? 25 : (usbPresent() ? 180 : 145);
  if (dimStage == 1) target = sleepDim ? 10 : 60;
  else if (dimStage == 2) target = 8;
  if (screenOff) target = 0;
  static uint8_t current = 255;
  if (target != current) {
    current = target;
    panel->setBrightness(target);
  }
}

// ---------- consola serie (provision de SD + depuracion) ----------

void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;
  if (sdSerialCommand(line)) return;

  // Browser save backup protocol: SAVEGET -> "SAVE <bytes>" + hex data + DONE;
  // SAVEPUT <bytes> -> OK + raw data -> DONE and reboot. The Pet checksum
  // rejects altered or incomplete snapshots before NVS is changed.
  if (line == "SAVEGET") {
    uint8_t backup[Pet::BACKUP_MAX_BYTES];
    size_t size = pet.exportBackup(backup, sizeof(backup));
    if (!size) { Serial.println("ERR"); return; }
    // Normal game logs must never block when no terminal is open, hence setup
    // uses a zero TX timeout. A backup is requested by a connected browser,
    // so temporarily wait for its complete (hex) response instead of dropping it.
    Serial.setTxTimeoutMs(1500);
    Serial.printf("SAVE %u\n", (unsigned)size);
    Serial.print("SAVEHEX ");
    static const char HEX_CHARS[] = "0123456789ABCDEF";
    for (size_t i = 0; i < size; ++i) { Serial.print(HEX_CHARS[backup[i] >> 4]); Serial.print(HEX_CHARS[backup[i] & 15]); }
    Serial.println();
    Serial.println("DONE");
    Serial.flush();
    Serial.setTxTimeoutMs(0);
    return;
  }
  if (line.startsWith("SAVEPUT ")) {
    uint32_t size = (uint32_t)line.substring(8).toInt();
    if (size == 0 || size > Pet::BACKUP_MAX_BYTES) { Serial.println("ERR"); return; }
    uint8_t backup[Pet::BACKUP_MAX_BYTES];
    Serial.println("OK");
    Serial.setTimeout(6000);
    size_t got = Serial.readBytes(backup, size);
    Serial.setTimeout(1000);
    if (got != size || !pet.importBackup(backup, size)) { Serial.println("ERR"); return; }
    Serial.println("DONE");
    delay(150);
    ESP.restart();
    return;
  }

  if (line == "PERF") {
    Serial.printf("screen=%s render=%lums max=%lums count=%lu skip=%lu loop=%lums loopMax=%lums interval=%u dirty ui=%d card=%d clock=%d help=%d kb=%d menu=%d battle=%d gallery=%d\n",
                  screenName(),
                  (unsigned long)perfRenderLastMs, (unsigned long)perfRenderMaxMs,
                  (unsigned long)perfRenderCount, (unsigned long)perfRenderSkipCount,
                  (unsigned long)perfLoopLastMs, (unsigned long)perfLoopMaxMs,
                  renderIntervalMs(), uiDirty, cardDirty, clockDirty, helpDirty,
                  keyboardDirty, gameMenuDirty, battleDirty, galleryDirty);
    perfRenderMaxMs = 0;
    perfLoopMaxMs = 0;
    Serial.println("DONE");
  } else if (line == "HATCH") {
    pet.eggTap(); pet.eggTap(); pet.eggTap();
    Serial.println("DONE");
  } else if (line.startsWith("SPEC ")) {
    int n = line.substring(5).toInt();
    if (n >= 1 && n <= DEX_COUNT) {
      pet.prevSpeciesId = pet.speciesId;
      pet.speciesId = n;
      Serial.printf("especie #%d %s\n", n, DEX_TBL[n].name);
    }
    Serial.println("DONE");
  } else if (line.startsWith("LVL ")) {
    pet.ageMinutes = (uint32_t)line.substring(4).toInt() * MINUTES_PER_LEVEL;
    Serial.println("DONE");
  } else if (line.startsWith("TIME ")) {
    uint32_t e = (uint32_t)line.substring(5).toInt();
    rtcSetEpoch(e);
    pet.setClock(e);
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line.startsWith("RTCSET ")) {  // solo RTC (simular apagados en pruebas)
    rtcSetEpoch((uint32_t)line.substring(7).toInt());
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line == "TIME") {
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line == "GAL") {
    galleryOpen = !galleryOpen;
    galleryDetail = 0;
    galleryDirty = true;
    if (!galleryOpen) galleryPmd.unload();
    Serial.println("DONE");
  } else if (line == "EGGS") {
    // simula 20 tiradas de huevo (no cambia el estado del juego)
    for (int i = 0; i < 20; i++) {
      int16_t d = pet.pickEggSpecies();
      Serial.printf("%d:%s(r%u) ", d, DEX_TBL[d].name, DEX_TBL[d].rarity);
    }
    Serial.println();
    Serial.println("DONE");
  } else if (line == "SHINY") {  // alterna shiny del actual (pruebas)
    pet.shiny = !pet.shiny;
    Serial.printf("shiny=%d\n", pet.shiny);
    Serial.println("DONE");
  } else if (line.startsWith("NICK ")) {
    pet.rename(line.substring(5).c_str());
    Serial.printf("nick=%s\n", pet.nick);
    Serial.println("DONE");
  } else if (line == "CAREDAY") {  // simula un dia nuevo cuidado (pruebas)
    pet.setClock(pet.lastSeenEpoch + 86400);
    pet.caress();
    Serial.printf("streak=%u bond=%u medals=0x%X\n", pet.streak, pet.bond, pet.medals);
    Serial.println("DONE");
  } else if (line == "BYE") {
    pet.startFarewell();
    Serial.println("DONE");
  } else if (line == "RUN") {
    pet.startRunaway();
    Serial.println("DONE");
  } else if (line == "BEEP") {
    sfxPlay(SFX_HATCH);  // prueba de audio
    Serial.println("DONE");
  } else if (line == "ABANDON") {
    pet.dbgRunawayReady();  // fuerza el estado "lista para escaparse" (test del boton)
    Serial.println("DONE");
  } else if (line == "WIPE") {
    pet.factoryReset();     // borra NVS y reinicia -> partida nueva (eleccion de inicial)
    Serial.println("DONE");
    delay(100);
    ESP.restart();
  } else if (line == "REG") {
    Serial.printf("pokedex %u/%u:", pet.registeredCount(), DEX_COUNT);
    for (int i = 1; i <= DEX_COUNT; i++)
      if (pet.isRegistered(i)) Serial.printf(" %d", i);
    Serial.println();
    Serial.println("DONE");
#ifdef TAMAPOKE_LOCAL_TEST
  } else if (line.startsWith("TESTMON ")) {
    int split = line.indexOf(' ', 8);
    int n = line.substring(8, split < 0 ? line.length() : split).toInt();
    int shinyFlag = split < 0 ? 0 : line.substring(split + 1).toInt();
    if (n >= 1 && n <= DEX_COUNT) {
      pet.prevSpeciesId = pet.speciesId;
      pet.speciesId = n;
      pet.shiny = shinyFlag != 0;
      pet.sleeping = false;
      pet.ceremony = CER_NONE;
      markUiDirty();
      Serial.printf("testmon #%d %s shiny=%d\n", n, DEX_TBL[n].name, pet.shiny);
    }
    Serial.println("DONE");
  } else if (line.startsWith("TESTEVO ")) {
    int split = line.indexOf(' ', 8);
    int source = line.substring(8, split < 0 ? line.length() : split).toInt();
    int target = split < 0 ? 0 : line.substring(split + 1).toInt();
    if (source >= 1 && source <= DEX_COUNT) {
      pet.speciesId = source;
      pet.prevSpeciesId = -1;
      pet.sleeping = false;
      pet.ceremony = CER_NONE;
      pet.resetEvolutionDeferral();
      pet.careMistakes = 0;
      uint8_t requiredLevel = pet.evolutionRequiredLevelFor(target);
      pet.ageMinutes = (uint32_t)(requiredLevel > 0 ? requiredLevel - 1 : 0) * MINUTES_PER_LEVEL;
      pet.fullness = pet.joy = pet.energy = pet.hygiene = 80;
      pet.bond = 60;
      pet.trAtk = pet.trDef = pet.trSpe = 0;
      uint32_t testEpoch = 1767225600UL + 12UL * 3600UL;
      if (target == 197) testEpoch += 9UL * 3600UL;
      pet.setClock(testEpoch);
      if (source == 236) {
        if (target == 106) pet.trAtk = 80;
        else if (target == 107) pet.trDef = 80;
        else pet.trAtk = pet.trDef = 40;
      }
      markUiDirty();
      Serial.printf("testevo #%d -> #%d level=%u bond=%u\n", source, target,
                    pet.level(), pet.bond);
    }
    Serial.println("DONE");
  } else if (line == "CAUGHT") {
    Serial.printf("caught %u/%u:", pet.caughtCount(), DEX_COUNT);
    for (int i = 1; i <= DEX_COUNT; i++)
      if (pet.isCaught(i)) Serial.printf(" %d", i);
    Serial.println();
    Serial.println("DONE");
  } else if (line.startsWith("CAUGHT ")) {
    int n = line.substring(7).toInt();
    if (n < 1 || n > DEX_COUNT) n = pet.speciesId;
    if (n >= 1 && n <= DEX_COUNT) {
      pet.registerCaught((int16_t)n);
      Serial.printf("caught #%d %s\n", n, DEX_TBL[n].name);
    }
    Serial.println("DONE");
  } else if (line.startsWith("BATTLE ")) {
    int split = line.indexOf(' ', 7);
    int n = line.substring(7, split < 0 ? line.length() : split).toInt();
    int8_t shinyOverride = -1;
    if (split >= 0) shinyOverride = line.substring(split + 1).toInt() ? 1 : 0;
    startBattleWith((int16_t)n, pet.level(), shinyOverride);
    Serial.printf("battle #%d %s shiny=%d caught=%d\n", battleDex,
                  (battleDex >= 1 && battleDex <= DEX_COUNT) ? DEX_TBL[battleDex].name : "?",
                  battleShiny, pet.isCaught(battleDex));
    Serial.println("DONE");
  } else if (line == "BATTLE") {
    startBattle();
    Serial.printf("battle #%d %s caught=%d\n", battleDex,
                  (battleDex >= 1 && battleDex <= DEX_COUNT) ? DEX_TBL[battleDex].name : "?",
                  pet.isCaught(battleDex));
    Serial.println("DONE");
#endif
  } else if (line == "SAVEINFO") {
    Serial.printf("fw=%s save=%s createdBoot=%d spec=%d level=%u egg=%d starter=%d age=%lu\n",
                  FW_VERSION, pet.saveLoadedFromNvs ? "loaded" : "created",
                  pet.saveCreatedThisBoot, pet.speciesId, pet.level(), pet.isEgg(),
                  pet.awaitingStarter(), (unsigned long)pet.ageMinutes);
    Serial.printf("battle wins=%u losses=%u streak=%u best=%u nick=%s\n",
                  pet.battleWins, pet.battleLosses, pet.battleStreak,
                  pet.bestBattleStreak, pet.nick);
    Serial.printf("records ball=%u catch=%u memo=%u sack=%u\n",
                  pet.gameHi, pet.catchHi, pet.memoHi, pet.strHi);
    Serial.println("DONE");
  } else if (line == "HEALTH") {
    Serial.printf("up=%lus heap=%u min=%u sd=%d mon=%d\n",
                  (unsigned long)(millis() / 1000), ESP.getFreeHeap(),
                  ESP.getMinFreeHeap(), sdReady, pmd.loaded || mon.loaded);
    Serial.println("DONE");
  } else if (line == "STATS") {
    Serial.printf("spec=%d nv=%u com=%u fel=%u ene=%u lim=%u desc=%u sd=%d mon=%d bat=%d usb=%d rtc=%u\n",
                  pet.speciesId, pet.level(), pet.fullness, pet.joy, pet.energy,
                  pet.hygiene, pet.careMistakes, sdReady, mon.loaded,
                  batPercent(), usbPresent(), rtcEpoch());
    Serial.printf("peso=%u fue=%u def=%u vel=%u genes=%u/%u/%u tr=%u/%u/%u baya=%d\n",
                  pet.weight, pet.atkStat(), pet.defStat(), pet.speStat(),
                  pet.geneAtk, pet.geneDef, pet.geneSpe,
                  pet.trAtk, pet.trDef, pet.trSpe, pet.berryKnown);
    Serial.printf("shiny=%d streak=%u/%u bond=%u medals=0x%X(%u) nick=%s\n",
                  pet.shiny, pet.streak, pet.bestStreak, pet.bond, pet.medals,
                  pet.totalMedals, pet.nick);
    Serial.printf("steps=%lu/%lu trail=%u shinyLuck=%u/4096 catchBonus=%u\n",
                  (unsigned long)pet.stepsToday, (unsigned long)pet.stepsTotal,
                  pet.stepTrailRank(), pet.stepShinyChancePer4096(), pet.stepCatchBonus());
    Serial.println("DONE");
  } else if (line == "IMU") {
    Serial.printf("imu=%d addr=0x%02X mag=%.2f gyro=%.0f rawPed=%lu softPed=%lu phase=%u hour=%d\n",
                  imuOk(), imuAddr(), imuLastMagG(), imuLastGyroDps(),
                  (unsigned long)imuPedometer(), (unsigned long)imuSoftwarePedometer(),
                  currentDayPhase(), sceneHour());
    Serial.println("DONE");
  } else if (line == "SHAKE") {
    bool okShake = pet.applyShake();
    Serial.printf("shake=%d joy=%u\n", okShake, pet.joy);
    Serial.println("DONE");
  } else if (line.startsWith("WALK ")) {
    uint16_t n = (uint16_t)line.substring(5).toInt();
    uint8_t gained = pet.applyWalk(n, rtcEpoch());
    Serial.printf("walk=%u today=%lu total=%lu joy=%u bond=%u\n", gained,
                  (unsigned long)pet.stepsToday, (unsigned long)pet.stepsTotal,
                  pet.joy, pet.bond);
    Serial.println("DONE");
  } else if (line == "STEPS") {
    Serial.printf("steps today=%lu total=%lu rank=%u shiny=%u/4096 catch+%u usb=%d imu=%d raw=%lu soft=%lu\n",
                  (unsigned long)pet.stepsToday, (unsigned long)pet.stepsTotal,
                  pet.stepTrailRank(), pet.stepShinyChancePer4096(), pet.stepCatchBonus(),
                  usbPresent(), imuOk(), (unsigned long)imuPedometer(),
                  (unsigned long)imuSoftwarePedometer());
    Serial.println("DONE");
  }
}

// ---------- entrada tactil ----------

bool inPetZone(int16_t x, int16_t y) {
  return x > 110 && x < 356 && y > 95 && y < 310;
}

bool inProfilePetPortrait(int16_t x, int16_t y) {
  int dx = x - CX, dy = y - 206;
  return dx * dx + dy * dy <= 76 * 76;
}

// el toque se resuelve al LEVANTAR el dedo para distinguir tap de deslizar
void handleTouch() {
  static uint32_t lastPoll = 0;
  uint32_t now = millis();
  if (deadlineActive(now, ignoreTouchUntil)) {
    gTouchIrq = false;
    wasPressed = false;
    return;
  }
  if (now - lastPoll < (powerSave && screenOff ? 80UL : 20UL)) return;  // 50 Hz activo; menos si pantalla apagada
  lastPoll = millis();
  // solo tocamos el bus si el chip aviso por INT o si el dedo sigue abajo (hay
  // que detectar el levantamiento). Leer el CST9217 dormido se colgaba ~1s y
  // congelaba el loop entero; SensorLib no respeta el timeout de Wire.
  if (!gTouchIrq && !wasPressed) return;
  gTouchIrq = false;
  int16_t x, y;
  bool pressed = touch.getPoint(&x, &y, 1) > 0;
  // El CST9217 a veces pierde el dedo un instante en mitad de un gesto: el dedo
  // solo se da por levantado tras GESTURE_RELEASE_MS sin contacto (deslizar lento).
  static uint32_t lastContact = 0;
  if (pressed) lastContact = now;
  else if (wasPressed && now - lastContact < GESTURE_RELEASE_MS) return;

  // Pantalla apagada por PWR: no despertar por cualquier roce accidental.
  // PWR corto sigue despertando al instante; touch requiere mantener pulsado.
  if (screenOff) {
    if (pressed && !wasPressed) {
      tX0 = tXl = x;
      tY0 = tYl = y;
      tStart = now;
      holdFired = false;
      wasPressed = true;
    } else if (pressed) {
      tXl = x;
      tYl = y;
      if (!holdFired && now - tStart >= 700UL &&
          abs(tXl - tX0) < 42 && abs(tYl - tY0) < 42) {
        screenOff = false;
        dimStage = 0;
        lastInteract = now;
        holdFired = true;
        swallowGesture = true;
        markUiDirty();
        lockTouchBrief(320);
        sfxPlay(SFX_TAP);
      }
    } else {
      wasPressed = false;
      holdFired = false;
    }
    return;
  }

  // saco de entrenamiento: cada toque cuenta al instante (aporrear rapido)
  if (sackOpen) {
    if (pressed && !wasPressed) {
      lastInteract = millis();
      sackTap();  // abandonar: PWR
    }
    wasPressed = pressed;
    return;
  }

  if (pressed && !wasPressed) {  // empieza el gesto
    galRotHasAngle = false;
    cleanRubHas = false;
    galRotAccum = 0;
    tX0 = tXl = x;
    tY0 = tYl = y;
    tStart = millis();
    holdFired = false;
    // casi apagada: el primer toque solo despierta. Atenuada (aun visible): el toque vale.
    swallowGesture = (dimStage >= 2) || screenOff;
    screenOff = false;
    lastInteract = millis();
    if (!swallowGesture && gameOpen && gameMode == 0) {
      gameTap(x, y);         // ball: cuenta al tocar, no al soltar
      swallowGesture = true; // evita doble tap al levantar el dedo
    } else if (!swallowGesture && gameOpen && gameMode == 1) {
      catchTap(x, y);        // juego de reflejos: cuenta al tocar, no al soltar
      swallowGesture = true; // evita doble tap al levantar el dedo
    }
  } else if (pressed) {  // sigue apoyado
    tXl = x;
    tYl = y;
    if (!swallowGesture && galleryOpen && galleryDetail) galleryRotateDrag(x, y);
    if (!swallowGesture && gameOpen && gameMode == 3) cleanRub(x, y);
    // pulsacion larga sin moverse sobre el bicho -> dialogo de soltar
    if (!holdFired && !swallowGesture && !galleryOpen && !cardOpen && !kbOpen && !clockOpen && !helpOpen && !gameOpen && millis() - tStart > 3000 &&
        abs(tXl - tX0) < 30 && abs(tYl - tY0) < 30 && inPetZone(tX0, tY0) &&
        !pet.isEgg() && !confirmUntil && !pet.ceremony) {
      confirmUntil = millis() + 10000;
      holdFired = true;
    }
  } else if (wasPressed) {  // levanta el dedo: resolver gesto
    lastInteract = millis();
    int dx = tXl - tX0, dy = tYl - tY0;
    uint32_t dt = millis() - tStart;
    if (galleryRotating) {
      galleryRotateEnd();  // el carrusel consume el gesto
    } else if (!holdFired && !swallowGesture) {
      switch (classifyGesture(dx, dy, dt)) {
        case G_SWIPE_RIGHT: onSwipe(1); break;
        case G_SWIPE_LEFT: onSwipe(-1); break;
        case G_SWIPE_DOWN: onSwipeV(1); break;
        case G_SWIPE_UP: onSwipeV(-1); break;
        case G_TAP: onTap(tX0, tY0); break;
        default: break;
      }
    }
  }
  wasPressed = pressed;
}

// PWR corto fuera de la pantalla principal: cierra lo que este abierto.
// Devuelve false en la pantalla principal (alli PWR bloquea la pantalla).
bool goBack() {
  uint32_t now = millis();
  if (pet.awaitingStarter()) {
    if (starterRegion == 0xFF) return false;
    starterRegion = 0xFF;  // de la eleccion de inicial a la de region
    starterDirty = true;
    return true;
  }
  if (netOpen) { closeNet(); return true; }
  if (helpOpen) { helpOpen = false; clockOpen = true; setPage = SETP_MENU; clockDirty = true; lockTouchBrief(); return true; }
  if (kbOpen) { kbOpen = false; cardDirty = true; lockTouchBrief(); return true; }  // renombrar: cancelar
  if (galleryOpen) {
    galleryRotating = false;
    if (galleryDetail) {
      galleryDetail = 0;
      galleryDirty = true;
    } else {
      galleryOpen = false;
      markUiDirty();
    }
    galleryPmd.unload();
    lockTouchBrief();
    return true;
  }
  if (gameOpen) { gameOpen = false; markUiDirty(); lockTouchBrief(); return true; }   // abandonar sin premio
  if (sackOpen) { sackOpen = false; markUiDirty(); lockTouchBrief(); return true; }
  if (battleOpen) {
    if (battleResolved) closeBattle();  // en pleno combate PWR no hace nada (no huir por error)
    return true;
  }
  if (clockOpen) {
    if (setPage != SETP_MENU) { setPage = SETP_MENU; clockDirty = true; }
    else { clockOpen = false; markUiDirty(); }
    lockTouchBrief();
    return true;
  }
  if (cardOpen) {
    if (expeditionTrainChoiceOpen) expeditionTrainChoiceOpen = false;
    else cardOpen = false;
    markUiDirty();
    lockTouchBrief();
    return true;
  }
  if (gameMenuOpen) { gameMenuOpen = false; markUiDirty(); lockTouchBrief(); return true; }
  if (choiceKind) { choiceKind = 0; markUiDirty(); return true; }
  if (confirmUntil) { confirmUntil = 0; markUiDirty(); return true; }
  if (feedMenuUntil) { feedMenuUntil = 0; markUiDirty(); return true; }
  if (wildPromptUntil && deadlineActive(now, wildPromptUntil)) {
    wildPromptUntil = 0;
    scheduleNextWild(now);
    markUiDirty();
    return true;
  }
  return false;
}

// deslizar vertical: abre la ficha (arriba) o los ajustes (abajo) desde la pantalla principal
void openClock();  // prototipo
int16_t boxDexAt(uint16_t index);
uint8_t boxPageCount();
int sceneHour();
uint8_t currentDayPhase();
StrId dayPhaseTextId(uint8_t phase);
bool mainScreenReadyForShake();
void maybeOfferMorning(uint32_t now);
uint32_t pmdActTotalMs(const PmdAct &a);
void startCleanGame();
void startTypeGame();
void cleanTap(int16_t x, int16_t y);
void typeTap(int16_t x, int16_t y);
void expeditionCardTap(int16_t x, int16_t y);
uint32_t expeditionNowEpoch();
bool expeditionHudVisible();
bool inExpeditionHudHit(int16_t x, int16_t y);
void openExpeditionCard();
void drawExpeditionHud();
bool inStepsHudHit(int16_t x, int16_t y);
void openStepsCard();
void drawStepsHud();

void onSwipeV(int dir) {
  if (netOpen || helpOpen) return;  // se sale con PWR
  if (pet.awaitingStarter()) return;  // bloqueado durante la eleccion de inicial
  if (wildPromptUntil && deadlineActive(millis(), wildPromptUntil)) return;
  if (wildPromptUntil) wildPromptUntil = 0;
  if (gameMenuOpen) return;
  if (gameOpen || galleryOpen || kbOpen || sackOpen || battleOpen || pet.ceremony) return;
  if (clockOpen || cardOpen) return;  // se sale con PWR
  if (dir > 0) {                    // deslizar abajo: ajustar hora
    if (!confirmUntil && !feedMenuUntil) openClock();
  } else if (!pet.isEgg() && !confirmUntil && !feedMenuUntil) {
    cardOpen = true;                // deslizar arriba: ficha
    cardPage = 0;
    cardDirty = true;
    lockTouchBrief();
    sfxPlay(SFX_MENU);
  }
}

// deslizar: dir +1 = hacia la derecha
void onSwipe(int dir) {
  if (netOpen) return;
  if (helpOpen) {
    if (dir < 0 && helpPage + 1 < HELP_PAGE_COUNT) { helpPage++; helpDirty = true; }
    else if (dir > 0 && helpPage > 0) { helpPage--; helpDirty = true; }
    sfxPlay(SFX_MENU);
    return;
  }
  if (pet.awaitingStarter()) return;  // bloqueado durante la eleccion de inicial
  if (wildPromptUntil && deadlineActive(millis(), wildPromptUntil)) return;
  if (wildPromptUntil) wildPromptUntil = 0;
  if (gameMenuOpen) return;
  if (gameOpen || kbOpen || clockOpen || battleOpen) return;
  if (cardOpen) {  // dentro de la ficha: cambiar paginas
    int p = (int)cardPage + (dir > 0 ? -1 : 1);  // izquierda avanza
    uint8_t old = cardPage;
    cardPage = p < 0 ? 0 : (p >= CARD_COUNT ? CARD_COUNT - 1 : p);
    if (cardPage != old) { expeditionTrainChoiceOpen = false; cardDirty = true; sfxPlay(SFX_MENU); }
    return;
  }
  if (!galleryOpen) {
    if (!pet.ceremony && !confirmUntil) {
      galleryOpen = true;
      galleryPage = 0;
      galleryDetail = 0;
      galleryDirty = true;
      lockTouchBrief();
      sfxPlay(SFX_MENU);
    }
    return;
  }
  if (galleryDetail) return;  // en detalle: PWR vuelve a la rejilla
  int np = galleryPage - dir;  // deslizar a la izquierda avanza pagina
  int maxPage = galleryPageCount() - 1;
  if (np < 0) np = 0;          // se sale con PWR
  if (np > maxPage) np = maxPage;
  if (np != galleryPage) {
    galleryPage = np;
    galleryDirty = true;
    sfxPlay(SFX_MENU);
  }
}

struct GameMenuTile {
  int16_t x, y, w, h;
  uint8_t game;
};

#define GAME_MENU_COUNT 6
static const GameMenuTile GAME_MENU_TILES[GAME_MENU_COUNT] = {
  { 88, 156, 138, 58, 0 },
  { 240, 156, 138, 58, 1 },
  { 88, 226, 138, 58, 2 },
  { 240, 226, 138, 58, 3 },
  { 88, 296, 138, 58, 4 },
  { 240, 296, 138, 58, 5 },
};

int8_t gameMenuHit(int16_t x, int16_t y) {
  for (uint8_t i = 0; i < GAME_MENU_COUNT; i++) {
    const GameMenuTile &t = GAME_MENU_TILES[i];
    if (x >= t.x && x <= t.x + t.w && y >= t.y && y <= t.y + t.h) return i;
  }
  return -1;
}

void startGameMenuChoice(uint8_t idx) {
  switch (GAME_MENU_TILES[idx].game) {
    case 0: startGame(); break;
    case 1: startCatchGame(); break;
    case 2: startMemoGame(); break;
    case 3: startCleanGame(); break;
    case 4: startTypeGame(); break;
    case 5: startTiltGame(); break;
  }
}

void onTap(int16_t x, int16_t y) {
  // Serial.printf("TOUCH %d %d\n", x, y);  // diagnostico (silenciado: satura el log)
  if (pet.awaitingStarter()) {  // primera partida: region y despues inicial
    if (starterRegion == 0xFF) {
      for (uint8_t i = 0; i < 3; i++) {
        int ry = STARTER_ROW_Y + i * (STARTER_ROW_H + STARTER_ROW_GAP);
        if (x >= 70 && x <= 396 && y >= ry && y <= ry + STARTER_ROW_H) {
          starterRegion = i;
          starterDirty = true;
          lockTouchBrief();
          sfxPlay(SFX_TAP);
          break;
        }
      }
      return;
    }
    for (int i = 0; i < 3; i++) {
      int ry = STARTER_ROW_Y + i * (STARTER_ROW_H + STARTER_ROW_GAP);
      if (x >= 70 && x <= 396 && y >= ry && y <= ry + STARTER_ROW_H) {
        pet.chooseStarter(STARTER_DEX[starterRegion][i]);
        starterRegion = 0xFF;
        markUiDirty();
        lockTouchBrief();
        sfxPlay(SFX_TAP);
        break;
      }
    }
    return;
  }
  if (galleryOpen) {
    galleryTap(x, y);
    return;
  }
  if (kbOpen) {
    keyboardTap(x, y);
    return;
  }
  if (helpOpen) {
    helpTap(x, y);
    return;
  }
  if (netOpen) {
    netTap(x, y);
    return;
  }
  if (clockOpen) {
    clockTap(x, y);
    return;
  }
  if (pet.ceremony) return;  // durante la despedida no hay botones
  if (cardOpen) {
    if (cardPage == 0 && inProfilePetPortrait(x, y)) {
      speciesChirpPlay(pet.speciesId, true);
    }
    else if (cardPage == 0 && y < 84) openKeyboard();  // tocar el nombre = renombrar
    else if (cardPage == 0 && y >= PROFILE_FRAME_Y && y <= PROFILE_FRAME_Y + PROFILE_ARROW_H) {
      uint8_t count = pet.unlockedCollectionFrameCount();
      uint8_t next = pet.collectionFrame;
      if (x >= PROFILE_PREV_X && x <= PROFILE_PREV_X + PROFILE_ARROW_W) next = pet.collectionFrame == 0 ? count - 1 : pet.collectionFrame - 1;
      else if (x >= PROFILE_NEXT_X && x <= PROFILE_NEXT_X + PROFILE_ARROW_W) next = (uint8_t)((pet.collectionFrame + 1) % count);
      else return;
      if (pet.setCollectionFrame(next)) {
        cardDirty = true;
        sfxPlay(SFX_MENU);
        lockTouchBrief();
      }
    }
    else if (cardPage == 3) {
      if (x >= BOX_SORT_X && x <= BOX_SORT_X + BOX_SORT_W && y >= BOX_SORT_Y && y <= BOX_SORT_Y + BOX_SORT_H) {
        boxSort = (boxSort + 1) % 3;
        boxPage = 0;
        cardDirty = true;
        sfxPlay(SFX_TAP);
      } else if (x >= 86 && x <= 180 && y >= BOX_NAV_Y && y <= BOX_NAV_Y + BOX_NAV_H) {
        if (boxPage > 0) { boxPage--; cardDirty = true; }
        sfxPlay(SFX_TAP);
      } else if (x >= 286 && x <= 380 && y >= BOX_NAV_Y && y <= BOX_NAV_Y + BOX_NAV_H) {
        uint8_t pages = boxPageCount();
        if (boxPage + 1 < pages) { boxPage++; cardDirty = true; }
        sfxPlay(SFX_TAP);
      } else {
        int row = (y - BOX_ROW0_Y) / BOX_ROW_STEP;
        if (y >= BOX_ROW0_Y && row < BOX_ROWS && x >= 58 && x <= 408 &&
            y <= BOX_ROW0_Y + row * BOX_ROW_STEP + BOX_ROW_H) {
          int16_t dex = boxDexAt((uint16_t)boxPage * BOX_ROWS + row);
          if (dex > 0) {
            cardOpen = false;
            galleryOpen = true;
            galleryDetail = dex;
            galleryDirty = true;
            lockTouchBrief();
            galleryPmd.load(dex, pet.isShinyRegistered(dex));
            sfxPlay(SFX_TAP);
            speciesChirpPlay(dex);
          }
        }
      }
    } else if (cardPage == 4 && y >= 286 && y <= 336 && x >= 82 && x <= 384) {
      cardOpen = false;
      markUiDirty();
      lockTouchBrief();
      startBattle();
    } else if (cardPage == 4 && y >= 330 && y <= 388 && x >= 82 && x <= 384) {
      cardOpen = false;            // boton ENTRENAR FUERZA
      markUiDirty();
      lockTouchBrief();
      startSack();
    } else if (cardPage == 7) {
      expeditionCardTap(x, y);
    }
    return;
  }
  if (gameOpen) {
    gameTap(x, y);
    return;
  }
  if (battleOpen) {
    battleTap(x, y);
    return;
  }
  if (petEventUntil && deadlineActive(millis(), petEventUntil) && inPetEventHit(x, y)) {
    acceptPetEvent();
    return;
  }
  if (gameMenuOpen) {
    int8_t hit = gameMenuHit(x, y);
    if (hit >= 0) { lockTouchBrief(140); startGameMenuChoice((uint8_t)hit); }
    else { gameMenuOpen = false; markUiDirty(); lockTouchBrief(); sfxPlay(SFX_TAP); }
    return;
  }
  if (wildPromptUntil) {
    if (deadlineActive(millis(), wildPromptUntil)) {
      bool fight = (x >= 93 && x <= 373 && y >= 226 && y <= 270);
      bool later = (x >= 93 && x <= 373 && y >= 278 && y <= 322);
      if (fight) {
        startBattleWith(wildPromptDex, wildPromptLevel, wildPromptShiny ? 1 : 0);
      } else if (later) {
        wildPromptUntil = 0;
        scheduleNextWild(millis());
        sfxPlay(SFX_TAP);
      }
    } else {
      wildPromptUntil = 0;
    }
    return;
  }
  if (inExpeditionHudHit(x, y)) {
    openExpeditionCard();
    return;
  }
  if (inStepsHudHit(x, y)) {
    openStepsCard();
    return;
  }
  if (choiceKind) {          // dialogo de decision o selector de objetivo evolutivo
    if (choiceKind == 3) {
      uint8_t options = pet.evolutionOptionCount();
      for (uint8_t i = 0; i < options; i++) {
        int y = 158 + i * 45;
        if (x >= 82 && x <= 394 && y <= 194 + i * 45 && y >= 158 + i * 45) {
          int16_t target = pet.evolutionOption(i);
          int16_t old = pet.speciesId;
          pet.evolveTo(target);
          evoPmd.load(old, pet.shiny);
          choiceKind = 0;
          return;
        }
      }
      if (y >= 392 && y <= 444) choiceKind = 0;
      return;
    }
    bool b1 = (x >= 93 && x <= 373 && y >= 206 && y <= 258);  // accion
    bool b2 = (x >= 93 && x <= 373 && y >= 268 && y <= 320);  // mantener / quedaros
    if (choiceKind == 1) {                 // evolucion
      if (b1) {
        if (pet.evolutionOptionCount() > 1) {
          choiceKind = 3;
          choiceUntil = millis() + 12000;
          return;
        }
        int16_t old = pet.speciesId;
        pet.evolveTo(pet.evolutionOption(0));
        evoPmd.load(old, pet.shiny);
      }
      else if (b2) pet.declineEvolve();
    } else if (choiceKind == 2) {          // despedida
      if (b1) pet.startFarewell();
      else if (b2) pet.declineFarewell();
    }
    choiceKind = 0;
    return;
  }
  if (confirmUntil) {        // dialogo "soltar?": SI / NO
    if (deadlineActive(millis(), confirmUntil) && x >= 118 && x <= 218 && y >= 252 && y <= 304) {
      pet.release();
    }
    confirmUntil = 0;
    return;
  }
  if (feedMenuUntil) {       // selector de comida
    if (deadlineActive(millis(), feedMenuUntil) && y >= FEED_MENU_Y && y <= FEED_MENU_Y + 64 && x >= 101 && x <= 365) {
      int item = (x - 101) / 66;
      if (item == 3) pet.feedCandy();
      else pet.feedBerry(item);
      sfxPlay(SFX_EAT);
    }
    feedMenuUntil = 0;
    return;
  }
  if (pet.isEgg()) {
    pet.eggTap();
    sfxPlay(SFX_TAP);
    return;
  }
  // boton de evolucion: abre el dialogo evolucionar/mantener
  if (pet.wantEvolveButton() && x >= EVO_BTN_X && x <= EVO_BTN_X + EVO_BTN_W &&
      y >= EVO_BTN_Y && y <= EVO_BTN_Y + EVO_BTN_H) {
    if (pet.canEvolveNow()) {
      choiceKind = 1; choiceUntil = millis() + 12000;
    } else {
      StrId blocked = pet.evolutionNeedsMoreBond() ? S_EVO_NEED_BOND : S_EVO_BLOCKED;
      snprintf(petEventMsg, sizeof(petEventMsg), "%s", T(blocked));
      petEventFeedbackUntil = millis() + 1800;
      sfxPlay(SFX_DENY);
    }
    return;
  }
  // botones de final (mismo recuadro): escapada directa; despedida abre dialogo
  if (x >= FAR_BTN_X && x <= FAR_BTN_X + FAR_BTN_W &&
      y >= FAR_BTN_Y && y <= FAR_BTN_Y + FAR_BTN_H) {
    if (pet.canRunawayNow()) { pet.startRunaway(); return; }
    if (pet.wantFarewellButton()) { choiceKind = 2; choiceUntil = millis() + 12000; return; }
  }
  for (int i = 0; i < 4; i++) {
    int dx = x - buttons[i].cx, dy = y - buttons[i].cy;
    if (abs(dx) <= BTN_HALF + 6 && abs(dy) <= BTN_HALF + 6) {
      Serial.printf("BTN %d\n", i);
      sfxPlay(SFX_TAP);
      if (i == 0) {
        if (!pet.sleeping) {
          feedMenuUntil = millis() + 6000;
          sfxPlay(SFX_MENU);
        }
      } else if (i == 1) {
        if (!pet.sleeping) {
          gameMenuOpen = true;
          gameMenuDirty = true;
          lockTouchBrief();
          sfxPlay(SFX_MENU);
        }
      } else if (i == 2) {
        pet.toggleLight();
      } else {
        startBath();
      }
      return;
    }
  }
  // tocar al bicho = caricia
  if (inPetZone(x, y)) {
    Serial.println("PET");
    uint8_t r = pet.interactPet(currentDayPhase() == 2);
    // Der Spezies-Chirp ist direkte Rueckmeldung fuer jeden Pet-Tap. Er darf
    // nicht von der zehnminuetigen Pflege-Belohnung abhaengen.
    if (!pet.sleeping && audioMode() == SOUND_FULL) speciesChirpPlay(pet.speciesId, true);
    if (r == PET_INTERACT_NONE) return;
    StrId msg = S_WAIT;
    if (r & PET_INTERACT_BOND) msg = S_BOND_GAIN;
    else if (r & PET_INTERACT_JOY) msg = S_HAPPY_FB;
    snprintf(petEventMsg, sizeof(petEventMsg), "%s", T(msg));
    petEventFeedbackUntil = millis() + 1600;
    if (!pet.sleeping && audioMode() != SOUND_FULL)
      sfxPlay((r & PET_INTERACT_BOND) ? SFX_HEART : SFX_TAP);
  }
}

// ---------- render ----------

bool gNight = false;  // noche real (por hora) o durmiendo: lo fija render()
uint16_t inkColor() { return gNight ? UI_INK_NIGHT : UI_INK; }

// ---------- escena de fondo: bioma del tipo + hora real del RTC ----------

#define C565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define HORIZON 232  // linea donde el cielo se encuentra con el suelo

uint16_t lerp565(uint16_t a, uint16_t b, int i, int n) {
  if (n <= 0) return a;
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  return (uint16_t)((((ar + (br - ar) * i / n) << 11)) |
                    (((ag + (bg - ag) * i / n) << 5)) | (ab + (bb - ab) * i / n));
}

void flushStampedFrame() {
  gfx->flush();
}

int sceneHour() {
  return sceneHourFromEpoch(pet.lastSeenEpoch);
}

uint8_t currentDayPhase() {
  return dayPhaseFromHour(sceneHour());
}

StrId dayPhaseTextId(uint8_t phase) {
  switch (phase) {
    case 0: return S_MORNING;
    case 2: return S_EVENING;
    case 3: return S_NIGHT;
    default: return S_DAY;
  }
}

// suelo de cada bioma de dia (de noche se mezcla hacia el azul nocturno)
static const uint16_t BIOME_SOIL[6] = {
  C565(0x7e, 0xc0, 0x7f),  // 0 pradera
  C565(0xdc, 0xca, 0x94),  // 1 playa (arena)
  C565(0x4f, 0x8a, 0x55),  // 2 bosque
  C565(0x8a, 0x55, 0x44),  // 3 volcan
  C565(0xa8, 0x90, 0x6a),  // 4 montana
  C565(0xe6, 0xee, 0xf5),  // 5 nieve
};

void drawClouds(uint32_t now, uint16_t col) {
  for (int k = 0; k < 2; k++) {
    int cx = (int)((now / 50 + k * 250) % 560) - 40;
    int cy = 70 + k * 34;
    gfx->fillCircle(cx, cy, 16, col);
    gfx->fillCircle(cx + 18, cy + 3, 13, col);
    gfx->fillCircle(cx - 15, cy + 4, 12, col);
  }
}

void drawScene(uint8_t biome, uint32_t now, bool night) {
  int h = sceneHour();
  uint16_t top, bot;
  if (night)            { top = C565(0x0c, 0x12, 0x24); bot = C565(0x1e, 0x26, 0x46); }
  else if (h < 8)       { top = C565(0xd1, 0x6a, 0x86); bot = C565(0xf3, 0xb8, 0x7c); }  // amanecer
  else if (h < 18)      { top = C565(0x8f, 0xc8, 0xea); bot = C565(0xdc, 0xee, 0xe6); }  // dia
  else                  { top = C565(0xc7, 0x5a, 0x4a); bot = C565(0xf0, 0xae, 0x64); }  // atardecer

  // cielo en bandas
  for (int y = 0; y < HORIZON; y += 8)
    gfx->fillRect(0, y, 466, 8, lerp565(top, bot, y, HORIZON));

  // sol o luna
  if (night) {
    gfx->fillCircle(360, 78, 24, C565(0xe8, 0xee, 0xf5));
    gfx->fillCircle(370, 72, 22, lerp565(top, bot, 78, HORIZON));  // creciente
    for (auto &st : STARS) gfx->fillRect(st[0], st[1], 4, 4, UI_WHITE);
  } else if (h < 18) {
    gfx->fillCircle(360, 84, 26, h < 8 ? C565(0xff, 0xd9, 0x8a) : C565(0xff, 0xe7, 0x9f));
    drawClouds(now, C565(0xff, 0xff, 0xff));
  } else {
    gfx->fillCircle(233, HORIZON - 6, 34, C565(0xff, 0xf1, 0xc8));  // sol poniente
  }

  // mar de la playa: una franja de agua sobre la arena
  uint16_t soil = BIOME_SOIL[biome < 6 ? biome : 0];
  if (night) soil = lerp565(soil, C565(0x16, 0x1c, 0x30), 9, 16);
  if (biome == 1) {
    uint16_t sea = night ? C565(0x1c, 0x34, 0x52) : C565(0x4f, 0x96, 0xc4);
    gfx->fillRect(0, HORIZON - 26, 466, 26, sea);
    for (int i = 0; i < 3; i++) {
      int wy = HORIZON - 22 + i * 7;
      uint16_t fc = night ? C565(0x3a, 0x58, 0x78) : C565(0xbf, 0xe6, 0xf5);
      gfx->fillRect(60 + ((now / 60 + i * 30) % 60), wy, 26, 2, fc);
      gfx->fillRect(300 - ((now / 60 + i * 20) % 60), wy, 26, 2, fc);
    }
  }

  // suelo
  gfx->fillRect(0, HORIZON, 466, 466 - HORIZON, soil);
  uint16_t hill = lerp565(soil, night ? C565(0x0c, 0x12, 0x24) : C565(0xff, 0xff, 0xff), 3, 16);
  gfx->fillRoundRect(-60, HORIZON - 14, 586, 60, 30, hill);

  // detalles del bioma
  uint16_t dk = lerp565(soil, C565(0x10, 0x18, 0x20), night ? 11 : 7, 16);
  if (biome == 2) {  // bosque: coniferas en silueta
    for (int tx : { 60, 150, 360, 416 }) {
      gfx->fillTriangle(tx, HORIZON - 46, tx - 16, HORIZON, tx + 16, HORIZON, dk);
      gfx->fillTriangle(tx, HORIZON - 60, tx - 12, HORIZON - 28, tx + 12, HORIZON - 28, dk);
    }
  } else if (biome == 3) {  // volcan: rocas y brasas
    gfx->fillTriangle(70, HORIZON, 40, HORIZON + 30, 100, HORIZON + 30, dk);
    gfx->fillTriangle(400, HORIZON + 4, 372, HORIZON + 30, 430, HORIZON + 30, dk);
    if (!night)
      for (int e = 0; e < 4; e++)
        gfx->fillRect(120 + e * 70, HORIZON + 8 + (e % 2) * 6, 4, 4, C565(0xff, 0x9b, 0x3a));
  } else if (biome == 4) {  // montana: cumbres al fondo
    gfx->fillTriangle(140, HORIZON - 50, 60, HORIZON, 220, HORIZON, dk);
    gfx->fillTriangle(330, HORIZON - 38, 250, HORIZON, 410, HORIZON, dk);
  } else if (biome == 5 && !night) {  // nieve: copos cayendo
    for (int f = 0; f < 10; f++) {
      int fx = (f * 53 + now / 40) % 466;
      int fy = (f * 90 + now / 18) % HORIZON;
      gfx->fillRect(fx, fy, 3, 3, UI_WHITE);
    }
  } else if (biome == 0) {  // pradera: matas de hierba
    for (int gx : { 80, 175, 300, 395 })
      for (int b = -1; b <= 1; b++)
        gfx->fillRect(gx + b * 5, HORIZON + 6, 2, 8 + (b == 0 ? 4 : 0), dk);
  }
}

// primera partida: elige inicial entre Bulbasaur / Charmander / Squirtle
void renderStarterRegionSelect() {
  if (!starterDirty) { perfRenderSkipCount++; return; }
  starterDirty = false;
  gfx->fillScreen(UI_BG_DAY);
  const char *t = T(S_CHOOSE_REGION);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(t) * 6, 68);
  gfx->print(t);
  for (uint8_t i = 0; i < 3; i++) {
    int ry = STARTER_ROW_Y + i * (STARTER_ROW_H + STARTER_ROW_GAP);
    gfx->fillRoundRect(70, ry, 326, STARTER_ROW_H, 14, lerp565(UI_BAR_OK, UI_WHITE, 6, 8));
    gfx->drawRoundRect(70, ry, 326, STARTER_ROW_H, 14, UI_BAR_OK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(3);
    const char *label = T(STARTER_REGION_LABELS[i]);
    gfx->setCursor(CX - strlen(label) * 9, ry + 24);
    gfx->print(label);
  }
  flushStampedFrame();
}

void renderStarterSelect() {
  if (!starterDirty) { perfRenderSkipCount++; return; }
  starterDirty = false;
  gfx->fillScreen(UI_BG_DAY);
  const char *t = T(S_CHOOSE_STARTER);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(t) * 6, 68);
  gfx->print(t);
  for (int i = 0; i < 3; i++) {
    int16_t d = STARTER_DEX[starterRegion][i];
    const DexEntry &de = DEX_TBL[d];
    int ry = STARTER_ROW_Y + i * (STARTER_ROW_H + STARTER_ROW_GAP);
    gfx->fillRoundRect(70, ry, 326, STARTER_ROW_H, 14, lerp565(de.accent, UI_WHITE, 6, 8));
    gfx->drawRoundRect(70, ry, 326, STARTER_ROW_H, 14, de.accent);
    const uint8_t *th = thumbs.get(d);     // miniatura del inicial (si la SD esta lista)
    if (th) drawThumb(th, 76, ry - 5, 3, false);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(3);
    gfx->setCursor(178, ry + 24);
    gfx->print(dexName(d));
  }
  flushStampedFrame();
}

void render() {
  if (staticScreenClean()) { perfRenderSkipCount++; return; }
  if (pet.awaitingStarter()) {  // primera partida: region y starter (prioridad total)
    if (starterRegion == 0xFF) renderStarterRegionSelect();
    else renderStarterSelect();
    return;
  }
  if (galleryOpen) {
    renderGallery();
    return;
  }
  if (gameOpen) {
    renderGame();
    return;
  }
  if (sackOpen) {
    renderSack();
    return;
  }
  if (battleOpen) {
    renderBattle();
    return;
  }
  if (kbOpen) {
    renderKeyboard();
    return;
  }
  if (helpOpen) {
    renderHelp();
    return;
  }
  if (netOpen) {
    renderNet();
    return;
  }
  if (clockOpen) {
    renderClock();
    return;
  }
  if (cardOpen) {
    renderCard();
    return;
  }
  int h = sceneHour();
  gNight = isVisualNight(h, pet.sleeping);
  // drawScene cubre los 466x466 completos: sin fillScreen(NEGRO) previo para
  // que un flush DMA solapado nunca capture negro a medias (anti-parpadeo)
  drawScene(pet.isEgg() ? 0 : DEX_TBL[pet.speciesId].biome, millis(), gNight);

  if (pet.ceremony) {
    const DexEntry &d = DEX_TBL[pet.speciesId];
    const char *msg = (pet.ceremony == CER_FAREWELL) ? T(S_FAREWELL)
                      : (pet.ceremony == CER_RUNAWAY) ? T(S_RUNAWAY)
                                                      : T(S_GOODBYE);
    drawHeader(dexName(pet.speciesId), d.accent, msg);
    drawCeremony();
    flushStampedFrame();
    return;
  }

  if (pet.isEgg()) {
    drawHeader(T(S_EGG_HDR), inkColor(), eggMsg());
    int s = 5, x = CX - 16 * s, y = PET_CY - 16 * s;
    if (deadlineActive(millis(), eggWiggleUntil))
      x += ((millis() / 40) % 2) ? 6 : -6;
    drawMap(SPR_EGG, SPRITE_H, x, y, s, false);
    if (pet.eggCracks() >= 1)
      for (auto &c : CRACK1) gfx->fillRect(x + c[0] * s, y + c[1] * s, s, s, INK_K);
    if (pet.eggCracks() >= 2)
      for (auto &c : CRACK2) gfx->fillRect(x + c[0] * s, y + c[1] * s, s, s, INK_K);
    if (pet.eggRarity() >= R_RARO) {
      const char *rar = (pet.eggRarity() == R_LEGENDARIO) ? T(S_EGG_LEGEND) : T(S_EGG_RARE);
      gfx->setTextColor(pet.eggRarity() == R_LEGENDARIO ? UI_BAR_WARN : 0x4C98);
      gfx->setTextSize(2);
      gfx->setCursor(CX - strlen(rar) * 6, 316);
      gfx->print(rar);
    }
    char reg[24];
    snprintf(reg, sizeof(reg), T(S_POKEDEX_FMT), pet.registeredCount(), DEX_COUNT);
    gfx->fillRect(0, 312, 466, 154, gNight ? UI_BG_NIGHT : UI_BG_DAY);
    gfx->setTextColor(inkColor());
    gfx->setTextSize(2);
    gfx->setCursor(CX - strlen(reg) * 6, 348);
    gfx->print(reg);
    drawStepsHud();
  } else {
    const DexEntry &d = DEX_TBL[pet.speciesId];
    char name[28];
    const char *base = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
    snprintf(name, sizeof(name), T(S_NAME_FMT), pet.shiny ? "*" : "", base, pet.level());
    drawHeader(name, gNight ? UI_INK_NIGHT : d.accent, statusMsg());
    drawStepsHud();
    drawPet();
    drawBath();
    drawPoops();
    drawExpeditionHud();
    drawBars();
    drawButtons();
    drawCelebration();
    drawPetEvent();
    if (gameMenuOpen) drawGameMenu();
    if (pet.wantEvolveButton()) drawEvolveButton();        // CTA rojo: evolucionar
    else if (pet.canRunawayNow()) drawRunawayButton();     // CTA sombrio: escapada (abandono)
    else if (pet.wantFarewellButton()) drawFarewellButton();  // CTA dorado: despedida
  }

  if (pet.sleeping) {
    gfx->setTextColor(UI_INK_NIGHT);
    gfx->setTextSize(3);
    gfx->setCursor(320, 130);
    gfx->print("Zz");
  }

  if (morningUntil) {
    if (deadlineReached(millis(), morningUntil)) morningUntil = 0;
    else {
      const char *hello = T(S_GOOD_MORNING);
      gfx->fillRoundRect(90, 78, 286, 44, 14, UI_WHITE);
      gfx->drawRoundRect(90, 78, 286, 44, 14, inkColor());
      gfx->setTextColor(inkColor());
      gfx->setTextSize(2);
      gfx->setCursor(CX - strlen(hello) * 6, 92);
      gfx->print(hello);
    }
  }

  if (wildPromptUntil) {
    if (deadlineReached(millis(), wildPromptUntil)) wildPromptUntil = 0;
    else drawWildPrompt();
  }

  // selector de comida
  if (feedMenuUntil) {
    if (deadlineReached(millis(), feedMenuUntil)) {
      feedMenuUntil = 0;
    } else {
      gfx->fillRoundRect(101, FEED_MENU_Y, 264, 64, 14, UI_WHITE);
      gfx->drawRoundRect(101, FEED_MENU_Y, 264, 64, 14, inkColor());
      drawMap(SPR_ICON_FOOD, 16, 110, FEED_MENU_Y + 8, 3, false);
      drawMap(SPR_ICON_BERRY_B, 16, 176, FEED_MENU_Y + 8, 3, false);
      drawMap(SPR_ICON_BERRY_G, 16, 242, FEED_MENU_Y + 8, 3, false);
      drawMap(SPR_ICON_CANDY, 16, 308, FEED_MENU_Y + 8, 3, false);
    }
  }

  // dialogo "soltar?" (pulsacion larga sobre el bicho)
  if (confirmUntil) {
    if (deadlineReached(millis(), confirmUntil)) {
      confirmUntil = 0;
    } else {
      gfx->fillRoundRect(94, 168, 278, 152, 16, UI_WHITE);
      gfx->drawRoundRect(94, 168, 278, 152, 16, UI_INK);
      char q[28];
      snprintf(q, sizeof(q), T(S_RELEASE_FMT), dexName(pet.speciesId));
      gfx->setTextColor(UI_INK);
      gfx->setTextSize(2);
      gfx->setCursor(CX - strlen(q) * 6, 196);
      gfx->print(q);
      gfx->fillRoundRect(118, 252, 100, 52, 12, UI_BAR_OK);
      gfx->setTextColor(UI_WHITE);
      gfx->setCursor(118 + (100 - (int)strlen(T(S_YES)) * 12) / 2, 270);
      gfx->print(T(S_YES));
      gfx->fillRoundRect(248, 252, 100, 52, 12, UI_BAR_BAD);
      gfx->setCursor(248 + (100 - (int)strlen(T(S_NO)) * 12) / 2, 270);
      gfx->print(T(S_NO));
    }
  }

  // dialogo de decision (evolucionar/mantener, despedirse/quedaros)
  if (choiceKind) {
    if (deadlineReached(millis(), choiceUntil)) choiceKind = 0;
    else drawChoiceDialog();
  }

  flushStampedFrame();
}

// ---------- minijuego: toques con la pokeball ----------

void drawGameMenu() {
  gameMenuDirty = false;
  gfx->fillRoundRect(78, 112, 310, 266, 18, UI_WHITE);
  gfx->drawRoundRect(78, 112, 310, 266, 18, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  const char *title = T(S_PLAY);
  gfx->setCursor(CX - strlen(title) * 9, 124);
  gfx->print(title);
  const char *labels[GAME_MENU_COUNT] = { T(S_GAME_BALL), T(S_GAME_CATCH), T(S_GAME_MEMO), T(S_GAME_CLEAN), T(S_GAME_TYPE), T(S_GAME_TILT) };
  uint16_t cols[GAME_MENU_COUNT] = { UI_BAR_BAD, UI_BAR_WARN, 0x4C98, UI_BAR_OK, 0xF3B7, 0x7A3F };
  for (int i = 0; i < GAME_MENU_COUNT; i++) {
    const GameMenuTile &t = GAME_MENU_TILES[i];
    gfx->fillRoundRect(t.x, t.y, t.w, t.h, 14, cols[i]);
    gfx->drawRoundRect(t.x, t.y, t.w, t.h, 14, UI_INK);
    gfx->setTextColor(i == 1 ? UI_INK : UI_BG_DAY);
    gfx->setTextSize(2);
    gfx->setCursor(t.x + (t.w - (int)strlen(labels[i]) * 12) / 2, t.y + (t.h - 16) / 2);
    gfx->print(labels[i]);
  }
}

void startGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  gameMenuOpen = false;
  gameOpen = true;
  gameMode = 0;
  gameOverUntil = 0;
  gameScore = 0;
  gameMisses = 0;
  gameNewHi = false;
  gameGain = 0;
  hitTime = 0;
  ballLastHitAt = 0;
  gamePetX = 233;
  respawnBall();
  sfxPlay(SFX_GAME_START);
}

void spawnCatchTarget() {
  catchX = 86 + random(294);
  catchY = 118 + random(206);
  // desde 4 puntos a veces sale una trampa: tocarla quita una vida, dejarla pasar no
  catchIcon = (gameScore >= 4 && random(100) < 25) ? CATCH_TRAP : random(3);
  uint32_t life = 980;
  uint32_t speedup = (uint32_t)gameScore * 35;
  if (speedup > 530) speedup = 530;
  life -= speedup;
  catchTargetUntil = millis() + life;
}

void startCatchGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  gameMenuOpen = false;
  gameOpen = true;
  gameMode = 1;
  gameOverUntil = 0;
  gameScore = 0;
  gameMisses = 0;
  gameNewHi = false;
  gameGain = 0;
  catchUntil = millis() + 20000;
  spawnCatchTarget();
  sfxPlay(SFX_GAME_START);
}

void startMemoRound() {
  if (memoLen < 14) {  // nunca el mismo color dos veces seguidas
    uint8_t c;
    do { c = random(4); } while (memoLen > 0 && c == memoSeq[memoLen - 1]);
    memoSeq[memoLen++] = c;
  }
  memoShow = 0;
  memoInput = 0;
  memoActivePad = -1;
  memoHintPad = -1;
  memoShowing = true;
  // pausa clara antes de cada secuencia: todos los botones se iluminan ("se repite")
  memoNextAt = millis() + (memoLen > 1 ? 1300 : 700);
  memoReadyUntil = memoNextAt;
  memoTurnUntil = 0;
}

void startMemoGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  gameMenuOpen = false;
  gameOpen = true;
  gameMode = 2;
  gameOverUntil = 0;
  gameScore = 0;
  gameNewHi = false;
  gameGain = 0;
  memoLen = 0;
  memoRounds = 0;
  memoFlashPad = -1;
  memoHintPad = -1;
  memoFlashUntil = memoFailUntil = memoTurnUntil = 0;
  startMemoRound();
  sfxPlay(SFX_GAME_START);
}

void spawnCleanSpot() {
  for (uint8_t i = 0; i < 4; i++) {
    if (cleanAlive[i]) continue;
    cleanX[i] = 88 + random(290);
    cleanY[i] = 122 + random(224);
    cleanAlive[i] = true;
    int16_t dirt = 180 + gameScore * 8;
    cleanDirt[i] = cleanDirt0[i] = dirt > 360 ? 360 : dirt;
    cleanBorn[i] = millis();
    cleanActive++;
    return;
  }
}

void startCleanGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  gameMenuOpen = false;
  gameOpen = true;
  gameMode = 3;
  gameOverUntil = 0;
  gameScore = 0;
  gameMisses = 0;
  gameNewHi = false;
  gameGain = 0;
  cleanActive = 0;
  for (uint8_t i = 0; i < 4; i++) cleanAlive[i] = false;
  cleanUntil = millis() + CLEAN_GAME_MS;
  cleanSpawnAt = millis();
  spawnCleanSpot();
  sfxPlay(SFX_GAME_START);
}

// tipos de ataque de la Gen 3 (sin hada)
static const uint8_t TYPE_ATK_POOL[] = {
  TYPE_NORMAL, TYPE_FIRE, TYPE_WATER, TYPE_ELECTRIC, TYPE_GRASS, TYPE_ICE,
  TYPE_FIGHTING, TYPE_POISON, TYPE_GROUND, TYPE_FLYING, TYPE_PSYCHIC, TYPE_BUG,
  TYPE_ROCK, TYPE_GHOST, TYPE_DRAGON, TYPE_DARK, TYPE_STEEL
};
#define TYPE_ATK_COUNT ((int)sizeof(TYPE_ATK_POOL))

// rival: un Pokemon de verdad (uno o dos tipos). Respuesta correcta: un tipo
// superefectivo; las otras dos al principio resistidas, despues tambien neutras.
void nextTypeQuestion() {
  uint8_t good[TYPE_ATK_COUNT], ng = 0;
  for (int tries = 0; tries < 60 && ng == 0; tries++) {
    typeEnemyDex = 1 + random(DEX_COUNT);
    typeEnemy = DEX_TBL[typeEnemyDex].type1;
    typeEnemy2 = DEX_TBL[typeEnemyDex].type2;
    ng = 0;
    for (int i = 0; i < TYPE_ATK_COUNT; i++)
      if (battleTypeEffectPct(TYPE_ATK_POOL[i], typeEnemy, typeEnemy2) > 100) good[ng++] = TYPE_ATK_POOL[i];
  }
  uint8_t correct = ng ? good[random(ng)] : TYPE_FIGHTING;
  uint8_t worstAllowed = gameScore < 8 ? 99 : 100;  // tabla del juego: 120 super, 100 neutro, 85 resistido
  uint8_t bad[TYPE_ATK_COUNT], nb = 0;
  for (int i = 0; i < TYPE_ATK_COUNT; i++)
    if (battleTypeEffectPct(TYPE_ATK_POOL[i], typeEnemy, typeEnemy2) <= worstAllowed) bad[nb++] = TYPE_ATK_POOL[i];
  if (nb < 2) {  // pocos resistidos: tambien valen los neutros
    nb = 0;
    for (int i = 0; i < TYPE_ATK_COUNT; i++)
      if (battleTypeEffectPct(TYPE_ATK_POOL[i], typeEnemy, typeEnemy2) <= 100) bad[nb++] = TYPE_ATK_POOL[i];
  }
  typeCorrect = (uint8_t)random(3);
  for (uint8_t i = 0; i < 3; i++) typeChoice[i] = TYPE_NONE;
  typeChoice[typeCorrect] = correct;
  for (uint8_t i = 0; i < 3; i++) {
    if (i == typeCorrect) continue;
    uint8_t cand;
    int guard = 0;
    do {
      cand = bad[random(nb)];
    } while ((cand == typeChoice[0] || cand == typeChoice[1] || cand == typeChoice[2]) && ++guard < 40);
    typeChoice[i] = cand;
  }
  // menos tiempo cuantos mas aciertos (5,2 s -> 3 s)
  int ms = 5200 - gameScore * 110;
  typeDurationMs = ms < 3000 ? 3000 : ms;
  typeUntil = millis() + typeDurationMs;
}

void startTypeGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  gameMenuOpen = false;
  gameOpen = true;
  gameMode = 4;
  gameOverUntil = 0;
  gameScore = 0;
  gameMisses = 0;
  gameNewHi = false;
  gameGain = 0;
  nextTypeQuestion();
  sfxPlay(SFX_GAME_START);
}

void respawnBall() {
  ballX = 112 + random(242);
  ballY = 82;
  float sp = 3.35f + gameScore * 0.14f;
  if (sp > 8.4f) sp = 8.4f;
  ballVX = random(2) ? sp : -sp;
  ballVX += ((int)random(9) - 4) * 0.28f;
  ballVY = 2.05f;
}

// zona de toque: se reduce con los puntos (50 -> 34 px)
int ballHitRadius() {
  int r = 50 - (gameScore * 16) / 30;
  return r < 34 ? 34 : r;
}

// gravedad: crece con los puntos, asi la partida acelera de verdad
float ballGravity() {
  float g = 1.10f + gameScore * 0.045f;
  return g > 3.2f ? 3.2f : g;
}

void gameTap(int16_t x, int16_t y) {
  if (gameMode == 1) {
    catchTap(x, y);
    return;
  }
  if (gameMode == 2) {
    memoTap(x, y);
    return;
  }
  if (gameMode == 3) {
    cleanTap(x, y);
    return;
  }
  if (gameMode == 4) {
    typeTap(x, y);
    return;
  }
  if (gameMode == 5) return;  // INCLINA se juega solo inclinando
  if (gameOverUntil) return;
  float dx = ballX - x, dy = ballY - y;
  int hitRadius = ballHitRadius();
  if (dx * dx + dy * dy < hitRadius * hitRadius) {  // toque a la bola!
    uint32_t now = millis();
    if (now - ballLastHitAt < 260 || ballVY < -0.25f) return;
    gameScore++;
    sfxPlay(SFX_MINIGAME_OK);
    // el toque la manda a una altura al azar dentro de la pantalla, nunca por
    // encima de donde empezo: la velocidad sale de la gravedad actual
    float apex = 96.0f + random(110);
    float rise = ballY - apex;
    if (rise < 40.0f) rise = 40.0f;
    ballVY = -sqrtf(2.0f * ballGravity() * rise);
    float drift = 0.38f + (gameScore >= 6 ? 0.08f : 0.0f) + (gameScore >= 14 ? 0.10f : 0.0f);
    float chaos = ((int)random(17) - 8) * 0.26f;
    ballVX += dx * drift + chaos;
    if (random(100) < 28) ballVX = -ballVX * (0.72f + random(45) * 0.01f);
    float vxMax = 6.0f + gameScore * 0.12f;  // mas rapida de lado con los puntos
    if (vxMax > 11.0f) vxMax = 11.0f;
    if (ballVX > vxMax) ballVX = vxMax;
    if (ballVX < -vxMax) ballVX = -vxMax;
    hitX = ballX;
    hitY = ballY;
    hitTime = now;
    ballLastHitAt = now;
  }
}

void finishCatchGame() {
  gameNewHi = (gameScore > pet.catchHi);
  gameGain = pet.applyCatchResult(gameScore);
  sfxPlay(gameNewHi && gameScore > 0 ? SFX_MEDAL : SFX_LEVEL);
  gameOverUntil = millis() + 4000;
}

void catchTap(int16_t x, int16_t y) {
  if (gameOverUntil) return;
  int dx = x - catchX, dy = y - catchY;
  if (dx * dx + dy * dy <= 52 * 52) {
    if (catchIcon == CATCH_TRAP) {  // trampa: cuenta como fallo
      if (++gameMisses >= 3) { finishCatchGame(); return; }
      sfxPlay(SFX_MINIGAME_BAD);
      spawnCatchTarget();
      return;
    }
    gameScore++;
    hitX = catchX;
    hitY = catchY;
    hitTime = millis();
    sfxPlay(SFX_MINIGAME_OK);
    spawnCatchTarget();
  } else if (++gameMisses >= 3) {
    finishCatchGame();
  } else {
    sfxPlay(SFX_MINIGAME_BAD);
  }
}

void finishMemoGame() {
  gameScore = memoRounds;
  gameNewHi = (memoRounds > pet.memoHi);
  gameGain = pet.applyMemoResult(memoRounds);
  sfxPlay(gameNewHi && memoRounds > 0 ? SFX_MEDAL : SFX_LEVEL);
  gameOverUntil = millis() + 4000;
}

int memoPadAt(int16_t x, int16_t y) {
  const int16_t px[4] = { 142, 324, 142, 324 };
  const int16_t py[4] = { 164, 164, 318, 318 };
  for (int i = 0; i < 4; i++) {
    int dx = x - px[i], dy = y - py[i];
    if (dx * dx + dy * dy <= 54 * 54) return i;
  }
  return -1;
}

void memoPadSound(uint8_t pad) {
  if (pad < 4) sfxPlay((uint8_t)(SFX_MEMO_PAD_0 + pad));
}

void memoTap(int16_t x, int16_t y) {
  if (gameOverUntil) return;
  if (memoShowing || memoFailUntil || deadlineActive(millis(), memoTurnUntil)) return;
  int pad = memoPadAt(x, y);
  if (pad < 0) return;
  if (pad != memoSeq[memoInput]) {
    memoFlashPad = pad;
    memoFlashGood = false;
    memoFlashUntil = millis() + 620;
    memoHintPad = memoSeq[memoInput];
    memoFailUntil = memoFlashUntil;
    sfxPlay(SFX_MINIGAME_BAD);
    return;
  }
  memoFlashPad = pad;
  memoFlashGood = true;
  memoFlashUntil = millis() + 180;
  memoPadSound((uint8_t)pad);
  memoInput++;
  if (memoInput >= memoLen) {
    memoRounds++;
    if (memoLen >= 14) finishMemoGame();
    else {
      sfxPlay(SFX_MINIGAME_OK);  // ronda superada
      startMemoRound();
    }
  }
}

void finishCleanGame() {
  gameNewHi = (gameScore > pet.cleanHi);
  gameGain = pet.applyCleanResult(gameScore);
  sfxPlay(gameNewHi && gameScore > 0 ? SFX_MEDAL : SFX_LEVEL);
  gameOverUntil = millis() + 4000;
}

// LIMPIA: las manchas no se quitan con un toque, hay que frotarlas con el dedo
void cleanTap(int16_t, int16_t) {}

void cleanRub(int16_t x, int16_t y) {
  if (gameOverUntil) return;
  if (!cleanRubHas) { cleanRubHas = true; cleanRubX = x; cleanRubY = y; return; }
  int dx = x - cleanRubX, dy = y - cleanRubY;
  int d = (int)sqrtf((float)(dx * dx + dy * dy));
  cleanRubX = x;
  cleanRubY = y;
  if (d <= 0) return;
  for (uint8_t i = 0; i < 4; i++) {
    if (!cleanAlive[i]) continue;
    int sx = x - cleanX[i], sy = y - cleanY[i];
    if (sx * sx + sy * sy > 46 * 46) continue;
    cleanDirt[i] -= d;
    if (cleanDirt[i] > 0) continue;
    cleanAlive[i] = false;
    if (cleanActive) cleanActive--;
    gameScore++;
    hitX = cleanX[i];
    hitY = cleanY[i];
    hitTime = millis();
    sfxPlay(SFX_MINIGAME_OK);
  }
}

void finishTypeGame() {
  gameNewHi = (gameScore > pet.typeHi);
  gameGain = pet.applyTypeResult(gameScore);
  sfxPlay(gameNewHi && gameScore > 0 ? SFX_MEDAL : SFX_LEVEL);
  gameOverUntil = millis() + 4000;
}

void typeTap(int16_t x, int16_t y) {
  if (gameOverUntil) return;
  int idx = -1;
  for (int i = 0; i < 3; i++) {
    int by = 210 + i * 60;
    if (x >= 70 && x <= 396 && y >= by - 8 && y <= by + 56) idx = i;
  }
  if (idx < 0) return;
  if ((uint8_t)idx == typeCorrect) {
    gameScore++;
    sfxPlay(SFX_MINIGAME_OK);
    nextTypeQuestion();
  } else {
    if (++gameMisses >= 3) finishTypeGame();
    else sfxPlay(SFX_MINIGAME_BAD);
  }
}

void stepGame() {
  float grav = ballGravity();
  ballVX += sinf((millis() + gameScore * 97) * 0.018f) * 0.11f;
  if (random(100) < 7) ballVX += ((int)random(7) - 3) * 0.22f;
  ballVY += grav;
  ballX += ballVX;
  ballY += ballVY;
  // rebote en la pared circular
  float dx = ballX - CX, dy = ballY - CY;
  float d = sqrtf(dx * dx + dy * dy);
  if (d > 205) {
    float nx = dx / d, ny = dy / d;
    float dot = ballVX * nx + ballVY * ny;
    if (dot > 0) {
      // la pared absorbe energia (antes la anadia y la bola podia subir sin fin)
      ballVX = (ballVX - 2 * dot * nx) * 0.9f;
      ballVY = (ballVY - 2 * dot * ny) * 0.85f;
      ballVX += ((int)random(9) - 4) * 0.18f;
      sfxPlay(SFX_BALL_BOUNCE);
    }
    ballX = CX + nx * 205;
    ballY = CY + ny * 205;
  }
  if (ballY < 82 && ballVY < 0) {  // techo suave: nunca por encima de la salida
    ballY = 82;
    ballVY = -ballVY * 0.4f;
  }
  if (ballY > 384) {  // al suelo
    if (++gameMisses >= 3) {
      gameNewHi = (gameScore > pet.gameHi);
      pet.playResult(gameScore);  // actualiza el record y da felicidad
      sfxPlay(gameNewHi && gameScore > 0 ? SFX_MEDAL : SFX_LEVEL);
      gameOverUntil = millis() + 4000;
    } else {
      respawnBall();
      sfxPlay(SFX_BALL_MISS);
    }
  }
  // el bicho la sigue por abajo
  float chase = (ballX - gamePetX) * 0.12f;
  if (chase > 7) chase = 7;
  if (chase < -7) chase = -7;
  gamePetX += chase;
}

// ---------- saco de entrenamiento (entrena la fuerza) ----------

void startSack() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  sackOpen = true;
  sackUntil = millis() + 10000;
  sackOverUntil = 0;
  sackHits = 0;
  sackShake = 0;
  sackNewHi = false;
  sfxPlay(SFX_GAME_START);
}

void sackTap() {
  if (deadlineReached(millis(), sackUntil)) return;  // ya termino el tiempo
  sackHits++;
  sackShake = 16;  // sacude el saco
  if ((sackHits & 1) == 1) sfxPlay(SFX_PLAY);
}

void drawGameScene();  // prototipo (definida mas abajo)
const char *battleTypeName(uint8_t type);
uint16_t battleTypeColor(uint8_t type);

void renderSack() {
  uint32_t now = millis();
  drawGameScene();  // fondo del habitat
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;

  // pantalla de resultado
  if (sackOverUntil) {
    if (deadlineReached(now, sackOverUntil)) { sackOpen = false; return; }
    char b[20];
    snprintf(b, sizeof(b), T(S_HITS_FMT), sackHits);
    gfx->setTextColor(ink);
    gfx->setTextSize(4);
    gfx->setCursor(CX - strlen(b) * 12, 150);
    gfx->print(b);
    char g[18];
    snprintf(g, sizeof(g), T(S_STR_GAIN_FMT), sackGain);
    gfx->setTextColor(UI_BAR_BAD);
    gfx->setTextSize(3);
    gfx->setCursor(CX - strlen(g) * 9, 210);
    gfx->print(g);
    gfx->setTextSize(2);
    if (sackNewHi && sackHits > 0) {
      gfx->setTextColor(UI_BAR_WARN);
      gfx->setCursor(CX - strlen(T(S_NEW_RECORD)) * 6, 256);
      gfx->print(T(S_NEW_RECORD));
    } else {
      char r[18];
      snprintf(r, sizeof(r), T(S_RECORD_FMT), pet.strHi);
      gfx->setTextColor(ink);
      gfx->setCursor(CX - strlen(r) * 6, 256);
      gfx->print(r);
    }
    flushStampedFrame();
    return;
  }

  // se acabaron los 10 s: aplicar entrenamiento
  if (deadlineReached(now, sackUntil)) {
    sackNewHi = (sackHits > pet.strHi);
    sackGain = pet.trainStrength(sackHits);
    sfxPlay(sackNewHi ? SFX_MEDAL : SFX_PLAY);
    sackOverUntil = now + 3500;
    flushStampedFrame();
    return;
  }

  // aporreo activo
  sackShake *= 0.84f;
  int off = (int)(sackShake * sinf(now * 0.05f));
  int sx = CX + off, top = 86, sy = 150;
  gfx->fillRect(CX - 3, 56, 6, top - 56, ink);          // gancho/cuerda
  gfx->fillRect(sx - 4, top - 30, 8, 34, ink);          // cadena
  gfx->fillRoundRect(sx - 42, top, 84, 150, 26, C565(0xb5, 0x3a, 0x3a));  // saco
  gfx->fillRoundRect(sx - 42, top, 84, 22, 18, C565(0x7e, 0x28, 0x28));   // tapa
  gfx->drawRoundRect(sx - 42, top, 84, 150, 26, ink);
  gfx->fillRect(sx - 42, top + 70, 84, 4, C565(0x7e, 0x28, 0x28));        // costura

  // contador de golpes
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", sackHits);
  gfx->setTextColor(ink);
  gfx->setTextSize(6);
  gfx->setCursor(CX - strlen(buf) * 18, 268);
  gfx->print(buf);

  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_HIT_FAST)) * 6, 322);
  gfx->print(T(S_HIT_FAST));

  // barra de tiempo
  uint32_t left = sackUntil - now;
  int bw = 280, fw = (int)((uint32_t)bw * left / 10000);
  gfx->fillRoundRect(CX - bw / 2, 350, bw, 16, 5, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(CX - bw / 2, 350, fw, 16, 5, UI_BAR_OK);

  flushStampedFrame();
}

// fondo del minijuego: hatibat del bicho (cielo por hora + suelo del bioma)
void drawGameScene() {
  int hh = sceneHour();
  bool night = hh < 6 || hh >= 20;
  uint16_t top, bot;
  if (night)       { top = C565(0x0c, 0x12, 0x24); bot = C565(0x1e, 0x26, 0x46); }
  else if (hh < 8) { top = C565(0xd1, 0x6a, 0x86); bot = C565(0xf3, 0xb8, 0x7c); }
  else if (hh < 18){ top = C565(0x8f, 0xc8, 0xea); bot = C565(0xdc, 0xee, 0xe6); }
  else             { top = C565(0xc7, 0x5a, 0x4a); bot = C565(0xf0, 0xae, 0x64); }
  int hor = 376;
  for (int y = 0; y < hor; y += 8)
    gfx->fillRect(0, y, 466, 8, lerp565(top, bot, y, hor));
  if (night)
    for (auto &st : STARS) gfx->fillRect(st[0], st[1], 4, 4, UI_WHITE);
  uint8_t bio = pet.isEgg() ? 0 : DEX_TBL[pet.speciesId].biome;
  uint16_t soil = BIOME_SOIL[bio < 6 ? bio : 0];
  if (night) soil = lerp565(soil, C565(0x16, 0x1c, 0x30), 9, 16);
  gfx->fillRect(0, hor, 466, 466 - hor, soil);
}

void drawGameResult(const char *recordFmt, uint16_t record, StrId gainFmt) {
  drawGameScene();
  if (deadlineReached(millis(), gameOverUntil)) {
    gameOpen = false;
    return;
  }
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  char buf[22];
  snprintf(buf, sizeof(buf), T(S_SCORE_FMT), gameScore);
  gfx->setTextColor(ink);
  gfx->setTextSize(4);
  gfx->setCursor(CX - strlen(buf) * 12, 148);
  gfx->print(buf);
  char gain[18];
  snprintf(gain, sizeof(gain), T(gainFmt), gameGain);
  gfx->setTextColor(gainFmt == S_DEF_GAIN_FMT ? 0x4C98 : (gainFmt == S_HYG_GAIN_FMT ? UI_BAR_OK : UI_BAR_WARN));
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(gain) * 9, 204);
  gfx->print(gain);
  gfx->setTextSize(2);
  if (gameNewHi && gameScore > 0) {
    gfx->setTextColor(UI_BAR_WARN);
    gfx->setCursor(CX - strlen(T(S_NEW_RECORD)) * 6, 256);
    gfx->print(T(S_NEW_RECORD));
  } else {
    char rec[20];
    snprintf(rec, sizeof(rec), recordFmt, record);
    gfx->setTextColor(ink);
    gfx->setCursor(CX - strlen(rec) * 6, 256);
    gfx->print(rec);
  }
  flushStampedFrame();
}

void renderCatchGame() {
  uint32_t now = millis();
  if (gameOverUntil) {
    drawGameResult(T(S_RECORD_FMT), pet.catchHi, S_SPD_GAIN_FMT);
    return;
  }
  if (deadlineReached(now, catchUntil) || gameMisses >= 3) {
    finishCatchGame();
    return;
  }
  if (deadlineReached(now, catchTargetUntil)) {
    // dejar pasar una trampa esta bien; dejar escapar una baya es un fallo
    if (catchIcon != CATCH_TRAP && ++gameMisses >= 3) {
      finishCatchGame();
      return;
    }
    spawnCatchTarget();
  }
  drawGameScene();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  gfx->setTextColor(ink);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_CATCH_TITLE)) * 9, 32);
  gfx->print(T(S_CATCH_TITLE));
  char score[16], rec[16];
  snprintf(score, sizeof(score), T(S_SCORE_FMT), gameScore);
  snprintf(rec, sizeof(rec), T(S_REC_FMT), pet.catchHi);
  gfx->setTextSize(2);
  gfx->setCursor(74, 78);
  gfx->print(score);
  gfx->setCursor(392 - (int)strlen(rec) * 12, 78);
  gfx->print(rec);
  for (int i = 0; i < 3; i++) {
    if (i < 3 - gameMisses) gfx->fillCircle(180 + i * 28, 104, 6, UI_BAR_BAD);
    else gfx->drawCircle(180 + i * 28, 104, 6, UI_TRACK);
  }
  if (catchIcon == CATCH_TRAP) {  // trampa: bola oscura con aspa roja
    gfx->fillCircle(catchX, catchY, 34, UI_INK);
    gfx->drawCircle(catchX, catchY, 36, UI_BAR_BAD);
    for (int k = -2; k <= 2; k++) {
      gfx->drawLine(catchX - 16 + k, catchY - 16, catchX + 16 + k, catchY + 16, UI_BAR_BAD);
      gfx->drawLine(catchX + 16 + k, catchY - 16, catchX - 16 + k, catchY + 16, UI_BAR_BAD);
    }
  } else {
    const char *const *icon = catchIcon == 0 ? SPR_ICON_FOOD : (catchIcon == 1 ? SPR_ICON_BERRY_B : SPR_ICON_BERRY_G);
    gfx->fillCircle(catchX, catchY, 34, UI_WHITE);
    gfx->drawCircle(catchX, catchY, 36, UI_BAR_WARN);
    drawMap(icon, 16, catchX - 24, catchY - 24, 3, false);
  }
  int bw = 280;
  if (gameScore >= 3 && gameScore <= 6) {  // aviso justo antes de las primeras trampas
    const char *h = T(S_CATCH_HINT);
    gfx->setTextColor(UI_BAR_BAD);
    gfx->setTextSize(2);
    gfx->setCursor(CX - (int)strlen(h) * 6, 336);
    gfx->print(h);
  }
  int fw = (int)((uint32_t)bw * (catchUntil - now) / 20000);
  if (fw < 0) fw = 0;
  gfx->fillRoundRect(CX - bw / 2, 362, bw, 16, 5, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(CX - bw / 2, 362, fw, 16, 5, UI_BAR_OK);
  uint32_t ht = millis() - hitTime;
  if (hitTime && ht < 220) gfx->drawCircle((int)hitX, (int)hitY, 42 + ht / 8, UI_BAR_WARN);
  flushStampedFrame();
}

void stepMemoGame() {
  uint32_t now = millis();
  if (memoFailUntil) {
    if (deadlineReached(now, memoFailUntil)) {
      memoFailUntil = 0;
      memoHintPad = -1;
      finishMemoGame();
    }
    return;
  }
  if (!memoShowing || deadlineActive(now, memoNextAt)) return;
  if (memoActivePad >= 0) {
    memoActivePad = -1;
    memoShow++;
    if (memoShow >= memoLen) {
      memoShowing = false;
      memoInput = 0;
      memoTurnUntil = now + 520;
      sfxPlay(SFX_MENU);  // te toca
    } else {
      memoNextAt = now + 150;
    }
    return;
  }
  memoActivePad = memoSeq[memoShow];
  memoPadSound((uint8_t)memoActivePad);
  memoNextAt = now + 480;
}

void renderMemoGame() {
  if (gameOverUntil) {
    drawGameResult(T(S_RECORD_FMT), pet.memoHi, S_DEF_GAIN_FMT);
    return;
  }
  stepMemoGame();
  drawGameScene();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  char roundBuf[18], rec[16];
  snprintf(roundBuf, sizeof(roundBuf), T(S_ROUND_FMT), memoRounds + 1);
  snprintf(rec, sizeof(rec), T(S_REC_FMT), pet.memoHi);
  gfx->setTextColor(ink);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_GAME_MEMO)) * 9, 34);
  gfx->print(T(S_GAME_MEMO));
  gfx->setTextSize(2);
  gfx->setCursor(60, 82);
  gfx->print(roundBuf);
  gfx->setCursor(310, 82);
  gfx->print(rec);
  const int16_t px[4] = { 142, 324, 142, 324 };
  const int16_t py[4] = { 164, 164, 318, 318 };
  const uint16_t col[4] = { UI_BAR_BAD, UI_BAR_WARN, 0x4C98, UI_BAR_OK };
  int active = memoShowing ? memoActivePad : (memoFailUntil ? memoHintPad : -1);
  bool ready = memoShowing && memoShow == 0 && memoActivePad < 0 && deadlineActive(millis(), memoReadyUntil);
  for (int i = 0; i < 4; i++) {
    uint16_t fill = (i == active || (ready && (millis() / 250) % 2)) ? lerp565(col[i], UI_WHITE, 5, 8) : col[i];
    gfx->fillCircle(px[i], py[i], 48, fill);
    gfx->drawCircle(px[i], py[i], 52, ink);
    if (i == active) {
      int pulse = 56 + (int)((millis() / 70) % 5);
      gfx->drawCircle(px[i], py[i], pulse, col[i]);
    }
    if (i == memoFlashPad && deadlineActive(millis(), memoFlashUntil)) {
      gfx->drawCircle(px[i], py[i], 60, memoFlashGood ? UI_BAR_OK : UI_BAR_BAD);
      gfx->drawCircle(px[i], py[i], 64, memoFlashGood ? UI_BAR_OK : UI_BAR_BAD);
    }
  }
  char phase[28];
  if (memoFailUntil) snprintf(phase, sizeof(phase), "%s", T(S_MEMO_WRONG));
  else if (memoShowing) snprintf(phase, sizeof(phase), "%s", ready ? T(S_MEMO_READY) : T(S_MEMO_WATCH));
  else snprintf(phase, sizeof(phase), T(S_MEMO_TURN_FMT), memoInput + 1, memoLen);
  // Freier Streifen zwischen den oberen und unteren Pads: dadurch bleibt die
  // Anleitung auf dem runden Display klar lesbar und liegt nicht auf Farben.
  gfx->fillRoundRect(78, 230, 310, 24, 7, night ? UI_BG_NIGHT : UI_BG_DAY);
  gfx->drawRoundRect(78, 230, 310, 24, 7, ink);
  gfx->setTextColor(memoFailUntil ? UI_BAR_BAD : (memoShowing ? UI_BAR_WARN : UI_BAR_OK));
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(phase) * 6, 234);
  gfx->print(phase);
  flushStampedFrame();
}

void renderCleanGame() {
  uint32_t now = millis();
  if (gameOverUntil) {
    drawGameResult(T(S_RECORD_FMT), pet.cleanHi, S_HYG_GAIN_FMT);
    return;
  }
  if (deadlineReached(now, cleanUntil) || gameMisses >= 3) {
    finishCleanGame();
    return;
  }
  // una mancha que se queda demasiado se extiende: fallo
  uint32_t life = 7500 - (uint32_t)(gameScore > 20 ? 20 : gameScore) * 150;
  for (uint8_t i = 0; i < 4; i++) {
    if (!cleanAlive[i] || now - cleanBorn[i] < life) continue;
    cleanAlive[i] = false;
    if (cleanActive) cleanActive--;
    if (++gameMisses >= 3) { finishCleanGame(); return; }
    sfxPlay(SFX_MINIGAME_BAD);
  }
  if (deadlineReached(now, cleanSpawnAt)) {
    if (cleanActive < 3) spawnCleanSpot();
    cleanSpawnAt = now + 1600 - (gameScore > 12 ? 600 : gameScore * 50);
  }
  drawGameScene();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  gfx->setTextColor(ink);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_CLEAN_TITLE)) * 9, 32);
  gfx->print(T(S_CLEAN_TITLE));
  char score[16], rec[16];
  snprintf(score, sizeof(score), T(S_SCORE_FMT), gameScore);
  snprintf(rec, sizeof(rec), T(S_REC_FMT), pet.cleanHi);
  gfx->setTextSize(2);
  gfx->setCursor(74, 78);
  gfx->print(score);
  gfx->setCursor(392 - (int)strlen(rec) * 12, 78);
  gfx->print(rec);
  for (int i = 0; i < 3; i++) {
    if (i < 3 - gameMisses) gfx->fillCircle(180 + i * 28, 104, 6, UI_BAR_BAD);
    else gfx->drawCircle(180 + i * 28, 104, 6, UI_TRACK);
  }
  for (uint8_t i = 0; i < 4; i++) {
    if (!cleanAlive[i]) continue;
    // la mancha encoge mientras se frota y se pone roja cuando esta a punto de extenderse
    int r = 12 + (int)(26L * cleanDirt[i] / (cleanDirt0[i] ? cleanDirt0[i] : 1));
    bool late = now - cleanBorn[i] > life * 2 / 3;
    gfx->fillCircle(cleanX[i], cleanY[i], r, C565(0x8a, 0x66, 0x45));
    gfx->drawCircle(cleanX[i], cleanY[i], r + 2, late ? UI_BAR_BAD : UI_INK);
    if (late) gfx->drawCircle(cleanX[i], cleanY[i], r + 4, UI_BAR_BAD);
    gfx->fillCircle(cleanX[i] - r / 3, cleanY[i] - r / 3, r / 5 + 1, C565(0x62, 0x45, 0x2e));
    gfx->fillCircle(cleanX[i] + r / 3, cleanY[i] + r / 6, r / 4 + 1, C565(0x62, 0x45, 0x2e));
  }
  int bw = 280;
  if (now + CLEAN_GAME_MS - cleanUntil < 3500) {  // instruccion al empezar
    const char *h = T(S_CLEAN_HINT);
    gfx->setTextColor(ink);
    gfx->setTextSize(2);
    gfx->setCursor(CX - (int)strlen(h) * 6, 330);
    gfx->print(h);
  }
  int fw = (int)((uint32_t)bw * (cleanUntil - now) / CLEAN_GAME_MS);
  if (fw < 0) fw = 0;
  gfx->fillRoundRect(CX - bw / 2, 362, bw, 16, 5, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(CX - bw / 2, 362, fw, 16, 5, UI_BAR_OK);
  uint32_t ht = millis() - hitTime;
  if (hitTime && ht < 220) gfx->drawCircle((int)hitX, (int)hitY, 42 + ht / 8, UI_BAR_OK);
  flushStampedFrame();
}

void renderTypeGame() {
  uint32_t now = millis();
  if (gameOverUntil) {
    drawGameResult(T(S_RECORD_FMT), pet.typeHi, S_ATK_GAIN_FMT);
    return;
  }
  if (deadlineReached(now, typeUntil)) {
    if (++gameMisses >= 3) {
      finishTypeGame();
      return;
    }
    sfxPlay(SFX_MINIGAME_BAD);
    nextTypeQuestion();
  }
  drawGameScene();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  gfx->setTextColor(ink);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_TYPE_TITLE)) * 9, 32);
  gfx->print(T(S_TYPE_TITLE));
  char score[16], rec[16];
  snprintf(score, sizeof(score), T(S_SCORE_FMT), gameScore);
  snprintf(rec, sizeof(rec), T(S_REC_FMT), pet.typeHi);
  gfx->setTextSize(2);
  gfx->setCursor(74, 78);
  gfx->print(score);
  gfx->setCursor(392 - (int)strlen(rec) * 12, 78);
  gfx->print(rec);
  for (int i = 0; i < 3; i++) {
    if (i < 3 - gameMisses) gfx->fillCircle(180 + i * 28, 104, 6, UI_BAR_BAD);
    else gfx->drawCircle(180 + i * 28, 104, 6, UI_TRACK);
  }

  // rival: nombre y sus tipos (uno o dos)
  gfx->fillRoundRect(90, 118, 286, 70, 14, lerp565(battleTypeColor(typeEnemy), UI_WHITE, 5, 8));
  gfx->drawRoundRect(90, 118, 286, 70, 14, ink);
  const char *enemyName = dexName(typeEnemyDex);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(enemyName) * 6, 128);
  gfx->print(enemyName);
  int cw1 = typeChipWidth(typeEnemy), cw2 = typeEnemy2 != TYPE_NONE ? typeChipWidth(typeEnemy2) : 0;
  int cx0 = CX - (cw1 + (cw2 ? cw2 + 8 : 0)) / 2;
  drawTypeChip(cx0, 156, typeEnemy);
  if (cw2) drawTypeChip(cx0 + cw1 + 8, 156, typeEnemy2);

  for (int i = 0; i < 3; i++) {
    int bx = 88;
    int by = 210 + i * 60;
    const char *label = battleTypeName(typeChoice[i]);
    gfx->fillRoundRect(bx, by, 290, 48, 12, lerp565(battleTypeColor(typeChoice[i]), UI_WHITE, 5, 8));
    gfx->drawRoundRect(bx, by, 290, 48, 12, ink);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(bx + (290 - (int)strlen(label) * 12) / 2, by + 17);
    gfx->print(label);
  }
  int bw = 280;
  int fw = (int)((uint32_t)bw * (typeUntil - now) / typeDurationMs);
  if (fw < 0) fw = 0;
  gfx->fillRoundRect(CX - bw / 2, 392, bw, 14, 5, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(CX - bw / 2, 392, fw, 14, 5, UI_BAR_OK);
  flushStampedFrame();
}

// ---------- INCLINA (acelerometro) ----------
static bool tiltFarFrom(int16_t x, int16_t y, float bx, float by, int dist) {
  float dx = x - bx, dy = y - by;
  return dx * dx + dy * dy >= (float)dist * dist;
}

// punto al azar dentro del campo, lejos de la bola, de la baya y de los agujeros
static void tiltRandomSpot(int16_t *ox, int16_t *oy) {
  for (int tries = 0; tries < 40; tries++) {
    float a = random(360) * (float)PI / 180.0f, r = 30 + random(TILT_FIELD_R - 60);
    int16_t x = (int16_t)(CX + r * cosf(a)), y = (int16_t)(CY + 20 + r * 0.85f * sinf(a));
    if (!tiltFarFrom(x, y, tiltBX, tiltBY, 80)) continue;
    if (!tiltFarFrom(x, y, tiltBerryX, tiltBerryY, 60)) continue;
    bool ok = true;
    for (uint8_t i = 0; i < tiltHoleCount; i++) if (!tiltFarFrom(x, y, tiltHoleX[i], tiltHoleY[i], 60)) ok = false;
    if (!ok) continue;
    *ox = x;
    *oy = y;
    return;
  }
  *ox = CX;
  *oy = CY + 60;
}

void startTiltGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  gameMenuOpen = false;
  gameOpen = true;
  gameMode = 5;
  gameOverUntil = 0;
  gameScore = 0;
  gameMisses = 0;
  gameNewHi = false;
  gameGain = 0;
  float x, y, z;
  if (imuAccel(&x, &y, &z)) { tiltNX = x; tiltNY = y; }
  tiltBX = CX; tiltBY = CY + 20; tiltVX = tiltVY = 0;
  tiltHoleCount = 0;
  tiltBerryX = CX; tiltBerryY = CY - 60;
  tiltUntil = millis() + TILT_GAME_MS;
  tiltSafeUntil = millis() + 1000;
  sfxPlay(SFX_GAME_START);
}

void finishTiltGame() {
  gameNewHi = (gameScore > pet.tiltHi);
  gameGain = pet.applyTiltResult(gameScore);
  sfxPlay(gameNewHi && gameScore > 0 ? SFX_MEDAL : SFX_LEVEL);
  gameOverUntil = millis() + 4000;
}

static void stepTiltGame(uint32_t now) {
  float x, y, z;
  if (imuAccel(&x, &y, &z)) {
    // 0,25 g de inclinacion ~ 1 px por frame^2
    tiltVX += (x - tiltNX) * TILT_SIGN_X * 4.0f;
    tiltVY += (y - tiltNY) * TILT_SIGN_Y * 4.0f;
  }
  tiltVX *= 0.95f;
  tiltVY *= 0.95f;
  float vmax = 13.0f;
  if (tiltVX > vmax) tiltVX = vmax;
  if (tiltVX < -vmax) tiltVX = -vmax;
  if (tiltVY > vmax) tiltVY = vmax;
  if (tiltVY < -vmax) tiltVY = -vmax;
  tiltBX += tiltVX;
  tiltBY += tiltVY;
  float dx = tiltBX - CX, dy = tiltBY - CY;
  float d = sqrtf(dx * dx + dy * dy);
  float lim = TILT_FIELD_R - TILT_BALL_R;
  if (d > lim) {  // rebote suave en el borde
    float nx = dx / d, ny = dy / d;
    float dot = tiltVX * nx + tiltVY * ny;
    if (dot > 0) { tiltVX -= 1.5f * dot * nx; tiltVY -= 1.5f * dot * ny; }
    tiltBX = CX + nx * lim;
    tiltBY = CY + ny * lim;
  }
  // baya
  if (!tiltFarFrom(tiltBerryX, tiltBerryY, tiltBX, tiltBY, 30)) {
    gameScore++;
    hitX = tiltBerryX;
    hitY = tiltBerryY;
    hitTime = now;
    sfxPlay(SFX_MINIGAME_OK);
    tiltRandomSpot(&tiltBerryX, &tiltBerryY);
    if ((gameScore == 3 || gameScore == 6 || gameScore == 9) && tiltHoleCount < TILT_HOLES) {
      tiltRandomSpot(&tiltHoleX[tiltHoleCount], &tiltHoleY[tiltHoleCount]);
      tiltHoleCount++;
    }
  }
  // agujeros
  if (deadlineReached(now, tiltSafeUntil)) {
    for (uint8_t i = 0; i < tiltHoleCount; i++) {
      if (tiltFarFrom(tiltHoleX[i], tiltHoleY[i], tiltBX, tiltBY, 18)) continue;
      if (++gameMisses >= 3) { finishTiltGame(); return; }
      sfxPlay(SFX_MINIGAME_BAD);
      tiltBX = CX; tiltBY = CY + 20; tiltVX = tiltVY = 0;
      tiltSafeUntil = now + 1200;
      break;
    }
  }
}

void renderTiltGame() {
  uint32_t now = millis();
  if (gameOverUntil) {
    drawGameResult(T(S_RECORD_FMT), pet.tiltHi, S_DEF_GAIN_FMT);
    return;
  }
  if (deadlineReached(now, tiltUntil)) { finishTiltGame(); return; }
  stepTiltGame(now);
  if (gameOverUntil) return;
  drawGameScene();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  gfx->drawCircle(CX, CY, TILT_FIELD_R, ink);
  for (uint8_t i = 0; i < tiltHoleCount; i++) {  // agujeros
    gfx->fillCircle(tiltHoleX[i], tiltHoleY[i], 22, C565(0x20, 0x18, 0x12));
    gfx->drawCircle(tiltHoleX[i], tiltHoleY[i], 23, UI_BAR_BAD);
  }
  gfx->fillCircle(tiltBerryX, tiltBerryY, 20, UI_WHITE);
  drawMap(SPR_ICON_BERRY_G, 16, tiltBerryX - 16, tiltBerryY - 16, 2, false);
  bool blink = !deadlineReached(now, tiltSafeUntil) && (now / 120) % 2;
  if (!blink) drawMap(SPR_ICON_PLAY, 16, (int)tiltBX - 16, (int)tiltBY - 16, 2, false);
  gfx->setTextColor(ink);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_TILT_TITLE)) * 9, 32);
  gfx->print(T(S_TILT_TITLE));
  char score[16], rec[16];
  snprintf(score, sizeof(score), T(S_SCORE_FMT), gameScore);
  snprintf(rec, sizeof(rec), T(S_REC_FMT), pet.tiltHi);
  gfx->setTextSize(2);
  gfx->setCursor(74, 78);
  gfx->print(score);
  gfx->setCursor(392 - (int)strlen(rec) * 12, 78);
  gfx->print(rec);
  for (int i = 0; i < 3; i++) {
    if (i < 3 - gameMisses) gfx->fillCircle(180 + i * 28, 104, 6, UI_BAR_BAD);
    else gfx->drawCircle(180 + i * 28, 104, 6, UI_TRACK);
  }
  if (now + TILT_GAME_MS - tiltUntil < 3000) {
    const char *h = T(S_TILT_HINT);
    gfx->setTextSize(2);
    gfx->setCursor(CX - (int)strlen(h) * 6, 330);
    gfx->print(h);
  }
  int bw = 220;
  int fw = (int)((uint32_t)bw * (tiltUntil - now) / TILT_GAME_MS);
  if (fw < 0) fw = 0;
  gfx->fillRoundRect(CX - bw / 2, 396, bw, 12, 5, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(CX - bw / 2, 396, fw, 12, 5, UI_BAR_OK);
  uint32_t ht = now - hitTime;
  if (hitTime && ht < 220) gfx->drawCircle((int)hitX, (int)hitY, 26 + ht / 8, UI_BAR_OK);
  flushStampedFrame();
}

void renderGame() {
  // sin fillScreen(NEGRO): drawGameScene cubre los 466x466 completos. Si el
  // DMA del flush anterior aun lee el buffer, vera contenido valido (no negro
  // a medio pintar), que era el parpadeo a 25 fps.
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;

  if (gameMode == 1) {
    renderCatchGame();
    return;
  }
  if (gameMode == 2) {
    renderMemoGame();
    return;
  }
  if (gameMode == 3) {
    renderCleanGame();
    return;
  }
  if (gameMode == 4) {
    renderTypeGame();
    return;
  }
  if (gameMode == 5) {
    renderTiltGame();
    return;
  }

  if (gameOverUntil) {
    drawGameScene();
    if (deadlineReached(millis(), gameOverUntil)) {
      gameOpen = false;
      return;
    }
    char buf[22];
    snprintf(buf, sizeof(buf), T(S_SCORE_FMT), gameScore);
    gfx->setTextColor(ink);
    gfx->setTextSize(4);
    gfx->setCursor(CX - strlen(buf) * 12, 160);
    gfx->print(buf);
    gfx->setTextSize(2);
    if (gameNewHi && gameScore > 0) {
      gfx->setTextColor(UI_BAR_WARN);
      gfx->setCursor(CX - strlen(T(S_NEW_RECORD)) * 6, 214);
      gfx->print(T(S_NEW_RECORD));
    } else {
      char rec[20];
      snprintf(rec, sizeof(rec), T(S_RECORD_FMT), pet.gameHi);
      gfx->setTextColor(ink);
      gfx->setCursor(CX - strlen(rec) * 6, 214);
      gfx->print(rec);
    }
    const char *msg = gameScore >= 10 ? T(S_GREAT_JOY) : T(S_PLUS_JOY);
    gfx->setTextColor(ink);
    gfx->setCursor(CX - strlen(msg) * 6, 250);
    gfx->print(msg);
    flushStampedFrame();
    return;
  }

  drawGameScene();
  stepGame();

  // marcador, record y vidas
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", gameScore);
  gfx->setTextColor(ink);
  gfx->setTextSize(4);
  gfx->setCursor(CX - strlen(buf) * 12, 30);
  gfx->print(buf);
  char rec[12];
  snprintf(rec, sizeof(rec), T(S_REC_FMT), pet.gameHi);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(rec) * 6, 76);
  gfx->print(rec);
  for (int i = 0; i < 3; i++) {
    if (i < 3 - gameMisses) gfx->fillCircle(180 + i * 28, 104, 6, UI_BAR_BAD);
    else gfx->drawCircle(180 + i * 28, 104, 6, UI_TRACK);
  }

  if (pmd.loaded) {
    uint8_t act = (ballX > gamePetX + 4) ? PMD_WALKR : (ballX < gamePetX - 4) ? PMD_WALKL : PMD_IDLE;
    if (!pmd.has(act)) act = PMD_IDLE;
    drawPmdAct(act, (int)gamePetX, 394, millis(), true, false, 3);
  } else if (mon.loaded) {
    int s = (mon.h * 2 > 130) ? 1 : 2;
    int w = mon.w * s, h = mon.h * s;
    uint16_t fm = mon.frameMs ? mon.frameMs : 100;
    uint16_t fi = (millis() / fm) % mon.frames;
    const uint8_t *fr = mon.data + (uint32_t)fi * mon.w * mon.h;
    int px = (int)gamePetX - w / 2, py = 394 - h;
    for (int r = 0; r < mon.h; r++)
      for (int c = 0; c < mon.w; c++) {
        uint8_t idx = fr[r * mon.w + c];
        if (idx == 0xFF) continue;
        gfx->fillRect(px + c * s, py + r * s, s, s, mon.pal[idx]);
      }
  }

  // anillo de impacto que se expande y desvanece (feedback suave del golpe)
  uint32_t ht = millis() - hitTime;
  if (hitTime && ht < 260) {
    int rad = 22 + (int)(ht / 6);
    gfx->drawCircle((int)hitX, (int)hitY, rad, C565(0xff, 0xe7, 0x9f));
    gfx->drawCircle((int)hitX, (int)hitY, rad - 2, C565(0xff, 0xd9, 0x8a));
  }

  // la pokeball
  drawMap(SPR_ICON_PLAY, 16, (int)ballX - 24, (int)ballY - 24, 3, false);

  flushStampedFrame();
}

// ---------- combate salvaje manual ----------

uint16_t battleHpFor(const BattleStats &stats) {
  return stats.hp ? stats.hp : (uint16_t)(30 + stats.level * 5 + stats.def);
}

BattleStats petBattleStats() {
  BattleStats stats = {};
  stats.level = pet.level();
  stats.atk = pet.atkStat();
  stats.def = pet.defStat();
  stats.spe = pet.speStat();
  stats.hp = 0;
  if (!pet.isEgg() && pet.speciesId >= 1 && pet.speciesId <= DEX_COUNT) {
    stats.type1 = DEX_TBL[pet.speciesId].type1;
    stats.type2 = DEX_TBL[pet.speciesId].type2;
  }
  return stats;
}

void scheduleNextWild(uint32_t now) {
  nextWildEligible = now + WILD_COOLDOWN_MS;
}

bool mainScreenReadyForWild() {
  if (screenOff || pet.awaitingStarter() || pet.isEgg() || pet.sleeping || pet.ceremony) return false;
  if (battleOpen || gameOpen || gameMenuOpen || sackOpen || cardOpen || galleryOpen || kbOpen || clockOpen || helpOpen) return false;
  if (feedMenuUntil || confirmUntil || choiceKind || bathUntil || petEventUntil) return false;
  if (pet.evolving() || pet.wantEvolveButton() || pet.canRunawayNow() || pet.wantFarewellButton()) return false;
  return true;
}

void maybeOfferWildEncounter(uint32_t now) {
  if (nextWildEligible == 0) {
    scheduleNextWild(now);
    return;
  }
  if (wildPromptUntil) return;
  if (now - lastWildCheck < WILD_CHECK_MS) return;
  lastWildCheck = now;
  if (deadlineActive(now, nextWildEligible)) return;
  if (!mainScreenReadyForWild()) return;
  int16_t cand = pickWildSpecies((uint8_t)random(100), currentDayPhase());
  uint8_t phase = currentDayPhase();
  uint8_t chance = (phase == 3) ? 4 : (phase == 0 ? 7 : 8);
  if (phase == 3 && cand >= 1 && cand <= DEX_COUNT) {
    const DexEntry &d = DEX_TBL[cand];
    if (d.type1 == TYPE_GHOST || d.type2 == TYPE_GHOST ||
        d.type1 == TYPE_POISON || d.type2 == TYPE_POISON) chance = 8;
  }
  if ((uint8_t)random(100) >= chance) return;

  wildPromptDex = cand;
  wildPromptLevel = wildLevelFor(pet.level(), (uint8_t)random(100));
  wildPromptShiny = (random(4096) < pet.stepShinyChancePer4096());
  wildPromptUntil = now + WILD_PROMPT_MS;
  scheduleNextWild(now);
  sfxPlay(SFX_MENU);
}

void scheduleNextPetEvent(uint32_t now) {
  nextPetEventEligible = now + PET_EVENT_COOLDOWN_MS;
}

bool mainScreenReadyForPetEvent() {
  if (screenOff || pet.awaitingStarter() || pet.isEgg() || pet.sleeping || pet.ceremony) return false;
  if (battleOpen || gameOpen || gameMenuOpen || sackOpen || cardOpen || galleryOpen || kbOpen || clockOpen || helpOpen) return false;
  if (feedMenuUntil || confirmUntil || choiceKind || bathUntil || wildPromptUntil) return false;
  if (pet.evolving() || pet.wantEvolveButton() || pet.canRunawayNow() || pet.wantFarewellButton()) return false;
  return true;
}

void maybeOfferPetEvent(uint32_t now) {
  if (nextPetEventEligible == 0) {
    scheduleNextPetEvent(now);
    return;
  }
  if (petEventUntil) {
    if (deadlineReached(now, petEventUntil)) petEventUntil = 0;
    return;
  }
  if (now - lastPetEventCheck < PET_EVENT_CHECK_MS) return;
  lastPetEventCheck = now;
  if (deadlineActive(now, nextPetEventEligible)) return;
  if (!mainScreenReadyForPetEvent()) return;
  uint8_t phase = currentDayPhase();
  uint8_t chance = (phase == 0) ? 14 : (phase == 3 ? 8 : 10);
  if ((uint8_t)random(100) >= chance) return;

  petEventType = (uint8_t)random(3);
  petEventUntil = now + PET_EVENT_PROMPT_MS;
  scheduleNextPetEvent(now);
  sfxPlay(SFX_EVENT_SPARKLE);
}

bool inPetEventHit(int16_t x, int16_t y) {
  int16_t ex = 366, ey = 286;
  int dx = x - ex, dy = y - ey;
  return dx * dx + dy * dy <= 44 * 44;
}

void acceptPetEvent() {
  uint8_t type = petEventType;
  petEventUntil = 0;
  scheduleNextPetEvent(millis());
  if (!pet.applyPetEvent(type)) return;
  StrId msg = S_EVENT_FOUND;
  if (type == PET_EVENT_HEART) msg = S_EVENT_PET;
  else if (type == PET_EVENT_SPARKLE) msg = S_EVENT_LUCKY;
  snprintf(petEventMsg, sizeof(petEventMsg), "%s", T(msg));
  petEventFeedbackUntil = millis() + 1800;
  sfxPlay(type == PET_EVENT_BERRY ? SFX_EAT : (type == PET_EVENT_SPARKLE ? SFX_EVENT_SPARKLE : SFX_HEART));
}

bool mainScreenReadyForShake() {
  if (screenOff || pet.awaitingStarter() || pet.isEgg() || pet.sleeping || pet.ceremony) return false;
  if (battleOpen || gameOpen || gameMenuOpen || sackOpen || galleryOpen || kbOpen || clockOpen || helpOpen) return false;
  if (feedMenuUntil || confirmUntil || choiceKind || bathUntil) return false;
  if (pet.evolving()) return false;
  return true;
}

void maybeOfferMorning(uint32_t now) {
  if (morningUntil) {
    if (deadlineReached(now, morningUntil)) morningUntil = 0;
    return;
  }
  if (screenOff || pet.awaitingStarter() || pet.ceremony) return;
  if (battleOpen || gameOpen || gameMenuOpen || sackOpen || cardOpen || galleryOpen || kbOpen || clockOpen || helpOpen) return;
  if (feedMenuUntil || confirmUntil || choiceKind || bathUntil || wildPromptUntil || petEventUntil) return;
  if (currentDayPhase() != 0) return;
  if (!pet.takeMorningGreeting()) return;
  morningUntil = now + 2500UL;
  if (!pet.sleeping && !pet.isEgg() && audioMode() == SOUND_FULL)
    speciesChirpPlay(pet.speciesId, true);
  else if (!pet.sleeping && !pet.isEgg())
    sfxPlay(SFX_HEART);
}

void closeBattle() {
  battleOpen = false;
  battleResolved = false;
  battleAttackMenuUntil = 0;
  battleCatchOffered = false;
  battleCatchTried = false;
  battleCatchDone = false;
  battleCatchSuccess = false;
  battleRespectCatch = false;
  battleCatchChance = 0;
  wildPmd.unload();
  markUiDirty();
  lockTouchBrief();
}

void startBattleWith(int16_t forcedDex, uint8_t forcedLevel, int8_t forcedShiny) {
  if (!canStartWildBattle(pet.isEgg(), pet.sleeping, pet.ceremony)) return;
  wildPromptUntil = 0;
  scheduleNextWild(millis());
  if (forcedDex >= 1 && forcedDex <= DEX_COUNT) {
    battleDex = forcedDex;
    battleLevel = forcedLevel ? forcedLevel : wildLevelFor(pet.level(), (uint8_t)random(100));
  } else {
    uint8_t speciesRoll = random(100);
    uint8_t levelRoll = random(100);
    battleDex = pickWildSpecies(speciesRoll, currentDayPhase());
    battleLevel = wildLevelFor(pet.level(), levelRoll);
  }
  battleShiny = forcedShiny < 0 ? (random(4096) < pet.stepShinyChancePer4096()) : forcedShiny != 0;
  battlePlayer = petBattleStats();
  battleEnemy = wildBattleStats(battleDex, battleLevel);
  battleEnemy.hp = 0;
  battleResult = {};
  battleRun = beginBattleRuntime(battlePlayer, battleEnemy);
  battleTurn = {};
  battleReward = {};
  battleMsg[0] = 0;
  battleAttackMenuUntil = 0;
  battleLowHpWarned = false;
  battleCatchOffered = false;
  battleCatchTried = false;
  battleCatchDone = false;
  battleCatchSuccess = false;
  battleRespectCatch = false;
  battleCatchChance = 0;
  battleResolved = false;
  battleOpen = true;
  battleDirty = true;
  wildPmd.unload();
  wildPmd.load(battleDex, battleShiny);
  sfxPlay(SFX_TAP);
  speciesChirpPlay(battleDex);
}

void startBattle() {
  startBattleWith(0, 0, -1);
}

void finishBattle() {
  if (battleResolved) return;
  battleResolved = true;
  battleDirty = true;
  if (battleTurn.playerWon) {
    bool closeWin = battleRun.playerHp <= battleRun.playerMaxHp / 3;
    battleReward = pet.applyBattleWin(battleDex, closeWin);
    battleCatchOffered = true;
    battleRespectCatch = false;
    battleCatchChance = pet.catchChanceForWild(battleDex, battleLevel, battlePlayer.level, closeWin);
    sfxPlay(SFX_BATTLE_WIN);
  } else {
    battleReward = {};
    pet.applyBattleLoss();
    bool closeLoss = battleRun.enemyHp > 0 && battleRun.enemyHp * 100UL <= battleRun.enemyMaxHp * 30UL;
    battleCatchChance = closeLoss ? pet.respectCatchChanceForWild(battleDex, battleLevel, battlePlayer.level) : 0;
    battleCatchOffered = battleCatchChance > 0;
    battleRespectCatch = battleCatchOffered;
    sfxPlay(SFX_BATTLE_LOSS);
  }
}

void performBattleAction(BattleAction action) {
  if (battleResolved) return;
  battleAttackMenuUntil = 0;
  battleLastAction = action;
  battleTurn = stepBattle(battleRun, action, (uint8_t)random(100));
  if (battleTurn.restFailed) {
    snprintf(battleMsg, sizeof(battleMsg), T(S_NO_REST));
  } else if (battleTurn.counterReady) {
    snprintf(battleMsg, sizeof(battleMsg), T(S_COUNTER_READY));
  } else if (battleTurn.playerRested) {
    char healMsg[18];
    snprintf(healMsg, sizeof(healMsg), T(S_RESTED_FMT), battleTurn.playerHeal);
    snprintf(battleMsg, sizeof(battleMsg), "%s %s", healMsg, T(S_GUARD));
  } else if (battleTurn.playerDamage > 0) {
    if (battleTurn.playerTypePct > 100) snprintf(battleMsg, sizeof(battleMsg), "%s %u", T(S_EFFECTIVE), battleTurn.playerDamage);
    else if (battleTurn.playerTypePct < 100) snprintf(battleMsg, sizeof(battleMsg), "%s %u", T(S_NOT_EFFECTIVE), battleTurn.playerDamage);
    else snprintf(battleMsg, sizeof(battleMsg), T(S_HIT_FMT), battleTurn.playerDamage);
  } else if (battleTurn.enemyDodged) {
    snprintf(battleMsg, sizeof(battleMsg), T(S_ENEMY_DODGED));
  } else if (battleTurn.playerDodged) {
    snprintf(battleMsg, sizeof(battleMsg), T(S_DODGED));
  } else {
    snprintf(battleMsg, sizeof(battleMsg), T(S_MISSED));
  }
  if (battleTurn.battleEnded) {
    finishBattle();
    return;
  }
  if (!battleLowHpWarned && battleRun.playerHp > 0 && battleRun.playerHp <= battleRun.playerMaxHp * 3 / 10) {
    battleLowHpWarned = true;
    sfxPlay(SFX_LOW_HP);
  }
  if (battleTurn.restFailed) sfxPlay(SFX_DENY);
  else if (battleTurn.counterReady) sfxPlay(SFX_COUNTER);
  else if (battleTurn.playerRested) sfxPlay(SFX_REST);
  else if (action == BATTLE_ATTACK_QUICK) sfxPlay(SFX_ATTACK_QUICK);
  else if (action == BATTLE_ATTACK_HEAVY) sfxPlay(SFX_ATTACK_HEAVY);
  else if (battleTurn.playerDamage > 0 && battleTurn.playerTypePct > 100) sfxPlay(SFX_EFFECTIVE);
  else if (battleTurn.playerDamage > 0 && battleTurn.playerTypePct < 100) sfxPlay(SFX_WEAK_HIT);
  else if (battleTurn.enemyDamage > 0) sfxPlay(SFX_ENEMY_HIT);
  else sfxPlay(battleTurn.playerDamage > 0 ? SFX_PLAY : SFX_TAP);
}

void battleTap(int16_t x, int16_t y) {
  if (battleResolved) {
    if (battleCatchOffered && !battleCatchDone) {
      if (x >= 76 && x <= 224 && y >= 392 && y <= 448) {
        bool closeWin = battleRun.playerHp <= battleRun.playerMaxHp / 3;
        battleCatchTried = true;
        battleCatchDone = true;
        if (battleRespectCatch) {
          battleCatchSuccess = pet.tryRespectCatchWild(battleDex, battleLevel, battlePlayer.level,
                                                       (uint8_t)random(100), battleShiny);
          battleCatchChance = pet.respectCatchChanceForWild(battleDex, battleLevel, battlePlayer.level);
        } else {
          battleCatchSuccess = pet.tryCatchWild(battleDex, battleLevel, battlePlayer.level, closeWin,
                                                (uint8_t)random(100), battleShiny);
          battleCatchChance = pet.catchChanceForWild(battleDex, battleLevel, battlePlayer.level, closeWin);
        }
        sfxPlay(battleCatchSuccess ? SFX_CATCH_OK : SFX_CATCH_FAIL);
        galleryDirty = true;
        battleDirty = true;
        return;
      }
      if (x >= 242 && x <= 390 && y >= 392 && y <= 448) {
        battleCatchDone = true;
        battleCatchTried = false;
        battleDirty = true;
        sfxPlay(SFX_TAP);
        return;
      }
      return;
    }
    if (x >= 118 && x <= 348 && y >= 392 && y <= 454) closeBattle();
    return;
  }
  if (battleAttackMenuUntil) {
    if (x >= 66 && x <= 232 && y >= 292 && y <= 352) {
      performBattleAction(BATTLE_ATTACK_QUICK);
      return;
    }
    if (x >= 234 && x <= 400 && y >= 292 && y <= 352) {
      performBattleAction(BATTLE_ATTACK_HEAVY);
      return;
    }
    battleAttackMenuUntil = 0;
    battleDirty = true;
    sfxPlay(SFX_TAP);
    return;
  }
  if (x >= 184 && x <= 282 && y >= 100 && y <= 136) {
    closeBattle();
    sfxPlay(SFX_TAP);
    return;
  }
  if (x >= 46 && x <= 174 && y >= 344 && y <= 428) {
    battleAttackMenuUntil = 1;
    sfxPlay(SFX_TAP);
  } else if (x >= 169 && x <= 297 && y >= 344 && y <= 428) {
    performBattleAction(BATTLE_DODGE);
  } else if (x >= 292 && x <= 420 && y >= 344 && y <= 428) {
    performBattleAction(BATTLE_REST);
  }
}

void drawBattleHpBar(int x, int y, uint16_t cur, uint16_t maxHp, uint16_t color) {
  if (maxHp == 0) maxHp = 1;
  int w = 150;
  int fw = (int)((uint32_t)cur * w / maxHp);
  if (fw > w) fw = w;
  gfx->fillRoundRect(x, y, w, 14, 4, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(x, y, fw, 14, 4, color);
}

void battleRewardText(char *buf, size_t len) {
  if (battleReward.amount == 0) { buf[0] = 0; return; }
  StrId fmt = S_SPD_GAIN_FMT;
  if (battleReward.stat == BATTLE_REWARD_ATK) fmt = S_ATK_GAIN_FMT;
  else if (battleReward.stat == BATTLE_REWARD_DEF) fmt = S_DEF_GAIN_FMT;
  snprintf(buf, len, T(fmt), battleReward.amount);
}

void drawBattleButtonLabel(int x, int y, int w, const char *label) {
  int px = (int)strlen(label) * 12;
  if (px <= w - 8) {
    gfx->setTextSize(2);
    gfx->setCursor(x + (w - px) / 2, y);
    gfx->print(label);
  } else {
    gfx->setTextSize(1);
    gfx->setCursor(x + (w - (int)strlen(label) * 6) / 2, y + 3);
    gfx->print(label);
    gfx->setTextSize(2);
  }
}

const char *battleTypeName(uint8_t type) {
  static const char *const TYPE_NAMES[LANG_COUNT][19] = {
    { "", "NORMAL", "FUEGO", "AGUA", "ELEC", "PLANTA", "HIELO", "LUCHA", "VENENO", "TIERRA", "VUELO", "PSI", "BICHO", "ROCA", "FANT", "DRAGON", "SINIE", "ACERO", "HADA" },
    { "", "NORMAL", "FIRE", "WATER", "ELEC", "GRASS", "ICE", "FIGHT", "POISON", "GROUND", "FLY", "PSY", "BUG", "ROCK", "GHOST", "DRAGON", "DARK", "STEEL", "FAIRY" },
    { "", "NORMAL", "FEU", "EAU", "ELEC", "PLANTE", "GLACE", "COMBAT", "POISON", "SOL", "VOL", "PSY", "INSECT", "ROCHE", "SPECTRE", "DRAGON", "TENEBR", "ACIER", "FEE" },
    { "", "NORMAL", "FEUER", "WASSER", "ELEKTRO", "PFLANZE", "EIS", "KAMPF", "GIFT", "BODEN", "FLUG", "PSYCHO", "KAEFER", "GESTEIN", "GEIST", "DRACHE", "UNLICHT", "STAHL", "FEE" },
    { "", "NORMALE", "FUOCO", "ACQUA", "ELETTRO", "ERBA", "GHIACCIO", "LOTTA", "VELENO", "TERRA", "VOLANTE", "PSICO", "COLEOT", "ROCCIA", "SPETTRO", "DRAGO", "BUIO", "ACCIAIO", "FOLLETTO" },
    { "", "NORMAL", "FOGO", "AGUA", "ELETR", "PLANTA", "GELO", "LUTA", "VENENO", "TERRA", "VOO", "PSI", "INSETO", "PEDRA", "FANT", "DRAGAO", "SOMBRIO", "ACO", "FADA" },
  };
  if (type < 19) return TYPE_NAMES[gLang][type];
  switch (type) {
    case TYPE_NORMAL: return "NORMAL";
    case TYPE_FIRE: return "FIRE";
    case TYPE_WATER: return "WATER";
    case TYPE_ELECTRIC: return "ELEC";
    case TYPE_GRASS: return "GRASS";
    case TYPE_ICE: return "ICE";
    case TYPE_FIGHTING: return "FIGHT";
    case TYPE_POISON: return "POISON";
    case TYPE_GROUND: return "GROUND";
    case TYPE_FLYING: return "FLY";
    case TYPE_PSYCHIC: return "PSY";
    case TYPE_BUG: return "BUG";
    case TYPE_ROCK: return "ROCK";
    case TYPE_GHOST: return "GHOST";
    case TYPE_DRAGON: return "DRAGON";
    case TYPE_DARK: return "DARK";
    case TYPE_STEEL: return "STEEL";
    case TYPE_FAIRY: return "FAIRY";
  }
  return "";
}

uint16_t battleTypeColor(uint8_t type) {
  switch (type) {
    case TYPE_FIRE: return 0xEA87;
    case TYPE_WATER: return 0x4C98;
    case TYPE_ELECTRIC: return 0xBCA1;
    case TYPE_GRASS: return 0x3C49;
    case TYPE_ICE: return 0x5D99;
    case TYPE_FIGHTING: return 0xA2A5;
    case TYPE_POISON: return 0x8A73;
    case TYPE_GROUND: return 0xB447;
    case TYPE_FLYING: return 0x8D7F;
    case TYPE_PSYCHIC: return 0xD28F;
    case TYPE_BUG: return 0x7CC4;
    case TYPE_ROCK: return 0x9407;
    case TYPE_GHOST: return 0x6B33;
    case TYPE_DRAGON: return 0x5A5F;
    case TYPE_DARK: return 0x5ACB;
    case TYPE_STEEL: return 0xA534;
    case TYPE_FAIRY: return 0xF3B7;
    default: return 0x8C4D;
  }
}

void typeText(char *buf, size_t len, const DexEntry &d) {
  if (d.type2 == TYPE_NONE) snprintf(buf, len, "%s", battleTypeName(d.type1));
  else snprintf(buf, len, "%s %s", battleTypeName(d.type1), battleTypeName(d.type2));
}

int typeChipWidth(uint8_t type) {
  return (int)strlen(battleTypeName(type)) * 6 + 14;
}

void drawTypeChip(int x, int y, uint8_t type) {
  if (type == TYPE_NONE) return;
  const char *label = battleTypeName(type);
  int w = typeChipWidth(type);
  gfx->fillRoundRect(x, y, w, 16, 5, lerp565(battleTypeColor(type), UI_WHITE, 5, 8));
  gfx->drawRoundRect(x, y, w, 16, 5, UI_INK);
  gfx->setTextSize(1);
  gfx->setTextColor(UI_INK);
  gfx->setCursor(x + 7, y + 5);
  gfx->print(label);
}

int drawTypeChips(int x, int y, const DexEntry &d, bool alignRight) {
  int w1 = typeChipWidth(d.type1);
  int w2 = d.type2 == TYPE_NONE ? 0 : typeChipWidth(d.type2);
  int total = w1 + (w2 ? 4 + w2 : 0);
  int sx = alignRight ? x - total : x;
  drawTypeChip(sx, y, d.type1);
  if (d.type2 != TYPE_NONE) drawTypeChip(sx + w1 + 4, y, d.type2);
  gfx->setTextSize(2);
  return sx;
}

void drawWildPrompt() {
  gfx->fillRoundRect(82, 156, 302, 178, 18, UI_WHITE);
  gfx->drawRoundRect(82, 156, 302, 178, 18, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_WILD_Q)) * 9, 176);
  gfx->print(T(S_WILD_Q));
  char name[28];
  snprintf(name, sizeof(name), "%s%s Lv.%u", wildPromptShiny ? "*" : "",
           dexName(wildPromptDex), wildPromptLevel);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(name) * 6, 206);
  gfx->print(name);

  gfx->fillRoundRect(93, 226, 280, 44, 12, UI_BAR_BAD);
  gfx->fillRoundRect(93, 278, 280, 44, 12, UI_TRACK);
  gfx->setTextColor(UI_WHITE);
  gfx->setCursor(CX - strlen(T(S_FIGHT)) * 6, 240);
  gfx->print(T(S_FIGHT));
  gfx->setTextColor(UI_BG_DAY);
  gfx->setCursor(CX - strlen(T(S_LATER)) * 6, 292);
  gfx->print(T(S_LATER));
}

void drawPetEvent() {
  uint32_t now = millis();
  if (petEventUntil && deadlineReached(now, petEventUntil)) petEventUntil = 0;
  if (!petEventUntil && (!petEventFeedbackUntil || deadlineReached(now, petEventFeedbackUntil))) return;

  int16_t ex = 366, ey = 286;
  if (petEventUntil) {
    gfx->fillCircle(ex, ey, 32, UI_WHITE);
    gfx->drawCircle(ex, ey, 34, UI_BAR_WARN);
    if (petEventType == PET_EVENT_BERRY) {
      drawMap(SPR_ICON_BERRY_G, 16, ex - 24, ey - 24, 3, false);
    } else if (petEventType == PET_EVENT_HEART) {
      drawMap(SPR_HEART, 32, ex - 32, ey - 32, 2, false);
    } else {
      gfx->setTextColor(UI_BAR_WARN);
      gfx->setTextSize(4);
      gfx->setCursor(ex - 12, ey - 18);
      gfx->print("*");
      gfx->fillCircle(ex - 14, ey + 12, 4, UI_WHITE);
      gfx->fillCircle(ex + 16, ey + 10, 4, UI_WHITE);
    }
  }
  if (petEventFeedbackUntil && deadlineActive(now, petEventFeedbackUntil)) {
    gfx->setTextColor(UI_BAR_WARN);
    gfx->setTextSize(2);
    gfx->setCursor(CX - strlen(petEventMsg) * 6, 292);
    gfx->print(petEventMsg);
  }
}

void renderBattle() {
  if (battleResolved) battleDirty = false;
  drawGameScene();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  const DexEntry &mine = DEX_TBL[pet.speciesId];
  const DexEntry &wild = DEX_TBL[battleDex];

  gfx->setTextColor(ink);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_WILD_BATTLE)) * 9, 34);
  gfx->print(T(S_WILD_BATTLE));

  char left[24], right[24];
  snprintf(left, sizeof(left), "%s Lv.%u", pet.nick[0] ? pet.nick : dexName(pet.speciesId), battlePlayer.level);
  snprintf(right, sizeof(right), "%s%s Lv.%u", battleShiny ? "*" : "",
           dexName(battleDex), battleLevel);
  gfx->setTextSize(2);
  gfx->setCursor(28, 82);
  gfx->print(left);
  int rightLen = strlen(right);
  int rightTextSize = rightLen <= 12 ? 2 : 1;
  int rightTextW = rightLen * (rightTextSize == 2 ? 12 : 6);
  int rightX = 408 - rightTextW;
  if (rightX < 246) rightX = 246;
  gfx->setTextSize(rightTextSize);
  gfx->setCursor(rightX, rightTextSize == 2 ? 82 : 88);
  gfx->print(right);
  gfx->setTextSize(2);

  uint16_t playerMax = battleRun.playerMaxHp;
  uint16_t enemyMax = battleRun.enemyMaxHp;
  uint16_t playerCur = battleRun.playerHp;
  uint16_t enemyCur = battleRun.enemyHp;
  drawBattleHpBar(28, 110, playerCur, playerMax, UI_BAR_OK);
  drawBattleHpBar(288, 110, enemyCur, enemyMax, UI_BAR_BAD);
  drawTypeChips(28, 130, mine, false);
  int wildChipLeft = drawTypeChips(438, 130, wild, true);

  if (!battleResolved) {
    gfx->fillRoundRect(188, 102, 90, 32, 9, UI_TRACK);
    gfx->setTextColor(UI_BG_DAY);
    gfx->setTextSize(2);
    gfx->setCursor(188 + (90 - (int)strlen(T(S_RUN_BATTLE)) * 12) / 2, 111);
    gfx->print(T(S_RUN_BATTLE));
  }
  // Links neben den Typ-Chips des Gegners (Gift/Flug usw.). Oben rechts
  // neben dem Namen liegt der Marker ausserhalb des runden Displays.
  if (pet.isCaught(battleDex))
    drawCaughtBattleMarker(wildChipLeft - 36, 122);

  if (pmd.loaded) drawPmdAct(PMD_IDLE, 142, 286, millis(), true, false, 3);
  else {
    const uint8_t *th = thumbs.get(pet.speciesId);
    if (th) drawThumb(th, 94, 166, 3, false);
  }
  if (wildPmd.loaded) drawPmdActM(wildPmd, PMD_IDLE, 328, 286, millis(), true, false, 3);
  else {
    const uint8_t *th = thumbs.get(battleDex);
    if (th) drawThumb(th, 280, 166, 3, false);
  }

  if (battleResolved) {
    const char *res = battleTurn.playerWon ? T(S_WIN) : T(S_LOSS);
    gfx->setTextColor(battleTurn.playerWon ? UI_BAR_OK : UI_BAR_BAD);
    gfx->setTextSize(4);
    gfx->setCursor(CX - strlen(res) * 12, 300);
    gfx->print(res);
    char rounds[20], damage[28];
    snprintf(rounds, sizeof(rounds), T(S_ROUNDS_FMT), battleRun.round);
    snprintf(damage, sizeof(damage), T(S_DAMAGE_FMT), battleRun.playerDamageTotal, battleRun.enemyDamageTotal);
    gfx->setTextColor(ink);
    gfx->setTextSize(2);
    gfx->setCursor(CX - strlen(rounds) * 6, 334);
    gfx->print(rounds);
    gfx->setCursor(CX - strlen(damage) * 6, 356);
    gfx->print(damage);
    if (battleTurn.playerWon) {
      char reward[20];
      battleRewardText(reward, sizeof(reward));
      if (reward[0]) {
        gfx->setTextColor(UI_BAR_WARN);
        gfx->setCursor(CX - strlen(reward) * 6, 378);
        gfx->print(reward);
      }
    } else if (battleRespectCatch && battleCatchOffered && !battleCatchDone) {
      gfx->setTextColor(UI_BAR_WARN);
      gfx->setTextSize(2);
      gfx->setCursor(CX - strlen(T(S_CLOSE_CHANCE)) * 6, 378);
      gfx->print(T(S_CLOSE_CHANCE));
    }
    if (battleCatchOffered && !battleCatchDone) {
      gfx->fillRoundRect(76, 396, 148, 52, 14, UI_BAR_OK);
      gfx->fillRoundRect(242, 396, 148, 52, 14, UI_TRACK);
      gfx->setTextColor(UI_BG_DAY);
      drawBattleButtonLabel(76, 414, 148, T(S_CATCH_WILD));
      drawBattleButtonLabel(242, 414, 148, T(S_LEAVE_WILD));
    } else {
      if (battleCatchDone && battleCatchTried) {
        const char *catchMsg = battleCatchSuccess ? T(S_CAUGHT_OK) : T(S_ESCAPED);
        gfx->setTextColor(battleCatchSuccess ? UI_BAR_OK : UI_BAR_BAD);
        gfx->setTextSize(2);
        gfx->setCursor(CX - strlen(catchMsg) * 6, 378);
        gfx->print(catchMsg);
      }
      gfx->fillRoundRect(118, 396, 230, 52, 14, UI_BAR_OK);
      gfx->setTextColor(UI_BG_DAY);
      gfx->setTextSize(3);
      gfx->setCursor(CX - strlen(T(S_OK)) * 9, 413);
      gfx->print(T(S_OK));
    }
  } else {
    char roundBuf[14];
    snprintf(roundBuf, sizeof(roundBuf), T(S_ROUND_SHORT_FMT), battleRun.round + 1);
    gfx->setTextColor(ink);
    gfx->setTextSize(2);
    gfx->setCursor(32, 318);
    gfx->print(roundBuf);
    if (battleMsg[0]) {
      gfx->setCursor(CX - strlen(battleMsg) * 6, 318);
      gfx->print(battleMsg);
      if (battleTurn.enemyDamage > 0) {
        char eb[16];
        snprintf(eb, sizeof(eb), "-%u", battleTurn.enemyDamage);
        gfx->setTextColor(UI_BAR_BAD);
        gfx->setCursor(CX - strlen(eb) * 6, 340);
        gfx->print(eb);
      }
    }

    if (battleAttackMenuUntil) {
      gfx->fillRoundRect(74, 298, 150, 46, 12, UI_BAR_BAD);
      gfx->fillRoundRect(242, 298, 150, 46, 12, UI_BAR_WARN);
      gfx->setTextColor(UI_BG_DAY);
      drawBattleButtonLabel(74, 312, 150, T(S_QUICK_ATTACK));
      drawBattleButtonLabel(242, 312, 150, T(S_HEAVY_ATTACK));
    }

    gfx->fillRoundRect(58, 358, 108, 58, 13, UI_BAR_BAD);
    gfx->fillRoundRect(179, 358, 108, 58, 13, 0x4C98);
    gfx->fillRoundRect(300, 358, 108, 58, 13, UI_BAR_OK);
    gfx->setTextColor(UI_BG_DAY);
    drawBattleButtonLabel(58, 380, 108, T(S_ATTACK));
    drawBattleButtonLabel(179, 380, 108, T(S_DODGE));
    char restLabel[18];
    snprintf(restLabel, sizeof(restLabel), "%s %u", T(S_REST), battleRun.restUsesLeft);
    drawBattleButtonLabel(300, 380, 108, restLabel);
  }

  flushStampedFrame();
}

// ---------- ficha del bicho (deslizar vertical) ----------

void drawCardStat(int y, const char *label, uint16_t val, uint16_t maxBar, uint16_t color) {
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(96, y);
  gfx->print(label);
  char num[8];
  snprintf(num, sizeof(num), "%u", val);
  gfx->setCursor(330, y);
  gfx->print(num);
  int bw = 160;
  int fw = (int)val * bw / maxBar;
  if (fw > bw) fw = bw;
  gfx->fillRoundRect(150, y + 2, bw, 11, 3, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(150, y + 2, fw, 11, 3, color);
}

// ---------- ajuste de hora en pantalla (deslizar abajo) ----------
// El usuario pone su hora LOCAL a ojo; el firmware la usa tal cual, asi que
// no hay que gestionar zona horaria. Preserva el dia (no rompe racha/edad).

void openClock() {  // abre el menu de ajustes
  setPage = SETP_MENU;
  clockOpen = true;
  clockDirty = true;
  lockTouchBrief();
  sfxPlay(SFX_MENU);
}

void applyClock() {
  uint32_t base = pet.lastSeenEpoch ? pet.lastSeenEpoch : rtcEpoch();
  uint32_t e = (base / 86400) * 86400 + (uint32_t)clockH * 3600 + (uint32_t)clockM * 60;
  rtcSetEpoch(e);
  pet.setClock(e);
  setPage = SETP_MENU;  // vuelve al menu de ajustes
  clockDirty = true;
  lockTouchBrief();
}

void drawClockBtn(int x, int y, const char *l) {
  gfx->fillRoundRect(x, y, 58, 58, 12, UI_WHITE);
  gfx->drawRoundRect(x, y, 58, 58, 12, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(4);
  gfx->setCursor(x + 17, y + 15);
  gfx->print(l);
}

#define NET_DIM_INK 0x8410

const char *soundModeLabel() {
  switch (audioMode()) {
    case SOUND_FULL: return T(S_SND_FULL);
    case SOUND_MED: return T(S_SND_MED);
    case SOUND_LOW: return T(S_SND_LOW);
    default: return T(S_SND_OFF);
  }
}

uint8_t nextSoundMode() {
  switch (audioMode()) {
    case SOUND_FULL: return SOUND_MED;
    case SOUND_MED: return SOUND_LOW;
    case SOUND_LOW: return SOUND_OFF;
    default: return SOUND_FULL;
  }
}
static const char *const LANG_CODES[LANG_COUNT] = { "ES", "EN", "FR", "DE", "IT", "PT" };

const char *powerSaveLabel() {
  return powerSave ? T(S_PSAVE_ON) : T(S_PSAVE_OFF);
}

static const char *const HELP_WORD[LANG_COUNT] = { "AYUDA", "HELP", "AIDE", "HILFE", "AIUTO", "AJUDA" };
static const char *const HELP_OK[LANG_COUNT] = { "OK", "OK", "OK", "OK", "OK", "OK" };

static const char *const HELP_TITLES[LANG_COUNT][HELP_PAGE_COUNT] = {
  { "CUIDADO", "SUENO/ENERGIA", "MINIJUEGOS", "COMBATE 1", "COMBATE 2", "COLECCION", "EXTRAS", "EXPEDICION" },
  { "CARE", "SLEEP/ENERGY", "MINIGAMES", "BATTLE 1", "BATTLE 2", "COLLECTION", "EXTRAS", "EXPEDITION" },
  { "SOIN", "SOMMEIL/ENE", "MINI-JEUX", "COMBAT 1", "COMBAT 2", "COLLECTION", "EXTRAS", "EXPEDITION" },
  { "PFLEGE", "SCHLAF/ENERGIE", "MINISPIELE", "KAMPF 1", "KAMPF 2", "SAMMLUNG", "EXTRAS", "EXPEDITION" },
  { "CURA", "SONNO/ENERGIA", "MINIGIOCHI", "LOTTA 1", "LOTTA 2", "COLLEZIONE", "EXTRA", "SPEDIZIONE" },
  { "CUIDADO", "SONO/ENERGIA", "MINIJOGOS", "BATALHA 1", "BATALHA 2", "COLECAO", "EXTRAS", "EXPEDICAO" },
};

static const char *const HELP_LINES[LANG_COUNT][HELP_PAGE_COUNT][HELP_LINE_COUNT] = {
  {
    { "Comida baja = descuido.", "Jugar sube alegria.", "Bano limpia suciedad.", "Tocar da alegria/vinc.", "Peso alto te frena.", "Dulce alegra, engorda." },
    { "Dormir recupera energia.", "Durmiendo todo baja lento.", "Luz despierta o duerme.", "PWR: atras; en casa bloq.", "Ahorro usa light sleep.", "Sin borrar conserva save." },
    { "Bola: toca la bola.", "Atrapa: toca iconos.", "Memo: repite secuencia.", "Limpia: toca manchas.", "Tipo: elige ventaja.", "Dan records y entreno." },
    { "Rapido: menos dano.", "Rival esquiva poco.", "Recibes algo menos dano.", "Fuerte: mas dano.", "Riesgo y contra mayor.", "No siempre conviene." },
    { "Esquivar evita dano.", "Si sale: Contra listo.", "Prox ataque pega mas.", "Ruhe/Descanso cura 2x.", "Tambien da Guardia.", "Tipos suben/bajan dano." },
    { "Pokedex: desliza lado.", "Criado y atrapado cuentan.", "10/25/50/100/151/160/200/251/300/386: marcos.", "Perfil: elige marco.", "Detalle conocido: chirp.", "SONIDO TODO: toca pet." },
    { "Diario da metas diarias.", "Eventos salen raros.", "Batallas salvajes opc.", "Captura tras ganar.", "Rachas y medallas quedan.", "Agita: juega. Paseo da FEL." },
    { "Expedicion: 15/30/60 min.", "Cuesta energia al salir.", "El bicho sigue disponible.", "Buen cuidado mejora premio.", "Recoge 1 objeto al volver.", "Chip abre esta tarjeta." },
  },
  {
    { "Low food = slip-up.", "Play raises joy.", "Bath cleans dirt.", "Petting gives joy/bond.", "High weight slows you.", "Candy cheers but fattens." },
    { "Sleep restores energy.", "Needs decay slower asleep.", "Light toggles sleep.", "PWR: back; locks at home.", "Power Save light-sleeps.", "No erase keeps saves." },
    { "Ball: tap the ball.", "Catch: tap icons.", "Memo: repeat sequence.", "Clean: tap stains.", "Type: pick advantage.", "Records and training." },
    { "Quick: lower damage.", "Enemy dodges less.", "You take less damage.", "Heavy: more damage.", "More risk/counterplay.", "Not always best." },
    { "Dodge avoids damage.", "Success: Counter ready.", "Next attack hits harder.", "Rest heals only 2x.", "Rest also gives Guard.", "Types change damage." },
    { "Pokedex: side swipe.", "Raised and caught count.", "10/25/50/100/151/160/200/251/300/386: frames.", "Profile: choose frame.", "Known detail: species chirp.", "SOUND FULL: tap pet." },
    { "Daily gives small goals.", "Events appear rarely.", "Wild battles are optional.", "Catch after winning.", "Streaks/medals persist.", "Shake to play. Walk gives JOY." },
    { "Expedition: 15/30/60 min.", "Energy is spent at start.", "Pet stays available.", "Care and bond improve finds.", "Claim 1 item when back.", "Chip opens this card." },
  },
  {
    { "Faim basse = erreur.", "Jouer monte la joie.", "Bain nettoie.", "Caresse donne lien/joie.", "Poids haut ralentit.", "Bonbon rend gros." },
    { "Sommeil rend energie.", "Besoins baissent moins.", "Lumiere dort/reveille.", "PWR: retour; verrou accueil", "Eco utilise light sleep.", "Sans erase garde save." },
    { "Balle: touche la balle.", "Attrape: touche icones.", "Memo: repete sequence.", "Nettoie: touche taches.", "Type: choisis avantage.", "Records et entrainement." },
    { "Rapide: degats bas.", "Ennemi esquive moins.", "Tu subis moins.", "Fort: degats hauts.", "Risque plus grand.", "Pas toujours meilleur." },
    { "Esquive evite degats.", "Succes: Contre pret.", "Prochaine attaque plus.", "Repos soigne 2 fois.", "Repos donne Garde.", "Types changent degats." },
    { "Pokedex: glisse cote.", "Eleve et capture comptent.", "10/25/50/100/151/160/200/251/300/386: cadres.", "Profil: choisis cadre.", "Detail connu: chirp.", "SON TOUT: touche pet." },
    { "Quotidien donne buts.", "Events rares.", "Combats sauvages option.", "Capture apres victoire.", "Series/medailles restent.", "Secoue: joue. Marche = JOIE." },
    { "Expedition: 15/30/60 min.", "Energie payee au depart.", "Le pet reste disponible.", "Soin/lien aide le butin.", "Prends 1 objet au retour.", "Chip ouvre cette carte." },
  },
  {
    { "Food 0 = Patzer.", "Spielen hebt Freude.", "Bad reinigt Hygiene.", "Streicheln gibt Bond.", "Hohes Gewicht bremst.", "Candy freut, macht dick." },
    { "Schlaf gibt Energie.", "Needs sinken langsamer.", "Licht: schlafen/wach.", "PWR: zurueck; Start sperrt", "Sparen nutzt Light Sleep.", "Ohne Erase bleibt Save." },
    { "Ball: Ball antippen.", "Fangen: Icons treffen.", "Memo: Folge merken.", "Putzen: Flecken tippen.", "Typ: Vorteil waehlen.", "Gibt Rekorde/Training." },
    { "Schnell: weniger Schaden.", "Gegner weicht selten aus.", "Du kassierst weniger.", "Stark: mehr Schaden.", "Mehr Risiko/Gegendruck.", "Nicht immer beste Wahl." },
    { "Ausweichen meidet Schaden.", "Klappt es: Konter bereit.", "Naechster Angriff staerker.", "Ruhen heilt nur 2x.", "Ruhen gibt auch Schutz.", "Typen aendern Schaden." },
    { "Pokedex: seitlich wischen.", "Aufz./gefangen zaehlen.", "10/25/50/100/151/160/200/251/300/386: Rahmen.", "Profil: Rahmen waehlen.", "Bekanntes Detail: Chirp.", "TON VOLL: Pet tippen." },
    { "Taeglich gibt Ziele.", "Events sind selten.", "Wildkampf ist optional.", "Fangen nach Sieg.", "Serien/Medaillen bleiben.", "Schuetteln spielt. Gehen = JOY." },
    { "Expedition: 15/30/60 Min.", "Kostet beim Start Energie.", "Pet bleibt verfuegbar.", "Pflege/Bond verbessert Fund.", "Fund danach einsammeln.", "Chip oeffnet diese Karte." },
  },
  {
    { "Cibo 0 = errore.", "Gioca aumenta gioia.", "Bagno pulisce.", "Carezza da legame.", "Peso alto rallenta.", "Dolce rallegra, ingrassa." },
    { "Sonno da energia.", "Bisogni calano meno.", "Luce dorme/sveglia.", "PWR: indietro; blocca home", "Risparmio usa light sleep.", "Senza erase salva." },
    { "Palla: tocca palla.", "Prendi: tocca icone.", "Memo: ripeti sequenza.", "Pulisci: tocca macchie.", "Tipo: scegli vantaggio.", "Record e allenamento." },
    { "Rapido: meno danni.", "Nemico schiva meno.", "Subisci meno danni.", "Forte: piu danni.", "Piu rischio.", "Non sempre migliore." },
    { "Schiva evita danni.", "Successo: contro pronto.", "Prox attacco piu forte.", "Riposo cura solo 2x.", "Riposo da Guardia.", "Tipi cambiano danni." },
    { "Pokedex: scorri lato.", "Allevato e preso contano.", "10/25/50/100/151/160/200/251/300/386: cornici.", "Profilo: scegli cornice.", "Dettaglio noto: chirp.", "SUONO TUTTO: tocca pet." },
    { "Quotidiano da obiettivi.", "Eventi rari.", "Lotte selvatiche opz.", "Cattura dopo vittoria.", "Serie/medaglie restano.", "Scuoti: gioca. Cammina = GIO." },
    { "Spedizione: 15/30/60 min.", "Energia spesa alla partenza.", "Il pet resta disponibile.", "Cura/legame migliora premio.", "Ritira 1 oggetto al ritorno.", "Chip apre questa carta." },
  },
  {
    { "Comida 0 = falha.", "Jogar sobe alegria.", "Banho limpa.", "Carinho da vinculo.", "Peso alto atrasa.", "Doce alegra, engorda." },
    { "Sono da energia.", "Necessidades caem menos.", "Luz dorme/acorda.", "PWR: voltar; bloqueia home", "Poupanca usa light sleep.", "Sem erase guarda save." },
    { "Bola: toque na bola.", "Pegar: toque icones.", "Memo: repita sequencia.", "Limpa: toque manchas.", "Tipo: escolha vantagem.", "Recordes e treino." },
    { "Rapido: dano menor.", "Rival desvia menos.", "Voce recebe menos.", "Forte: dano maior.", "Mais risco.", "Nem sempre melhor." },
    { "Desviar evita dano.", "Sucesso: contra pronto.", "Prox ataque mais forte.", "Descanso cura so 2x.", "Descanso da Guarda.", "Tipos mudam dano." },
    { "Pokedex: deslize lado.", "Criado e apanhado contam.", "10/25/50/100/151/160/200/251/300/386: molduras.", "Perfil: escolha moldura.", "Detalhe conhecido: chirp.", "SOM TUDO: toque pet." },
    { "Diario da metas.", "Eventos sao raros.", "Batalha selvagem opc.", "Captura apos vitoria.", "Series/medalhas ficam.", "Agita: joga. Andar da ALE." },
    { "Expedicao: 15/30/60 min.", "Energia gasta ao sair.", "Pet fica disponivel.", "Cuidado/laco melhora premio.", "Recolhe 1 item ao voltar.", "Chip abre este cartao." },
  },
};

static void setButton(int x, int y, int w, int h, const char *label, uint16_t fill, uint16_t ink, uint8_t size);

void renderHelp() {
  helpDirty = false;
  gfx->fillScreen(UI_BG_DAY);
  uint8_t lang = (gLang < LANG_COUNT) ? (uint8_t)gLang : (uint8_t)LANG_EN;
  if (helpPage >= HELP_PAGE_COUNT) helpPage = 0;

  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  const char *h = HELP_WORD[lang];
  gfx->setCursor(CX - strlen(h) * 9, 36);
  gfx->print(h);

  gfx->setTextSize(2);
  gfx->setTextColor(UI_BAR_BAD);
  const char *title = HELP_TITLES[lang][helpPage];
  gfx->setCursor(CX - strlen(title) * 6, 76);
  gfx->print(title);

  gfx->setTextColor(UI_INK);
  for (uint8_t i = 0; i < HELP_LINE_COUNT; i++) {
    const char *line = HELP_LINES[lang][helpPage][i];
    gfx->setCursor(CX - strlen(line) * 6, 116 + i * 34);
    gfx->print(line);
  }

  char pg[12];
  snprintf(pg, sizeof(pg), "%u/%u", helpPage + 1, HELP_PAGE_COUNT);
  gfx->setTextColor(NET_DIM_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(pg) * 6, 330);
  gfx->print(pg);
  // flechas grandes; se sale con PWR
  setButton(HELP_PREV_X, HELP_NAV_Y, HELP_NAV_W, HELP_NAV_H, "<", helpPage > 0 ? UI_WHITE : UI_TRACK, UI_INK, 3);
  setButton(HELP_NEXT_X, HELP_NAV_Y, HELP_NAV_W, HELP_NAV_H, ">", helpPage + 1 < HELP_PAGE_COUNT ? UI_WHITE : UI_TRACK, UI_INK, 3);
  flushStampedFrame();
}

void openHelp() {
  helpPage = 0;
  helpOpen = true;
  clockOpen = false;
  helpDirty = true;
  lockTouchBrief();
  sfxPlay(SFX_MENU);
}

void helpTap(int16_t x, int16_t y) {
  bool prev = x < CX, next = x >= CX;  // toda la mitad de la pantalla vale como flecha
  if (y >= HELP_NAV_Y && y < HELP_NAV_Y + HELP_NAV_H) {
    prev = x >= HELP_PREV_X && x < HELP_PREV_X + HELP_NAV_W;
    next = x >= HELP_NEXT_X && x < HELP_NEXT_X + HELP_NAV_W;
  }
  if (next && helpPage + 1 < HELP_PAGE_COUNT) { helpPage++; helpDirty = true; sfxPlay(SFX_MENU); }
  else if (prev && helpPage > 0) { helpPage--; helpDirty = true; sfxPlay(SFX_MENU); }
}

// ---------- ajustes: menu de paginas con botones grandes ----------
#define SET_ITEM_X 83
#define SET_ITEM_W 300
#define SET_ITEM_H 44
#define SET_ITEM_Y0 74
#define SET_ITEM_STEP 47
#define SET_ITEMS 7
#define SET_BIG_X 83
#define SET_BIG_W 300
#define SET_BIG_H 56

static void setButton(int x, int y, int w, int h, const char *label, uint16_t fill, uint16_t ink, uint8_t size) {
  gfx->fillRoundRect(x, y, w, h, 14, fill);
  gfx->drawRoundRect(x, y, w, h, 14, UI_INK);
  gfx->setTextSize(size);
  gfx->setTextColor(ink);
  int cw = 6 * size;
  char buf[40];
  snprintf(buf, sizeof(buf), "%.*s", (w - 8) / cw, label);
  gfx->setCursor(x + (w - (int)strlen(buf) * cw) / 2, y + (h - 8 * size) / 2 + 1);
  gfx->print(buf);
}

static void setTitle(const char *t, uint8_t size, int y) {
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(size);
  gfx->setCursor(CX - (int)strlen(t) * 3 * size, y);
  gfx->print(t);
}

static const char *settingsItemLabel(uint8_t i, char *buf, size_t len) {
  switch (i) {
    case 0: return T(S_SET_TIME);
    case 1: return T(S_SET_SOUNDSCREEN);
    case 2: return T(S_WIFI);
    case 3: snprintf(buf, len, "%s%s", T(S_UPDATE), otaUpdateAvailable() ? " !" : ""); return buf;
    case 4: return "GPS";
    case 5: return HELP_WORD[gLang < LANG_COUNT ? gLang : LANG_EN];
    default: return "INFO";
  }
}

static void renderSettingsMenu() {
  setTitle(T(S_SETTINGS), 3, 30);
  char buf[24];
  for (uint8_t i = 0; i < SET_ITEMS; i++) {
    bool hot = i == 3 && otaUpdateAvailable();
    setButton(SET_ITEM_X, SET_ITEM_Y0 + i * SET_ITEM_STEP, SET_ITEM_W, SET_ITEM_H,
              settingsItemLabel(i, buf, sizeof(buf)), hot ? UI_BAR_WARN : UI_WHITE, UI_INK, 2);
  }
}

// pagina Ora: hora manual, fuso orario y hora legal (la hora de internet usa los dos)
#define TIME_TZ_Y 204
#define TIME_DST_Y 254
#define TIME_OK_Y 318
static void renderSettingsTime() {
  setTitle(T(S_SET_TIME), 2, 30);
  char t[8];
  snprintf(t, sizeof(t), "%02d:%02d", clockH, clockM);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(6);
  gfx->setCursor(CX - 90, 54);
  gfx->print(t);
  drawClockBtn(104, 116, "-");  // hora -
  drawClockBtn(170, 116, "+");  // hora +
  drawClockBtn(252, 116, "-");  // min -
  drawClockBtn(318, 116, "+");  // min +
  gfx->setTextSize(1);
  gfx->setTextColor(NET_DIM_INK);
  gfx->setCursor(121, 184);
  gfx->print(T(S_HOUR));
  gfx->setCursor(275, 184);
  gfx->print(T(S_MIN));
  uint8_t tz = netTimeZone();
  char tzl[32];
  snprintf(tzl, sizeof(tzl), "<  %s  >", tz == TZ_OFF ? T(S_TZ_MANUAL) : TZ_ZONES[tz].label);
  setButton(66, TIME_TZ_Y, 334, 44, tzl, UI_WHITE, UI_INK, 2);
  uint8_t dm = netDstMode();
  const char *dl = dm == DSTM_ON ? T(S_DST_ON) : dm == DSTM_OFF ? T(S_DST_OFF) : T(S_DST_AUTO);
  setButton(66, TIME_DST_Y, 334, 44, dl, tz == TZ_OFF ? UI_BG_DAY : UI_WHITE, tz == TZ_OFF ? UI_TRACK : UI_INK, 2);
  // con la hora de internet activa, el ajuste manual se corrige en la siguiente sincronizacion
  if (tz != TZ_OFF) {
    char sl[48];
    uint32_t ls = netLastTimeSync();
    if (ls) snprintf(sl, sizeof(sl), T(S_TZ_SYNCED_FMT), (unsigned)((ls / 3600) % 24), (unsigned)((ls / 60) % 60));
    else snprintf(sl, sizeof(sl), "%s", T(S_TZ_WAIT));
    gfx->setTextSize(1);
    gfx->setTextColor(NET_DIM_INK);
    gfx->setCursor(CX - (int)strlen(sl) * 3, 304);
    gfx->print(sl);
  }
  setButton(133, TIME_OK_Y, 200, 52, T(S_OK), UI_BAR_OK, UI_BG_DAY, 3);
}

static void renderSettingsSound() {
  setTitle(T(S_SET_SOUNDSCREEN), 2, 44);
  uint8_t snd = audioMode();
  setButton(SET_BIG_X, 100, SET_BIG_W, SET_BIG_H, soundModeLabel(), snd ? UI_BAR_OK : UI_WHITE, snd ? UI_BG_DAY : UI_INK, 2);
  setButton(SET_BIG_X, 172, SET_BIG_W, SET_BIG_H, powerSaveLabel(), powerSave ? UI_BAR_WARN : UI_WHITE, UI_INK, 2);
  char lp[32];
  snprintf(lp, sizeof(lp), "%s: %s  >", T(S_LANG_LABEL), LANG_CODES[gLang]);
  setButton(SET_BIG_X, 244, SET_BIG_W, SET_BIG_H, lp, UI_WHITE, UI_INK, 2);
}

static void renderSettingsInfo() {
  setTitle("INFO", 3, 40);
  char l[6][48];
  snprintf(l[0], sizeof(l[0]), "FW v%s", FW_VERSION);
  snprintf(l[1], sizeof(l[1]), "SAVE %s", pet.saveLoadedFromNvs ? T(S_OK) : T(S_STATUS_NEW));
  snprintf(l[2], sizeof(l[2]), "SD %s  SPR %s", sdReady ? "OK" : "NO", (pmd.loaded || mon.loaded) ? T(S_OK) : T(S_STATUS_FLASH_NO));
  if (pet.isEgg()) snprintf(l[3], sizeof(l[3]), "PET %s", T(S_EGG_HDR));
  else {
    char level[12];
    snprintf(level, sizeof(level), T(S_LEVEL_SHORT_FMT), pet.level());
    snprintf(l[3], sizeof(l[3]), "PET #%d %s", pet.speciesId, level);
  }
  snprintf(l[4], sizeof(l[4]), "SAVE %lums", (unsigned long)gPetLastSaveMs);
  snprintf(l[5], sizeof(l[5]), "LOOP MAX %lums %s", (unsigned long)perfLoopMaxMs, perfSlowMs ? perfSlowName : "");
  for (int i = 0; i < 6; i++) {
    gfx->setTextColor(i >= 4 ? NET_DIM_INK : UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - (int)strlen(l[i]) * 6, 100 + i * 36);
    gfx->print(l[i]);
  }
}

// pagina GPS (placa -G): estado, coordenadas, satelites y hora UTC del modulo
static void renderSettingsGps() {
  setTitle("GPS", 3, 40);
  const GpsInfo &g = gpsInfo();
  const char *st;
  uint16_t col;
  if (!gpsPresent()) { st = T(S_GPS_NONE); col = UI_BAR_BAD; }
  else if (g.fix) { st = T(S_GPS_FIX); col = UI_BAR_OK; }
  else { st = T(S_GPS_SEARCHING); col = UI_BAR_WARN; }
  gfx->setTextColor(col);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(st) * 6, 84);
  gfx->print(st);
  if (!gpsPresent()) return;
  char l[5][32];
  int n = 0;
  if (g.fix) {
    snprintf(l[n++], sizeof(l[0]), "LAT %.5f %c", g.lat < 0 ? -g.lat : g.lat, g.lat < 0 ? 'S' : 'N');
    snprintf(l[n++], sizeof(l[0]), "LON %.5f %c", g.lon < 0 ? -g.lon : g.lon, g.lon < 0 ? 'W' : 'E');
    snprintf(l[n++], sizeof(l[0]), "ALT %.0f m", g.altM);
  }
  snprintf(l[n++], sizeof(l[0]), "SAT %u / %u", g.satsUsed, g.satsView);
  if (g.timeValid) snprintf(l[n++], sizeof(l[0]), "UTC %02u:%02u:%02u", g.hh, g.mm, g.ss);
  for (int i = 0; i < n; i++) {
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - (int)strlen(l[i]) * 6, 124 + i * 34);
    gfx->print(l[i]);
  }
  if (!g.fix) {
    const char *hint = T(S_GPS_HINT);
    gfx->setTextColor(NET_DIM_INK);
    gfx->setTextSize(1);
    gfx->setCursor(CX - (int)strlen(hint) * 3, 330);
    gfx->print(hint);
  }
}

void renderClock() {
  clockDirty = false;
  gfx->fillScreen(UI_BG_DAY);
  switch (setPage) {
    case SETP_TIME: renderSettingsTime(); break;
    case SETP_SOUND: renderSettingsSound(); break;
    case SETP_INFO: renderSettingsInfo(); break;
    case SETP_GPS: renderSettingsGps(); break;
    default: renderSettingsMenu(); break;
  }
  flushStampedFrame();
}

static void openSettingsPage(uint8_t page) {
  if (page == SETP_TIME) {
    uint32_t e = pet.lastSeenEpoch ? pet.lastSeenEpoch : rtcEpoch();
    clockH = (e / 3600) % 24;
    clockM = (e / 60) % 60;
  }
  setPage = page;
  clockDirty = true;
  lockTouchBrief();
  sfxPlay(SFX_MENU);
}

void clockTap(int16_t x, int16_t y) {
  switch (setPage) {
    case SETP_MENU: {
      if (x < SET_ITEM_X || x >= SET_ITEM_X + SET_ITEM_W || y < SET_ITEM_Y0) return;
      int i = (y - SET_ITEM_Y0) / SET_ITEM_STEP;
      if (i >= SET_ITEMS || y > SET_ITEM_Y0 + i * SET_ITEM_STEP + SET_ITEM_H + 3) return;
      switch (i) {
        case 0: openSettingsPage(SETP_TIME); break;
        case 1: openSettingsPage(SETP_SOUND); break;
        case 2: openNet(NETP_WIFI); break;
        case 3: openNet(NETP_UPDATE); break;
        case 4: openSettingsPage(SETP_GPS); break;
        case 5: openHelp(); break;
        default: openSettingsPage(SETP_INFO); break;
      }
      return;
    }
    case SETP_TIME:
      if (y >= TIME_TZ_Y && y < TIME_TZ_Y + 44 && x >= 66 && x < 400) {  // fuso: izquierda/derecha
        uint8_t tz = netTimeZone();
        netSetTimeZone(x < CX ? (tz + TZ_COUNT - 1) % TZ_COUNT : (tz + 1) % TZ_COUNT);
        clockDirty = true;
        sfxPlay(SFX_TAP);
        return;
      }
      if (y >= TIME_DST_Y && y < TIME_DST_Y + 44 && x >= 66 && x < 400 && netTimeZone() != TZ_OFF) {
        netSetDstMode((netDstMode() + 1) % DSTM_COUNT);
        clockDirty = true;
        sfxPlay(SFX_TAP);
        return;
      }
      if (y >= 116 && y <= 174) {  // fila de botones +/-
        if (x >= 104 && x < 162) clockH = (clockH + 23) % 24;
        else if (x >= 170 && x < 228) clockH = (clockH + 1) % 24;
        else if (x >= 252 && x < 310) clockM = (clockM + 59) % 60;
        else if (x >= 318 && x < 376) clockM = (clockM + 1) % 60;
        else return;
        clockDirty = true;
        sfxPlay(SFX_TAP);
        return;
      }
      if (x >= 133 && x < 333 && y >= TIME_OK_Y && y < TIME_OK_Y + 52) { applyClock(); sfxPlay(SFX_MENU); }
      return;
    case SETP_SOUND:
      if (x < SET_BIG_X || x >= SET_BIG_X + SET_BIG_W) return;
      if (y >= 100 && y < 100 + SET_BIG_H) {
        audioSetMode(nextSoundMode());
        clockDirty = true;
        if (audioEnabled()) sfxPlay(SFX_LEVEL);  // confirma el nuevo modo si no esta apagado
      } else if (y >= 172 && y < 172 + SET_BIG_H) {
        setPowerSave(!powerSave);
        clockDirty = true;
        sfxPlay(powerSave ? SFX_MENU : SFX_TAP);
      } else if (y >= 244 && y < 244 + SET_BIG_H) {
        setLang((Lang)((gLang + 1) % LANG_COUNT));
        clockDirty = true;
        sfxPlay(SFX_TAP);
      }
      return;
    default:
      return;
  }
}

// ---------- Wi-Fi y actualizaciones (desde la pantalla de ajustes) ----------
// netOpen es una capa sobre clockOpen: al cerrar se vuelve a los ajustes y todas
// las comprobaciones de "pantalla de ajustes abierta" siguen valiendo.

#define NET_DIM 0x8410
#define NET_ROWS_SAVED 4
#define NET_ROW0_Y 164
#define NET_ROW_STEP 49
#define NET_ROW_H 44
#define NET_TOP_Y 90
#define NET_ROWS_SCAN 5
#define NET_SCAN_Y0 84
#define NET_NAV_Y 336

static const char *const PW_KEYS[3][4] = {
  { "1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm._@" },
  { "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM._@" },
  { "!\"#$%&'()*", "+,-./:;<=>", "?@[\\]^_`{|", "}~        " },
};
#define PW_X 33
#define PW_Y 132
#define PW_KW 40
#define PW_KH 46
#define PW_BOT_Y 322
#define PW_BOT_H 44

static bool inBox(int16_t x, int16_t y, int bx, int by, int bw, int bh) {
  return x >= bx && x < bx + bw && y >= by && y < by + bh;
}

static void netPill(int x, int y, int w, int h, const char *label, uint16_t fill, uint16_t ink, uint8_t size) {
  gfx->fillRoundRect(x, y, w, h, 10, fill);
  gfx->drawRoundRect(x, y, w, h, 10, UI_INK);
  gfx->setTextSize(size);
  gfx->setTextColor(ink);
  int cw = 6 * size;
  int maxc = (w - 6) / cw;
  char buf[48];
  snprintf(buf, sizeof(buf), "%.*s", maxc, label);
  gfx->setCursor(x + (w - (int)strlen(buf) * cw) / 2, y + (h - 8 * size) / 2 + 1);
  gfx->print(buf);
}

// texto centrado, recortado a maxW pixeles
static void netText(int y, const char *s, uint16_t col, uint8_t size, int maxW = 360) {
  int cw = 6 * size;
  char buf[64];
  snprintf(buf, sizeof(buf), "%.*s", maxW / cw, s);
  gfx->setTextSize(size);
  gfx->setTextColor(col);
  gfx->setCursor(CX - (int)strlen(buf) * cw / 2, y);
  gfx->print(buf);
}

static void netBars(int x, int y, int8_t rssi, uint16_t col) {
  int bars = rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : 1;
  for (int i = 0; i < 4; i++) {
    int h = 5 + i * 4;
    if (i < bars) gfx->fillRect(x + i * 7, y + 18 - h, 5, h, col);
    else gfx->drawRect(x + i * 7, y + 18 - h, 5, h, UI_TRACK);
  }
}

void openNet(uint8_t page) {
  netOpen = true;
  netPage = page;
  netDirty = true;
  netForgetIdx = -1;
  netListPage = 0;
  netScanPage = 0;
  lockTouchBrief();
  sfxPlay(SFX_MENU);
  if (page == NETP_UPDATE && !netBusy() && netSaved().count) {
    OtaStatus s = otaStatus();
    if (s == OTA_IDLE || s == OTA_ERROR || s == OTA_UP_TO_DATE || s == OTA_SPRITES_DONE) otaStartCheck(true);
  }
}

void closeNet() {
  OtaStatus s = otaStatus();
  if (s == OTA_SPRITES || s == OTA_FIRMWARE || s == OTA_REBOOT) return;  // no salir a medias
  if (netPage == NETP_PASS || netPage == NETP_SCAN) {  // volver a la lista de redes
    memset(pwBuf, 0, sizeof(pwBuf));
    pwLen = 0;
    netPage = NETP_WIFI;
    netDirty = true;
    lockTouchBrief();
    sfxPlay(SFX_TAP);
    return;
  }
  netOpen = false;
  netClearJoin();
  clockDirty = true;
  lockTouchBrief();
  sfxPlay(SFX_TAP);
}

void openPassword(const char *ssid) {
  snprintf(pwSsid, sizeof(pwSsid), "%s", ssid);
  memset(pwBuf, 0, sizeof(pwBuf));
  int idx = netListFind(netSaved(), ssid);
  if (idx >= 0) snprintf(pwBuf, sizeof(pwBuf), "%s", netSaved().e[idx].pass);
  pwLen = strlen(pwBuf);
  pwMode = 0;
  netPage = NETP_PASS;
  netDirty = true;
  lockTouchBrief();
  sfxPlay(SFX_MENU);
}

static void renderNetWifi() {
  netText(30, T(S_WIFI), UI_INK, 3);
  const NetList &l = netSaved();
  char line[48];
  uint16_t col = UI_INK;
  switch (netJoinStatus()) {
    case JOIN_BUSY: snprintf(line, sizeof(line), "%s", T(S_NET_CONNECTING)); col = UI_BAR_WARN; break;
    case JOIN_OK: snprintf(line, sizeof(line), "%s", T(S_NET_JOIN_OK)); col = UI_BAR_OK; break;
    case JOIN_WRONG_PASS: snprintf(line, sizeof(line), "%s", T(S_NET_WRONG_PASS)); col = UI_BAR_BAD; break;
    case JOIN_NOT_FOUND: snprintf(line, sizeof(line), "%s", T(S_NET_JOIN_NOTFOUND)); col = UI_BAR_BAD; break;
    case JOIN_FAILED: snprintf(line, sizeof(line), "%s", T(S_NET_JOIN_FAIL)); col = UI_BAR_BAD; break;
    default: snprintf(line, sizeof(line), T(S_NET_SAVED_FMT), (unsigned)l.count, (unsigned)NET_MAX); break;
  }
  netText(64, line, col, 2);

  bool on = netAutoCheck();
  netPill(66, NET_TOP_Y, 160, 44, on ? T(S_NET_AUTO_ON) : T(S_NET_AUTO_OFF), on ? UI_BAR_OK : UI_WHITE, on ? UI_BG_DAY : UI_INK, 2);
  netPill(240, NET_TOP_Y, 160, 44, T(S_NET_SEARCH), netBusy() ? UI_TRACK : UI_WHITE, UI_INK, 2);
  // estado de la hora de internet (el fuso se elige en Ajustes > Ora)
  uint8_t tz = netTimeZone();
  char syncLine[48];
  uint32_t ls = netLastTimeSync();
  if (tz == TZ_OFF) snprintf(syncLine, sizeof(syncLine), "%s", T(S_TZ_MANUAL));
  else if (ls) snprintf(syncLine, sizeof(syncLine), T(S_TZ_SYNCED_FMT), (unsigned)((ls / 3600) % 24), (unsigned)((ls / 60) % 60));
  else snprintf(syncLine, sizeof(syncLine), "%s", T(S_TZ_WAIT));
  netText(140, T(S_NET_AUTO_HINT), NET_DIM, 1);
  netText(150, syncLine, NET_DIM, 1);

  int order[NET_MAX];
  int n = netListOrder(l, order);
  if (!n) {
    netText(240, T(S_NET_NONE), UI_INK, 2);
    netText(270, T(S_NET_TAP_SEARCH), NET_DIM, 1);
  }
  int pages = (n + NET_ROWS_SAVED - 1) / NET_ROWS_SAVED;
  if (netListPage >= pages) netListPage = pages ? pages - 1 : 0;
  for (int r = 0; r < NET_ROWS_SAVED; r++) {
    int k = netListPage * NET_ROWS_SAVED + r;
    if (k >= n) break;
    int idx = order[k];
    int y = NET_ROW0_Y + r * NET_ROW_STEP;
    bool pending = netForgetIdx == idx;
    gfx->fillRoundRect(58, y, 350, NET_ROW_H, 10, UI_WHITE);
    gfx->drawRoundRect(58, y, 350, NET_ROW_H, 10, pending ? UI_BAR_BAD : UI_INK);
    gfx->setTextSize(2);
    gfx->setTextColor(pending ? UI_BAR_BAD : (k == 0 ? UI_BAR_OK : UI_INK));
    gfx->setCursor(70, y + 15);
    char s[24];
    snprintf(s, sizeof(s), "%.22s", pending ? T(S_NET_FORGET_Q) : l.e[idx].ssid);
    gfx->print(s);
    gfx->fillRoundRect(348, y + 4, 54, NET_ROW_H - 8, 8, pending ? UI_BAR_BAD : UI_TRACK);
    gfx->setTextColor(pending ? UI_WHITE : UI_INK);
    gfx->setCursor(369, y + 15);
    gfx->print("X");
  }
  if (pages > 1) {
    netPill(100, 366, 64, 44, "<", netListPage ? UI_WHITE : UI_TRACK, UI_INK, 2);
    netPill(302, 366, 64, 44, ">", netListPage + 1 < pages ? UI_WHITE : UI_TRACK, UI_INK, 2);
    char pg[12];
    snprintf(pg, sizeof(pg), "%u/%u", (unsigned)(netListPage + 1), (unsigned)pages);
    netText(381, pg, UI_INK, 2);
  }
}

static void netWifiTap(int16_t x, int16_t y) {
  if (inBox(x, y, 66, NET_TOP_Y, 160, 44)) {
    netSetAutoCheck(!netAutoCheck());
    sfxPlay(SFX_TAP);
    return;
  }
  if (inBox(x, y, 240, NET_TOP_Y, 160, 44)) {
    if (netBusy()) { sfxPlay(SFX_DENY); return; }
    netClearJoin();
    netPage = NETP_SCAN;
    netScanPage = 0;
    netDirty = true;
    netStartScan();
    sfxPlay(SFX_MENU);
    return;
  }
  const NetList &l = netSaved();
  int order[NET_MAX];
  int n = netListOrder(l, order);
  int pages = (n + NET_ROWS_SAVED - 1) / NET_ROWS_SAVED;
  if (pages > 1 && y >= 366 && y < 410) {
    if (x >= 100 && x < 164 && netListPage > 0) { netListPage--; netForgetIdx = -1; netDirty = true; sfxPlay(SFX_MENU); }
    if (x >= 302 && x < 366 && netListPage + 1 < pages) { netListPage++; netForgetIdx = -1; netDirty = true; sfxPlay(SFX_MENU); }
    return;
  }
  for (int r = 0; r < NET_ROWS_SAVED; r++) {
    int k = netListPage * NET_ROWS_SAVED + r;
    if (k >= n) break;
    int idx = order[k];
    int ry = NET_ROW0_Y + r * NET_ROW_STEP;
    if (!inBox(x, y, 58, ry, 350, NET_ROW_H)) continue;
    if (netBusy()) { sfxPlay(SFX_DENY); return; }
    if (x >= 340) {  // X: primer toque pide confirmacion, el segundo borra
      if (netForgetIdx == idx) {
        netForget(idx);
        netForgetIdx = -1;
        sfxPlay(SFX_TAP);
      } else {
        netForgetIdx = idx;
        sfxPlay(SFX_DENY);
      }
      netDirty = true;
      return;
    }
    netForgetIdx = -1;
    openPassword(l.e[idx].ssid);  // cambiar la contrasena guardada
    return;
  }
  if (netForgetIdx >= 0) { netForgetIdx = -1; netDirty = true; }
}

static void renderNetScan() {
  netText(26, T(S_NET_NETWORKS), UI_INK, 3);
  NetScanStatus st = netScanStatus();
  int n = netScanCount();
  if (st == SCAN_BUSY) netText(58, T(S_NET_SCANNING), UI_BAR_WARN, 2);
  else if (n == 0) netText(58, T(S_NET_NOTFOUND), UI_BAR_BAD, 2);
  int pages = (n + NET_ROWS_SCAN - 1) / NET_ROWS_SCAN;
  if (netScanPage >= pages) netScanPage = pages ? pages - 1 : 0;
  if (st != SCAN_BUSY) {
    for (int r = 0; r < NET_ROWS_SCAN; r++) {
      int k = netScanPage * NET_ROWS_SCAN + r;
      if (k >= n) break;
      const NetScanItem &it = netScanItem(k);
      int y = NET_SCAN_Y0 + r * NET_ROW_STEP;
      gfx->fillRoundRect(58, y, 350, NET_ROW_H, 10, UI_WHITE);
      gfx->drawRoundRect(58, y, 350, NET_ROW_H, 10, UI_INK);
      gfx->setTextSize(2);
      gfx->setTextColor(it.saved ? UI_BAR_OK : UI_INK);
      gfx->setCursor(68, y + 15);
      char s[24];
      snprintf(s, sizeof(s), "%.20s", it.ssid);
      gfx->print(s);
      if (it.open) {  // red abierta: sin contrasena
        gfx->setTextSize(1);
        gfx->setTextColor(NET_DIM);
        gfx->setCursor(320, y + 19);
        gfx->print("OPEN");
      }
      netBars(372, y + 12, it.rssi, it.saved ? UI_BAR_OK : UI_INK);
    }
  }
  netPill(86, NET_NAV_Y, 64, 44, "<", netScanPage ? UI_WHITE : UI_TRACK, UI_INK, 2);
  netPill(160, NET_NAV_Y, 146, 44, T(S_NET_SEARCH), netBusy() ? UI_TRACK : UI_WHITE, UI_INK, 2);
  netPill(316, NET_NAV_Y, 64, 44, ">", netScanPage + 1 < pages ? UI_WHITE : UI_TRACK, UI_INK, 2);
}

static void netScanTap(int16_t x, int16_t y) {
  int n = netScanCount();
  int pages = (n + NET_ROWS_SCAN - 1) / NET_ROWS_SCAN;
  if (y >= NET_NAV_Y && y < NET_NAV_Y + 44) {
    if (x >= 86 && x < 150 && netScanPage > 0) { netScanPage--; netDirty = true; sfxPlay(SFX_MENU); }
    else if (x >= 316 && x < 380 && netScanPage + 1 < pages) { netScanPage++; netDirty = true; sfxPlay(SFX_MENU); }
    else if (x >= 160 && x < 306) {
      if (netStartScan()) { netScanPage = 0; sfxPlay(SFX_MENU); }
      else sfxPlay(SFX_DENY);
    }
    return;
  }
  if (netScanStatus() == SCAN_BUSY || netBusy()) return;
  for (int r = 0; r < NET_ROWS_SCAN; r++) {
    int k = netScanPage * NET_ROWS_SCAN + r;
    if (k >= n) break;
    if (!inBox(x, y, 58, NET_SCAN_Y0 + r * NET_ROW_STEP, 350, NET_ROW_H)) continue;
    const NetScanItem &it = netScanItem(k);
    if (it.open) {  // sin contrasena: probar directamente
      netStartJoin(it.ssid, "");
      netPage = NETP_WIFI;
      netDirty = true;
      sfxPlay(SFX_MENU);
    } else {
      openPassword(it.ssid);
    }
    return;
  }
}

static void renderNetPass() {
  netText(34, pwSsid, UI_INK, 2);
  netText(62, T(S_NET_PASSWORD), NET_DIM, 1);
  gfx->fillRoundRect(63, 78, 340, 42, 9, UI_WHITE);
  gfx->drawRoundRect(63, 78, 340, 42, 9, UI_INK);
  gfx->setTextSize(2);
  gfx->setTextColor(UI_INK);
  gfx->setCursor(73, 91);
  const char *shown = pwLen > 25 ? pwBuf + (pwLen - 25) : pwBuf;  // se ve el final
  gfx->print(shown);
  gfx->print("_");

  for (int r = 0; r < 4; r++) {
    const char *row = PW_KEYS[pwMode][r];
    for (int c = 0; c < 10; c++) {
      char ch = row[c];
      int x = PW_X + c * PW_KW, y = PW_Y + r * PW_KH;
      if (ch == ' ') continue;
      gfx->fillRoundRect(x + 2, y + 2, PW_KW - 4, PW_KH - 4, 6, UI_WHITE);
      gfx->drawRoundRect(x + 2, y + 2, PW_KW - 4, PW_KH - 4, 6, UI_INK);
      gfx->setTextSize(2);
      gfx->setTextColor(UI_INK);
      gfx->setCursor(x + PW_KW / 2 - 5, y + PW_KH / 2 - 7);
      gfx->print(ch);
    }
  }
  netPill(53, PW_BOT_Y, 60, PW_BOT_H, pwMode == 1 ? "abc" : "ABC", pwMode == 1 ? UI_BAR_WARN : UI_WHITE, UI_INK, 2);
  netPill(117, PW_BOT_Y, 60, PW_BOT_H, pwMode == 2 ? "abc" : "#+=", pwMode == 2 ? UI_BAR_WARN : UI_WHITE, UI_INK, 2);
  netPill(181, PW_BOT_Y, 104, PW_BOT_H, "___", UI_WHITE, UI_INK, 2);
  netPill(289, PW_BOT_Y, 60, PW_BOT_H, "<-", UI_BAR_WARN, UI_INK, 2);
  netPill(353, PW_BOT_Y, 60, PW_BOT_H, "OK", UI_BAR_OK, UI_BG_DAY, 2);
}

static void netPassTap(int16_t x, int16_t y) {
  if (y >= PW_Y && y < PW_Y + 4 * PW_KH && x >= PW_X && x < PW_X + 10 * PW_KW) {
    char ch = PW_KEYS[pwMode][(y - PW_Y) / PW_KH][(x - PW_X) / PW_KW];
    if (ch == ' ') return;
    if (pwLen < NET_PASS_LEN) {
      pwBuf[pwLen++] = ch;
      pwBuf[pwLen] = 0;
      sfxPlay(SFX_TAP);
    } else {
      sfxPlay(SFX_DENY);
    }
    netDirty = true;
    return;
  }
  if (y < PW_BOT_Y || y >= PW_BOT_Y + PW_BOT_H) return;
  if (x >= 53 && x < 113) { pwMode = pwMode == 1 ? 0 : 1; netDirty = true; sfxPlay(SFX_TAP); }
  else if (x >= 117 && x < 177) { pwMode = pwMode == 2 ? 0 : 2; netDirty = true; sfxPlay(SFX_TAP); }
  else if (x >= 181 && x < 285) {
    if (pwLen < NET_PASS_LEN) { pwBuf[pwLen++] = ' '; pwBuf[pwLen] = 0; }
    netDirty = true;
    sfxPlay(SFX_TAP);
  } else if (x >= 289 && x < 349) {
    if (pwLen) pwBuf[--pwLen] = 0;
    netDirty = true;
    sfxPlay(SFX_TAP);
  } else if (x >= 353 && x < 413) {
    // WPA pide al menos 8 caracteres; una red abierta va sin contrasena
    if ((pwLen && pwLen < 8) || !netStartJoin(pwSsid, pwBuf)) { sfxPlay(SFX_DENY); return; }
    memset(pwBuf, 0, sizeof(pwBuf));
    pwLen = 0;
    netPage = NETP_WIFI;
    netListPage = 0;
    netDirty = true;
    lockTouchBrief();
    sfxPlay(SFX_MENU);
  }
}

static StrId otaErrorText(OtaError e) {
  switch (e) {
    case OTA_ERR_NO_NETWORK: return S_UPD_ERR_NONET;
    case OTA_ERR_NO_RELEASE: return S_UPD_ERR_NOREL;
    case OTA_ERR_BAD_FILE: return S_UPD_ERR_BAD;
    case OTA_ERR_SD: return S_UPD_ERR_SD;
    case OTA_ERR_SD_FULL: return S_UPD_ERR_SDFULL;
    case OTA_ERR_FLASH: return S_UPD_ERR_FLASH;
    default: return S_UPD_ERR_DOWNLOAD;
  }
}

// boton principal de la pagina de actualizacion: 0 ninguno, 1 buscar, 2 instalar, 3 abrir Wi-Fi
static uint8_t netUpdateAction() {
  if (netBusy()) return 0;
  OtaStatus s = otaStatus();
  if (s == OTA_REBOOT) return 0;
  if (!netSaved().count) return 3;
  if (s == OTA_AVAILABLE) return 2;
  return 1;
}

static void renderNetUpdate() {
  netText(36, T(S_UPDATE), UI_INK, 3);
  char line[64];
  snprintf(line, sizeof(line), T(S_UPD_CURRENT_FMT), FW_VERSION);
  netText(88, line, UI_INK, 2);
  if (otaLatest()[0]) {
    snprintf(line, sizeof(line), T(S_UPD_LATEST_FMT), otaLatest());
    netText(114, line, otaUpdateAvailable() ? UI_BAR_WARN : UI_INK, 2);
  }
  if (otaUpdateAvailable()) {
    const char *what = otaFirmwareNewer() && otaSpritesNeeded() ? "Firmware + Sprites"
                       : otaSpritesNeeded() ? "Sprites" : "Firmware";
    netText(140, what, NET_DIM, 1);
  }

  OtaStatus s = otaStatus();
  uint16_t col = UI_INK;
  line[0] = 0;
  switch (s) {
    case OTA_IDLE: if (!netSaved().count) { snprintf(line, sizeof(line), "%s", T(S_UPD_NEED_WIFI)); col = UI_BAR_WARN; } break;
    case OTA_CONNECTING: snprintf(line, sizeof(line), "%s", T(S_NET_CONNECTING)); col = UI_BAR_WARN; break;
    case OTA_CHECKING: snprintf(line, sizeof(line), "%s", T(S_UPD_CHECKING)); col = UI_BAR_WARN; break;
    case OTA_UP_TO_DATE: snprintf(line, sizeof(line), "%s", T(S_UPD_UPTODATE)); col = UI_BAR_OK; break;
    case OTA_AVAILABLE: snprintf(line, sizeof(line), "%s", T(S_UPD_AVAILABLE)); col = UI_BAR_WARN; break;
    case OTA_SPRITES: snprintf(line, sizeof(line), "Sprites %u%%", (unsigned)otaProgress()); col = UI_BAR_WARN; break;
    case OTA_FIRMWARE: snprintf(line, sizeof(line), "Firmware %u%%", (unsigned)otaProgress()); col = UI_BAR_WARN; break;
    case OTA_SPRITES_DONE: snprintf(line, sizeof(line), "%s", T(S_UPD_SPR_DONE)); col = UI_BAR_OK; break;
    case OTA_REBOOT: snprintf(line, sizeof(line), "%s", T(S_UPD_RESTARTING)); col = UI_BAR_OK; break;
    case OTA_ERROR: snprintf(line, sizeof(line), "%s", T(otaErrorText(otaError()))); col = UI_BAR_BAD; break;
  }
  if (line[0]) netText(178, line, col, 2);
  if (s == OTA_SPRITES || s == OTA_FIRMWARE) {
    gfx->fillRoundRect(83, 210, 300, 18, 8, UI_TRACK);
    gfx->fillRoundRect(83, 210, 3 * otaProgress() < 16 ? 16 : 3 * otaProgress(), 18, 8, UI_BAR_OK);
    netText(240, T(S_UPD_KEEP_ON), NET_DIM, 1);
  }
  uint8_t a = netUpdateAction();
  if (a == 1) netPill(123, 280, 220, 48, T(S_UPD_CHECK), UI_WHITE, UI_INK, 2);
  else if (a == 2) netPill(123, 280, 220, 48, T(S_UPD_INSTALL), UI_BAR_OK, UI_BG_DAY, 2);
  else if (a == 3) netPill(123, 280, 220, 48, T(S_WIFI), UI_BAR_WARN, UI_INK, 2);
}

static void netUpdateTap(int16_t x, int16_t y) {
  if (!inBox(x, y, 123, 280, 220, 48)) return;
  switch (netUpdateAction()) {
    case 1: if (otaStartCheck(true)) sfxPlay(SFX_MENU); break;
    case 2: if (otaStartInstall()) sfxPlay(SFX_LEVEL); break;
    case 3: netPage = NETP_WIFI; netDirty = true; sfxPlay(SFX_MENU); break;
    default: break;
  }
}

void renderNet() {
  netDirty = false;
  netSeenGen = netGen();
  gfx->fillScreen(UI_BG_DAY);
  switch (netPage) {
    case NETP_SCAN: renderNetScan(); break;
    case NETP_PASS: renderNetPass(); break;
    case NETP_UPDATE: renderNetUpdate(); break;
    default: renderNetWifi(); break;
  }
  flushStampedFrame();
}

void netTap(int16_t x, int16_t y) {
  switch (netPage) {
    case NETP_SCAN: netScanTap(x, y); break;
    case NETP_PASS: netPassTap(x, y); break;
    case NETP_UPDATE: netUpdateTap(x, y); break;
    default: netWifiTap(x, y); break;
  }
}

// en cada vuelta del loop: aviso, recarga de sprites y reinicio tras instalar
void netService(uint32_t now) {
  netLoop(now);
  uint32_t localEpoch;
  if (netTakeTime(&localEpoch)) {  // ora da internet: RTC y reloj del juego
    rtcSetEpoch(localEpoch);
    pet.setClock(localEpoch);
    clockDirty = true;
    if (netOpen) netDirty = true;
  }
  if (netOpen && netGen() != netSeenGen) netDirty = true;
  if (otaTakeNotice()) {
    snprintf(petEventMsg, sizeof(petEventMsg), "%s", T(S_UPD_NOTICE));
    petEventFeedbackUntil = now + 5000;
    clockDirty = true;
    if (audioEnabled()) sfxPlay(SFX_MEDAL);
  }
  if (otaTakeSpritesChanged()) {
    thumbs.unload();
    thumbs.load();
    sdDirty = true;  // ensureMon recarga el sprite del bicho
  }
  static uint32_t rebootAt = 0;
  if (otaStatus() == OTA_REBOOT) {
    if (!rebootAt) rebootAt = now + 2500;
    else if ((int32_t)(now - rebootAt) >= 0) {
      uint32_t e = rtcEpoch();
      if (e) pet.lastSeenEpoch = e;
      pet.flushSave();
      Serial.println("OTA restart");
      delay(200);
      ESP.restart();
    }
  }
}

// llama + numero de racha arriba a la izquierda
void drawStreakBadge() {
  if (pet.streak < 1) return;
  int x = 26, y = 16;
  gfx->fillTriangle(x + 8, y, x + 1, y + 17, x + 15, y + 17, UI_BAR_BAD);
  gfx->fillTriangle(x + 8, y + 7, x + 4, y + 17, x + 12, y + 17, UI_BAR_WARN);
  char s[6];
  snprintf(s, sizeof(s), "%u", pet.streak);
  gfx->setTextColor(inkColor());
  gfx->setTextSize(2);
  gfx->setCursor(x + 22, y + 2);
  gfx->print(s);
}

// banner temporal: medalla nueva o hito de racha
void drawCelebration() {
  const char *l1 = nullptr, *l2 = nullptr;
  char buf[20];
  if (pet.showMedal()) {
    for (int i = 0; i < MED_COUNT; i++)
      if (pet.newMedal & (1 << i)) { l2 = medalName(i); break; }
    l1 = T(S_MEDAL_BANNER);
  } else if (pet.showDexReward()) {
    l1 = T(S_NEW_FRAME);
    l2 = T((StrId)(S_RANK_TRAINER + pet.collectionRank()));
  } else if (pet.showMilestone()) {
    snprintf(buf, sizeof(buf), T(S_STREAK_DAYS_FMT), pet.streak);
    l1 = T(S_GREAT);
    l2 = buf;
  } else if (pet.showStepReward()) {
    l1 = T(S_STEP_REWARD);
    if (pet.lastStepReward() == STEP_REWARD_TRAIL_RANK) {
      snprintf(buf, sizeof(buf), T(S_TRAIL_RANK_FMT), pet.stepTrailRank());
      l2 = buf;
    } else if (pet.lastStepReward() == STEP_REWARD_ENERGY) {
      l2 = T(S_ITEM_ENERGY);
    } else if (pet.lastStepReward() == STEP_REWARD_TRAIN) {
      l2 = T(S_ITEM_TRAIN);
    } else {
      l2 = T(S_ITEM_SNACK);
    }
  }
  if (!l1) return;
  gfx->fillRoundRect(73, 150, 320, 96, 16, UI_BAR_WARN);
  gfx->drawRoundRect(73, 150, 320, 96, 16, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(l1) * 9, 176);
  gfx->print(l1);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(l2) * 6, 212);
  gfx->print(l2);
}

uint16_t collectionFrameColor(uint8_t frame) {
  static const uint16_t COLORS[] = {
    UI_TRACK, UI_BAR_WARN, UI_BAR_OK, 0x4C98, UI_BAR_BAD, 0xF3B7, 0x7B5C,
    0x7BEF, 0xD452, 0xFD20, 0x051D,
  };
  return COLORS[frame < sizeof(COLORS) / sizeof(COLORS[0]) ? frame : 0];
}

void drawCaughtBattleMarker(int x, int y) {
  // Gleicher 16er-Pokeball wie im Minispiel, 2x neben den Typ-Chips (32 px).
  drawMap(SPR_ICON_PLAY, 16, x, y, 2, false);
}

static void drawFrameCorner(int x, int y, int sx, int sy, uint16_t color, uint8_t len) {
  gfx->drawLine(x, y, x + sx * len, y, color);
  gfx->drawLine(x, y, x, y + sy * len, color);
}

void drawCollectionFrame(uint8_t frame) {
  // Rahmen liegen hinter dem Portrait: Der Sprite bleibt voll sichtbar und
  // die frueheren ueberlagernden Kreise koennen nicht wieder auftauchen.
  if (frame >= pet.unlockedCollectionFrameCount()) frame = 0;
  const int left = 108, right = 358, top = 94, bottom = 302;
  uint16_t color = collectionFrameColor(frame);
  uint8_t len = 22 + frame * 3;
  drawFrameCorner(left, top, 1, 1, color, len);
  drawFrameCorner(right, top, -1, 1, color, len);
  drawFrameCorner(left, bottom, 1, -1, color, len);
  drawFrameCorner(right, bottom, -1, -1, color, len);

  if (frame >= 1) {
    uint16_t accent = frame == 4 ? UI_BAR_WARN : color;
    gfx->fillCircle(left + len + 7, top, 3, accent);
    gfx->fillCircle(right - len - 7, top, 3, accent);
    gfx->fillCircle(left + len + 7, bottom, 3, accent);
    gfx->fillCircle(right - len - 7, bottom, 3, accent);
  }
  if (frame >= 2) {
    uint8_t inner = len - 6;
    drawFrameCorner(left + 6, top + 6, 1, 1, color, inner);
    drawFrameCorner(right - 6, top + 6, -1, 1, color, inner);
    drawFrameCorner(left + 6, bottom - 6, 1, -1, color, inner);
    drawFrameCorner(right - 6, bottom - 6, -1, -1, color, inner);
  }
  if (frame >= 3) {
    gfx->fillRect(CX - 3, top - 3, 6, 6, color);
    gfx->fillRect(CX - 3, bottom - 3, 6, 6, color);
  }
  if (frame >= 4) {
    gfx->drawLine(left + 2, top + len + 10, left + 10, top + len + 2, UI_BAR_WARN);
    gfx->drawLine(right - 2, top + len + 10, right - 10, top + len + 2, UI_BAR_WARN);
    gfx->drawLine(left + 2, bottom - len - 10, left + 10, bottom - len - 2, UI_BAR_WARN);
    gfx->drawLine(right - 2, bottom - len - 10, right - 10, bottom - len - 2, UI_BAR_WARN);
  }
  if (frame >= 5) {
    gfx->fillCircle(CX, top - 12, 3, UI_BAR_WARN);
    gfx->fillCircle(CX, bottom + 12, 3, UI_BAR_WARN);
    gfx->fillCircle(left - 12, CY, 3, UI_BAR_WARN);
    gfx->fillCircle(right + 12, CY, 3, UI_BAR_WARN);
  }
}

// medallas en la ficha: badge con etiqueta, color si conseguida
void drawMedalBadge(int x, int y, int i) {
  bool got = pet.hasMedal(1 << i);
  gfx->fillRoundRect(x, y, 100, 24, 6, got ? UI_BAR_OK : UI_TRACK);
  if (!got) gfx->drawRoundRect(x, y, 100, 24, 6, UI_TRACK);
  gfx->setTextColor(got ? UI_BG_DAY : 0x9492);
  gfx->setTextSize(2);
  gfx->setCursor(x + (100 - (int)strlen(medalLabel(i)) * 12) / 2, y + 5);
  gfx->print(medalLabel(i));
}

// pagina 0: perfil (retrato grande, identidad, racha, vinculo, baya)
void renderCardProfile() {
  const DexEntry &d = DEX_TBL[pet.speciesId];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  char head[26];
  snprintf(head, sizeof(head), T(S_NAME_FMT), pet.shiny ? "*" : "", nm, pet.level());
  gfx->setTextColor(d.accent);
  // auto-encoge: a tamano 3 los nombres largos no caben en la franja estrecha de
  // arriba de la pantalla redonda, asi que se cortaban por el borde
  int hlen = strlen(head);
  int hts = (hlen <= 11) ? 3 : 2;
  gfx->setTextSize(hts);
  gfx->setCursor(CX - hlen * (hts == 3 ? 9 : 6), hts == 3 ? 34 : 40);
  gfx->print(head);
  if (pet.nick[0]) {  // especie real bajo el apodo
    gfx->setTextColor(UI_TRACK);
    gfx->setTextSize(2);
    const char *speciesName = dexName(pet.speciesId);
    gfx->setCursor(CX - (strlen(speciesName) + 2) * 6, 64);
    gfx->printf("(%s)", speciesName);
  }

  drawCollectionFrame(pet.collectionFrame);
  if (pmd.loaded) drawPmdAct(PMD_IDLE, CX, 206, millis(), true, false, 4);

  // racha con llama
  int sx = 138, sy = 224;
  gfx->fillTriangle(sx + 8, sy, sx + 1, sy + 18, sx + 15, sy + 18, UI_BAR_BAD);
  gfx->fillTriangle(sx + 8, sy + 7, sx + 4, sy + 18, sx + 12, sy + 18, UI_BAR_WARN);
  char rl[30];
  snprintf(rl, sizeof(rl), T(S_STREAK_FMT), pet.streak, pet.bestStreak);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(sx + 24, sy + 2);
  gfx->print(rl);

  drawCardStat(258, T(S_VIN), pet.bond, 100, C565(0xd4, 0x52, 0x7e));

  const char *berry = !pet.berryKnown ? T(S_BERRY_UNK)
                      : pet.lovesBerry(0) ? T(S_BERRY_RED)
                      : pet.lovesBerry(1) ? T(S_BERRY_BLUE)
                                          : T(S_BERRY_GREEN);
  char info[40];
  snprintf(info, sizeof(info), T(S_INFO_FMT), berry,
           (unsigned long)(pet.ageMinutes / 1440));
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(info) * 6, 296);
  gfx->print(info);

  gfx->setTextColor(NET_DIM_INK);
  gfx->setTextSize(1);
  gfx->setCursor(CX - strlen(T(S_RENAME_HINT)) * 3, 320);
  gfx->print(T(S_RENAME_HINT));

  const char *rank = T((StrId)(S_RANK_TRAINER + pet.collectionRank()));
  char known[22], frame[22];
  snprintf(known, sizeof(known), T(S_KNOWN_FMT), pet.knownDexCount(), DEX_COUNT);
  snprintf(frame, sizeof(frame), T(S_FRAME_FMT), pet.collectionFrame + 1, pet.unlockedCollectionFrameCount());
  // cornice della collezione: frecce grandi ai lati, testo al centro
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(1);
  gfx->setCursor(CX - (int)strlen(T(S_COLLECTION)) * 3, PROFILE_FRAME_Y + 4);
  gfx->print(T(S_COLLECTION));
  gfx->setTextColor(collectionFrameColor(pet.collectionFrame));
  gfx->setCursor(CX - (int)strlen(rank) * 3, PROFILE_FRAME_Y + 18);
  gfx->print(rank);
  gfx->setTextColor(UI_INK);
  gfx->setCursor(CX - (int)strlen(frame) * 3, PROFILE_FRAME_Y + 32);
  gfx->print(frame);
  (void)known;
  bool many = pet.unlockedCollectionFrameCount() > 1;
  gfx->fillRoundRect(PROFILE_PREV_X, PROFILE_FRAME_Y, PROFILE_ARROW_W, PROFILE_ARROW_H, 12, many ? UI_WHITE : UI_BG_DAY);
  gfx->drawRoundRect(PROFILE_PREV_X, PROFILE_FRAME_Y, PROFILE_ARROW_W, PROFILE_ARROW_H, 12, many ? UI_INK : UI_TRACK);
  gfx->fillRoundRect(PROFILE_NEXT_X, PROFILE_FRAME_Y, PROFILE_ARROW_W, PROFILE_ARROW_H, 12, many ? UI_WHITE : UI_BG_DAY);
  gfx->drawRoundRect(PROFILE_NEXT_X, PROFILE_FRAME_Y, PROFILE_ARROW_W, PROFILE_ARROW_H, 12, many ? UI_INK : UI_TRACK);
  gfx->setTextColor(many ? UI_INK : UI_TRACK);
  gfx->setTextSize(3);
  gfx->setCursor(PROFILE_PREV_X + (PROFILE_ARROW_W - 18) / 2, PROFILE_FRAME_Y + 11);
  gfx->print("<");
  gfx->setCursor(PROFILE_NEXT_X + (PROFILE_ARROW_W - 18) / 2, PROFILE_FRAME_Y + 11);
  gfx->print(">");
  gfx->setTextSize(2);
}

StrId personalityNameId(PetPersonality p) {
  switch (p) {
    case PERS_PLAYFUL: return S_PERS_PLAYFUL;
    case PERS_BRAVE: return S_PERS_BRAVE;
    case PERS_CALM: return S_PERS_CALM;
    case PERS_LAZY: return S_PERS_LAZY;
    default: return S_PERS_BALANCED;
  }
}

StrId personalityHintId(PetPersonality p) {
  switch (p) {
    case PERS_PLAYFUL: return S_PERS_PLAYFUL_HINT;
    case PERS_BRAVE: return S_PERS_BRAVE_HINT;
    case PERS_CALM: return S_PERS_CALM_HINT;
    case PERS_LAZY: return S_PERS_LAZY_HINT;
    default: return S_PERS_BALANCED_HINT;
  }
}

uint16_t personalityColor(PetPersonality p) {
  switch (p) {
    case PERS_PLAYFUL: return UI_BAR_WARN;
    case PERS_BRAVE: return UI_BAR_BAD;
    case PERS_CALM: return 0x4C98;
    case PERS_LAZY: return 0xB3C8;
    default: return UI_BAR_OK;
  }
}

void drawPersonalityRecord(int x, int y, const char *label, uint16_t val, uint16_t color) {
  gfx->fillRoundRect(x, y, 118, 34, 8, UI_WHITE);
  gfx->drawRoundRect(x, y, 118, 34, 8, color);
  gfx->setTextColor(color);
  gfx->setTextSize(1);
  gfx->setCursor(x + 10, y + 6);
  gfx->print(label);
  char num[8];
  snprintf(num, sizeof(num), "%u", val);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(x + 118 - 12 - (int)strlen(num) * 12, y + 14);
  gfx->print(num);
}

// pagina 1: personalidad calculada + records largos, sin tocar balance
void renderCardPersonality() {
  PetPersonality pers = pet.personality();
  const char *title = T(S_PERSONALITY);
  const char *name = T(personalityNameId(pers));
  const char *hint = T(personalityHintId(pers));
  uint16_t col = personalityColor(pers);

  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(title) * 9, 44);
  gfx->print(title);

  gfx->fillRoundRect(62, 86, 342, 70, 16, col);
  gfx->setTextColor(UI_BG_DAY);
  int nts = (strlen(name) <= 10) ? 3 : 2;
  gfx->setTextSize(nts);
  gfx->setCursor(CX - strlen(name) * (nts == 3 ? 9 : 6), nts == 3 ? 104 : 111);
  gfx->print(name);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(hint) * 6, 136);
  gfx->print(hint);

  drawCardStat(182, T(S_VIN), pet.bond, 100, C565(0xd4, 0x52, 0x7e));
  drawCardStat(220, T(S_BAR_JOY), pet.joy, 100, UI_BAR_WARN);

  char age[20];
  snprintf(age, sizeof(age), T(S_AGE_DAYS_FMT), (unsigned long)(pet.ageMinutes / 1440));
  gfx->setTextColor(NET_DIM_INK);
  gfx->setTextSize(1);
  gfx->setCursor(CX - strlen(age) * 3, 250);  // piccolo, sopra RECORDS (prima si sovrapponeva)
  gfx->print(age);

  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_RECORDS)) * 6, 270);
  gfx->print(T(S_RECORDS));
  drawPersonalityRecord(52, 294, T(S_GAME_BALL), pet.gameHi, UI_BAR_OK);
  drawPersonalityRecord(178, 294, T(S_GAME_CATCH), pet.catchHi, UI_BAR_WARN);
  drawPersonalityRecord(304, 294, T(S_GAME_MEMO), pet.memoHi, 0x4C98);
  drawPersonalityRecord(52, 334, T(S_GAME_CLEAN), pet.cleanHi, UI_BAR_OK);
  drawPersonalityRecord(178, 334, T(S_GAME_TYPE), pet.typeHi, 0xF3B7);
  drawPersonalityRecord(304, 334, T(S_BATTLE), pet.bestBattleStreak, UI_BAR_BAD);
}

StrId dailyGoalLabelId(uint8_t goalType) {
  switch (goalType) {
    case DAILY_GOAL_CARE: return S_GOAL_CARE;
    case DAILY_GOAL_PLAY: return S_GOAL_PLAY;
    case DAILY_GOAL_BATTLE: return S_GOAL_BATTLE;
    case DAILY_GOAL_CATCH: return S_GOAL_CATCH;
    case DAILY_GOAL_MEMO: return S_GOAL_MEMO;
    default: return S_GOAL_CARE;
  }
}

uint16_t dailyGoalColor(uint8_t goalType) {
  switch (goalType) {
    case DAILY_GOAL_CARE: return C565(0xd4, 0x52, 0x7e);
    case DAILY_GOAL_PLAY: return UI_BAR_WARN;
    case DAILY_GOAL_BATTLE: return UI_BAR_BAD;
    case DAILY_GOAL_CATCH: return UI_BAR_OK;
    case DAILY_GOAL_MEMO: return 0x4C98;
    default: return UI_INK;
  }
}

void drawDailyGoalRow(int y, uint8_t idx) {
  uint8_t type = pet.dailyGoalType[idx];
  uint8_t target = pet.dailyGoalTarget(type);
  uint8_t progress = pet.dailyGoalProgress[idx] > target ? target : pet.dailyGoalProgress[idx];
  bool done = pet.dailyGoalComplete(idx);
  uint16_t col = dailyGoalColor(type);
  gfx->fillRoundRect(58, y, 350, 52, 12, done ? col : UI_WHITE);
  gfx->drawRoundRect(58, y, 350, 52, 12, col);
  gfx->setTextSize(2);
  gfx->setTextColor(done ? UI_BG_DAY : UI_INK);
  gfx->setCursor(82, y + 18);
  gfx->print(T(dailyGoalLabelId(type)));

  char prog[12];
  snprintf(prog, sizeof(prog), "%u/%u", progress, target);
  gfx->setCursor(286, y + 18);
  gfx->print(done ? T(S_DONE) : prog);
  if (done) {
    gfx->fillCircle(374, y + 26, 12, UI_BG_DAY);
    gfx->setTextColor(col);
    gfx->setCursor(368, y + 18);
    gfx->print("v");
  }
}

// pagina 2: objetivos diarios
void renderCardDaily() {
  pet.ensureDailyGoals();
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_DAILY)) * 9, 44);
  gfx->print(T(S_DAILY));
  const char *phase = T(dayPhaseTextId(currentDayPhase()));
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(phase) * 6, 74);
  gfx->print(phase);

  uint8_t done = 0;
  for (uint8_t i = 0; i < DAILY_GOAL_COUNT; i++) {
    if (pet.dailyGoalComplete(i)) done++;
    drawDailyGoalRow(104 + i * 70, i);
  }

  char bonus[24];
  snprintf(bonus, sizeof(bonus), "%s %u/%u", T(S_REWARD), done, DAILY_GOAL_COUNT);
  gfx->setTextColor(done == DAILY_GOAL_COUNT ? UI_BAR_OK : UI_TRACK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(bonus) * 6, 324);
  gfx->print(bonus);
}


bool boxComesBefore(int16_t a, int16_t b) {
  if (boxSort == 1) {
    const DexEntry &da = DEX_TBL[a];
    const DexEntry &db = DEX_TBL[b];
    if (da.type1 != db.type1) return da.type1 < db.type1;
    if (da.type2 != db.type2) return da.type2 < db.type2;
  } else if (boxSort == 2) {
    bool ra = pet.isRegistered(a);
    bool rb = pet.isRegistered(b);
    if (ra != rb) return ra;
  }
  return a < b;
}

uint16_t boxBuildList(int16_t *out) {
  uint16_t n = 0;
  for (int16_t dex = 1; dex <= DEX_COUNT; dex++)
    if (pet.isCaught(dex)) out[n++] = dex;
  for (uint16_t i = 1; i < n; i++) {
    int16_t v = out[i];
    int j = i - 1;
    while (j >= 0 && boxComesBefore(v, out[j])) {
      out[j + 1] = out[j];
      j--;
    }
    out[j + 1] = v;
  }
  return n;
}

uint8_t boxPageCount() {
  uint16_t count = pet.caughtCount();
  uint8_t pages = (count + BOX_ROWS - 1) / BOX_ROWS;
  return pages > 0 ? pages : 1;
}

int16_t boxDexAt(uint16_t index) {
  int16_t list[DEX_COUNT];
  uint16_t n = boxBuildList(list);
  return index < n ? list[index] : 0;
}

const char *boxSortLabel() {
  if (boxSort == 1) return T(S_SORT_TYPE);
  if (boxSort == 2) return T(S_SORT_RAISED);
  return T(S_SORT_DEX);
}

void renderCardBox() {
  uint8_t pages = boxPageCount();
  if (boxPage >= pages) boxPage = pages - 1;

  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_BOX)) * 9, 26);
  gfx->print(T(S_BOX));

  // orden: boton centrado bajo el titulo (antes salia del circulo a la derecha)
  const char *sort = boxSortLabel();
  gfx->fillRoundRect(BOX_SORT_X, BOX_SORT_Y, BOX_SORT_W, BOX_SORT_H, 12, UI_WHITE);
  gfx->drawRoundRect(BOX_SORT_X, BOX_SORT_Y, BOX_SORT_W, BOX_SORT_H, 12, UI_INK);
  gfx->setTextColor(UI_INK);
  uint8_t sortTs = (int)strlen(sort) * 12 <= BOX_SORT_W - 10 ? 2 : 1;
  gfx->setTextSize(sortTs);
  gfx->setCursor(BOX_SORT_X + (BOX_SORT_W - (int)strlen(sort) * 6 * sortTs) / 2, BOX_SORT_Y + (BOX_SORT_H - 8 * sortTs) / 2 + 1);
  gfx->print(sort);

  char caught[24], known[24], goal[22];
  snprintf(caught, sizeof(caught), T(S_CAUGHT_COUNT_FMT), pet.caughtCount(), DEX_COUNT);
  snprintf(known, sizeof(known), T(S_KNOWN_FMT), pet.knownDexCount(), DEX_COUNT);
  snprintf(goal, sizeof(goal), T(S_DEX_GOAL_FMT), pet.nextDexGoal());
  gfx->setTextSize(2);
  gfx->setTextColor(UI_INK);
  gfx->setCursor(CX - (int)strlen(caught) * 6, 106);
  gfx->print(caught);
  char sub[48];
  snprintf(sub, sizeof(sub), "%s   %s", known, goal);
  gfx->setTextColor(NET_DIM_INK);
  gfx->setTextSize(1);
  gfx->setCursor(CX - (int)strlen(sub) * 3, 126);
  gfx->print(sub);

  if (pet.caughtCount() == 0) {
    gfx->fillRoundRect(82, 178, 302, 72, 16, UI_WHITE);
    gfx->drawRoundRect(82, 178, 302, 72, 16, UI_TRACK);
    gfx->setTextColor(UI_TRACK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - strlen(T(S_NO_CATCHES)) * 6, 207);
    gfx->print(T(S_NO_CATCHES));
    return;
  }

  int16_t boxList[DEX_COUNT];
  uint16_t boxCount = boxBuildList(boxList);
  uint16_t first = (uint16_t)boxPage * BOX_ROWS;
  for (uint8_t i = 0; i < BOX_ROWS; i++) {
    uint16_t index = first + i;
    int16_t dex = index < boxCount ? boxList[index] : 0;
    if (dex <= 0) break;
    const DexEntry &d = DEX_TBL[dex];
    int y = BOX_ROW0_Y + i * BOX_ROW_STEP;
    bool raised = pet.isRegistered(dex);
    gfx->fillRoundRect(58, y, 350, BOX_ROW_H, 10, UI_WHITE);
    gfx->drawRoundRect(58, y, 350, BOX_ROW_H, 10, d.accent);
    char name[24];
    snprintf(name, sizeof(name), "#%03d %s", dex, dexName(dex));
    int ts = strlen(name) <= 16 ? 2 : 1;
    gfx->setTextSize(ts);
    gfx->setTextColor(UI_INK);
    gfx->setCursor(72, y + (ts == 2 ? 9 : 8));
    gfx->print(name);
    char types[22];
    typeText(types, sizeof(types), d);
    gfx->setTextSize(1);
    gfx->setTextColor(battleTypeColor(d.type1));
    gfx->setCursor(72, y + 30);
    gfx->print(types);
    if (raised) {
      gfx->setTextColor(UI_BAR_OK);
      gfx->setCursor(330, y + 18);
      gfx->print(T(S_RAISED_MARK));
    }
  }
  gfx->fillRoundRect(86, BOX_NAV_Y, 94, BOX_NAV_H, 12, boxPage > 0 ? UI_TRACK : C565(0xe4, 0xe8, 0xee));
  gfx->fillRoundRect(286, BOX_NAV_Y, 94, BOX_NAV_H, 12, boxPage + 1 < pages ? UI_TRACK : C565(0xe4, 0xe8, 0xee));
  gfx->setTextColor(UI_BG_DAY);
  gfx->setTextSize(3);
  gfx->setCursor(124, BOX_NAV_Y + 12);
  gfx->print("<");
  gfx->setCursor(324, BOX_NAV_Y + 12);
  gfx->print(">");
  char pg[12];
  snprintf(pg, sizeof(pg), T(S_PAGE_FMT), boxPage + 1, pages);
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(pg) * 6, BOX_NAV_Y + 15);
  gfx->print(pg);
}

// pagina 3: combate (4 barras + botones)
void renderCardStats() {
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_BATTLE)) * 9, 48);
  gfx->print(T(S_BATTLE));

  drawCardStat(112, T(S_STAT_ATK), pet.atkStat(), 260, UI_BAR_BAD);
  drawCardStat(150, T(S_STAT_DEF), pet.defStat(), 260, 0x4C98);
  drawCardStat(188, T(S_STAT_SPE), pet.speStat(), 260, UI_BAR_WARN);
  drawCardStat(226, T(S_STAT_WGT), pet.weight, 100, 0xB3C8);

  char wl[20], bs[18], bb[16];
  snprintf(wl, sizeof(wl), T(S_WL_FMT), pet.battleWins, pet.battleLosses);
  snprintf(bs, sizeof(bs), T(S_BSTREAK_FMT), pet.battleStreak);
  snprintf(bb, sizeof(bb), T(S_BBEST_FMT), pet.bestBattleStreak);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(74, 266);
  gfx->print(wl);
  gfx->setCursor(210, 266);
  gfx->print(bs);
  gfx->setCursor(334, 266);
  gfx->print(bb);

  gfx->fillRoundRect(96, 294, 274, 36, 11, 0x4C98);
  gfx->setTextColor(UI_BG_DAY);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_WILD_BATTLE)) * 6, 305);
  gfx->print(T(S_WILD_BATTLE));

  // boton: saco de entrenamiento de fuerza
  gfx->fillRoundRect(96, 338, 274, 36, 11, UI_BAR_BAD);
  gfx->setTextColor(UI_BG_DAY);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_TRAIN_STR)) * 6, 349);
  gfx->print(T(S_TRAIN_STR));
}

// pagina 2: medallas con etiqueta descriptiva
void renderCardMedals() {
  int got = 0;
  for (int i = 0; i < MED_COUNT; i++)
    if (pet.hasMedal(1 << i)) got++;
  char head[20];
  snprintf(head, sizeof(head), T(S_MEDALS_FMT), got, MED_COUNT);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(head) * 9, 48);
  gfx->print(head);

  // rejilla 3x3 dentro del circulo: nombre corto grande y descripcion debajo
  for (int i = 0; i < MED_COUNT; i++) {
    int x = 62 + (i % 3) * 118, y = 96 + (i / 3) * 84;
    bool g = pet.hasMedal(1 << i);
    gfx->fillRoundRect(x, y, 108, 74, 12, g ? UI_BAR_OK : UI_TRACK);
    gfx->setTextColor(g ? UI_BG_DAY : 0x8410);
    gfx->setTextSize(2);
    const char *lbl = medalLabel(i);
    gfx->setCursor(x + (108 - (int)strlen(lbl) * 12) / 2, y + 16);
    gfx->print(lbl);
    gfx->setTextSize(1);
    const char *dsc = medalDesc(i);
    gfx->setCursor(x + (108 - (int)strlen(dsc) * 6) / 2, y + 48);
    gfx->print(dsc);
  }
}

// pagina 3: progreso (nivel, evolucion, descuidos) — saca a la luz mecanicas
// que antes eran invisibles (cuanto falta para subir/evolucionar y por que)
void renderCardProgress() {
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_PROGRESS)) * 9, 44);
  gfx->print(T(S_PROGRESS));

  // nivel grande
  char lv[10];
  snprintf(lv, sizeof(lv), T(S_LVL_FMT), pet.level());
  gfx->setTextSize(5);
  gfx->setCursor(CX - strlen(lv) * 15, 86);
  gfx->print(lv);

  // barra de progreso al siguiente nivel (1 nivel = 60 min de juego)
  uint8_t into = pet.ageMinutes % MINUTES_PER_LEVEL;
  int bx = 93, bw = 280, by = 158, bh = 22;
  gfx->fillRoundRect(bx, by, bw, bh, 6, UI_TRACK);
  int fw = (bw - 4) * into / MINUTES_PER_LEVEL;
  if (fw > 0) gfx->fillRoundRect(bx + 2, by + 2, fw, bh - 4, 5, UI_BAR_OK);
  char nx[26];
  snprintf(nx, sizeof(nx), T(S_NEXT_LVL_FMT), MINUTES_PER_LEVEL - into, pet.level() + 1);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(nx) * 6, by + 32);
  gfx->print(nx);

  // estado de evolucion
  gfx->setTextColor(UI_TRACK);
  gfx->setCursor(CX - strlen(T(S_EVO_LABEL)) * 6, 230);
  gfx->print(T(S_EVO_LABEL));
  char evoBuf[28];
  const char *evo;
  uint16_t evoCol = UI_INK;
  if (!pet.hasEvolutionPath(pet.speciesId)) {
    evo = T(S_FINAL_FORM);
  } else {
    int needed = pet.evolutionRequiredLevel();
    if (pet.evolutionUnlocked()) {
      if (pet.canEvolveNow()) { evo = T(S_EVO_READY); evoCol = UI_BAR_OK; }
      else { evo = T(S_EVO_BLOCKED); evoCol = UI_BAR_BAD; }
    } else if (pet.level() >= needed) {
      evo = T(pet.evolutionNeedsMoreBond() ? S_EVO_NEED_BOND : S_EVO_BLOCKED);
      evoCol = UI_BAR_BAD;
    } else {
      snprintf(evoBuf, sizeof(evoBuf), T(S_EVO_IN_FMT), needed - pet.level());
      evo = evoBuf;
    }
  }
  gfx->setTextColor(evoCol);
  gfx->setCursor(CX - strlen(evo) * 6, 256);
  gfx->print(evo);

  // descuidos (retrasan la evolucion)
  char ms[24];
  snprintf(ms, sizeof(ms), T(S_MISTAKES_FMT), pet.careMistakes);
  gfx->setTextColor(pet.careMistakes > 0 ? UI_BAR_BAD : UI_INK);
  gfx->setCursor(CX - strlen(ms) * 6, 312);
  gfx->print(ms);
}

StrId stepRewardTextId(uint8_t index) {
  if (index == 0) return S_ITEM_SNACK;
  if (index == 1) return S_ITEM_ENERGY;
  return S_ITEM_TRAIN;
}

void renderCardSteps() {
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_STEPS)) * 9, 44);
  gfx->print(T(S_STEPS));

  char today[28], total[28];
  snprintf(today, sizeof(today), T(S_STEPS_TODAY_FMT), (unsigned long)pet.stepsToday);
  snprintf(total, sizeof(total), T(S_STEPS_TOTAL_FMT), (unsigned long)pet.stepsTotal);
  // dos recuadros dentro del circulo (antes salian por los lados)
  gfx->fillRoundRect(64, 80, 166, 56, 14, UI_WHITE);
  gfx->drawRoundRect(64, 80, 166, 56, 14, UI_BAR_OK);
  gfx->fillRoundRect(236, 80, 166, 56, 14, UI_WHITE);
  gfx->drawRoundRect(236, 80, 166, 56, 14, UI_BAR_WARN);
  gfx->setTextColor(UI_INK);
  uint8_t sts = ((int)strlen(today) * 12 <= 158 && (int)strlen(total) * 12 <= 158) ? 2 : 1;
  gfx->setTextSize(sts);
  gfx->setCursor(64 + (166 - (int)strlen(today) * 6 * sts) / 2, 108 - 4 * sts);
  gfx->print(today);
  gfx->setCursor(236 + (166 - (int)strlen(total) * 6 * sts) / 2, 108 - 4 * sts);
  gfx->print(total);
  gfx->setTextSize(2);

  char rank[24], shiny[24], catchBuf[24];
  snprintf(rank, sizeof(rank), T(S_TRAIL_RANK_FMT), pet.stepTrailRank());
  uint16_t units = pet.stepShinyChancePer4096();
  uint16_t denominator = units ? (uint16_t)((4096U + units / 2U) / units) : 0;
  snprintf(shiny, sizeof(shiny), T(S_SHINY_LUCK_FMT), denominator);
  snprintf(catchBuf, sizeof(catchBuf), T(S_CATCH_BONUS_FMT), pet.stepCatchBonus());
  gfx->setTextColor(UI_BAR_WARN);
  gfx->setCursor(CX - (int)strlen(rank) * 6, 158);
  gfx->print(rank);
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(1);
  gfx->setCursor(76, 184);
  gfx->print(shiny);
  gfx->setCursor(300, 184);
  gfx->print(catchBuf);

  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_DAILY)) * 6, 208);
  gfx->print(T(S_DAILY));
  for (uint8_t i = 0; i < STEP_DAILY_GOAL_COUNT; i++) {
    uint32_t goal = pet.stepGoal(i);
    uint32_t progress = pet.stepsToday < goal ? pet.stepsToday : goal;
    bool done = pet.stepGoalComplete(i);
    int y = 224 + i * 50;
    uint16_t col = i == 0 ? UI_BAR_OK : (i == 1 ? UI_BAR_WARN : UI_BAR_BAD);
    gfx->fillRoundRect(52, y, 362, 44, 10, done ? col : UI_WHITE);
    gfx->drawRoundRect(52, y, 362, 44, 10, col);
    gfx->setTextColor(done ? UI_BG_DAY : UI_INK);
    gfx->setTextSize(1);
    char goalText[18], prog[18];
    snprintf(goalText, sizeof(goalText), T(S_STEP_GOAL_FMT), (unsigned long)goal);
    snprintf(prog, sizeof(prog), "%lu/%lu", (unsigned long)progress, (unsigned long)goal);
    gfx->setCursor(66, y + 18);
    gfx->print(goalText);
    gfx->setCursor(226, y + 18);
    gfx->print(T(stepRewardTextId(i)));
    gfx->setCursor(350, y + 18);
    gfx->print(done ? T(S_DONE) : prog);
  }
}

StrId expeditionItemText(ExpeditionItem item) {
  switch (item) {
    case EXP_ITEM_SNACK: return S_ITEM_SNACK;
    case EXP_ITEM_ENERGY: return S_ITEM_ENERGY;
    case EXP_ITEM_CARE: return S_ITEM_CARE;
    case EXP_ITEM_TRAIN: return S_ITEM_TRAIN;
    default: return S_WAIT;
  }
}

StrId expeditionSiteText(ExpeditionSite site) {
  if (site == EXP_SITE_VOLCANO) return S_EXP_VOLCANO;
  if (site == EXP_SITE_FOREST) return S_EXP_FOREST;
  return S_EXP_COAST;
}

uint16_t expeditionSiteColor(ExpeditionSite site) {
  return site == EXP_SITE_VOLCANO ? UI_BAR_BAD : site == EXP_SITE_FOREST ? UI_BAR_OK : 0x4C98;
}

uint16_t expeditionItemColor(ExpeditionItem item) {
  switch (item) {
    case EXP_ITEM_SNACK: return UI_BAR_WARN;
    case EXP_ITEM_ENERGY: return 0x4C98;
    case EXP_ITEM_CARE: return UI_BAR_OK;
    case EXP_ITEM_TRAIN: return UI_BAR_BAD;
    default: return UI_TRACK;
  }
}

uint32_t expeditionNowEpoch() {
  uint32_t nowEpoch = rtcEpoch();
  return nowEpoch ? nowEpoch : pet.lastSeenEpoch;
}

static const int EXP_HUD_X = 286;
static const int EXP_HUD_Y = 128;
static const int EXP_HUD_W = 112;
static const int EXP_HUD_H = 34;

// Der obere Rand des runden Displays ist an den Ecken nicht sichtbar. Der
// Schritt-Chip sitzt deshalb links ueber dem Namen, innerhalb der Kreisflaeche;
// rechts bleibt der Expeditions-Hinweis frei.
static const int STEP_HUD_X = 138;
static const int STEP_HUD_Y = 20;
static const int STEP_HUD_W = 190;
static const int STEP_HUD_H = 44;

bool inStepsHudHit(int16_t x, int16_t y) {
  return x >= STEP_HUD_X && x <= STEP_HUD_X + STEP_HUD_W &&
         y >= STEP_HUD_Y && y <= STEP_HUD_Y + STEP_HUD_H;
}

void openStepsCard() {
  cardOpen = true;
  cardPage = 8;
  expeditionTrainChoiceOpen = false;
  cardDirty = true;
  lockTouchBrief();
  sfxPlay(SFX_MENU);
}

void drawStepsHud() {
  uint16_t color = pet.stepsToday >= 5000UL ? UI_BAR_WARN :
                   pet.stepsToday >= 500UL ? UI_BAR_OK : UI_TRACK;
  char label[24];
  snprintf(label, sizeof(label), "%s %lu", T(S_STEPS), (unsigned long)pet.stepsToday);
  gfx->fillRoundRect(STEP_HUD_X, STEP_HUD_Y, STEP_HUD_W, STEP_HUD_H, 12, UI_WHITE);
  gfx->drawRoundRect(STEP_HUD_X, STEP_HUD_Y, STEP_HUD_W, STEP_HUD_H, 12, color);
  int left = STEP_HUD_X + 6;
  if (pet.streak >= 1 && !pet.isEgg()) {  // racha: llama + dias, a la izquierda
    int fx = STEP_HUD_X + 12, fy = STEP_HUD_Y + 13;
    gfx->fillTriangle(fx + 7, fy, fx, fy + 17, fx + 14, fy + 17, UI_BAR_BAD);
    gfx->fillTriangle(fx + 7, fy + 7, fx + 3, fy + 17, fx + 11, fy + 17, UI_BAR_WARN);
    char st[6];
    snprintf(st, sizeof(st), "%u", pet.streak);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(1);
    gfx->setCursor(fx + 17, fy + 10);
    gfx->print(st);
    left = fx + 18 + (int)strlen(st) * 6;
  }
  drawBattery();
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  int tw = (int)strlen(label) * 12;
  int avail = STEP_HUD_X + STEP_HUD_W - (batPercent() >= 0 ? 42 : 6) - left;
  gfx->setCursor(left + (avail - tw) / 2, STEP_HUD_Y + 15);
  gfx->print(label);
}

bool expeditionHudVisible() {
  if (pet.isEgg() || pet.ceremony || cardOpen || gameOpen || sackOpen || battleOpen ||
      galleryOpen || kbOpen || clockOpen || helpOpen) return false;
  if (gameMenuOpen || feedMenuUntil || confirmUntil || choiceKind || bathUntil ||
      petEventUntil || wildPromptUntil) return false;
  return pet.expeditionHudState(expeditionNowEpoch()) != EXP_HUD_HIDDEN;
}

bool inExpeditionHudHit(int16_t x, int16_t y) {
  return expeditionHudVisible() && x >= EXP_HUD_X && x <= EXP_HUD_X + EXP_HUD_W &&
         y >= EXP_HUD_Y && y <= EXP_HUD_Y + EXP_HUD_H;
}

void openExpeditionCard() {
  cardOpen = true;
  cardPage = 7;
  expeditionTrainChoiceOpen = false;
  cardDirty = true;
  lockTouchBrief();
  sfxPlay(SFX_MENU);
}

void drawExpeditionHud() {
  if (!expeditionHudVisible()) return;

  uint32_t nowEpoch = expeditionNowEpoch();
  ExpeditionHudState state = pet.expeditionHudState(nowEpoch);
  uint16_t color = state == EXP_HUD_READY ? UI_BAR_OK :
                   state == EXP_HUD_BAG ? UI_BAR_WARN : 0x4C98;
  char label[18];
  if (state == EXP_HUD_ACTIVE) {
    uint32_t left = (pet.expeditionEndEpoch - nowEpoch + 59UL) / 60UL;
    snprintf(label, sizeof(label), "%s %lum", T(expeditionSiteText((ExpeditionSite)pet.expeditionSite)), (unsigned long)left);
  } else if (state == EXP_HUD_READY) {
    snprintf(label, sizeof(label), "%s", T(S_EXP_READY));
  } else {
    snprintf(label, sizeof(label), "%s x%u", T(S_EXP_HUD_BAG), pet.expeditionItemCount());
  }

  gfx->fillRoundRect(EXP_HUD_X, EXP_HUD_Y, EXP_HUD_W, EXP_HUD_H, 8, UI_WHITE);
  gfx->drawRoundRect(EXP_HUD_X, EXP_HUD_Y, EXP_HUD_W, EXP_HUD_H, 8, color);
  gfx->fillCircle(EXP_HUD_X + 17, EXP_HUD_Y + 17, 9, color);
  gfx->setTextColor(UI_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(EXP_HUD_X + 14, EXP_HUD_Y + 12);
  if (state == EXP_HUD_READY) gfx->print("!");
  else if (state == EXP_HUD_BAG) gfx->print("+");
  else gfx->print(">");
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(1);
  gfx->setCursor(EXP_HUD_X + 31, EXP_HUD_Y + 13);
  gfx->print(label);
}

// columnas de lugares y duraciones de la expedicion (dentro del circulo)
#define EXP_COL_X0 66
#define EXP_COL_W 100
#define EXP_COL_STEP 117
#define EXP_SITE_Y 72
#define EXP_SITE_H 44
#define EXP_TIME_Y 128
#define EXP_TIME_H 56
#define EXP_ITEM_Y0 218
#define EXP_ITEM_Y1 280

int expeditionColumnAt(int16_t x) {
  for (int i = 0; i < 3; i++) {
    int cx0 = EXP_COL_X0 + i * EXP_COL_STEP - 4;
    if (x >= cx0 && x < cx0 + EXP_COL_W + 8) return i;
  }
  return -1;
}

void drawExpeditionItem(int x, int y, ExpeditionItem item) {
  const int w = 172, h = 54;
  uint8_t count = pet.itemCounts[item];
  uint16_t col = expeditionItemColor(item);
  gfx->fillRoundRect(x, y, w, h, 9, count ? UI_WHITE : C565(0xe4, 0xe8, 0xee));
  gfx->drawRoundRect(x, y, w, h, 9, count ? col : UI_TRACK);
  gfx->fillCircle(x + 22, y + 27, 12, count ? col : UI_TRACK);
  if (item == EXP_ITEM_ENERGY) gfx->fillRect(x + 20, y + 17, 5, 20, UI_WHITE);
  else if (item == EXP_ITEM_CARE) gfx->fillCircle(x + 22, y + 22, 4, UI_WHITE);
  else if (item == EXP_ITEM_TRAIN) gfx->fillRect(x + 16, y + 25, 12, 4, UI_WHITE);

  const char *label = T(expeditionItemText(item));
  gfx->setTextSize(1);
  gfx->setTextColor(count ? UI_INK : UI_TRACK);
  gfx->setCursor(x + 42, y + 16);
  gfx->print(label);
  char amount[6];
  snprintf(amount, sizeof(amount), "x%u", count);
  gfx->setTextSize(2);
  gfx->setCursor(x + 136, y + 29);
  gfx->print(amount);
}

void renderExpeditionTrainChoice() {
  gfx->fillRoundRect(58, 118, 350, 190, 14, UI_WHITE);
  gfx->drawRoundRect(58, 118, 350, 190, 14, UI_INK);
  const char *title = T(S_ITEM_TRAIN);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(title) * 6, 136);
  gfx->print(title);

  const StrId labels[3] = { S_TRAIN_ATK, S_TRAIN_DEF, S_TRAIN_SPE };
  const uint8_t values[3] = { pet.trAtk, pet.trDef, pet.trSpe };
  const uint16_t cols[3] = { UI_BAR_BAD, 0x4C98, UI_BAR_WARN };
  for (uint8_t i = 0; i < 3; i++) {
    int x = 74 + i * 108;
    bool usable = values[i] < 100;
    gfx->fillRoundRect(x, 172, 102, 66, 9, usable ? cols[i] : UI_TRACK);
    gfx->setTextColor(UI_BG_DAY);
    gfx->setTextSize(2);
    const char *label = T(labels[i]);
    gfx->setCursor(x + (102 - (int)strlen(label) * 12) / 2, 184);
    gfx->print(label);
    if (usable) {
      gfx->setTextSize(1);
      gfx->setCursor(x + 37, 210);
      gfx->print("+2");
    } else {
      const char *maxed = T(S_ITEM_MAXED);
      gfx->setTextSize(1);
      gfx->setCursor(x + (102 - (int)strlen(maxed) * 6) / 2, 210);
      gfx->print(maxed);
    }
  }
}

void renderCardExpedition() {
  uint32_t nowEpoch = expeditionNowEpoch();
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(T(S_EXPEDITION)) * 9, 34);
  gfx->print(T(S_EXPEDITION));

  if (pet.expeditionReady(nowEpoch)) {
    char found[34];
    snprintf(found, sizeof(found), T(S_FOUND_ITEM_FMT), T(expeditionItemText((ExpeditionItem)pet.expeditionRewardItem)));
    gfx->setTextColor(UI_BAR_OK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - strlen(found) * 6, 78);
    gfx->print(found);
    gfx->setTextColor(expeditionSiteColor((ExpeditionSite)pet.expeditionSite));
    gfx->setTextSize(1);
    gfx->setCursor(CX - (int)strlen(T(expeditionSiteText((ExpeditionSite)pet.expeditionSite))) * 3, 92);
    gfx->print(T(expeditionSiteText((ExpeditionSite)pet.expeditionSite)));
    gfx->fillRoundRect(98, 98, 270, 48, 11, UI_BAR_OK);
    gfx->setTextColor(UI_BG_DAY);
    gfx->setTextSize(2);
    gfx->setCursor(CX - strlen(T(S_EXP_CLAIM)) * 6, 111);
    gfx->print(T(S_EXP_CLAIM));
  } else if (pet.expeditionActive(nowEpoch)) {
    uint32_t left = (pet.expeditionEndEpoch - nowEpoch + 59UL) / 60UL;
    char back[28];
    snprintf(back, sizeof(back), T(S_EXP_IN_FMT), (unsigned)left);
    gfx->setTextColor(0x4C98);
    gfx->setTextSize(3);
    gfx->setCursor(CX - strlen(back) * 9, 86);
    gfx->print(back);
    gfx->setTextColor(UI_TRACK);
    gfx->setTextSize(1);
    gfx->setCursor(CX - 90, 116);
    gfx->print(T(S_WAIT));
  } else {
    const ExpeditionSite sites[3] = { EXP_SITE_COAST, EXP_SITE_VOLCANO, EXP_SITE_FOREST };
    for (uint8_t i = 0; i < 3; i++) {
      bool selected = expeditionSiteChoice == sites[i];
      uint16_t col = expeditionSiteColor(sites[i]);
      int sx = EXP_COL_X0 + i * EXP_COL_STEP;
      gfx->fillRoundRect(sx, EXP_SITE_Y, EXP_COL_W, EXP_SITE_H, 10, selected ? col : UI_WHITE);
      gfx->drawRoundRect(sx, EXP_SITE_Y, EXP_COL_W, EXP_SITE_H, 10, col);
      gfx->setTextColor(selected ? UI_BG_DAY : UI_INK);
      const char *name = T(expeditionSiteText(sites[i]));
      uint8_t ts = (int)strlen(name) * 12 <= EXP_COL_W - 8 ? 2 : 1;
      gfx->setTextSize(ts);
      gfx->setCursor(sx + (EXP_COL_W - (int)strlen(name) * 6 * ts) / 2, EXP_SITE_Y + (EXP_SITE_H - 8 * ts) / 2 + 1);
      gfx->print(name);
    }
    char seals[20]; snprintf(seals, sizeof(seals), T(S_EXP_SEALS_FMT), pet.hoennSealCount());
    gfx->setTextColor(pet.hoennSealCount() == 3 ? UI_BAR_WARN : NET_DIM_INK); gfx->setTextSize(1);
    gfx->setCursor(CX - (int)strlen(seals) * 3, EXP_SITE_Y + EXP_SITE_H + 5); gfx->print(seals);
    const uint8_t minutes[3] = { 15, 30, 60 };
    const StrId labels[3] = { S_EXP_15, S_EXP_30, S_EXP_60 };
    for (uint8_t i = 0; i < 3; i++) {
      uint8_t cost = Pet::expeditionEnergyCost(minutes[i]);
      bool available = pet.canStartExpedition(minutes[i], nowEpoch);
      uint16_t col = i == 0 ? UI_BAR_OK : i == 1 ? 0x4C98 : UI_BAR_BAD;
      int bx = EXP_COL_X0 + i * EXP_COL_STEP;
      gfx->fillRoundRect(bx, EXP_TIME_Y, EXP_COL_W, EXP_TIME_H, 10, available ? col : UI_TRACK);
      gfx->setTextColor(UI_BG_DAY);
      gfx->setTextSize(2);
      const char *label = T(labels[i]);
      gfx->setCursor(bx + (EXP_COL_W - (int)strlen(label) * 12) / 2, EXP_TIME_Y + 8);
      gfx->print(label);
      char costText[12];
      snprintf(costText, sizeof(costText), "-%u ENE", cost);
      gfx->setTextSize(1);
      gfx->setCursor(bx + (EXP_COL_W - (int)strlen(costText) * 6) / 2, EXP_TIME_Y + 32);
      gfx->print(costText);
      char goalText[24]; snprintf(goalText, sizeof(goalText), T(S_EXP_WALK_PROGRESS), 0, Pet::expeditionStepGoal(minutes[i]));
      gfx->setCursor(bx + (EXP_COL_W - (int)strlen(goalText) * 6) / 2, EXP_TIME_Y + 44); gfx->print(goalText);
    }
    const char *warn = nullptr;
    char need[24];
    if (pet.expeditionInventoryFull()) warn = T(S_INV_FULL);
    else if (pet.energy < 12) { snprintf(need, sizeof(need), T(S_NEED_ENE_FMT), 12); warn = need; }
    if (warn) {
      gfx->setTextColor(UI_BAR_BAD);
      gfx->setTextSize(1);
      gfx->setCursor(CX - strlen(warn) * 3, EXP_TIME_Y + EXP_TIME_H + 4);
      gfx->print(warn);
    }
  }

  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_INVENTORY)) * 6, 199);
  gfx->print(T(S_INVENTORY));
  if (pet.expeditionEndEpoch && pet.expeditionMinutes) {
    char progress[48];
    if (pet.expeditionWalkBonusEarned()) snprintf(progress, sizeof(progress), "%s", T(S_EXP_WALK_EARNED));
    else snprintf(progress, sizeof(progress), T(S_EXP_WALK_PROGRESS), pet.expeditionSteps, Pet::expeditionStepGoal(pet.expeditionMinutes));
    gfx->setTextSize(1); gfx->setTextColor(pet.expeditionWalkBonusEarned() ? UI_BAR_OK : UI_INK);
    gfx->setCursor(CX - (int)strlen(progress) * 3, 160); gfx->print(progress);
  }
  drawExpeditionItem(50, EXP_ITEM_Y0, EXP_ITEM_SNACK);
  drawExpeditionItem(244, EXP_ITEM_Y0, EXP_ITEM_ENERGY);
  drawExpeditionItem(50, EXP_ITEM_Y1, EXP_ITEM_CARE);
  drawExpeditionItem(244, EXP_ITEM_Y1, EXP_ITEM_TRAIN);
  if (expeditionTrainChoiceOpen) renderExpeditionTrainChoice();
}

int8_t expeditionItemAt(int16_t x, int16_t y) {
  if ((y < EXP_ITEM_Y0 || y > EXP_ITEM_Y0 + 54) && (y < EXP_ITEM_Y1 || y > EXP_ITEM_Y1 + 54)) return -1;
  bool left = x >= 50 && x <= 222;
  bool right = x >= 244 && x <= 416;
  if (!left && !right) return -1;
  uint8_t row = y >= EXP_ITEM_Y1 ? 1 : 0;
  return row * 2 + (right ? 1 : 0);
}

void expeditionCardTap(int16_t x, int16_t y) {
  if (expeditionTrainChoiceOpen) {
    if (y >= 172 && y <= 238 && x >= 74 && x <= 398) {
      int8_t stat = (x - 74) / 108;
      if (stat > TRAIN_STAT_SPE || !pet.useExpeditionItem(EXP_ITEM_TRAIN, stat)) sfxPlay(SFX_DENY);
      else sfxPlay(SFX_ITEM_USE);
      expeditionTrainChoiceOpen = false;
      cardDirty = true;
      lockTouchBrief();
      return;
    }
    expeditionTrainChoiceOpen = false;
    cardDirty = true;
    sfxPlay(SFX_TAP);
    return;
  }
  uint32_t nowEpoch = expeditionNowEpoch();
  if (pet.expeditionReady(nowEpoch)) {
    if (x >= 98 && x <= 368 && y >= 98 && y <= 146) {
      ExpeditionItem item = pet.claimExpedition(nowEpoch);
      if (item != EXP_ITEM_NONE) sfxPlay(SFX_EXPEDITION_CLAIM);
      else sfxPlay(SFX_DENY);
      cardDirty = true;
      lockTouchBrief();
    }
    return;
  }
  if (!pet.expeditionActive(nowEpoch) && y >= EXP_SITE_Y && y <= EXP_SITE_Y + EXP_SITE_H) {
    int idx = expeditionColumnAt(x);
    if (idx >= 0) { expeditionSiteChoice = (ExpeditionSite)idx; cardDirty = true; sfxPlay(SFX_TAP); lockTouchBrief(); }
    return;
  }
  if (!pet.expeditionActive(nowEpoch) && y >= EXP_TIME_Y && y <= EXP_TIME_Y + EXP_TIME_H) {
    const uint8_t minutes[3] = { 15, 30, 60 };
    int idx = expeditionColumnAt(x);
    if (idx >= 0 && pet.startExpedition(minutes[idx], nowEpoch, (uint8_t)random(100), (uint8_t)random(4), expeditionSiteChoice, (uint8_t)random(100))) {
      sfxPlay(SFX_EXPEDITION_START);
      cardDirty = true;
      lockTouchBrief();
    } else {
      sfxPlay(SFX_DENY);
    }
    return;
  }

  int8_t item = expeditionItemAt(x, y);
  if (item < 0) return;
  ExpeditionItem selected = (ExpeditionItem)item;
  if (pet.itemCounts[selected] == 0) {
    sfxPlay(SFX_DENY);
    return;
  }
  if (selected == EXP_ITEM_TRAIN) {
    expeditionTrainChoiceOpen = true;
    cardDirty = true;
    sfxPlay(SFX_MENU);
  } else if (pet.useExpeditionItem(selected)) {
    cardDirty = true;
    sfxPlay(SFX_ITEM_USE);
    lockTouchBrief();
  }
}

void renderCard() {
  cardDirty = false;
  gfx->fillScreen(UI_BG_DAY);
  if (cardPage == 0) renderCardProfile();
  else if (cardPage == 1) renderCardPersonality();
  else if (cardPage == 2) renderCardDaily();
  else if (cardPage == 3) renderCardBox();
  else if (cardPage == 4) renderCardStats();
  else if (cardPage == 5) renderCardMedals();
  else if (cardPage == 6) renderCardProgress();
  else if (cardPage == 7) renderCardExpedition();
  else renderCardSteps();

  // indicador de paginas + ayuda
  int dotsX = CX - ((CARD_COUNT - 1) * 24) / 2;
  for (int i = 0; i < CARD_COUNT; i++) {
    if (i == cardPage) gfx->fillCircle(dotsX + i * 24, 400, 5, UI_INK);
    else gfx->drawCircle(dotsX + i * 24, 400, 4, UI_INK);
  }
  flushStampedFrame();
}

// ---------- teclado para renombrar ----------

static const char KB_KEYS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-";  // 28 + DEL + OK = 30
#define KB_COLS 6
#define KB_X 40
#define KB_Y 150
#define KB_W 64
#define KB_H 52

void openKeyboard() {
  kbOpen = true;
  keyboardDirty = true;
  lockTouchBrief();
  strncpy(nameBuf, pet.nick, sizeof(nameBuf) - 1);
  nameBuf[sizeof(nameBuf) - 1] = 0;
  nameLen = strlen(nameBuf);
  sfxPlay(SFX_MENU);
}

void renderKeyboard() {
  keyboardDirty = false;
  gfx->fillScreen(UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(T(S_NAME)) * 6, 56);
  gfx->print(T(S_NAME));
  // buffer actual
  gfx->fillRoundRect(83, 84, 300, 40, 8, UI_WHITE);
  gfx->drawRoundRect(83, 84, 300, 40, 8, UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(95, 94);
  gfx->print(nameLen ? nameBuf : "_");

  for (int i = 0; i < 30; i++) {
    int x = KB_X + (i % KB_COLS) * KB_W, y = KB_Y + (i / KB_COLS) * KB_H;
    bool special = (i >= 28);
    gfx->fillRoundRect(x, y, KB_W - 6, KB_H - 6, 6, special ? UI_BAR_WARN : UI_WHITE);
    gfx->drawRoundRect(x, y, KB_W - 6, KB_H - 6, 6, UI_INK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    if (i < 28) {
      gfx->setCursor(x + KB_W / 2 - 9, y + KB_H / 2 - 10);
      gfx->print(KB_KEYS[i]);
    } else {
      const char *lab = (i == 28) ? "<-" : "OK";
      gfx->setCursor(x + KB_W / 2 - 15, y + KB_H / 2 - 10);
      gfx->print(lab);
    }
  }
  flushStampedFrame();
}

void keyboardTap(int16_t x, int16_t y) {
  int col = (x - KB_X) / KB_W, row = (y - KB_Y) / KB_H;
  if (col < 0 || col >= KB_COLS || row < 0 || row >= 5) return;
  int i = row * KB_COLS + col;
  if (i >= 30) return;
  sfxPlay(SFX_TAP);
  if (i == 28) {  // borrar
    if (nameLen) nameBuf[--nameLen] = 0;
    keyboardDirty = true;
  } else if (i == 29) {  // OK
    pet.rename(nameBuf);
    kbOpen = false;
    cardDirty = true;
    lockTouchBrief();
  } else if (nameLen < sizeof(nameBuf) - 1) {
    nameBuf[nameLen++] = KB_KEYS[i];
    nameBuf[nameLen] = 0;
    keyboardDirty = true;
  }
}

// ---------- galeria pokedex ----------

#define GAL_X 73
#define GAL_Y 128
#define GAL_CELL 74
#define GAL_ROWS 3
#define GAL_COLS 4
#define GAL_PER_PAGE (GAL_ROWS * GAL_COLS)
#define GAL_FILTER_Y 70
#define GAL_FILTER_H 46
#define GAL_REGION_X 78
#define GAL_FILTER_X 238
#define GAL_FILTER_W 150
#define GAL_NAV_Y 356
#define GAL_NAV_H 48
#define GAL_PREV_X 96
#define GAL_NEXT_X 280
#define GAL_NAV_W 90

bool galleryDexVisible(int16_t dex) {
  if (dex < 1 || dex > DEX_COUNT) return false;
  if (galleryRegion == 1 && dex > 151) return false;
  if (galleryRegion == 2 && (dex < 152 || dex > 251)) return false;
  if (galleryRegion == 3 && dex < 252) return false;
  if (galleryFilter == 1) return pet.isRegistered(dex);
  if (galleryFilter == 2) return pet.isCaught(dex);
  if (galleryFilter == 3) return pet.isShinyRegistered(dex);
  return true;
}

uint16_t galleryFilteredCount() {
  if (galleryFilter == 0) return DEX_COUNT;
  uint16_t count = 0;
  for (int16_t dex = 1; dex <= DEX_COUNT; dex++)
    if (galleryDexVisible(dex)) count++;
  return count;
}

int galleryPageCount() {
  uint16_t count = galleryFilteredCount();
  int pages = (count + GAL_PER_PAGE - 1) / GAL_PER_PAGE;
  return pages > 0 ? pages : 1;
}

int16_t galleryDexAt(uint16_t index) {
  for (int16_t dex = 1; dex <= DEX_COUNT; dex++) {
    if (!galleryDexVisible(dex)) continue;
    if (index == 0) return dex;
    index--;
  }
  return 0;
}

// dibuja una miniatura centrada en su celda; sil=true la pinta en tinta
void drawThumb(const uint8_t *b, int x, int y, int s, bool sil) {
  uint8_t w = b[0], h = b[1], n = b[2];
  const uint8_t *pal = b + 3;
  const uint8_t *d = pal + n * 2;
  int ox = x + (GAL_CELL - w * s) / 2;
  int oy = y + (GAL_CELL - h * s) / 2;
  for (int r = 0; r < h; r++) {
    for (int c = 0; c < w; c++) {
      uint8_t idx = d[r * w + c];
      if (idx == 0xFF) continue;
      uint16_t col = sil ? INK_K : (uint16_t)(pal[idx * 2] | (pal[idx * 2 + 1] << 8));
      gfx->fillRect(ox + c * s, oy + r * s, s, s, col);
    }
  }
}

// Pokemon visible (con los filtros actuales) a 'steps' posiciones de dex; 0 si no hay
int16_t galleryNeighbor(int16_t dex, int steps) {
  int dir = steps > 0 ? 1 : -1;
  for (int n = steps > 0 ? steps : -steps; n > 0; n--) {
    int16_t d = dex;
    do {
      d += dir;
      if (d < 1 || d > DEX_COUNT) return 0;
    } while (!galleryDexVisible(d));
    dex = d;
  }
  return dex;
}

void galleryRotateDrag(int16_t x, int16_t y) {
  float dx = x - CX, dy = y - CY;
  if (dx * dx + dy * dy < 50.0f * 50.0f) { galRotHasAngle = false; return; }  // cerca del centro el angulo no es fiable
  float a = atan2f(dy, dx);
  if (!galRotHasAngle) { galRotLastAngle = a; galRotHasAngle = true; return; }
  float d = a - galRotLastAngle;
  if (d > (float)PI) d -= 2 * (float)PI;
  if (d < -(float)PI) d += 2 * (float)PI;
  galRotLastAngle = a;
  galRotAccum += d;
  if (!galleryRotating) {
    if (fabsf(galRotAccum) < GAL_ROT_START) return;
    galleryRotating = true;  // empieza el carrusel
    galRotDex = galleryDetail;
    galRotAccum = galRotAccum > 0 ? galRotAccum - GAL_ROT_START : galRotAccum + GAL_ROT_START;
    galleryPmd.unload();
  }
  // sentido horario = siguiente, antihorario = anterior; tope en los extremos
  while (galRotAccum >= GAL_ROT_STEP) {
    int16_t n = galleryNeighbor(galRotDex, 1);
    if (!n) { galRotAccum = GAL_ROT_STEP * 0.4f; break; }
    galRotDex = n;
    galRotAccum -= GAL_ROT_STEP;
    sfxPlay(SFX_TAP);
  }
  while (galRotAccum <= -GAL_ROT_STEP) {
    int16_t n = galleryNeighbor(galRotDex, -1);
    if (!n) { galRotAccum = -GAL_ROT_STEP * 0.4f; break; }
    galRotDex = n;
    galRotAccum += GAL_ROT_STEP;
    sfxPlay(SFX_TAP);
  }
  if (galRotAccum > 0 && !galleryNeighbor(galRotDex, 1)) galRotAccum = fminf(galRotAccum, GAL_ROT_STEP * 0.4f);
  if (galRotAccum < 0 && !galleryNeighbor(galRotDex, -1)) galRotAccum = fmaxf(galRotAccum, -GAL_ROT_STEP * 0.4f);
}

void galleryRotateEnd() {
  galleryRotating = false;
  int16_t pick = galRotDex;
  if (galRotAccum >= GAL_ROT_STEP / 2) { int16_t n = galleryNeighbor(galRotDex, 1); if (n) pick = n; }
  else if (galRotAccum <= -GAL_ROT_STEP / 2) { int16_t n = galleryNeighbor(galRotDex, -1); if (n) pick = n; }
  galRotAccum = 0;
  galleryDetail = pick;
  galleryPmd.load(pick, pet.isShinyRegistered(pick));
  sfxPlay(SFX_MENU);
  if (pet.isRegistered(pick) || pet.isCaught(pick)) speciesChirpPlay(pick);
}

// miniatura centrada en (cx, cy)
void drawThumbAt(const uint8_t *b, int cx, int cy, int s, bool sil) {
  drawThumb(b, cx - GAL_CELL / 2, cy - GAL_CELL / 2, s, sil);
}

// carrusel: el Pokemon actual en grande arriba de un arco, vecinos a los lados
void renderGalleryCarousel() {
  gfx->fillScreen(UI_BG_DAY);
  float frac = galRotAccum / GAL_ROT_STEP;  // -1..1 entre pasos
  const int order[5] = { -2, 2, -1, 1, 0 };  // los lejanos primero, el central encima
  int16_t center = galRotDex;
  for (int i = 0; i < 5; i++) {
    int k = order[i];
    int16_t dex = k == 0 ? center : galleryNeighbor(center, k);
    if (!dex) continue;
    float pos = k - frac;  // 0 = arriba en el centro
    float th = (270.0f - pos * 40.0f) * (float)PI / 180.0f;
    int px = (int)lroundf(CX + 170.0f * cosf(th));
    int py = (int)lroundf(330.0f + 170.0f * sinf(th));
    float dist = fabsf(pos);
    int sc = dist < 0.5f ? 4 : (dist < 1.5f ? 3 : 2);
    const uint8_t *t = thumbs.get(dex);
    bool known = pet.isRegistered(dex) || pet.isCaught(dex);
    if (t) drawThumbAt(t, px, py, sc, !known);
  }
  // nombre del que quedara elegido al soltar
  int16_t pick = center;
  if (frac >= 0.5f) { int16_t n = galleryNeighbor(center, 1); if (n) pick = n; }
  else if (frac <= -0.5f) { int16_t n = galleryNeighbor(center, -1); if (n) pick = n; }
  bool known = pet.isRegistered(pick) || pet.isCaught(pick);
  char head[24];
  snprintf(head, sizeof(head), "N.%03d %s", pick, known ? dexName(pick) : "???");
  gfx->setTextColor(known ? DEX_TBL[pick].accent : UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(head) * 6, 346);
  gfx->print(head);
  flushStampedFrame();
}

void renderGallery() {
  if (galleryDetail && galleryRotating) { renderGalleryCarousel(); return; }
  if (galleryDetail) {  // vista detalle: se redibuja siempre (animada)
    gfx->fillScreen(UI_BG_DAY);
    const DexEntry &d = DEX_TBL[galleryDetail];
    bool reg = pet.isRegistered(galleryDetail);
    bool caught = pet.isCaught(galleryDetail);
    bool known = reg || caught;
    char head[24];
    snprintf(head, sizeof(head), "N.%03d %s%s", galleryDetail,
             pet.isShinyRegistered(galleryDetail) ? "*" : "", known ? dexName(galleryDetail) : "???");
    gfx->setTextColor(known ? d.accent : UI_INK);
    int glen = strlen(head);
    int gts = (glen <= 13) ? 3 : 2;  // auto-encoge nombres largos (no caben a t3)
    gfx->setTextSize(gts);
    gfx->setCursor(CX - glen * (gts == 3 ? 9 : 6), gts == 3 ? 56 : 60);
    gfx->print(head);
    if (known) {
      char types[24];
      typeText(types, sizeof(types), d);
      gfx->setTextColor(battleTypeColor(d.type1));
      gfx->setTextSize(2);
      gfx->setCursor(CX - strlen(types) * 6, 94);
      gfx->print(types);
    }
    if (galleryPmd.loaded) {
      // animado y a color si se conoce; silueta estatica si no (estilo "?")
      drawPmdActM(galleryPmd, PMD_IDLE, CX, 300, known ? millis() : 0, true, !known, 6);
    } else {
      const uint8_t *t = thumbs.get(galleryDetail);
      if (t) drawThumb(t, CX - GAL_CELL, 135, 4, !known);
    }
    if (reg) {
      const char *mark = T(S_RAISED_MARK);
      gfx->setTextColor(UI_BAR_OK);
      gfx->setTextSize(2);
      gfx->setCursor(CX - strlen(mark) * 6, caught ? 354 : 366);
      gfx->print(mark);
    }
    if (caught) {
      const char *mark = T(S_CAUGHT_MARK);
      gfx->setTextColor(UI_BAR_WARN);
      gfx->setTextSize(2);
      gfx->setCursor(CX - strlen(mark) * 6, reg ? 376 : 366);
      gfx->print(mark);
    }
    const char *hint = T(S_DEX_ROTATE_HINT);
    gfx->setTextColor(NET_DIM_INK);
    gfx->setTextSize(1);
    gfx->setCursor(CX - (int)strlen(hint) * 3, 402);
    gfx->print(hint);
    flushStampedFrame();
    return;
  }

  if (!galleryDirty) return;  // la rejilla es estatica
  galleryDirty = false;

  gfx->fillScreen(UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - 7 * 9, 24);
  gfx->print("POKEDEX");

  char head[28];
  snprintf(head, sizeof(head), T(S_RAISED_CAUGHT_FMT), pet.registeredCount(), pet.caughtCount());
  gfx->setTextSize(1);
  gfx->setTextColor(NET_DIM_INK);
  gfx->setCursor(CX - strlen(head) * 3, 56);
  gfx->print(head);

  // dos botones grandes que ciclan: region y filtro (antes 8 chips de 16 px)
  const char *regions[4] = { T(S_FILTER_ALL), "KANTO", "JOHTO", "HOENN" };
  const char *filters[4] = { T(S_FILTER_ALL), T(S_RAISED_MARK), T(S_CAUGHT_MARK), "SHINY" };
  char rl[24], fl[24];
  snprintf(rl, sizeof(rl), "%s >", regions[galleryRegion & 3]);
  snprintf(fl, sizeof(fl), "%s >", filters[galleryFilter & 3]);
  gfx->fillRoundRect(GAL_REGION_X, GAL_FILTER_Y, GAL_FILTER_W, GAL_FILTER_H, 12, galleryRegion ? 0x4C98 : UI_WHITE);
  gfx->drawRoundRect(GAL_REGION_X, GAL_FILTER_Y, GAL_FILTER_W, GAL_FILTER_H, 12, UI_INK);
  gfx->fillRoundRect(GAL_FILTER_X, GAL_FILTER_Y, GAL_FILTER_W, GAL_FILTER_H, 12, galleryFilter ? UI_BAR_WARN : UI_WHITE);
  gfx->drawRoundRect(GAL_FILTER_X, GAL_FILTER_Y, GAL_FILTER_W, GAL_FILTER_H, 12, UI_INK);
  gfx->setTextSize(2);
  gfx->setTextColor(galleryRegion ? UI_WHITE : UI_INK);
  gfx->setCursor(GAL_REGION_X + (GAL_FILTER_W - (int)strlen(rl) * 12) / 2, GAL_FILTER_Y + 16);
  gfx->print(rl);
  gfx->setTextColor(UI_INK);
  gfx->setCursor(GAL_FILTER_X + (GAL_FILTER_W - (int)strlen(fl) * 12) / 2, GAL_FILTER_Y + 16);
  gfx->print(fl);

  for (int r = 0; r < GAL_ROWS; r++) {
    for (int c = 0; c < GAL_COLS; c++) {
      int16_t dex = galleryDexAt(galleryPage * GAL_PER_PAGE + r * GAL_COLS + c);
      if (dex <= 0) continue;
      int x = GAL_X + c * GAL_CELL, y = GAL_Y + r * GAL_CELL;
      const uint8_t *t = thumbs.get(dex);
      if (t) {
        bool reg = pet.isRegistered(dex);
        bool caught = pet.isCaught(dex);
        bool known = reg || caught;
        drawThumb(t, x, y, 2, !known);
        if (pet.isShinyRegistered(dex)) {
          gfx->setTextColor(UI_BAR_WARN);
          gfx->setTextSize(2);
          gfx->setCursor(x + 62, y + 4);
          gfx->print("*");
        } else if (caught && !reg) {
          gfx->setTextColor(UI_BAR_WARN);
          gfx->setTextSize(1);
          gfx->setCursor(x + 60, y + 6);
          gfx->print("C");
        }
      } else {
        char num[6];
        snprintf(num, sizeof(num), "%d", dex);
        gfx->setTextColor(UI_TRACK);
        gfx->setTextSize(2);
        gfx->setCursor(x + 24, y + 32);
        gfx->print(num);
      }
    }
  }
  // paginas: flechas grandes y numero (con 386 Pokemon los puntos no cabian)
  int pages = galleryPageCount();
  gfx->fillRoundRect(GAL_PREV_X, GAL_NAV_Y, GAL_NAV_W, GAL_NAV_H, 12, galleryPage > 0 ? UI_WHITE : UI_BG_DAY);
  gfx->drawRoundRect(GAL_PREV_X, GAL_NAV_Y, GAL_NAV_W, GAL_NAV_H, 12, galleryPage > 0 ? UI_INK : UI_TRACK);
  gfx->fillRoundRect(GAL_NEXT_X, GAL_NAV_Y, GAL_NAV_W, GAL_NAV_H, 12, galleryPage + 1 < pages ? UI_WHITE : UI_BG_DAY);
  gfx->drawRoundRect(GAL_NEXT_X, GAL_NAV_Y, GAL_NAV_W, GAL_NAV_H, 12, galleryPage + 1 < pages ? UI_INK : UI_TRACK);
  gfx->setTextSize(3);
  gfx->setTextColor(galleryPage > 0 ? UI_INK : UI_TRACK);
  gfx->setCursor(GAL_PREV_X + (GAL_NAV_W - 18) / 2, GAL_NAV_Y + 13);
  gfx->print("<");
  gfx->setTextColor(galleryPage + 1 < pages ? UI_INK : UI_TRACK);
  gfx->setCursor(GAL_NEXT_X + (GAL_NAV_W - 18) / 2, GAL_NAV_Y + 13);
  gfx->print(">");
  char pg[12];
  snprintf(pg, sizeof(pg), "%d/%d", galleryPage + 1, pages);
  gfx->setTextSize(2);
  gfx->setTextColor(UI_INK);
  gfx->setCursor(CX - (int)strlen(pg) * 6, GAL_NAV_Y + 16);
  gfx->print(pg);
  flushStampedFrame();
}

void galleryTap(int16_t x, int16_t y) {
  if (galleryDetail) {  // tocar el detalle: su sonido (se vuelve con PWR)
    if (pet.isRegistered(galleryDetail) || pet.isCaught(galleryDetail)) speciesChirpPlay(galleryDetail, true);
    return;
  }
  if (y >= GAL_FILTER_Y && y < GAL_FILTER_Y + GAL_FILTER_H) {
    if (x >= GAL_REGION_X && x < GAL_REGION_X + GAL_FILTER_W) galleryRegion = (galleryRegion + 1) & 3;
    else if (x >= GAL_FILTER_X && x < GAL_FILTER_X + GAL_FILTER_W) galleryFilter = (galleryFilter + 1) & 3;
    else return;
    galleryPage = 0;
    galleryDirty = true;
    sfxPlay(SFX_TAP);
    return;
  }
  if (y >= GAL_NAV_Y && y < GAL_NAV_Y + GAL_NAV_H) {
    int pages = galleryPageCount();
    if (x >= GAL_PREV_X && x < GAL_PREV_X + GAL_NAV_W && galleryPage > 0) { galleryPage--; galleryDirty = true; sfxPlay(SFX_MENU); }
    else if (x >= GAL_NEXT_X && x < GAL_NEXT_X + GAL_NAV_W && galleryPage + 1 < pages) { galleryPage++; galleryDirty = true; sfxPlay(SFX_MENU); }
    return;
  }
  if (x < GAL_X || y < GAL_Y) return;
  int c = (x - GAL_X) / GAL_CELL, r = (y - GAL_Y) / GAL_CELL;
  if (c < 0 || c >= GAL_COLS || r < 0 || r >= GAL_ROWS) return;
  int16_t dex = galleryDexAt(galleryPage * GAL_PER_PAGE + r * GAL_COLS + c);
  if (dex <= 0) return;
  galleryDetail = dex;
  galleryPmd.load(dex, pet.isShinyRegistered(dex));
  sfxPlay(SFX_MENU);
  if (pet.isRegistered(dex) || pet.isCaught(dex)) speciesChirpPlay(dex);
}

void drawBattery() {
  int pc = batPercent();
  if (pc < 0) return;  // sin bateria conectada
  // dentro de la pildora de pasos, a la derecha
  int x = STEP_HUD_X + STEP_HUD_W - 38, y = STEP_HUD_Y + (STEP_HUD_H - 11) / 2, w = 24, h = 11;
  bool charging = batCharging();
  uint16_t col = charging ? UI_BAR_OK
                 : (pc >= 40) ? UI_INK  // la pildora es siempre blanca
                 : (pc >= 15) ? UI_BAR_WARN
                              : UI_BAR_BAD;
  gfx->drawRoundRect(x, y, w, h, 2, col);
  gfx->fillRect(x + w, y + 3, 3, 5, col);  // borne
  if (charging) {
    // rayo de carga (zigzag) en vez de la barra de nivel
    uint16_t bolt = C565(0xff, 0xd9, 0x4a);
    int bx = x + w / 2;
    gfx->fillTriangle(bx + 3, y + 1, bx - 4, y + 6, bx + 1, y + 6, bolt);
    gfx->fillTriangle(bx - 1, y + 5, bx + 4, y + 5, bx - 3, y + 10, bolt);
  } else {
    int fw = (w - 4) * pc / 100;
    if (fw > 0) gfx->fillRect(x + 2, y + 2, fw, h - 4, col);
  }
}

void drawHeader(const char *name, uint16_t nameColor, const char *msg) {
  gfx->setTextColor(nameColor);
  gfx->setTextSize(3);
  gfx->setCursor(CX - strlen(name) * 9, 72);
  gfx->print(name);
  gfx->setTextColor(inkColor());
  gfx->setTextSize(2);
  gfx->setCursor(CX - strlen(msg) * 6, 102);
  gfx->print(msg);
}

// animacion de la ceremonia (10s): despedida = reverencia con corazones y se
// aleja caminando; escapada = se asusta y sale corriendo. Sustituye al idle.
void drawCeremony() {
  if (!pmd.loaded) { drawPet(); return; }  // respaldo si no hay sprite PMD
  uint32_t now = millis();
  float t = pet.ceremonyT();               // 0..1 a lo largo de los 10s
  bool panic = (pet.ceremony == CER_RUNAWAY);
  int x = CX, y = PET_GROUND;
  uint8_t act = PMD_IDLE;

  if (panic) {
    // final triste: penumbra azulada + lluvia
    for (int i = 0; i < 46; i++) {
      int rx = (i * 47 + now / 3) % 466;
      int ry = (i * 91 + now / 2) % 470;
      gfx->drawLine(rx, ry, rx - 3, ry + 12, C565(0x6a, 0x84, 0xb0));
    }
    bool fade = false;
    if (t < 0.30f) {                       // cabizbajo, temblando
      act = pmd.has(PMD_HURT) ? PMD_HURT : PMD_IDLE;
      x = CX + (int)(4 * sinf(now * 0.04f));
    } else {                               // se aleja despacio y se desvanece
      act = pmd.has(PMD_WALKL) ? PMD_WALKL : PMD_IDLE;
      x = CX - (int)(((t - 0.30f) / 0.70f) * (CX + 120));
      fade = (t > 0.6f) && ((now / 160) % 2 == 0);  // parpadea hacia la silueta
    }
    drawPmdAct(act, x, y, now, true, fade, 5);  // fade=silueta: se difumina al irse
    // lagrima cayendo del bicho
    if (t < 0.55f) {
      int ty = y - 150 + (int)((now / 6) % 40);
      gfx->fillRect(x + 6, ty, 3, 6, C565(0x9a, 0xc4, 0xe8));
    }
    return;
  }

  // despedida epica: halo dorado pulsante + chispas y corazones que ascienden
  int gcy = PET_GROUND - 96;
  for (int k = 0; k < 4; k++) {
    int r = 60 + k * 34 + (int)(10 * sinf(now * 0.02f));
    gfx->drawCircle(CX, gcy, r, C565(0xff, 0xdf, 0x8a));
  }
  for (int i = 0; i < 16; i++) {
    int px = (i * 71 + 28) % 466;
    int py = 410 - (int)((now / 8 + i * 70) % 360);   // suben y reaparecen abajo
    if (py < 30) continue;
    if (i % 4 == 0) drawMap(SPR_HEART, 32, px - 8, py - 8, 1, false);  // corazoncito
    else gfx->fillRect(px, py, 4, 4, (i % 2) ? C565(0xff, 0xe7, 0x9f) : C565(0xff, 0x9a, 0xc0));
  }

  if (t < 0.45f) {                         // reverencia / pose de despedida
    act = pmd.has(PMD_POSE) ? PMD_POSE : (pmd.has(PMD_NOD) ? PMD_NOD : PMD_IDLE);
  } else {                                 // se aleja por la derecha
    act = pmd.has(PMD_WALKR) ? PMD_WALKR : PMD_IDLE;
    x = CX + (int)(((t - 0.45f) / 0.55f) * (CX + 140));
  }
  drawPmdAct(act, x, y, now, true, false, 5);
  if (pet.showHeart())                     // corazon grande siguiendo al bicho
    drawMap(SPR_HEART, 32, x + 50, y - 190, 2, false);
}

// dialogo de decision (2 botones apilados): evolucionar/mantener o despedirse/quedaros
void drawChoiceDialog() {
  if (choiceKind == 3) {
    gfx->fillRoundRect(52, 96, 356, 350, 18, UI_WHITE);
    gfx->drawRoundRect(52, 96, 356, 350, 18, UI_INK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    const char *title = T(S_EVO_Q);
    gfx->setCursor(CX - (int)strlen(title) * 6, 116);
    gfx->print(title);
    uint8_t options = pet.evolutionOptionCount();
    for (uint8_t i = 0; i < options; i++) {
      int16_t target = pet.evolutionOption(i);
      int y = 158 + i * 45;
      gfx->fillRoundRect(82, y, 312, 36, 10, UI_BAR_OK);
      gfx->setTextColor(UI_WHITE);
      const char *name = dexName(target);
      gfx->setCursor(CX - (int)strlen(name) * 6, y + 11);
      gfx->print(name);
    }
    gfx->fillRoundRect(82, 392, 312, 40, 10, UI_TRACK);
    gfx->setTextColor(UI_INK);
    const char *cancel = T(S_EVO_KEEP);
    gfx->setCursor(CX - (int)strlen(cancel) * 6, 404);
    gfx->print(cancel);
    return;
  }
  const char *q, *o1, *o2;
  uint16_t c1, c2, t1, t2;
  if (choiceKind == 1) {  // evolucion
    q = T(S_EVO_Q); o1 = T(S_EVO_TAP); o2 = T(S_EVO_KEEP);
    c1 = UI_BAR_BAD; t1 = UI_WHITE; c2 = UI_TRACK; t2 = UI_INK;
  } else {                // despedida
    q = T(S_FAR_Q); o1 = T(S_FAR_GO); o2 = T(S_FAR_STAY);
    c1 = UI_BAR_WARN; t1 = UI_INK; c2 = UI_BAR_OK; t2 = UI_WHITE;
  }
  gfx->fillRoundRect(73, 156, 320, 188, 16, UI_WHITE);
  gfx->drawRoundRect(73, 156, 320, 188, 16, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(q) * 6, 176);
  gfx->print(q);
  gfx->fillRoundRect(93, 206, 280, 52, 12, c1);     // boton accion
  gfx->setTextColor(t1);
  gfx->setCursor(CX - (int)strlen(o1) * 6, 224);
  gfx->print(o1);
  gfx->fillRoundRect(93, 268, 280, 52, 12, c2);     // boton mantener/quedaros
  gfx->setTextColor(t2);
  gfx->setCursor(CX - (int)strlen(o2) * 6, 286);
  gfx->print(o2);
}

// boton-CTA rojo y grande para evolucionar (pulsa para llamar la atencion)
void drawEvolveButton() {
  uint32_t now = millis();
  int p = (int)(5 * sinf(now * 0.006f));  // late: -5..5
  int x = EVO_BTN_X - p, y = EVO_BTN_Y - p, w = EVO_BTN_W + 2 * p, h = EVO_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 18, UI_BAR_BAD);
  gfx->drawRoundRect(x, y, w, h, 18, UI_WHITE);
  gfx->drawRoundRect(x + 2, y + 2, w - 4, h - 4, 16, UI_WHITE);
  gfx->setTextColor(UI_WHITE);
  gfx->setTextSize(3);
  const char *t = T(S_EVO_TAP);
  gfx->setCursor(CX - (int)strlen(t) * 9, y + h / 2 - 11);
  gfx->print(t);
}

// boton-CTA dorado de despedida: "<nombre> quiere decirte algo..."
void drawFarewellButton() {
  uint32_t now = millis();
  int p = (int)(4 * sinf(now * 0.005f));
  int x = FAR_BTN_X - p, y = FAR_BTN_Y - p, w = FAR_BTN_W + 2 * p, h = FAR_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 16, UI_BAR_WARN);
  gfx->drawRoundRect(x, y, w, h, 16, UI_INK);
  char buf[52];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  snprintf(buf, sizeof(buf), T(S_FAREWELL_BTN), nm);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(buf) * 6, y + h / 2 - 8);
  gfx->print(buf);
}

// boton-CTA sombrio de escapada por abandono: "<nombre> se siente abandonado..."
// (final triste: azul-gris oscuro, latido lento y apagado)
void drawRunawayButton() {
  uint32_t now = millis();
  int p = (int)(3 * sinf(now * 0.003f));
  int x = FAR_BTN_X - p, y = FAR_BTN_Y - p, w = FAR_BTN_W + 2 * p, h = FAR_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 16, C565(0x3a, 0x44, 0x5a));
  gfx->drawRoundRect(x, y, w, h, 16, C565(0x70, 0x80, 0x98));
  char buf[52];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  snprintf(buf, sizeof(buf), T(S_RUNAWAY_BTN), nm);
  gfx->setTextColor(C565(0xc8, 0xd2, 0xe0));
  gfx->setTextSize(2);
  gfx->setCursor(CX - (int)strlen(buf) * 6, y + h / 2 - 8);
  gfx->print(buf);
}

// animacion epica de evolucion: halo radial + rayos giratorios + parpadeo del
// sprite acelerando + chispas que salen disparadas + fogonazo final
void drawEvolveFX(uint32_t now) {
  float t = pet.evolveT();          // 0..1
  int cx = CX, cy = PET_GROUND - 96;

  // halo radial que crece y pulsa
  int halo = 36 + (int)(t * 150) + (int)(8 * sinf(now * 0.02f));
  for (int k = 0; k < 4; k++) {
    int r = halo - k * 7;
    if (r > 0) gfx->drawCircle(cx, cy, r, UI_WHITE);
  }
  // rayos giratorios desde el centro del bicho
  float base = now * 0.004f;
  for (int i = 0; i < 12; i++) {
    float a = base + i * (float)(PI / 6);
    int len = 90 + (int)(70 * (0.5f + 0.5f * sinf(now * 0.012f + i)));
    gfx->drawLine(cx, cy, cx + (int)(cosf(a) * len), cy + (int)(sinf(a) * len), UI_WHITE);
  }
  // parpadeo entre la forma ANTERIOR y la NUEVA (siluetas), acelerando; al
  // final (t>0.9) se queda fija en la nueva para el fogonazo de revelado
  int period = 60 + (int)(220 * (1.0f - t));
  bool showOld = t < 0.9f && evoPmd.loaded && ((now / period) % 2) == 0;
  if (showOld) drawPmdActM(evoPmd, PMD_IDLE, cx, PET_GROUND, 0, true, true, 5);
  else drawPmdAct(PMD_IDLE, cx, PET_GROUND, 0, true, true, 5);
  // chispas que salen disparadas
  for (int i = 0; i < 10; i++) {
    float a = i * (float)(PI / 5) + t * 4.0f;
    int d = (int)((now / 14 + i * 33) % 200);
    int sx = cx + (int)(cosf(a) * d), sy = cy + (int)(sinf(a) * d);
    gfx->fillRect(sx - 2, sy - 2, 5, 5, (i & 1) ? C565(0xff, 0xe0, 0x70) : UI_WHITE);
  }
  // fogonazo final antes de revelar la forma nueva
  if (t > 0.9f) gfx->fillCircle(cx, cy, (int)(300 * (t - 0.9f) / 0.1f), UI_WHITE);
}

void drawPet() {
  if (pmd.loaded) {
    drawPetPMD();
    return;
  }
  if (mon.loaded) {
    drawPetSD();
    return;
  }
  int fi = flashIdxForDex(pet.speciesId);
  if (fi < 0) {
    // sin SD y sin sprite de flash: aviso claro de que faltan sprites
    gfx->setTextColor(inkColor());
    gfx->setTextSize(6);
    gfx->setCursor(CX - 18, PET_CY - 80);
    gfx->print("?");
    gfx->setTextSize(2);
    const char *l1 = T(S_NO_SPRITES);
    gfx->setCursor(CX - (int)strlen(l1) * 6, PET_CY - 4);
    gfx->print(l1);
    const char *l2 = T(S_LOAD_SPRITES);
    gfx->setCursor(CX - (int)strlen(l2) * 6, PET_CY + 20);
    gfx->print(l2);
    return;
  }
  const Species &sp = SPECIES[fi];
  int s = sp.scale;
  int x = CX - 16 * s;
  int y = PET_CY - 16 * s;

  // animacion de evolucion: alterna la silueta de la forma anterior y la nueva
  if (pet.evolving()) {
    bool flash = (millis() / 300) % 2;
    int16_t showDex = (flash && pet.prevSpeciesId >= 0) ? pet.prevSpeciesId : pet.speciesId;
    int sfi = flashIdxForDex(showDex);
    if (sfi >= 0) {
      const Species &show = SPECIES[sfi];
      drawMap(show.sprite, SPRITE_H, CX - 16 * show.scale, PET_CY - 16 * show.scale, show.scale, flash);
    }
    return;
  }

  PetMood m = pet.mood();
  if (m == MOOD_HAPPY && (millis() / 500) % 2) y -= 6;  // saltito

  drawMap(sp.sprite, SPRITE_H, x, y, s, false);

  // expresiones superpuestas usando las anclas de la especie
  bool blink = (millis() % 3500 < 300);
  if (m == MOOD_SLEEPING || blink) {
    overlayEye(sp, x, y, s, sp.eyeColL);
    overlayEye(sp, x, y, s, sp.eyeColR);
  }
  if (m == MOOD_EATING) overlayMouth(sp, x, y, s, true);
  else if (m == MOOD_SAD) overlayMouth(sp, x, y, s, false);

  if (pet.showHeart()) drawMap(SPR_HEART, 32, x + 20 * s, y - 2 * s, 2, false);
}

// ---------- escena de bano ----------

void startBath() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony || bathUntil) return;
  bathUntil = millis() + 3000;
  bathPending = true;
  sfxPlay(SFX_EVENT_SPARKLE);
  int cx = (int)beh.x;
  for (auto &b : bubbles) {
    b.x = cx - 70 + random(140);
    b.y = PET_GROUND - random(150);
    b.r = 8 + random(16);
    b.ph = random(64);
  }
}

void drawBath() {
  if (!bathUntil) return;
  uint32_t now = millis();
  if (deadlineReached(now, bathUntil)) {
    bathUntil = 0;
    if (bathPending) {
      bathPending = false;
      pet.clean();
      // pose de alegria al quedar limpio
      if (pmd.has(PMD_POSE)) {
        beh.mode = 2;
        beh.act = PMD_POSE;
        beh.t0 = now;
        beh.until = now + pmdActTotalMs(pmd.acts[PMD_POSE]) * 2;
      }
    }
    return;
  }
  uint32_t left = bathUntil - now;
  if (left > 800) {
    // espuma: pompas meciendose y subiendo poco a poco
    float t = now / 220.0f;
    for (auto &b : bubbles) {
      int bx = b.x + (int)(sinf(t + b.ph) * 6);
      int by = b.y - (int)((3000 - left) / 90);
      gfx->fillCircle(bx, by, b.r, UI_WHITE);
      gfx->drawCircle(bx, by, b.r, 0x7E3D);
      gfx->fillCircle(bx - b.r / 3, by - b.r / 3, b.r / 4, UI_BG_DAY);
    }
  } else {
    // las pompas revientan: destellos
    for (int i = 0; i < 8; i++) {
      auto &b = bubbles[i];
      int sx = b.x + (i % 3) * 6 - 6, sy = b.y - 18;
      uint16_t col = (i % 2) ? UI_BAR_WARN : UI_WHITE;
      gfx->fillRect(sx - 6, sy - 1, 13, 3, col);
      gfx->fillRect(sx - 1, sy - 6, 3, 13, col);
    }
  }
}

// ---------- mascota PMD: comportamiento ----------

uint32_t pmdActTotalMs(const PmdAct &a) {
  uint32_t t = 0;
  for (uint8_t i = 0; i < a.frames; i++) t += a.ms[i];
  return t ? t : 100;
}

uint8_t pmdFrameAt(const PmdAct &a, uint32_t t, bool loop) {
  uint32_t total = pmdActTotalMs(a);
  if (!loop && t >= total) return a.frames - 1;
  t %= total;
  uint8_t i = 0;
  while (t >= a.ms[i]) {
    t -= a.ms[i];
    i = (i + 1) % a.frames;
  }
  return i;
}

// dibuja una accion anclada por la base (centro-x, suelo) y devuelve su escala
// dibuja una accion de un PmdMon concreto (m); drawPmdAct usa el global pmd
void drawPmdActM(PmdMon &m, uint8_t actId, int cx, int groundY, uint32_t t, bool loop, bool sil, uint8_t maxS) {
  const PmdAct &a = m.acts[actId];
  if (!a.frames) return;
  uint8_t sBase = m.acts[PMD_IDLE].h ? 170 / m.acts[PMD_IDLE].h : 5;
  if (sBase < 2) sBase = 2;
  if (sBase > maxS) sBase = maxS;
  uint8_t s = sBase;
  while (s > 2 && a.h * s > 250) s--;  // acciones con frame grande (ataque)
  uint8_t fi = pmdFrameAt(a, t, loop);
  const uint8_t *fr = a.data + (uint32_t)fi * a.w * a.h;
  // anclar por los pies (a.base), no por el alto del lienzo: asi las acciones
  // con padding distinto (Hurt, Eat...) quedan todas a la misma altura de suelo
  int x0 = cx - a.w * s / 2, y0 = groundY - (a.base ? a.base : a.h) * s;
  for (int r = 0; r < a.h; r++) {
    const uint8_t *row = fr + r * a.w;
    for (int c = 0; c < a.w; c++) {
      uint8_t idx = row[c];
      if (idx == 0xFF) continue;
      gfx->fillRect(x0 + c * s, y0 + r * s, s, s, sil ? INK_K : m.pal[idx]);
    }
  }
}
void drawPmdAct(uint8_t actId, int cx, int groundY, uint32_t t, bool loop, bool sil, uint8_t maxS) {
  drawPmdActM(pmd, actId, cx, groundY, t, loop, sil, maxS);
}

// elige el siguiente capricho del bicho cuando esta contento
void behNext() {
  uint32_t now = millis();
  beh.t0 = now;
  int r = random(100);
  uint8_t walkChance = (currentDayPhase() == 3) ? 10 : 35;
  if (r < walkChance && (pmd.has(PMD_WALKL) || pmd.has(PMD_WALKR))) {
    beh.mode = 1;  // paseo
    beh.targetX = 150 + random(176);
    beh.until = now + 15000;
  } else if (r < 60) {
    // gesto aleatorio entre los disponibles
    // (Hop fuera: salta demasiado alto; Sit fuera: mira hacia atras)
    static const uint8_t flair[] = { PMD_POSE, PMD_NOD, PMD_BREATH };
    uint8_t pick[3], n = 0;
    for (uint8_t f : flair)
      if (pmd.has(f)) pick[n++] = f;
    if (n) {
      beh.mode = 2;
      beh.act = pick[random(n)];
      beh.until = now + pmdActTotalMs(pmd.acts[beh.act]);
      return;
    }
    beh.mode = 0;
    beh.until = now + 2000 + random(3000);
  } else {
    beh.mode = 0;  // mirar al frente
    beh.until = now + 2000 + random(3000);
  }
}

void drawPetPMD() {
  uint32_t now = millis();

  if (pet.evolving()) {
    drawEvolveFX(now);
    return;
  }
  if (evoPmd.loaded) evoPmd.unload();  // termino la evolucion: libera la forma anterior

  PetMood m = pet.mood();
  uint8_t act;
  bool loop = true;
  if (m == MOOD_SLEEPING && pmd.has(PMD_SLEEP)) {
    act = PMD_SLEEP;
    beh.mode = 0;
  } else if (m == MOOD_EATING && pmd.has(PMD_EAT)) {
    act = PMD_EAT;
    beh.t0 = 0;
  } else if (m == MOOD_SAD && pmd.has(PMD_HURT)) {
    act = PMD_HURT;
  } else {
    // contento: el planificador decide (idle / paseo / gesto)
    if (deadlineReached(now, beh.until)) behNext();
    if (beh.mode == 1) {
      float d = beh.targetX - beh.x;
      if (fabsf(d) < 4) {
        behNext();
        act = PMD_IDLE;
      } else {
        beh.x += (d > 0 ? 3.0f : -3.0f);
        act = (d > 0) ? PMD_WALKR : PMD_WALKL;
      }
    } else {
      act = (beh.mode == 2) ? beh.act : PMD_IDLE;
      loop = false;
    }
    if (!pmd.has(act)) act = PMD_IDLE;
  }

  drawPmdAct(act, (int)beh.x, PET_GROUND, now - beh.t0, loop || act == PMD_IDLE, false, 5);

  if (pet.showHeart()) drawMap(SPR_HEART, 32, (int)beh.x + 50, PET_GROUND - 190, 2, false);
}

// sprite animado desde la SD: zoom entero por pixel, frames a su ritmo
void drawPetSD() {
  int s = mon.scale;
  int w = mon.w * s, h = mon.h * s;
  int x = CX - w / 2;
  int y = PET_CY - h / 2;

  bool sil = false;
  if (pet.evolving()) {
    sil = (millis() / 300) % 2;
  } else if (pet.mood() == MOOD_HAPPY && (millis() / 500) % 2) {
    y -= 6;  // saltito
  }

  uint16_t fm = mon.frameMs ? mon.frameMs : 100;
  uint16_t fi = pet.sleeping ? 0 : (millis() / fm) % mon.frames;
  const uint8_t *fr = mon.data + (uint32_t)fi * mon.w * mon.h;
  for (int r = 0; r < mon.h; r++) {
    const uint8_t *row = fr + r * mon.w;
    for (int c = 0; c < mon.w; c++) {
      uint8_t idx = row[c];
      if (idx == 0xFF) continue;
      gfx->fillRect(x + c * s, y + r * s, s, s, sil ? INK_K : mon.pal[idx]);
    }
  }

  // emotes en vez de expresiones (los sprites importados no tienen anclas)
  if (pet.showHeart()) drawMap(SPR_HEART, 32, x + w - 30, y - 50, 2, false);
}

// ojo cerrado: borra el ojo 3x4 y dibuja el parpado
void overlayEye(const Species &sp, int x, int y, int s, int col) {
  gfx->fillRect(x + col * s, y + sp.eyeRow * s, 3 * s, 4 * s, sp.bodyColor);
  gfx->fillRect(x + col * s, y + (sp.eyeRow + 2) * s, 3 * s, s, INK_K);
}

// borra la sonrisa base y pinta boca abierta (comer) o ceno (triste)
void overlayMouth(const Species &sp, int x, int y, int s, bool open) {
  int mc = sp.mouthCol, mr = sp.mouthRow;
  gfx->fillRect(x + (mc - 3) * s, y + mr * s, 7 * s, 2 * s, sp.bodyColor);
  if (open) {
    gfx->fillRect(x + (mc - 2) * s, y + mr * s, 5 * s, 2 * s, INK_K);
  } else {
    gfx->fillRect(x + (mc - 2) * s, y + mr * s, 5 * s, s, INK_K);
    gfx->fillRect(x + (mc - 3) * s, y + (mr + 1) * s, s, s, INK_K);
    gfx->fillRect(x + (mc + 3) * s, y + (mr + 1) * s, s, s, INK_K);
  }
}

void drawPoops() {
  for (int i = 0; i < pet.poops; i++) {
    drawMap(SPR_POOP, 32, 36 + i * 46, 244, 2, false);
  }
}

// Barras de necesidades: cuatro arcos junto al borde redondo (dos a cada lado),
// cada uno con el icono de su boton. Se llenan hacia arriba.
// Los pixeles de cada arco se calculan una vez (PSRAM) con su posicion a lo largo
// del arco (0-255): dibujar es solo pintar la lista, sin trigonometria por frame.
#define ARC_R_IN 206
#define ARC_R_OUT 221
#define ARC_ICON_R 180

static ArcShape needArcs[4] = {
  { 186, 224, nullptr, 0 },  // comida: arriba izquierda
  { 136, 174, nullptr, 0 },  // energia: abajo izquierda
  { 354, 316, nullptr, 0 },  // alegria: arriba derecha
  { 44, 6, nullptr, 0 },     // higiene: abajo derecha
};

static void arcPoint(float deg, float r, int &x, int &y) {
  float a = deg * (float)PI / 180.0f;
  x = (int)lroundf(CX + r * cosf(a));
  y = (int)lroundf(CY + r * sinf(a));
}

static void buildArc(ArcShape &a) {
  float lo = a.startDeg < a.endDeg ? a.startDeg : a.endDeg;
  float hi = a.startDeg < a.endDeg ? a.endDeg : a.startDeg;
  float rMid = (ARC_R_IN + ARC_R_OUT) / 2.0f, capR = (ARC_R_OUT - ARC_R_IN) / 2.0f + 0.5f;
  int sx, sy, ex, ey;
  arcPoint(a.startDeg, rMid, sx, sy);
  arcPoint(a.endDeg, rMid, ex, ey);
  // solo el rectangulo que contiene el arco (antes se recorria toda la pantalla)
  int bx0 = LCD_WIDTH, by0 = LCD_HEIGHT, bx1 = 0, by1 = 0;
  for (float dg = lo; dg <= hi + 0.01f; dg += 1.0f) {
    for (int r = ARC_R_IN - 9; r <= ARC_R_OUT + 1; r += ARC_R_OUT + 10 - ARC_R_IN) {
      int px, py;
      arcPoint(dg, r, px, py);
      if (px < bx0) bx0 = px; if (px > bx1) bx1 = px;
      if (py < by0) by0 = py; if (py > by1) by1 = py;
    }
  }
  bx0 = bx0 > 10 ? bx0 - 10 : 0; by0 = by0 > 10 ? by0 - 10 : 0;
  bx1 = bx1 + 10 < LCD_WIDTH ? bx1 + 10 : LCD_WIDTH - 1; by1 = by1 + 10 < LCD_HEIGHT ? by1 + 10 : LCD_HEIGHT - 1;
  for (int pass = 0; pass < 2; pass++) {  // 1: contar, 2: guardar
    uint16_t n = 0;
    for (int y = by0; y <= by1; y++) {
      for (int x = bx0; x <= bx1; x++) {
        float dx = x - CX, dy = y - CY, r2 = dx * dx + dy * dy;
        if (r2 < (ARC_R_IN - 9) * (ARC_R_IN - 9) || r2 > (ARC_R_OUT + 1) * (ARC_R_OUT + 1)) continue;
        float deg = atan2f(dy, dx) * 180.0f / (float)PI;
        if (deg < 0) deg += 360.0f;
        float t;
        bool inRing = r2 >= ARC_R_IN * ARC_R_IN && r2 <= ARC_R_OUT * ARC_R_OUT;
        if (inRing && deg >= lo && deg <= hi) {
          t = (deg - a.startDeg) / (a.endDeg - a.startDeg);
        } else if ((x - sx) * (x - sx) + (y - sy) * (y - sy) <= capR * capR) {
          t = 0;  // punta redondeada del inicio
        } else if ((x - ex) * (x - ex) + (y - ey) * (y - ey) <= capR * capR) {
          t = 1;  // punta del final
        } else {
          continue;
        }
        if (pass == 1) a.pix[n] = { (uint16_t)x, (uint16_t)y, (uint8_t)(t * 255.0f + 0.5f) };
        n++;
      }
    }
    if (pass == 0) {
      a.pix = (ArcPix *)ps_malloc(sizeof(ArcPix) * n);
      if (!a.pix) return;
    }
    a.n = n;
  }
}

static void drawNeedArc(ArcShape &a, uint8_t val, const char *const *icon) {
  if (!a.pix) buildArc(a);
  uint16_t fill = (val >= 50) ? UI_BAR_OK : (val >= 25) ? UI_BAR_WARN : UI_BAR_BAD;
  uint16_t track = gNight ? 0x4208 : UI_TRACK;
  uint16_t *fb = gfx->getFramebuffer();
  uint16_t limit = val ? (uint16_t)(val * 255 / 100) : 0;
  if (fb && a.pix) {
    for (uint16_t i = 0; i < a.n; i++) {
      const ArcPix &p = a.pix[i];
      fb[p.y * LCD_WIDTH + p.x] = (val && p.t <= limit) ? fill : track;
    }
  }
  int ix, iy;
  arcPoint((a.startDeg + a.endDeg) / 2, ARC_ICON_R, ix, iy);
  gfx->fillCircle(ix, iy, 19, gNight ? UI_BG_NIGHT : UI_WHITE);
  gfx->drawCircle(ix, iy, 19, fill);
  gfx->drawCircle(ix, iy, 18, fill);
  drawMap(icon, 16, ix - 16, iy - 16, 2, false);
}

void drawBars() {
  drawNeedArc(needArcs[0], pet.fullness, SPR_ICON_FOOD);
  drawNeedArc(needArcs[1], pet.energy, SPR_ICON_LIGHT);
  drawNeedArc(needArcs[2], pet.joy, SPR_ICON_PLAY);
  drawNeedArc(needArcs[3], pet.hygiene, SPR_ICON_CLEAN);
}

void drawButtons() {
  for (int i = 0; i < 4; i++) {
    bool off = pet.sleeping && i != 2;  // durmiendo solo funciona LUZ
    int bx = buttons[i].cx - BTN_HALF, by = buttons[i].cy - BTN_HALF;
    gfx->fillRoundRect(bx, by, 2 * BTN_HALF, 2 * BTN_HALF, 18, pet.sleeping ? UI_BG_NIGHT : UI_WHITE);
    gfx->drawRoundRect(bx, by, 2 * BTN_HALF, 2 * BTN_HALF, 18, inkColor());
    if (!off) drawMap(buttons[i].icon, 16, buttons[i].cx - 24, buttons[i].cy - 24, 3, false);
  }
}

const char *eggMsg() {
  switch (pet.eggCracks()) {
    case 0: return T(S_EGG_TOUCH);
    case 1: return T(S_EGG_MOVES);
    default: return T(S_EGG_ALMOST);
  }
}

const char *statusMsg() {
  if (pet.evolving()) return T(S_EVOLVING);
  if (bathUntil) return "Splish splash!";  // onomatopeya universal
  if (pet.sleeping) return "Zzz...";
  if (pet.eating()) return T(S_EATING);
  if (pet.showHeart()) return T(S_LIKES);
  if (pet.fullness < 25) return T(S_HUNGRY);
  if (pet.hygiene < 25) return T(S_NEEDS_BATH);
  if (pet.energy < 25) return T(S_EXHAUSTED);
  if (pet.joy < 25) return T(S_SAD);
  if (pet.weight > 60) return T(S_CHUBBY);
  if (pet.shiny && pet.ageMinutes < 15) return T(S_IS_SHINY);
  return T(S_HAPPY);
}

// dibuja un mapa de n x n pixeles escalado; silhouette=true lo pinta en tinta
void drawMap(const char *const *map, int n, int x, int y, int s, bool silhouette) {
  for (int r = 0; r < n; r++) {
    for (int c = 0; c < n; c++) {
      char ch = map[r][c];
      if (ch == '.') continue;
      gfx->fillRect(x + c * s, y + r * s, s, s, silhouette ? INK_K : spriteColor(ch));
    }
  }
}
