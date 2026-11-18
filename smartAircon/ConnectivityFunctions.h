// Wi-Fi, OTA, MQTT. Versione 2.5 (17/11/2026).

void OTABegin()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(NET_SSID, NET_PSW);
  while (WiFi.status() != WL_CONNECTED)
    delay(500);
  ArduinoOTA.setHostname(OTA_HOSTNAME);   // senza «/»: l'host mDNS per l'aggiornamento OTA
  ArduinoOTA.onStart([]() { Serial.println("OTA: inizio"); });
  ArduinoOTA.onEnd([]() { Serial.println("\nOTA: fine"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) { Serial.printf("OTA: %u%%\r", (progress / (total / 100))); });
  ArduinoOTA.onError([](ota_error_t error) { Serial.printf("OTA: errore %u\n", error); });
  ArduinoOTA.begin();
}

// Sensori in Home Assistant creati via MQTT discovery (stato dal JSON della risposta della macchina interna).
void PublishDiscovery()
{
  const char *dev = "\"dev\":{\"ids\":[\"smartaircon_ingresso\"],\"name\":\"Climatizzatore (controller RS485)\","
                    "\"mf\":\"DIY\",\"mdl\":\"M5Stack Atom Lite + RS485\",\"sw\":\"" FW_VERSION "\"},"
                    "\"avty_t\":\"" avail_topic "\"";
  struct Sensore { const char *comp, *id, *nome, *tpl, *extra; };
  const Sensore s[] = {
    {"binary_sensor", "compressore", "Compressore", "{{ 'ON' if value_json.compressor else 'OFF' }}", ",\"dev_cla\":\"running\""},
    {"binary_sensor", "accensione_richiesta", "Accensione richiesta", "{{ 'ON' if value_json.requested else 'OFF' }}", ",\"dev_cla\":\"power\""},
    {"binary_sensor", "partenza_caldo", "Partenza a caldo", "{{ 'ON' if value_json.hot_start else 'OFF' }}", ",\"dev_cla\":\"running\",\"ic\":\"mdi:fan-clock\""},
    {"binary_sensor", "acceso", "Acceso (secondo la macchina)", "{{ 'ON' if value_json.power else 'OFF' }}", ",\"dev_cla\":\"power\""},
    {"sensor", "ventola_reale", "Ventola interna reale", "{{ value_json.fan_actual }}", ",\"ic\":\"mdi:fan\",\"stat_cla\":\"measurement\""},
    {"sensor", "modo_macchina", "Modalità (secondo la macchina)", "{{ value_json.mode_name }}", ",\"ic\":\"mdi:tune-variant\""},
    {"sensor", "impostata_macchina", "Temperatura impostata (secondo la macchina)", "{{ value_json.setpoint }}", ",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"stat_cla\":\"measurement\""},
    {"sensor", "t_ambiente", "Temperatura ambiente (ripresa)", "{{ value_json.t_room }}", ",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"stat_cla\":\"measurement\""},
    {"sensor", "t_batteria_interna", "Temperatura batteria interna", "{{ value_json.t_coil_in }}", ",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"stat_cla\":\"measurement\""},
    {"sensor", "t_mandata_compressore", "Temperatura mandata compressore", "{{ value_json.t_discharge }}", ",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"stat_cla\":\"measurement\""},
    {"sensor", "t_batteria_esterna", "Temperatura batteria esterna", "{{ value_json.t_coil_out }}", ",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"stat_cla\":\"measurement\""},
    {"sensor", "t_esterna_unita", "Temperatura esterna (unità esterna)", "{{ value_json.t_outdoor }}", ",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"stat_cla\":\"measurement\""},
    {"sensor", "byte_18", "Byte 18 (da capire)", "{{ value_json.b18 }}", ",\"ic\":\"mdi:help-box-outline\",\"ent_cat\":\"diagnostic\""},
    {"sensor", "risposta_raw", "Risposta grezza", "{{ value_json.raw[:255] }}", ",\"ic\":\"mdi:code-brackets\",\"ent_cat\":\"diagnostic\""},
  };
  char topic[128], msg[900];
  client.publish("homeassistant/binary_sensor/smartaircon_unita_esterna/config", "", true);   // decodifica sbagliata (≤ 2.4): toglie l'entità
  for (const Sensore &x : s)
  {
    snprintf(topic, sizeof(topic), "homeassistant/%s/smartaircon_%s/config", x.comp, x.id);
    snprintf(msg, sizeof(msg), "{\"name\":\"%s\",\"uniq_id\":\"smartaircon_%s\",\"stat_t\":\"" reply_json "\",\"val_tpl\":\"%s\"%s,%s}",
             x.nome, x.id, x.tpl, x.extra, dev);
    client.publish(topic, msg, true);
  }
  // diagnostica dalle statistiche
  const Sensore d[] = {
    {"sensor", "pacchetti_ok", "Risposte ricevute", "{{ value_json.frames_ok }}", ",\"stat_cla\":\"total_increasing\",\"ent_cat\":\"diagnostic\""},
    {"sensor", "crc_errati", "Risposte con CRC errato", "{{ value_json.crc_errors }}", ",\"stat_cla\":\"total_increasing\",\"ent_cat\":\"diagnostic\""},
    {"sensor", "wifi", "Segnale Wi-Fi", "{{ value_json.rssi }}", ",\"unit_of_meas\":\"dBm\",\"dev_cla\":\"signal_strength\",\"ent_cat\":\"diagnostic\""},
    {"sensor", "uptime", "Acceso da (s)", "{{ value_json.uptime_s }}", ",\"unit_of_meas\":\"s\",\"dev_cla\":\"duration\",\"ent_cat\":\"diagnostic\""},
  };
  for (const Sensore &x : d)
  {
    snprintf(topic, sizeof(topic), "homeassistant/%s/smartaircon_%s/config", x.comp, x.id);
    snprintf(msg, sizeof(msg), "{\"name\":\"%s\",\"uniq_id\":\"smartaircon_%s\",\"stat_t\":\"" stats_topic "\",\"val_tpl\":\"%s\"%s,%s}",
             x.nome, x.id, x.tpl, x.extra, dev);
    client.publish(topic, msg, true);
  }
  // registro (una riga per evento) e campo per le prove sui byte sconosciuti
  snprintf(msg, sizeof(msg), "{\"name\":\"Ultima riga di log\",\"uniq_id\":\"smartaircon_log\",\"stat_t\":\"" log_topic "\","
           "\"val_tpl\":\"{{ value[:250] }}\",\"ic\":\"mdi:text-box-search-outline\",\"ent_cat\":\"diagnostic\",%s}", dev);
  client.publish("homeassistant/sensor/smartaircon_log/config", msg, true);
  snprintf(msg, sizeof(msg), "{\"name\":\"Prova byte (POS=HEX o clear)\",\"uniq_id\":\"smartaircon_override\",\"cmd_t\":\"" override_cmd "\","
           "\"stat_t\":\"" override_state "\",\"max\":40,\"ic\":\"mdi:test-tube\",\"ent_cat\":\"config\",%s}", dev);
  client.publish("homeassistant/text/smartaircon_override/config", msg, true);
  snprintf(msg, sizeof(msg), "{\"name\":\"Intervallo invio comandi\",\"uniq_id\":\"smartaircon_intervallo\",\"cmd_t\":\"" interval_cmd "\","
           "\"stat_t\":\"" interval_state "\",\"min\":0,\"max\":5000,\"step\":50,\"unit_of_meas\":\"ms\",\"ret\":true,"
           "\"mode\":\"box\",\"ic\":\"mdi:timer-sync-outline\",\"ent_cat\":\"config\",%s}", dev);
  client.publish("homeassistant/number/smartaircon_intervallo/config", msg, true);
  snprintf(msg, sizeof(msg), "{\"name\":\"Comandi inviati\",\"uniq_id\":\"smartaircon_inviati\",\"stat_t\":\"" stats_topic "\","
           "\"val_tpl\":\"{{ value_json.tx }}\",\"stat_cla\":\"total_increasing\",\"ent_cat\":\"diagnostic\",%s}", dev);
  client.publish("homeassistant/sensor/smartaircon_inviati/config", msg, true);
  snprintf(msg, sizeof(msg), "{\"name\":\"Registra trama\",\"uniq_id\":\"smartaircon_registra\",\"cmd_t\":\"" capture_cmd "\","
           "\"stat_t\":\"" capture_state "\",\"ic\":\"mdi:record-rec\",\"ent_cat\":\"config\",%s}", dev);
  client.publish("homeassistant/switch/smartaircon_registra/config", msg, true);
}

boolean reconnect()
{
  String clientId = NAME;
  if (client.connect(clientId.c_str(), AIO_USERNAME, AIO_KEY, avail_topic, 0, true, "offline"))
  {
    client.subscribe(velocity_cmd);
    client.subscribe(setpoint_cmd);
    client.subscribe(mode_cmd);
    client.subscribe(override_cmd);
    client.subscribe(interval_cmd);
    client.subscribe(capture_cmd);
    client.publish(avail_topic, "online", true);
    PublishDiscovery();
    PublishOverrideState();                                               // così in HA i campi non restano «sconosciuto»
    client.publish(interval_state, String(sendInterval).c_str(), true);
    client.publish(capture_state, capturing ? "ON" : "OFF", true);
    MqttLog("connesso, firmware " FW_VERSION);
    return 1;
  }
  return 0;
}

void callback(char *topic, byte *payload, unsigned int length)
{
  char testo[64];
  unsigned int n = length < sizeof(testo) - 1 ? length : sizeof(testo) - 1;
  memcpy(testo, payload, n);   // copia terminata da '\0' (prima si scriveva oltre la fine del payload)
  testo[n] = '\0';

  if (DEBUGLEVEL >= 1)
  {
    Serial.printf("MQTT %s = %s\n", topic, testo);
  }

  if (!strcmp(capture_cmd, topic))
  {
    SetCapture(!strcmp(testo, "ON"), "");
    return;
  }
  if (!strcmp(interval_cmd, topic))
  {
    long v = atol(testo);
    sendInterval = (v < 0) ? 0 : (v > 5000 ? 5000 : v);
    client.publish(interval_state, String(sendInterval).c_str(), true);
    client.publish(capture_state, capturing ? "ON" : "OFF", true);
    char riga[64];
    snprintf(riga, sizeof(riga), "intervallo invio: %lu ms", sendInterval);
    MqttLog(riga);
    return;
  }
  if (!strcmp(override_cmd, topic))
  {
    HandleOverride(testo);
    return;
  }
  if (!strcmp(velocity_cmd, topic))
    velocity = (uint8_t)atoi(testo);
  if (!strcmp(setpoint_cmd, topic))
    setpoint = (uint16_t)atoi(testo);
  if (!strcmp(mode_cmd, topic))
    mode = (uint8_t)atoi(testo);

  client.publish(velocity_state, String(velocity).c_str());
  client.publish(velocity_available, "1");
  client.publish(setpoint_state, String(setpoint).c_str());
  client.publish(setpoint_available, "1");
  client.publish(mode_state, String(mode).c_str());
  client.publish(mode_available, "1");
}
