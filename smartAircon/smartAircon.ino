/*------------------------------------------------------
 * smartAircon — controller del climatizzatore canalizzato via RS485 (M5Stack Atom Lite + RS485)
 *
 * Versione 2 (16/11/2026), sulla base del firmware originale (v1):
 *  - comandi invariati (aircon/velocity|setpoint|mode/cmd), stesso pacchetto clonato verso la macchina interna;
 *  - in più legge le risposte della macchina interna (7A 7A A1 21 …), controlla il CRC e le pubblica:
 *      aircon/reply/raw        risposta in esadecimale (anche su aircon/reply/state, come testo)
 *      aircon/reply/json       risposta decodificata (vedi AirconFunctions.h)
 *      aircon/currentTemp/state temperatura ambiente, il topic che usa il climate di Home Assistant
 *      aircon/tx/raw           ultimo pacchetto inviato, in esadecimale
 *      aircon/log              una riga per ogni byte della risposta che cambia (per il reverse engineering)
 *      aircon/stats            contatori (pacchetti buoni, CRC errati, altri pacchetti), RSSI, uptime, memoria
 *  - prova dei byte sconosciuti del pacchetto inviato: aircon/debug/override con "15=48" (posizione=valore esadecimale),
 *    "clear" per annullare; solo posizioni 13–28, scade da sola dopo 15 minuti; stato su aircon/debug/state;
 *  - sensori creati da soli in Home Assistant (MQTT discovery).
 --------------------------------------------------------*/
//Debug sulla seriale USB
#define DEBUGLEVEL        1

#include "secrets.h"   // NET_SSID, NET_PSW, AIO_USERNAME, AIO_KEY

//HomeAssistant MQTT
#ifndef AIO_SERVER
#define AIO_SERVER        "homeassistant.local"   // broker MQTT: meglio definirlo in secrets.h
#endif
#define AIO_SERVERPORT    1883
#define MQTTPOST_INT      60L * 1000L
#define NAME              "smartAircon/ingresso"
#define OTA_HOSTNAME      "smartaircon-ingresso"
#define FW_VERSION        "2.5 (17/11/2026)"

#define velocity_cmd        "aircon/velocity/cmd"
#define velocity_state      "aircon/velocity/state"
#define velocity_available  "aircon/velocity/tele"

#define setpoint_cmd        "aircon/setpoint/cmd"
#define setpoint_state      "aircon/setpoint/state"
#define setpoint_available  "aircon/setpoint/tele"

#define mode_cmd          "aircon/mode/cmd"
#define mode_state        "aircon/mode/state"
#define mode_available    "aircon/mode/tele"

#define currentTemp_state "aircon/currentTemp/state"
#define reply_state       "aircon/reply/state"
#define reply_raw         "aircon/reply/raw"
#define reply_json        "aircon/reply/json"
#define tx_raw            "aircon/tx/raw"
#define log_topic         "aircon/log"
#define stats_topic       "aircon/stats"
#define override_cmd      "aircon/debug/override"
#define override_state    "aircon/debug/state"
#define avail_topic       "aircon/status"
#define interval_cmd      "aircon/debug/interval"
#define interval_state    "aircon/debug/interval/state"
#define capture_cmd       "aircon/debug/capture"
#define capture_state     "aircon/debug/capture/state"
#define capture_topic     "aircon/capture"

/*------------------------------------------------------
 * LIBRARIES
 --------------------------------------------------------*/
#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <M5Atom.h>

/*------------------------------------------------------
 * VARIABLES
 --------------------------------------------------------*/
boolean mqtt_status = false;
boolean wifi_status = false;
long int lastUpdateTime = 0;
bool firstLoop = true;

uint8_t  velocity = 0;
uint8_t  setpoint = 24;
uint8_t  mode = 1;

WiFiClient Client;
PubSubClient client(Client);

#include "AirconFunctions.h"
#include "ConnectivityFunctions.h"

void setup()
{
  Serial.begin(115200);
  Serial.print("Connecting to ");
  Serial.println(NET_SSID);

  OTABegin();
  wifi_status = true;

  client.setServer(AIO_SERVER, AIO_SERVERPORT);
  client.setCallback(callback);
  client.setBufferSize(4096);   // JSON, discovery e blocchi della registrazione (fino a 3,6 kB) superano i 256 byte di default

  M5.begin(true, false, true);
  Serial2.begin(9600, SERIAL_8N1, 22, 19);
}

void loop()
{
  ArduinoOTA.handle();
  client.loop();

  if (WiFi.status() == WL_CONNECTED)
  {
    if (!client.connected())
      mqtt_status = reconnect();
    else
      mqtt_status = true;
    wifi_status = true;
  }
  else
  {
    wifi_status = false;
    mqtt_status = false;
  }

  //Protocol polling: legge il bus, decodifica le risposte, poi invia il comando
  SetValues(velocity, setpoint, mode);
  PublishReplies();
  CaptureLoop();

  if ((millis() - lastUpdateTime >= MQTTPOST_INT) || firstLoop)
  {
    firstLoop = false;
    client.publish(velocity_state, String(velocity).c_str());
    client.publish(velocity_available, "1");
    client.publish(setpoint_state, String(setpoint).c_str());
    client.publish(setpoint_available, "1");
    client.publish(mode_state, String(mode).c_str());
    client.publish(mode_available, "1");
    PublishStats();
    lastUpdateTime = millis();
  }
}
