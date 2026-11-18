// Protocollo RS485 con la macchina interna. Versione 2.5 (17/11/2026).
//
// Pacchetto inviato (31 byte), clone di quello del controller originale:
//   7A 7A 21 A1 1F 00 00 A3 15 00 80 [CMD] [TEMP] 00 80 3E 0A 06 00…00 [CRC alto] [CRC basso]
//   CMD: nibble alto = ventola (0 auto, 1–5, D turbo); nibble basso: bit3 = acceso, bit0–2 = modo (1 freddo, 2 deumidifica,
//        3 ventilazione, 4 caldo) → 1 = spento, 9 freddo, A deumidifica, B ventilazione, C caldo
//   TEMP: temperatura impostata (da spento: impostata − 16)
//   Byte 13–17 (00 80 3E 0A 06): sconosciuti. Ipotesi: 0x3E = 62 − 40 = 22 °C misurati dal controller («follow me»).
//
// Risposta della macchina interna (37 byte): 7A 7A A1 21 25 00 00 A1 13 09 33 [11] [12] [13] [14] [15] 00… [31–34] [CRC]
//   Decodifica dedotta dalle catture del bus (temperature = valore − 40):
//   11: bit0–2 modo, bit4 accensione richiesta (si accende al comando e si spegne per primo), bit6 acceso,
//       bit7 compressore in funzione. All'accensione in caldo: 03 → 13 → 14 → 54 → D4; allo spegnimento D4 → C4 → 04.
//   12: velocità reale ventola interna (0 ferma, 2 bassa … 6 alta, 7 turbo). Il «bit4 unità esterna attiva» delle
//       versioni ≤ 2.4 era sbagliato: in 10 minuti di caldo col compressore acceso non si è mai visto (17/11/2026).
//   13: temperatura impostata · 14: temperatura ambiente alla ripresa · 15: batteria interna
//   17: 04 = partenza a caldo (anti aria fredda): per 3 minuti dall'avvio del compressore la ventola resta ferma
//       (2 min) e poi al minimo, ignorando la velocità chiesta
//   18: 10 dopo ~4 minuti di compressore in caldo, 00 allo spegnimento: da capire
//   31: mandata compressore · 32 e 34: batteria esterna (sempre uguali) · 33: temperatura esterna
//   (tutte confermate dal giro in caldo del 17/11/2026: mandata 9 → 80 °C, batteria esterna 9 → 1 °C, esterna 8–10 °C)
// Il JSON pubblicato riporta anche la risposta grezza, così la decodifica si può correggere senza riflashare.

#define PAYLOAD_LEN   31
#define CMD_POS       11
#define TEMP_POS      12
#define START_CRC     29
#define REPLY_LEN     37
#define OVR_FIRST     13          // posizioni del pacchetto inviato che si possono provare a cambiare
#define OVR_LAST      28
#define OVR_TIMEOUT   (15UL * 60UL * 1000UL)

uint16_t modbusCRC16(const uint8_t *buf, uint32_t len);
void MqttLog(const char *riga);

// ---- stato della ricezione
uint8_t  lastReply[REPLY_LEN];
bool     haveReply = false;
bool     replyChanged = false;
unsigned long lastReplyPublish = 0;
uint32_t framesOk = 0, crcErrors = 0, otherFrames = 0, garbage = 0;
uint8_t  lastTx[PAYLOAD_LEN];
unsigned long sendInterval = 1000;   // ms tra un invio e l'altro (0 = come la versione 1: bus sempre occupato)
uint32_t txCount = 0;

// ---- registrazione della trama (interruttore «Registra trama» in HA): ogni raffica letta dal bus e ogni comando inviato,
//      con i millisecondi, raccolti e mandati su aircon/capture una volta al secondo; si ferma da sola dopo 60 minuti
#define CAP_MAX       (60UL * 60UL * 1000UL)
bool     capturing = false;
unsigned long captureSince = 0, capLastFlush = 0;
char     capBuf[3600];
int      capLen = 0;
uint32_t capLines = 0;

void CaptureFlush()
{
  if (capLen && client.connected())
    client.publish(capture_topic, capBuf);
  capLen = 0;
  capBuf[0] = '\0';
  capLastFlush = millis();
}

void CaptureAdd(const char *tag, const uint8_t *b, int n)
{
  if (!capturing)
    return;
  if (capLen + 24 + 3 * n > (int)sizeof(capBuf) - 2)
    CaptureFlush();
  capLen += snprintf(capBuf + capLen, sizeof(capBuf) - capLen, "%lu %s ", millis(), tag);
  for (int i = 0; i < n && capLen < (int)sizeof(capBuf) - 4; i++)
    capLen += snprintf(capBuf + capLen, sizeof(capBuf) - capLen, "%02X%s", b[i], i + 1 < n ? " " : "");
  capLen += snprintf(capBuf + capLen, sizeof(capBuf) - capLen, "\n");
  capLines++;
}

void SetCapture(bool on, const char *motivo)
{
  if (on && !capturing) { capLen = 0; capLines = 0; captureSince = capLastFlush = millis(); }
  if (!on && capturing) CaptureFlush();
  capturing = on;
  client.publish(capture_state, on ? "ON" : "OFF", true);
  char riga[96];
  snprintf(riga, sizeof(riga), "registrazione trama %s%s", on ? "avviata" : "fermata", motivo);
  MqttLog(riga);
}

void CaptureLoop()
{
  if (!capturing)
    return;
  if (millis() - captureSince > CAP_MAX)
    SetCapture(false, " dopo 60 minuti");
  else if (millis() - capLastFlush >= 1000UL)
    CaptureFlush();
}

// ---- prove sui byte sconosciuti del pacchetto inviato
int16_t  overrideVal[PAYLOAD_LEN];      // -1 = nessuna prova
unsigned long overrideSince = 0;
bool     overrideInit = false;

static void hexString(const uint8_t *b, int n, char *out)
{
  for (int i = 0; i < n; i++)
    sprintf(out + 3 * i, "%02X ", b[i]);
  out[n ? 3 * n - 1 : 0] = '\0';
}

void MqttLog(const char *riga)
{
  if (client.connected())
    client.publish(log_topic, riga);
  if (DEBUGLEVEL >= 1)
    Serial.println(riga);
}

void PublishOverrideState()
{
  if (!overrideInit) { for (int i = 0; i < PAYLOAD_LEN; i++) overrideVal[i] = -1; overrideInit = true; }
  char msg[200] = "nessuna prova";
  int k = 0;
  for (int i = 0; i < PAYLOAD_LEN; i++)
    if (overrideVal[i] >= 0)
      k += snprintf(msg + k, sizeof(msg) - k, "%s%d=%02X", k ? " " : "", i, overrideVal[i]);
  client.publish(override_state, msg, true);
}

// "15=48" (posizione=valore esadecimale) oppure "clear"
void HandleOverride(const char *testo)
{
  if (!overrideInit) { for (int i = 0; i < PAYLOAD_LEN; i++) overrideVal[i] = -1; overrideInit = true; }
  char riga[96];
  if (!strncmp(testo, "clear", 5))
  {
    for (int i = 0; i < PAYLOAD_LEN; i++) overrideVal[i] = -1;
    MqttLog("prova annullata: pacchetto originale");
  }
  else
  {
    int pos = -1; unsigned int val = 0;
    if (sscanf(testo, "%d=%x", &pos, &val) == 2 && pos >= OVR_FIRST && pos <= OVR_LAST && val <= 0xFF)
    {
      overrideVal[pos] = val;
      overrideSince = millis();
      snprintf(riga, sizeof(riga), "prova: byte %d = %02X (scade tra 15 minuti)", pos, val);
      MqttLog(riga);
    }
    else
    {
      snprintf(riga, sizeof(riga), "prova rifiutata: \"%s\" (usa POS=HEX con POS %d-%d, oppure clear)", testo, OVR_FIRST, OVR_LAST);
      MqttLog(riga);
    }
  }
  PublishOverrideState();
}

// byte della risposta che sono temperature (cambiano spesso di 1 grado)
static bool isTempByte(int k) { return k == 14 || k == 15 || (k >= 31 && k <= 34); }
bool tempChanged = false;

// Cerca nel buffer le risposte della macchina interna; registra i byte che cambiano.
void ParseBus(const uint8_t *buf, uint8_t n)
{
  for (int i = 0; i + 5 <= n; i++)
  {
    if (buf[i] != 0x7A || buf[i + 1] != 0x7A)
      continue;
    uint8_t len = buf[i + 4];
    if (len < 7 || i + len > n)
      { garbage++; continue; }
    uint16_t crc = modbusCRC16(buf + i, len - 2);
    if (buf[i + len - 2] != (crc >> 8) || buf[i + len - 1] != (crc & 0xFF))
      { crcErrors++; i += 1; continue; }
    if (buf[i + 2] == 0xA1 && buf[i + 3] == 0x21 && len == REPLY_LEN)
    {
      framesOk++;
      if (haveReply)
      {
        for (int k = 0; k < REPLY_LEN - 2; k++)
          if (buf[i + k] != lastReply[k])
          {
            if (isTempByte(k))
            {
              tempChanged = true;          // le temperature oscillano di ±1: niente log, pubblicate ogni 30 s
              continue;
            }
            char riga[64];
            snprintf(riga, sizeof(riga), "rx byte %d: %02X -> %02X", k, lastReply[k], buf[i + k]);
            MqttLog(riga);
            replyChanged = true;
          }
      }
      else
        replyChanged = true;
      memcpy(lastReply, buf + i, REPLY_LEN);
      haveReply = true;
    }
    else
      otherFrames++;      // per esempio pacchetti 21 A1 di un altro controller sul bus
    i += len - 1;
  }
}

static const char *modeName(uint8_t m)
{
  switch (m & 0x07) { case 1: return "freddo"; case 2: return "deumidifica"; case 3: return "ventilazione"; case 4: return "caldo"; default: return "?"; }
}

// Pubblica la risposta (grezza + decodificata): subito se cambia lo stato (acceso, compressore, modo, ventola…),
// al massimo ogni 30 s se cambiano solo le temperature, e comunque ogni 60 s.
void PublishReplies()
{
  if (!haveReply || !client.connected())
    return;
  unsigned long da = millis() - lastReplyPublish;
  if (!replyChanged && !(tempChanged && da >= 30000UL) && da < 60000UL)
    return;
  replyChanged = tempChanged = false;
  lastReplyPublish = millis();

  char raw[3 * REPLY_LEN + 1];
  hexString(lastReply, REPLY_LEN, raw);
  client.publish(reply_raw, raw);
  client.publish(reply_state, raw);

  const uint8_t *r = lastReply;
  char json[700];
  snprintf(json, sizeof(json),
    "{\"power\":%d,\"compressor\":%d,\"requested\":%d,\"mode\":%d,\"mode_name\":\"%s\","
    "\"fan_actual\":%d,\"hot_start\":%d,\"setpoint\":%d,\"t_room\":%d,\"t_coil_in\":%d,"
    "\"t_discharge\":%d,\"t_coil_out\":%d,\"t_outdoor\":%d,\"t_coil_out2\":%d,"
    "\"b11\":\"%02X\",\"b12\":\"%02X\",\"b17\":\"%02X\",\"b18\":\"%02X\",\"raw\":\"%s\"}",
    (r[11] >> 6) & 1, (r[11] >> 7) & 1, (r[11] >> 4) & 1, r[11] & 0x07, modeName(r[11]),
    r[12] & 0x0F, (r[17] >> 2) & 1, (int)r[13] - 40, (int)r[14] - 40, (int)r[15] - 40,
    (int)r[31] - 40, (int)r[32] - 40, (int)r[33] - 40, (int)r[34] - 40,
    r[11], r[12], r[17], r[18], raw);
  client.publish(reply_json, json);
  client.publish(currentTemp_state, String((int)r[14] - 40).c_str());
}

void PublishStats()
{
  if (!client.connected())
    return;
  char json[256];
  snprintf(json, sizeof(json),
    "{\"frames_ok\":%u,\"crc_errors\":%u,\"other_frames\":%u,\"garbage\":%u,\"tx\":%u,\"interval_ms\":%lu,\"rssi\":%d,\"uptime_s\":%lu,\"heap\":%u,\"fw\":\"%s\"}",
    framesOk, crcErrors, otherFrames, garbage, txCount, sendInterval, WiFi.RSSI(), millis() / 1000UL, ESP.getFreeHeap(), FW_VERSION);
  client.publish(stats_topic, json);
  char tx[3 * PAYLOAD_LEN + 1];
  hexString(lastTx, PAYLOAD_LEN, tx);
  client.publish(tx_raw, tx);
}

// Legge il bus senza bloccare; dopo almeno 5 ms di silenzio decodifica quello che ha letto e, se è passato
// l'intervallo di invio, manda il comando. Intervallo 0 = come la versione 1 (invio dopo ogni pausa di 5 ms):
// però così il bus è quasi sempre occupato e la macchina interna risponde di rado.
// L'intervallo si cambia da Home Assistant (aircon/debug/interval, ms, messaggio «retained»).

void SetValues(uint8_t velocity, uint8_t setpoint, uint8_t mode)
{
  const uint8_t cmd[PAYLOAD_LEN] = {0x7a, 0x7a, 0x21, 0xa1, 0x1f, 0x00, 0x00, 0xa3, 0x15, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x3e,
                                    0x0a, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  if (!overrideInit) { for (int i = 0; i < PAYLOAD_LEN; i++) overrideVal[i] = -1; overrideInit = true; }

  // le prove scadono da sole
  bool attiva = false;
  for (int i = 0; i < PAYLOAD_LEN; i++) attiva |= overrideVal[i] >= 0;
  if (attiva && millis() - overrideSince > OVR_TIMEOUT)
  {
    for (int i = 0; i < PAYLOAD_LEN; i++) overrideVal[i] = -1;
    MqttLog("prova scaduta: pacchetto originale");
    PublishOverrideState();
  }

  static uint8_t buffer[256];
  static uint8_t index = 0;
  static unsigned long lastByte = 0, lastSend = 0;
  static int led = -1;

  while (Serial2.available())
  {
    uint8_t c = Serial2.read();
    if (index < sizeof(buffer))
      buffer[index++] = c;
    lastByte = millis();
  }
  if (millis() - lastByte <= 5)
    return;                       // il bus è occupato
  if (index)
  {
    CaptureAdd(index == sizeof(buffer) ? "RX+" : "RX", buffer, index);   // RX+ = raffica più lunga del buffer, troncata
    ParseBus(buffer, index);
    index = 0;
  }
  if (millis() - lastSend < sendInterval)
    return;

  uint8_t payload[PAYLOAD_LEN];
  for (int i = 0; i < PAYLOAD_LEN; i++)
    payload[i] = (overrideVal[i] >= 0) ? (uint8_t)overrideVal[i] : cmd[i];
  payload[CMD_POS] = (mode == 1) ? (mode & 0x0F) : (((velocity & 0x0F) << 4) | (mode & 0x0F));
  payload[TEMP_POS] = (mode == 1) ? setpoint - 16 : setpoint;
  uint16_t crc = modbusCRC16(payload, START_CRC);
  payload[START_CRC] = (uint8_t)(crc >> 8);
  payload[START_CRC + 1] = (uint8_t)(crc & 0xFF);
  Serial2.write(payload, PAYLOAD_LEN);
  Serial2.flush();
  CaptureAdd("TX", payload, PAYLOAD_LEN);                // aspetta la fine della trasmissione (~32 ms), poi conta il silenzio da lì
  lastSend = lastByte = millis();
  txCount++;
  memcpy(lastTx, payload, PAYLOAD_LEN);

  int colore = haveReply ? 0x00ff00 : 0xff00ff;   // verde: la macchina risponde
  if (colore != led)
  {
    M5.dis.drawpix(0, colore);
    led = colore;
  }
  M5.update();
}

uint16_t modbusCRC16(const uint8_t *buf, uint32_t len)
{
  uint16_t crc = 0xFFFF;
  for (uint32_t i = 0; i < len; i++)
  {
    crc ^= buf[i];
    for (uint8_t bit = 0; bit < 8; bit++)
      crc = (crc & 0x0001) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}
