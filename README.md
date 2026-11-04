# Ecran d'etapes — Waveshare ESP32-S3-RLCD-4.2

Firmware C++/Arduino pour l'ecran Waveshare ESP32-S3-RLCD-4.2 :

- heure locale en haut a gauche (NTP, Europe/Brussels par defaut) ;
- barre de minuteur a sa droite : le noir avance avec le temps ecoule ;
- texte du minuteur inverse progressivement de couleur quand le noir passe dessous ;
- trois etapes configurables et cochables a distance ;
- pilotage par MQTT en Wi-Fi.

## Premiere installation

1. Installer [PlatformIO](https://platformio.org/install) dans VS Code.
2. Copier `src/config.example.h` vers `src/config.h` et remplir Wi-Fi/MQTT.
3. Ouvrir le dossier `firmware` dans VS Code puis executer **PlatformIO: Upload**.
4. Ouvrir le moniteur serie a 115200 bauds. Le firmware y indique le sujet MQTT ecoute.

Le premier demarrage demande une connexion Wi-Fi pour regler l'heure par NTP. L'ecran affiche `--:--` tant que l'heure n'est pas disponible.

## MQTT

Avec `DEVICE_ID` valant `ecran-atelier` :

| Role | Sujet |
| --- | --- |
| Commandes | `workshop/ecran-atelier/command` |
| Etat publie (retain) | `workshop/ecran-atelier/state` |

Exemples de charges JSON :

```json
{"timer":"start","seconds":900}
```

```json
{"timer":"pause"}
```

```json
{"timer":"resume"}
```

```json
{"timer":"reset","seconds":300}
```

```json
{"steps":["Mettre les lunettes","Brancher le cable","Appuyer sur START"]}
```

```json
{"step":{"index":0,"done":true}}
```

Les indices d'etapes vont de `0` a `2`. Publier les commandes en `retain` est utile pour que l'ecran retrouve les trois etapes apres un redemarrage.
