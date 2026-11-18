"""Registrazione della trama RS485 del climatizzatore su file (16/11/2026).

Con l'interruttore «Registra trama» (switch.climatizzatore_controller_rs485_registra_trama, firmware smartAircon 2.4+)
il controller manda su aircon/capture, una volta al secondo, tutto quello che passa sul bus:
    <millis del controller> RX|RX+|TX <byte in esadecimale>
(RX = raffica letta tra due silenzi di 5 ms, RX+ = troncata a 256 byte, TX = comando inviato dal controller).
Qui ogni registrazione diventa un file /config/smartaircon/catture/AAAAMMGG-HHMMSS.txt, con l'ora di Home Assistant
davanti a ogni riga e, in mezzo, i comandi mandati da HA (modo, temperatura, ventola, prove sui byte) per capire
cosa ha causato cosa. Il controller si ferma da solo dopo 60 minuti.
"""
import os
from homeassistant.util import dt as dt_util

CARTELLA = "/config/smartaircon/catture"
SENSORE = "sensor.registrazione_trama_climatizzatore"
_reg = {"file": None, "righe": 0, "inizio": None, "ultimo": None}


@pyscript_compile
def _scrivi(percorso, testo):
    os.makedirs(os.path.dirname(percorso), exist_ok=True)
    with open(percorso, "a") as f:
        f.write(testo)


def _aggiorna_sensore():
    attivo = _reg["file"] is not None
    state.set(SENSORE, "in corso" if attivo else "ferma", {
        "friendly_name": "Registrazione trama climatizzatore", "icon": "mdi:record-rec" if attivo else "mdi:file-document-outline",
        "file": _reg["file"] or _reg["ultimo"], "righe": _reg["righe"], "inizio": _reg["inizio"]})


def _ora():
    return dt_util.now().strftime("%H:%M:%S")


@time_trigger("startup")
def trama_avvio():
    _aggiorna_sensore()


@mqtt_trigger("aircon/debug/capture/state")
def trama_stato(payload=None):
    if payload == "ON" and _reg["file"] is None:
        adesso = dt_util.now()
        _reg.update(file=f"{CARTELLA}/{adesso.strftime('%Y%m%d-%H%M%S')}.txt", righe=0, inizio=adesso.strftime("%d/%m/%Y %H:%M:%S"))
        task.executor(_scrivi, _reg["file"], f"# trama climatizzatore, inizio {_reg['inizio']}\n"
                      "# ora_HA millis_controller RX|RX+|TX byte...   ·   righe «HA ...» = comandi mandati da Home Assistant\n")
    elif payload == "OFF" and _reg["file"] is not None:
        task.executor(_scrivi, _reg["file"], f"# fine {dt_util.now().strftime('%d/%m/%Y %H:%M:%S')}, {_reg['righe']} righe\n")
        _reg.update(ultimo=_reg["file"], file=None)
    _aggiorna_sensore()


@mqtt_trigger("aircon/capture")
def trama_dati(payload=None):
    if _reg["file"] is None or not payload:
        return
    ora, testo, n = _ora(), "", 0
    for riga in payload.strip().split("\n"):
        testo += f"{ora} {riga}\n"
        n += 1
    task.executor(_scrivi, _reg["file"], testo)
    _reg["righe"] += n
    _aggiorna_sensore()


@mqtt_trigger("aircon/+/cmd")
@mqtt_trigger("aircon/debug/override")
@mqtt_trigger("aircon/debug/interval")
def trama_comandi(topic=None, payload=None):
    if _reg["file"] is not None:
        task.executor(_scrivi, _reg["file"], f"{_ora()} HA {topic} = {payload}\n")
