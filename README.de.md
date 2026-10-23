<p align="center">
  <a href="README.md">English</a> · <a href="README.zh-CN.md">简体中文</a> · <a href="README.de.md">Deutsch</a>
</p>

# gbs-anywhere ESP32-S3

**Mühle, Espressomaschine und Kaffeeablauf — auf einem ESP32-S3.**

[![CI](https://github.com/zpmdd/gbs-anywhere-esp32/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/zpmdd/gbs-anywhere-esp32/actions/workflows/ci.yml)
[![Lizenz: AGPL-3.0-or-later](https://img.shields.io/badge/license-AGPL--3.0--or--later-53694d)](LICENSE)
[![Plattform: ESP32-S3](https://img.shields.io/badge/platform-ESP32--S3-53694d)](docs/COMPATIBILITY.md)
[![Status: experimentell](https://img.shields.io/badge/status-experimental-b78c50)](docs/PROGRESS.md)

<picture>
  <source media="(max-width: 640px)" srcset="docs/images/project-cover-mobile.png">
  <img src="docs/images/project-cover.png" alt="Native AMOLED-Oberfläche mit synthetischen Demodaten.">
</picture>

Mahlkönig-Generic-WiFi-Rückmeldung und La-Marzocco-Cloud-Anbindung ohne dauerhaft
laufenden Computer, Container oder NAS. Zur Wahl stehen eine kleine Bridge ohne
Display und eine integrierte Touchscreen-Version mit Maschinensteuerung,
Rezeptübernahme und LAN-Updates. Ein Computer wird für die Ersteinrichtung und
Kopplung benötigt.

**[Einstieg](#einstieg)** · **[Kompatibilität](docs/COMPATIBILITY.md)** · **[Bildergalerie](docs/GALLERY.md)** · **[Validierung & Planung](docs/PROGRESS.md)**

> Experimentelle Firmware mit Praxistests bestimmter Builds. Internet und
> La Marzocco Cloud sind erforderlich. Ob der Mahlgrad angepasst wird,
> entscheidet die Mühle; eine bestätigte Rückmeldung garantiert keine Verstellung.

## Von der Mahlung zur Rückmeldung

| Schritt | Ablauf |
| --- | --- |
| **1 · Mahlen** | Der ESP nimmt einen neuen GbS-Auftrag der gekoppelten Mühle an. |
| **2 · Rezept übernehmen** | Die integrierte Firmware ordnet die aktuelle Mahlung dem normalen CSV-Export zu und setzt das Zielgewicht während der Puck-Vorbereitung. |
| **3 · Beziehen** | Nach Prüfung des Zielgewichts starten Sie den Bezug. Maschine und Waage liefern gemessene Zeit und Getränkemasse an die Cloud. |
| **4 · Rückmelden** | Der ESP sendet die Messwerte an die Mühle und zeigt deren Empfangsbestätigung an. |

Die schlanke Bridge übernimmt nur die Rückmeldung, ohne Rezeptübernahme oder
Maschinensteuerung. [Integrierte Firmware →](docs/INTEGRATED.md)

## Funktionen

- **Mühlenanbindung:** neue Aufträge, gemessene Bezugsergebnisse, Monitor-/Aktivmodus,
  Verbindungsdiagnose und persistente Aufzeichnungen gegen unbeabsichtigte Wiederholung.
- **Maschinensteuerung:** Schnellwahl des Getränkezielgewichts, Brühtemperatur,
  Dampfstufen, Brew-by-Weight-Einstellungen, Prebrew, Ein/Aus und Smart Standby.
- **Native Oberfläche:** LVGL, große Ziffern, Symbole und Farben, Bezugsdetails,
  Verlauf und Bearbeitung vorhandener Zeitpläne.
- **AMOLED im Akkubetrieb:** Waagenstatus und Ladestand in einem Symbol,
  separater Geräteakku, letzter Waagenladestand auch nach dem Ausschalten,
  Aufheizprognosen, optionale Rotation, Tasten und akustische Hinweise.
- **Wartung:** authentifizierte LAN-OTA-Updates, gepufferte Diagnose und
  Framebuffer-Vorschau in den integrierten Builds. Anwendungs-OTA erhält die NVS-Konfiguration.

Die AMOLED-Anpassung liegt im öffentlichen Branch
[`codex/amoled-216-local-adaptation`](https://github.com/zpmdd/gbs-anywhere-esp32/tree/codex/amoled-216-local-adaptation)
und ist noch nicht in `main` enthalten.

## Oberfläche

<table>
  <tr>
    <td align="center"><img src="docs/images/amoled-link.png" width="400" alt="Rezeptdetails mit bestätigter Übernahme von 20 auf 30 Gramm"><br><strong>Rezept und Rückmeldung</strong></td>
    <td align="center"><img src="docs/images/amoled-history.png" width="400" alt="Verlauf mit gemessener Getränkemasse und Bezugsdauer"><br><strong>Bezugsverlauf</strong></td>
  </tr>
</table>

Die Bilder stammen aus dem kompilierten LVGL-Renderer und verwenden
**synthetische Demodaten**. Die nativen Bildschirmtexte sind derzeit Chinesisch.
Die Bilder sind keine Gerätefotos und kein Nachweis eines neuen Praxistests.
[Weitere Ansichten und Bildquellen →](docs/GALLERY.md)

## Hardware

| Board / Variante | Display · Flash / PSRAM | Quelle und Stand |
| --- | --- | --- |
| **Waveshare ESP32-S3-Touch-LCD-1.69**, integriert | 240 × 280 · 16 MB / 8 MB | `main`, `touchscreen`; Touch, Rezeptübernahme in beide Richtungen, OTA und Waagenstatus praktisch geprüft. |
| **Waveshare ESP32-S3-Touch-AMOLED-2.16**, integriert | 480 × 480 · 16 MB / 8 MB | AMOLED-Branch, `amoled216`; Rotation, PWR-Zyklen, Akkuhistorie und Bezüge geprüft. Letzte Anzeigekorrektur benötigt noch einen Praxistest über mehrere Bezüge. |
| **ESP32-S3 ohne Display**, schlanke Bridge | 8 MB / 8 MB octal | `main`, Root-Anwendung; Praxisergebnisse gelten jeweils für den geprüften Build. |
| **ESP32-S3 ohne Display**, integriert | 8 MB / 8 MB octal | `main`, `headless`; lokal gebaut, eigene Hardwareabnahme steht aus. |
| **1,69-Zoll-Vorschau mit externem Hub** | 240 × 280 · 16 MB / 8 MB | `main`, Root-`GBS_TOUCHSCREEN=ON`; Simulator/Client geprüft, separater Hub erforderlich. |

| Kaffeegerät | Umfang |
| --- | --- |
| **Mahlkönig E65W GbS** | Praktisch geprüfte Rückmeldung und integrierte Rezeptübernahme; Generic-WiFi-Kopplung erforderlich. |
| **E65T / E80W / E80T GbS** | Dieselbe Generic-WiFi-Protokollfamilie, in diesem ESP32-Projekt nicht am Gerät geprüft. |
| **La Marzocco Linea Mini R** | Praktischer Aufbau für Steuerung, Cloud-Ergebnisse und Brew-by-Weight. |
| **Mit der Maschine gekoppelte Brew-by-Weight-Waage** | Messwerte und Akkudaten kommen über Maschine/Cloud; keine direkte Kopplung mit dem ESP. |
| **E64 WS / andere Maschinen** | Hier nicht implementiert oder nicht eigenständig geprüft. Der Desktop-Upstream verwendet für E64 WS einen anderen Protokollweg. |

Firmware-Dateien verschiedener Boards sind nicht austauschbar.
[Vollständige Kompatibilitäts- und Funktionsmatrix →](docs/COMPATIBILITY.md)

## Einstieg

1. Passendes Boardprofil und ESP-IDF auswählen.
2. Einmalig mit der unterstützten E65/E80-Desktop-Einrichtung koppeln und die private Konfiguration exportieren.
3. Bisherigen Client stoppen: Eine gekoppelte Mühlenidentität darf nur einen aktiven Eigentümer haben.
4. Nach der passenden Anleitung bauen und provisionieren; neue Importe starten im Monitormodus.
5. Bereitschaft abwarten und vor dem Bezug das Zielgewicht prüfen.

| Ziel | Anleitung |
| --- | --- |
| Ein Gerät mit LCD, Hub und Steuerung | [Integrierte Firmware und OTA](docs/INTEGRATED.md) |
| Akkubetrieb mit 2,16-Zoll-AMOLED | [AMOLED-Branch und Validierung](docs/AMOLED216.md) |
| Kleine Bridge mit Browserstatus | [Schlanke Bridge einrichten](docs/COMPANION.md) |
| Display für einen vorhandenen Hub | [Externer-Hub-Vorschauclient](docs/TOUCHSCREEN.md) |

Beispiel für das integrierte LCD mit ESP-IDF **5.5.4**:

```sh
git clone https://github.com/zpmdd/gbs-anywhere-esp32.git
cd gbs-anywhere-esp32
. "$HOME/esp/esp-idf/export.sh"
cd firmware/integrated
sh tools/check-integrated.sh
sh tools/build-integrated.sh touchscreen
```

Diese Befehle bauen und testen. Sicherung, Erstinstallation und spätere
Anwendungs-OTA sind in den jeweiligen Boardanleitungen beschrieben.

## Sprachen

| Bereich | Verfügbar |
| --- | --- |
| **Projektübersicht** | [English](README.md), [简体中文](README.zh-CN.md), [Deutsch](README.de.md) |
| **Browserstatus der schlanken Bridge** | Englisch, Deutsch, Französisch, Italienisch, Japanisch, Koreanisch, vereinfachtes und traditionelles Chinesisch |
| **Native LCD-/AMOLED-Oberfläche** | Derzeit chinesische Texte; die acht Webübersetzungen gelten nicht für die nativen Bildschirme. |
| **Technische Anleitungen** | Englisch; Übersetzungen und Korrekturen sind willkommen. |

Der Browser übernimmt die bevorzugte Sprache und speichert eine manuelle Wahl.
Übersetzungen sind eingebettet, ein Übersetzungsdienst wird nicht benötigt.

## Validierung und Mitwirkung

[Rezeptübernahme am LCD](docs/RECIPE_FOLLOW_ACCEPTANCE.md) ·
[Waagenstatus und OTA](docs/SCALE_STATE_ACCEPTANCE.md) ·
[AMOLED-Builds und offene Prüfungen](docs/AMOLED216.md#validation-status-2026-10-23) ·
[Fortschritt](docs/PROGRESS.md) · [Mitwirken](CONTRIBUTING.md)

Eine Produktionszertifizierung für Secure Boot / Flash Encryption liegt nicht
vor. Zugangsdaten und NVS-Sicherungen schützen und ein vertrauenswürdiges LAN
verwenden. Langzeitbetrieb, Ausfälle und weitere Modelle müssen noch geprüft
werden; ein erfolgreicher Build ersetzt keinen Praxistest.

Unabhängiger C-/ESP-IDF-Begleiter von
[Skamba/gbs-anywhere](https://github.com/Skamba/gbs-anywhere), mit Dank an Skamba,
erikp152 und die Mitwirkenden. Getrennte Pflege und Versionsnummern gegenüber
der Rust-Desktopanwendung. **AGPL-3.0-or-later**, siehe [LICENSE](LICENSE),
[NOTICE](NOTICE) und [Upstream-Abgleich](docs/UPSTREAM.md).
LVGL, Noto Sans CJK, Inter, Tabler Icons und Boardkomponenten behalten ihre
jeweiligen Lizenzen. Keine Verbindung zu Mahlkönig, Hemro, La Marzocco oder Waveshare.
