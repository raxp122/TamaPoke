# TamaPoke – regole del progetto

Firmware ESP32-S3 (Arduino) per la Waveshare ESP32-S3-Touch-AMOLED-1.75 (AMOLED tondo 466×466,
touch CST9217, RTC PCF85063, PMU AXP2101, IMU QMI8658, audio ES8311, microSD in SDMMC 1-bit),
più un installer da browser in `web/`.

## Dove sta cosa

- `TamaPoke.ino`: interfaccia, schermate, input, loop principale (file grande: cerca prima di leggerlo tutto).
- `pet.*` logica e salvataggio del Pokémon (NVS, namespace `tamapoke`), `battle.*` lotte, `dex.h` tabella specie.
- `sdmon.*` sprite e miniature dalla microSD (`/mons`), `audio.*` suoni sintetici, `i18n.*` testi in 6 lingue.
- `netlogic.*` logica pura di Wi-Fi/OTA (reti salvate, versioni, `version.json`, parser TPAK), coperta dai test.
- `netota.*` Wi-Fi e aggiornamenti OTA sul dispositivo (task FreeRTOS, NVS namespace `tpnet`).
- `.github/workflows/release.yml`: compila a ogni push; a ogni push su `main` pubblica una release GitHub
  con `version.json`, `tamapoke-app.bin`, `tamapoke-sprites.pak` (più i file per il primo flash via USB).
- `tools/`: script per sprite e installer web; `tests/`: test nativi.
- `tools/sim/`: simulatore dello schermo sul PC (il vero `TamaPoke.ino` con hardware finto). `bash tools/sim/run.sh`
  esegue i copioni in `tools/sim/scenes/` e scrive in `tools/sim/out/` gli screenshot e `layout-report.md`
  (elementi fuori dal cerchio, pulsanti sotto 44 px). Le modifiche all'interfaccia si controllano qui prima dell'hardware.

## Versioni

- Release automatiche: `1.37.<numero build>`, scritte dalla CI in `version_ci.h` (mai in git).
- Senza `version_ci.h` (build locale) vale `FW_VERSION` in `TamaPoke.ino`.
- Profili di build: `-DTAMAPOKE_GEN3_RELEASE` (pubblico) o `-DTAMAPOKE_LOCAL_TEST` (debug con `TESTMON`, `TESTEVO`…).
  Il build pubblico non deve contenere comandi di debug.

## Regole

- Il salvataggio vive nell'NVS: niente migrazioni inutili, mai "Erase device"/cancellazione della flash negli
  aggiornamenti. La tabella partizioni (`app3M_fat9M_16MB`, due slot OTA) non si cambia senza un piano per i salvataggi.
- Nuova logica: prima nei test nativi (`tests/`), poi UI e hardware.
- Ogni PR aggiunge le sue modifiche in `CHANGELOG.md` (sezione "Next", in inglese); al merge la sezione
  prende il numero della release pubblicata.
- Testi della firmware solo ASCII (il font non ha accenti); ogni nuova stringa va aggiunta in tutte e 6 le lingue
  di `i18n.cpp` nello stesso ordine dell'enum in `i18n.h`.
- Sprite sulla microSD: l'installer web manda `sprites.pak` + `sprites-gen3-update.pak`; l'OTA manda
  `tamapoke-sprites.pak` (stesso contenuto, un file solo) e solo quando è cambiato.
- `web/dev.html`, `web/manifest-local.json` e `TAMAPOKE_LOCAL_TEST` non vanno nell'installer pubblico.
- Niente binari storici in git; niente audio originale Pokémon/ROM: l'audio resta sintetico.
- Push, merge e release solo quando l'utente lo chiede (ogni merge su `main` diventa una release OTA).

## Build e test

```bash
cd tests && make CXX=clang++ clean test      # con g++ recente pet.cpp non passa -Werror=extra
```

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi,PartitionScheme=app3M_fat9M_16MB" \
  --build-property compiler.cpp.extra_flags=-DTAMAPOKE_GEN3_RELEASE --build-path build .
```

Toolchain: core ESP32 Arduino 3.3.10, GFX Library for Arduino 1.6.7, SensorLib 0.4.1, XPowersLib 0.3.3.
L'hardware (touch, clipping sullo schermo tondo, Wi-Fi reale, SD, conservazione del salvataggio) resta la prova finale.

`AI_HANDOFF.md`, `progress.md` e `HOENN_WALK_LOCAL.md` sono note storiche dell'autore originale (in parte in tedesco):
utili come contesto, non come regole.
