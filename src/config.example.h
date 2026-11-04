#pragma once

// Copie ce fichier sous le nom config.h puis renseigne ton réseau.
// config.h est ignoré par Git : aucun secret ne sera publié.
#define WIFI_SSID "mon-wifi"
#define WIFI_PASSWORD "mot-de-passe-wifi"

#define MQTT_HOST "192.168.1.10"
#define MQTT_PORT 1883
#define MQTT_USER ""
#define MQTT_PASSWORD ""

// Chaque écran doit avoir son identifiant propre.
#define DEVICE_ID "ecran-atelier"

// Fuseau Bruxelles/Paris, avec passage automatique heure d'été/hiver.
#define TZ_INFO "CET-1CEST,M3.5.0,M10.5.0/3"
