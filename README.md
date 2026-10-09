# Voice-Controlled Violin Practice Assistant

A violin practice prototype combining an ESP32-S3, an Orange Pi web dashboard, and projected visual guidance. It provides real-time pitch feedback, voice-controlled note and piece selection, score progression, and targeted passage review.

## Demonstrated interaction

- Play a single target note and use pitch feedback to correct intonation.
- Change the target note by a Chinese voice command, without first ending practice.
- Practice a piece while accepted notes advance the score and its displayed rows.
- Request review of a difficult passage.
- Switch pieces during practice.

The current demonstration uses Chinese voice commands and an interface with English video captions. These are prototype capabilities, not a controlled assessment of learning outcomes or noise robustness.

## System

The ESP32-S3 captures microphone audio, estimates pitch, handles practice state and voice commands, and exchanges telemetry/control with the Orange Pi over USB serial. The Orange Pi runs a Flask dashboard and the song library. Its `/hud` page can be displayed through an HDMI projector; `/control` provides controls on another computer on the same network. Cloud speech recognition requires configured credentials and network access; pitch processing is performed on the ESP32.

Firmware pin assignments are in `main/app_config.h`. Use compatible microphone, display, and LED wiring matching those definitions. The tested board configuration uses ESP32-S3 and 16 MB flash; check hardware before flashing. Exact component model numbers and a wiring photograph should be added to `docs/` before presenting this as a fully documented hardware reproduction package.

## Repository layout

- `main/`: ESP32 firmware and component dependency manifest.
- `OrangePi1/legacy_web/`: Flask app, serial bridge, teacher/review logic, HTML, CSS and JavaScript.
- `sdkconfig.defaults`: sanitized snapshot of the release configuration.
- `partitions.csv`: flash partition layout, when present in the source project.
- `docs/`: hardware photographs and wiring notes.
- `media/`: demonstration video and representative image.

Historical internal directory/project names are retained to keep build paths compatible.

## Run the dashboard on Orange Pi

Use Python 3.9 or later, and a supported environment for the declared dependencies. From the repository:

```bash
cd OrangePi1/legacy_web
python3 -m venv .venv
.venv/bin/python -m pip install -r requirements.txt
VIOLIN_AUTO_SERIAL=1 VIOLIN_SERIAL_PORT=/dev/ttyUSB0 .venv/bin/python app.py
```

Replace `/dev/ttyUSB0` with the actual ESP32 serial port. Ensure the user can access that port. Open `http://127.0.0.1:8040/hud` on the Orange Pi. From another computer, replace `127.0.0.1` with the Orange Pi's LAN IP and open `/control`.

The app initializes its database and default songs on first run. Personal song databases and practice history are deliberately excluded. Custom arrangements from the private project are not automatically included; import them separately through the song upload interface if authorized to distribute them.

## Build ESP32 firmware

Use ESP-IDF **5.5.4**. Dependencies are declared in `main/idf_component.yml` and resolved by the component manager; generated `managed_components/` and build directories are not committed.

Copy `main/app_secrets.example.h` to `main/app_secrets.h`, then fill in Wi-Fi, speech service credentials and the Orange Pi's LAN URL. Do not commit this private file. From an ESP-IDF shell at the repository root:

```text
idf.py set-target esp32s3
idf.py build
idf.py -p YOUR_PORT flash
```

For Windows, `YOUR_PORT` might be `COM4`; on Linux it might be `/dev/ttyUSB0`. On a new checkout, target/configuration initialization may reset local build configuration; retain a private backup of any existing working project. The supplied defaults retain the tested configuration, including selected wake models, but a fresh checkout has not been verified on every hardware variant.

## Voice examples

Say `你好小智`, wait for command capture, then give the command:

| Command | Meaning |
|---|---|
| `练习D四` / `练习第四` | Practice D4 |
| `练习E五` / `我要练习一五` | Practice E5 |
| `练习升D四` | Practice D-sharp 4 |
| `练习生日快乐` | Select Happy Birthday |
| `练习找朋友` | Select Find a Friend |

Cloud transcription can fail, particularly when speech and instrument audio overlap. An explicit diagnostic serial command such as `START_NOTE:D4` can separate speech recognition issues from target-switching issues.

## Release status

The stabilized demo release is **V10.8.2-SINGLE**. The contributor's local demonstration covers pitch correction, single-note voice switching, piece practice, passage review and piece switching. Rhythm/follow practice remains experimental and is not a validated timing assessment. This repository makes no claim of a novel pitch algorithm, measured learning benefit, or universal noise immunity.

The preparation tool checks packaging and selected private configuration values; it does not certify a clean-room firmware build or scan every possible secret format. Keep the private complete project and successful flash artifacts outside this repository.

## Demo and documentation

After adding the demonstration file, view [the demo](media/demo.mp4). Add a system photograph under `media/system.jpg` and wiring notes in `docs/`. The demo currently illustrates system operation, not a user study.

## License and dependencies

A project license has not yet been selected. Before describing this as an open-source release, choose a license for the original code you are authorized to license. ESP-IDF and third-party components retain their respective licenses; their generated distributions are downloaded separately and are not relicensed by this repository.
