"use strict";

/*
 * The clips of a voice pack: names (folder/name as the firmware asks for them) and the text
 * each one speaks, per language. Ported from the generator notebook that made the German pack;
 * the other languages translate its texts. Numbers stay digits in the texts: the voice reads them
 * in its own language.
 *
 * VOICEPACKS lists the manufacturer's packs (the file is voices/<id>.ubpack, the id is the value
 * of system_language_string) with the language of their texts; a pack whose language has no
 * texts here can be used, but not generated.
 */

/* v with the decimal mark of the language ("1,5" or "1.5") */
const decimal = (v, mark) => v.toFixed(v % 1 ? 1 : 0).replace(".", mark);

const VOICEPACK_LANGUAGES = {
  de: {
    label: "Deutsch",
    hour: (h) => `${h} Uhr`,
    hours: (h) => (h === 1 ? "1 Stunde" : `${h} Stunden`),
    minutes: (m) => (m === 1 ? "1 Minute" : `${m} Minuten`),
    metres: (m) => `${m} Meter`,
    kmh: (v) => `${v} Kilometer pro Stunde`,
    number: (n) => `${n}`,
    percent: (p) => `${p} Prozent`,
    ms: (v) => `${decimal(v, ",")} Meter pro Sekunde`,
    sink: (v) => `minus ${decimal(v, ",")} Meter pro Sekunde`,
    cms: (v) => `${v} Zentimeter pro Sekunde`,
    g: (v) => `${decimal(v, ",")} G`,
    deg: (d) => `${d} Grad`,
    words: {
      "time/duration": "Flugzeit:", "time/time": "Uhrzeit:",
      "nav/wind_speed": "Windgeschwindigkeit", "nav/wind": "Windrichtung", "nav/heading": "Richtung",
      "nav/e": "Ost", "nav/n": "Nord", "nav/ne": "Nord Ost", "nav/nw": "Nord West", "nav/s": "Süd", "nav/se": "Süd Ost",
      "nav/sw": "Süd West", "nav/w": "West",
      altitude_max: "Maximale Höhe:", auto_shutdown: "Automatische Abschaltung in:", bat_charging: "Batterie lädt",
      bat_empty: "Batterie: leer", bat_low: "Batterie: schwach", bat_med: "Batterie: mittel", bat_good: "Batterie: voll",
      ready: "Bereit", wait_for_takeoff: "Warte auf Start", press_button_to_confirm: "Taste zum Bestätigen drücken",
      bluetooth_connected: "Bluetooth verbunden", bluetooth_disconnected: "Bluetooth getrennt", bluetooth_off: "Bluetooth deaktiviert",
      bluetooth_on: "Bluetooth aktiviert", contact: "Bitte kontaktieren Sie das Support Team", dot: "Punkt", error: "Fehler",
      firmware: "Firmware", gps_disabled: "GPS deaktiviert", gps_ok: "GPS ok", integration_settings: "Integrationsstufe",
      plus: "Plus", minus: "Minus", takeoff: "Abflug", language: "Sprache: Deutsch", language_select: "Wähle deine Sprache",
      language_selected: "Deutsch ausgewählt", lift_threshold_settings: "Grenzwert für Steigton", loading_config: "Lädt Konfiguration",
      no_gps: "Suche GPS", profile_1: "Profil 1", profile_2: "Profil 2", profile_3: "Profil 3", profile_acro: "Acro",
      profile_alpinism: "Singleskin", profile_bivouac: "Biwak", profile_competition: "Comp", profile_cross: "Cross Country",
      profile_hike: "Hike and Fly", profile_paragliding: "Gleitschirm", profile_paramotor: "Paramotor",
      profile_select: "Profil auswählen", profile_selected: "ausgewählt", profile_speedriding: "Speedriding",
      profile_tandem: "Tandem", recording: "Aufzeichnung",
      reset_hold: "Halte die Taste zum Zurücksetzen auf Werkseinstellungen gedrückt",
      reset_wait: "Auslieferungszustand wird geladen, bitte warten", sink_alarm_settings: "Grenzwert für Sinkton",
      speed_max: "Maximale Geschwindigkeit", takeoff_off: "Abflugerkennung deaktiviert", takeoff_on: "Abflugerkennung aktiviert",
      tap_disabled: "Doppeltipp deaktiviert", timezone: "Zeitzone", update_fail: "Fehler: Update fehlgeschlagen",
      update_ok: "Update erfolgreich", updating: "aktualisiere", vario_disabled: "Vario deaktiviert", version: "Version:",
      voice_auto_off: "Automatische Ansage deaktiviert", voice_auto_on: "Automatische Ansage aktiviert", voice_pack: "Sprachpaket",
      voice_update: "Bitte Sprachpaket aktualisieren", voice_updated: "Sprachpaket aktualisiert", vol_0: "Vario stumm",
      vol_1: "Lautstärke: leise", vol_2: "Lautstärke: mittel", vol_3: "Lautstärke: laut", vol_4: "Lautstärke: sehr laut",
      vz_max: "Maximales Steigen", wait: "bitte warten", warning: "Achtung", weak_lift_off: "Nullschieberton deaktiviert",
      weak_lift_on: "Nullschieberton aktiviert", battery: "Batterie", bluetooth: "Bluetooth",
      bluetooth_pair: "Bluetooth Kopplung aktiviert", bluetooth_pin: "Bluetooth Pincode lautet",
      bluetooth_remote_event: "Bluetooth Fernbedienungsmodus", gps: "GPS", landing: "Landung", memory: "Speicher",
      sensors_imu: "Beschleunigungssensor", sensors_pressure: "Barometer", solar_cell: "Solarzelle",
      vario_sink_off: "Sinkton deaktiviert", vz_min: "Maximales Sinken",
      /* extension clips of UltraBeep */
      glide_ratio: "Gleitzahl", g_load: "Beschleunigung", bank_angle: "Querneigung", thermal_exit: "Thermikausstieg",
    },
  },
  en: {
    label: "English",
    hour: (h) => `${h} o'clock`,
    hours: (h) => (h === 1 ? "1 hour" : `${h} hours`),
    minutes: (m) => (m === 1 ? "1 minute" : `${m} minutes`),
    metres: (m) => `${m} metres`,
    kmh: (v) => `${v} kilometres per hour`,
    number: (n) => `${n}`,
    percent: (p) => `${p} percent`,
    ms: (v) => `${decimal(v, ".")} metres per second`,
    sink: (v) => `minus ${decimal(v, ".")} metres per second`,
    cms: (v) => `${v} centimetres per second`,
    g: (v) => `${decimal(v, ".")} G`,
    deg: (d) => `${d} degrees`,
    words: {
      "time/duration": "Flight time:", "time/time": "Time:",
      "nav/wind_speed": "Wind speed", "nav/wind": "Wind direction", "nav/heading": "Heading",
      "nav/e": "East", "nav/n": "North", "nav/ne": "North east", "nav/nw": "North west", "nav/s": "South", "nav/se": "South east",
      "nav/sw": "South west", "nav/w": "West",
      altitude_max: "Maximum altitude:", auto_shutdown: "Automatic shutdown in:", bat_charging: "Battery charging",
      bat_empty: "Battery: empty", bat_low: "Battery: low", bat_med: "Battery: medium", bat_good: "Battery: full",
      ready: "Ready", wait_for_takeoff: "Waiting for takeoff", press_button_to_confirm: "Press the button to confirm",
      bluetooth_connected: "Bluetooth connected", bluetooth_disconnected: "Bluetooth disconnected", bluetooth_off: "Bluetooth disabled",
      bluetooth_on: "Bluetooth enabled", contact: "Please contact the support team", dot: "point", error: "Error",
      firmware: "Firmware", gps_disabled: "GPS disabled", gps_ok: "GPS ok", integration_settings: "Integration level",
      plus: "plus", minus: "minus", takeoff: "Takeoff", language: "Language: English", language_select: "Choose your language",
      language_selected: "English selected", lift_threshold_settings: "Lift tone threshold", loading_config: "Loading configuration",
      no_gps: "Searching GPS", profile_1: "Profile 1", profile_2: "Profile 2", profile_3: "Profile 3", profile_acro: "Acro",
      profile_alpinism: "Single skin", profile_bivouac: "Bivouac", profile_competition: "Competition", profile_cross: "Cross country",
      profile_hike: "Hike and fly", profile_paragliding: "Paragliding", profile_paramotor: "Paramotor",
      profile_select: "Select profile", profile_selected: "selected", profile_speedriding: "Speedriding",
      profile_tandem: "Tandem", recording: "Recording",
      reset_hold: "Hold the button to reset to factory settings",
      reset_wait: "Loading factory settings, please wait", sink_alarm_settings: "Sink tone threshold",
      speed_max: "Maximum speed", takeoff_off: "Takeoff detection disabled", takeoff_on: "Takeoff detection enabled",
      tap_disabled: "Double tap disabled", timezone: "Time zone", update_fail: "Error: update failed",
      update_ok: "Update successful", updating: "updating", vario_disabled: "Vario disabled", version: "Version:",
      voice_auto_off: "Automatic announcements disabled", voice_auto_on: "Automatic announcements enabled", voice_pack: "Voice pack",
      voice_update: "Please update the voice pack", voice_updated: "Voice pack updated", vol_0: "Vario muted",
      vol_1: "Volume: low", vol_2: "Volume: medium", vol_3: "Volume: loud", vol_4: "Volume: very loud",
      vz_max: "Maximum climb", wait: "please wait", warning: "Warning", weak_lift_off: "Weak lift tone disabled",
      weak_lift_on: "Weak lift tone enabled", battery: "Battery", bluetooth: "Bluetooth",
      bluetooth_pair: "Bluetooth pairing enabled", bluetooth_pin: "The Bluetooth pin code is",
      bluetooth_remote_event: "Bluetooth remote control mode", gps: "GPS", landing: "Landing", memory: "Memory",
      sensors_imu: "Accelerometer", sensors_pressure: "Barometer", solar_cell: "Solar cell",
      vario_sink_off: "Sink tone disabled", vz_min: "Maximum sink",
      glide_ratio: "Glide ratio", g_load: "g-load", bank_angle: "Bank angle", thermal_exit: "Thermal exit",
    },
  },
  fr: {
    label: "Français",
    hour: (h) => (h === 1 ? "1 heure" : `${h} heures`),
    hours: (h) => (h === 1 ? "1 heure" : `${h} heures`),
    minutes: (m) => (m === 1 ? "1 minute" : `${m} minutes`),
    metres: (m) => `${m} mètres`,
    kmh: (v) => (v === 1 ? "1 kilomètre par heure" : `${v} kilomètres par heure`),
    number: (n) => `${n}`,
    percent: (p) => `${p} pour cent`,
    ms: (v) => `${decimal(v, ",")} mètres par seconde`,
    sink: (v) => `moins ${decimal(v, ",")} mètres par seconde`,
    cms: (v) => `${v} centimètres par seconde`,
    g: (v) => `${decimal(v, ",")} G`,
    deg: (d) => (d < 2 ? `${d} degré` : `${d} degrés`),
    words: {
      "time/duration": "Temps de vol :", "time/time": "Heure :",
      "nav/wind_speed": "Vitesse du vent", "nav/wind": "Direction du vent", "nav/heading": "Cap",
      "nav/e": "Est", "nav/n": "Nord", "nav/ne": "Nord-est", "nav/nw": "Nord-ouest", "nav/s": "Sud", "nav/se": "Sud-est",
      "nav/sw": "Sud-ouest", "nav/w": "Ouest",
      altitude_max: "Altitude maximale :", auto_shutdown: "Arrêt automatique dans :", bat_charging: "Batterie en charge",
      bat_empty: "Batterie : vide", bat_low: "Batterie : faible", bat_med: "Batterie : moyenne", bat_good: "Batterie : pleine",
      ready: "Prêt", wait_for_takeoff: "En attente du décollage", press_button_to_confirm: "Appuyez sur le bouton pour confirmer",
      bluetooth_connected: "Bluetooth connecté", bluetooth_disconnected: "Bluetooth déconnecté", bluetooth_off: "Bluetooth désactivé",
      bluetooth_on: "Bluetooth activé", contact: "Veuillez contacter le support", dot: "virgule", error: "Erreur",
      firmware: "Firmware", gps_disabled: "GPS désactivé", gps_ok: "GPS ok", integration_settings: "Niveau d'intégration",
      plus: "plus", minus: "moins", takeoff: "Décollage", language: "Langue : français", language_select: "Choisissez votre langue",
      language_selected: "Français sélectionné", lift_threshold_settings: "Seuil du son de montée", loading_config: "Chargement de la configuration",
      no_gps: "Recherche du GPS", profile_1: "Profil 1", profile_2: "Profil 2", profile_3: "Profil 3", profile_acro: "Acro",
      profile_alpinism: "Monosurface", profile_bivouac: "Bivouac", profile_competition: "Compétition", profile_cross: "Cross",
      profile_hike: "Marche et vol", profile_paragliding: "Parapente", profile_paramotor: "Paramoteur",
      profile_select: "Choisissez le profil", profile_selected: "sélectionné", profile_speedriding: "Speedriding",
      profile_tandem: "Biplace", recording: "Enregistrement",
      reset_hold: "Maintenez le bouton pour rétablir les réglages d'usine",
      reset_wait: "Chargement des réglages d'usine, veuillez patienter", sink_alarm_settings: "Seuil du son de descente",
      speed_max: "Vitesse maximale", takeoff_off: "Détection du décollage désactivée", takeoff_on: "Détection du décollage activée",
      tap_disabled: "Double tape désactivée", timezone: "Fuseau horaire", update_fail: "Erreur : mise à jour échouée",
      update_ok: "Mise à jour réussie", updating: "mise à jour en cours", vario_disabled: "Vario désactivé", version: "Version :",
      voice_auto_off: "Annonces automatiques désactivées", voice_auto_on: "Annonces automatiques activées", voice_pack: "Pack vocal",
      voice_update: "Veuillez mettre à jour le pack vocal", voice_updated: "Pack vocal mis à jour", vol_0: "Vario muet",
      vol_1: "Volume : faible", vol_2: "Volume : moyen", vol_3: "Volume : fort", vol_4: "Volume : très fort",
      vz_max: "Montée maximale", wait: "veuillez patienter", warning: "Attention", weak_lift_off: "Son de montée faible désactivé",
      weak_lift_on: "Son de montée faible activé", battery: "Batterie", bluetooth: "Bluetooth",
      bluetooth_pair: "Appairage Bluetooth activé", bluetooth_pin: "Le code PIN Bluetooth est",
      bluetooth_remote_event: "Mode télécommande Bluetooth", gps: "GPS", landing: "Atterrissage", memory: "Mémoire",
      sensors_imu: "Accéléromètre", sensors_pressure: "Baromètre", solar_cell: "Cellule solaire",
      vario_sink_off: "Son de descente désactivé", vz_min: "Descente maximale",
      glide_ratio: "Finesse", g_load: "Facteur de charge", bank_angle: "Inclinaison", thermal_exit: "Sortie de thermique",
    },
  },
  it: {
    label: "Italiano",
    hour: (h) => `ore ${h}`,
    hours: (h) => (h === 1 ? "1 ora" : `${h} ore`),
    minutes: (m) => (m === 1 ? "1 minuto" : `${m} minuti`),
    metres: (m) => `${m} metri`,
    kmh: (v) => (v === 1 ? "1 chilometro all'ora" : `${v} chilometri all'ora`),
    number: (n) => `${n}`,
    percent: (p) => `${p} per cento`,
    ms: (v) => (v === 1 ? "1 metro al secondo" : `${decimal(v, ",")} metri al secondo`),
    sink: (v) => (v === 1 ? "meno 1 metro al secondo" : `meno ${decimal(v, ",")} metri al secondo`),
    cms: (v) => `${v} centimetri al secondo`,
    g: (v) => `${decimal(v, ",")} G`,
    deg: (d) => `${d} gradi`,
    words: {
      "time/duration": "Tempo di volo:", "time/time": "Ora:",
      "nav/wind_speed": "Velocità del vento", "nav/wind": "Direzione del vento", "nav/heading": "Direzione",
      "nav/e": "Est", "nav/n": "Nord", "nav/ne": "Nord-est", "nav/nw": "Nord-ovest", "nav/s": "Sud", "nav/se": "Sud-est",
      "nav/sw": "Sud-ovest", "nav/w": "Ovest",
      altitude_max: "Altitudine massima:", auto_shutdown: "Spegnimento automatico tra:", bat_charging: "Batteria in carica",
      bat_empty: "Batteria: scarica", bat_low: "Batteria: bassa", bat_med: "Batteria: media", bat_good: "Batteria: piena",
      ready: "Pronto", wait_for_takeoff: "In attesa del decollo", press_button_to_confirm: "Premi il pulsante per confermare",
      bluetooth_connected: "Bluetooth connesso", bluetooth_disconnected: "Bluetooth disconnesso", bluetooth_off: "Bluetooth disattivato",
      bluetooth_on: "Bluetooth attivato", contact: "Contattare il supporto", dot: "virgola", error: "Errore",
      firmware: "Firmware", gps_disabled: "GPS disattivato", gps_ok: "GPS ok", integration_settings: "Livello di integrazione",
      plus: "più", minus: "meno", takeoff: "Decollo", language: "Lingua: italiano", language_select: "Scegli la lingua",
      language_selected: "Italiano selezionato", lift_threshold_settings: "Soglia del suono di salita", loading_config: "Caricamento della configurazione",
      no_gps: "Ricerca del GPS", profile_1: "Profilo 1", profile_2: "Profilo 2", profile_3: "Profilo 3", profile_acro: "Acro",
      profile_alpinism: "Monosuperficie", profile_bivouac: "Bivacco", profile_competition: "Competizione", profile_cross: "Cross country",
      profile_hike: "Hike and fly", profile_paragliding: "Parapendio", profile_paramotor: "Paramotore",
      profile_select: "Seleziona il profilo", profile_selected: "selezionato", profile_speedriding: "Speedriding",
      profile_tandem: "Tandem", recording: "Registrazione",
      reset_hold: "Tieni premuto il pulsante per ripristinare le impostazioni di fabbrica",
      reset_wait: "Caricamento delle impostazioni di fabbrica, attendere", sink_alarm_settings: "Soglia del suono di discesa",
      speed_max: "Velocità massima", takeoff_off: "Rilevamento del decollo disattivato", takeoff_on: "Rilevamento del decollo attivato",
      tap_disabled: "Doppio tocco disattivato", timezone: "Fuso orario", update_fail: "Errore: aggiornamento non riuscito",
      update_ok: "Aggiornamento riuscito", updating: "aggiornamento in corso", vario_disabled: "Vario disattivato", version: "Versione:",
      voice_auto_off: "Annunci automatici disattivati", voice_auto_on: "Annunci automatici attivati", voice_pack: "Pacchetto vocale",
      voice_update: "Aggiornare il pacchetto vocale", voice_updated: "Pacchetto vocale aggiornato", vol_0: "Vario silenzioso",
      vol_1: "Volume: basso", vol_2: "Volume: medio", vol_3: "Volume: alto", vol_4: "Volume: molto alto",
      vz_max: "Salita massima", wait: "attendere", warning: "Attenzione", weak_lift_off: "Suono di salita debole disattivato",
      weak_lift_on: "Suono di salita debole attivato", battery: "Batteria", bluetooth: "Bluetooth",
      bluetooth_pair: "Associazione Bluetooth attivata", bluetooth_pin: "Il codice PIN Bluetooth è",
      bluetooth_remote_event: "Modalità telecomando Bluetooth", gps: "GPS", landing: "Atterraggio", memory: "Memoria",
      sensors_imu: "Accelerometro", sensors_pressure: "Barometro", solar_cell: "Cella solare",
      vario_sink_off: "Suono di discesa disattivato", vz_min: "Discesa massima",
      glide_ratio: "Efficienza", g_load: "Fattore di carico", bank_angle: "Inclinazione", thermal_exit: "Uscita dalla termica",
    },
  },
  es: {
    label: "Español",
    hour: (h) => (h === 1 ? "1 hora" : `${h} horas`),
    hours: (h) => (h === 1 ? "1 hora" : `${h} horas`),
    minutes: (m) => (m === 1 ? "1 minuto" : `${m} minutos`),
    metres: (m) => `${m} metros`,
    kmh: (v) => (v === 1 ? "1 kilómetro por hora" : `${v} kilómetros por hora`),
    number: (n) => `${n}`,
    percent: (p) => `${p} por ciento`,
    ms: (v) => (v === 1 ? "1 metro por segundo" : `${decimal(v, ",")} metros por segundo`),
    sink: (v) => (v === 1 ? "menos 1 metro por segundo" : `menos ${decimal(v, ",")} metros por segundo`),
    cms: (v) => `${v} centímetros por segundo`,
    g: (v) => `${decimal(v, ",")} G`,
    deg: (d) => `${d} grados`,
    words: {
      "time/duration": "Tiempo de vuelo:", "time/time": "Hora:",
      "nav/wind_speed": "Velocidad del viento", "nav/wind": "Dirección del viento", "nav/heading": "Rumbo",
      "nav/e": "Este", "nav/n": "Norte", "nav/ne": "Noreste", "nav/nw": "Noroeste", "nav/s": "Sur", "nav/se": "Sureste",
      "nav/sw": "Suroeste", "nav/w": "Oeste",
      altitude_max: "Altitud máxima:", auto_shutdown: "Apagado automático en:", bat_charging: "Batería cargando",
      bat_empty: "Batería: vacía", bat_low: "Batería: baja", bat_med: "Batería: media", bat_good: "Batería: llena",
      ready: "Listo", wait_for_takeoff: "Esperando el despegue", press_button_to_confirm: "Pulsa el botón para confirmar",
      bluetooth_connected: "Bluetooth conectado", bluetooth_disconnected: "Bluetooth desconectado", bluetooth_off: "Bluetooth desactivado",
      bluetooth_on: "Bluetooth activado", contact: "Por favor, contacte con el servicio técnico", dot: "coma", error: "Error",
      firmware: "Firmware", gps_disabled: "GPS desactivado", gps_ok: "GPS ok", integration_settings: "Nivel de integración",
      plus: "más", minus: "menos", takeoff: "Despegue", language: "Idioma: español", language_select: "Elige tu idioma",
      language_selected: "Español seleccionado", lift_threshold_settings: "Umbral del tono de ascenso", loading_config: "Cargando la configuración",
      no_gps: "Buscando GPS", profile_1: "Perfil 1", profile_2: "Perfil 2", profile_3: "Perfil 3", profile_acro: "Acro",
      profile_alpinism: "Monocapa", profile_bivouac: "Vivac", profile_competition: "Competición", profile_cross: "Cross country",
      profile_hike: "Hike and fly", profile_paragliding: "Parapente", profile_paramotor: "Paramotor",
      profile_select: "Selecciona el perfil", profile_selected: "seleccionado", profile_speedriding: "Speedriding",
      profile_tandem: "Tándem", recording: "Grabación",
      reset_hold: "Mantén pulsado el botón para restablecer los ajustes de fábrica",
      reset_wait: "Cargando los ajustes de fábrica, espere", sink_alarm_settings: "Umbral del tono de descenso",
      speed_max: "Velocidad máxima", takeoff_off: "Detección de despegue desactivada", takeoff_on: "Detección de despegue activada",
      tap_disabled: "Doble toque desactivado", timezone: "Zona horaria", update_fail: "Error: actualización fallida",
      update_ok: "Actualización correcta", updating: "actualizando", vario_disabled: "Vario desactivado", version: "Versión:",
      voice_auto_off: "Avisos automáticos desactivados", voice_auto_on: "Avisos automáticos activados", voice_pack: "Paquete de voz",
      voice_update: "Por favor, actualice el paquete de voz", voice_updated: "Paquete de voz actualizado", vol_0: "Vario silenciado",
      vol_1: "Volumen: bajo", vol_2: "Volumen: medio", vol_3: "Volumen: alto", vol_4: "Volumen: muy alto",
      vz_max: "Ascenso máximo", wait: "espere", warning: "Atención", weak_lift_off: "Tono de ascenso débil desactivado",
      weak_lift_on: "Tono de ascenso débil activado", battery: "Batería", bluetooth: "Bluetooth",
      bluetooth_pair: "Emparejamiento Bluetooth activado", bluetooth_pin: "El código PIN de Bluetooth es",
      bluetooth_remote_event: "Modo mando a distancia Bluetooth", gps: "GPS", landing: "Aterrizaje", memory: "Memoria",
      sensors_imu: "Acelerómetro", sensors_pressure: "Barómetro", solar_cell: "Célula solar",
      vario_sink_off: "Tono de descenso desactivado", vz_min: "Descenso máximo",
      glide_ratio: "Planeo", g_load: "Factor de carga", bank_angle: "Inclinación", thermal_exit: "Salida de la térmica",
    },
  },
  pt: {
    label: "Português",
    hour: (h) => (h === 1 ? "1 hora" : `${h} horas`),
    hours: (h) => (h === 1 ? "1 hora" : `${h} horas`),
    minutes: (m) => (m === 1 ? "1 minuto" : `${m} minutos`),
    metres: (m) => `${m} metros`,
    kmh: (v) => (v === 1 ? "1 quilómetro por hora" : `${v} quilómetros por hora`),
    number: (n) => `${n}`,
    percent: (p) => `${p} por cento`,
    ms: (v) => (v === 1 ? "1 metro por segundo" : `${decimal(v, ",")} metros por segundo`),
    sink: (v) => (v === 1 ? "menos 1 metro por segundo" : `menos ${decimal(v, ",")} metros por segundo`),
    cms: (v) => `${v} centímetros por segundo`,
    g: (v) => `${decimal(v, ",")} G`,
    deg: (d) => `${d} graus`,
    words: {
      "time/duration": "Tempo de voo:", "time/time": "Horas:",
      "nav/wind_speed": "Velocidade do vento", "nav/wind": "Direção do vento", "nav/heading": "Rumo",
      "nav/e": "Este", "nav/n": "Norte", "nav/ne": "Nordeste", "nav/nw": "Noroeste", "nav/s": "Sul", "nav/se": "Sudeste",
      "nav/sw": "Sudoeste", "nav/w": "Oeste",
      altitude_max: "Altitude máxima:", auto_shutdown: "Desligamento automático em:", bat_charging: "Bateria a carregar",
      bat_empty: "Bateria: vazia", bat_low: "Bateria: fraca", bat_med: "Bateria: média", bat_good: "Bateria: cheia",
      ready: "Pronto", wait_for_takeoff: "À espera da descolagem", press_button_to_confirm: "Prima o botão para confirmar",
      bluetooth_connected: "Bluetooth conectado", bluetooth_disconnected: "Bluetooth desconectado", bluetooth_off: "Bluetooth desativado",
      bluetooth_on: "Bluetooth ativado", contact: "Por favor, contacte o suporte", dot: "vírgula", error: "Erro",
      firmware: "Firmware", gps_disabled: "GPS desativado", gps_ok: "GPS ok", integration_settings: "Nível de integração",
      plus: "mais", minus: "menos", takeoff: "Descolagem", language: "Idioma: português", language_select: "Escolha o seu idioma",
      language_selected: "Português selecionado", lift_threshold_settings: "Limiar do som de subida", loading_config: "A carregar a configuração",
      no_gps: "A procurar GPS", profile_1: "Perfil 1", profile_2: "Perfil 2", profile_3: "Perfil 3", profile_acro: "Acro",
      profile_alpinism: "Single skin", profile_bivouac: "Bivaque", profile_competition: "Competição", profile_cross: "Cross country",
      profile_hike: "Hike and fly", profile_paragliding: "Parapente", profile_paramotor: "Paramotor",
      profile_select: "Selecione o perfil", profile_selected: "selecionado", profile_speedriding: "Speedriding",
      profile_tandem: "Tandem", recording: "Gravação",
      reset_hold: "Mantenha o botão premido para repor as definições de fábrica",
      reset_wait: "A carregar as definições de fábrica, aguarde", sink_alarm_settings: "Limiar do som de descida",
      speed_max: "Velocidade máxima", takeoff_off: "Deteção de descolagem desativada", takeoff_on: "Deteção de descolagem ativada",
      tap_disabled: "Toque duplo desativado", timezone: "Fuso horário", update_fail: "Erro: a atualização falhou",
      update_ok: "Atualização concluída", updating: "a atualizar", vario_disabled: "Vário desativado", version: "Versão:",
      voice_auto_off: "Anúncios automáticos desativados", voice_auto_on: "Anúncios automáticos ativados", voice_pack: "Pacote de voz",
      voice_update: "Por favor, atualize o pacote de voz", voice_updated: "Pacote de voz atualizado", vol_0: "Vário silenciado",
      vol_1: "Volume: baixo", vol_2: "Volume: médio", vol_3: "Volume: alto", vol_4: "Volume: muito alto",
      vz_max: "Subida máxima", wait: "aguarde", warning: "Atenção", weak_lift_off: "Som de subida fraca desativado",
      weak_lift_on: "Som de subida fraca ativado", battery: "Bateria", bluetooth: "Bluetooth",
      bluetooth_pair: "Emparelhamento Bluetooth ativado", bluetooth_pin: "O código PIN do Bluetooth é",
      bluetooth_remote_event: "Modo de controlo remoto Bluetooth", gps: "GPS", landing: "Aterragem", memory: "Memória",
      sensors_imu: "Acelerómetro", sensors_pressure: "Barómetro", solar_cell: "Célula solar",
      vario_sink_off: "Som de descida desativado", vz_min: "Descida máxima",
      glide_ratio: "Planeio", g_load: "Fator de carga", bank_angle: "Inclinação", thermal_exit: "Saída da térmica",
    },
  },
  nl: {
    label: "Nederlands",
    hour: (h) => `${h} uur`,
    hours: (h) => `${h} uur`,
    minutes: (m) => (m === 1 ? "1 minuut" : `${m} minuten`),
    metres: (m) => `${m} meter`,
    kmh: (v) => `${v} kilometer per uur`,
    number: (n) => `${n}`,
    percent: (p) => `${p} procent`,
    ms: (v) => `${decimal(v, ",")} meter per seconde`,
    sink: (v) => `min ${decimal(v, ",")} meter per seconde`,
    cms: (v) => `${v} centimeter per seconde`,
    g: (v) => `${decimal(v, ",")} G`,
    deg: (d) => `${d} graden`,
    words: {
      "time/duration": "Vliegtijd:", "time/time": "Tijd:",
      "nav/wind_speed": "Windsnelheid", "nav/wind": "Windrichting", "nav/heading": "Koers",
      "nav/e": "Oost", "nav/n": "Noord", "nav/ne": "Noordoost", "nav/nw": "Noordwest", "nav/s": "Zuid", "nav/se": "Zuidoost",
      "nav/sw": "Zuidwest", "nav/w": "West",
      altitude_max: "Maximale hoogte:", auto_shutdown: "Automatisch uitschakelen over:", bat_charging: "Batterij laadt op",
      bat_empty: "Batterij: leeg", bat_low: "Batterij: zwak", bat_med: "Batterij: half", bat_good: "Batterij: vol",
      ready: "Gereed", wait_for_takeoff: "Wachten op de start", press_button_to_confirm: "Druk op de knop om te bevestigen",
      bluetooth_connected: "Bluetooth verbonden", bluetooth_disconnected: "Bluetooth verbroken", bluetooth_off: "Bluetooth uitgeschakeld",
      bluetooth_on: "Bluetooth ingeschakeld", contact: "Neem contact op met de ondersteuning", dot: "komma", error: "Fout",
      firmware: "Firmware", gps_disabled: "GPS uitgeschakeld", gps_ok: "GPS ok", integration_settings: "Integratieniveau",
      plus: "plus", minus: "min", takeoff: "Start", language: "Taal: Nederlands", language_select: "Kies je taal",
      language_selected: "Nederlands geselecteerd", lift_threshold_settings: "Drempel van de stijgtoon", loading_config: "Configuratie wordt geladen",
      no_gps: "Zoeken naar GPS", profile_1: "Profiel 1", profile_2: "Profiel 2", profile_3: "Profiel 3", profile_acro: "Acro",
      profile_alpinism: "Single skin", profile_bivouac: "Bivak", profile_competition: "Competitie", profile_cross: "Cross country",
      profile_hike: "Hike and fly", profile_paragliding: "Paragliden", profile_paramotor: "Paramotor",
      profile_select: "Kies een profiel", profile_selected: "geselecteerd", profile_speedriding: "Speedriding",
      profile_tandem: "Tandem", recording: "Opname",
      reset_hold: "Houd de knop ingedrukt om de fabrieksinstellingen te herstellen",
      reset_wait: "Fabrieksinstellingen worden geladen, even geduld", sink_alarm_settings: "Drempel van de daaltoon",
      speed_max: "Maximale snelheid", takeoff_off: "Startdetectie uitgeschakeld", takeoff_on: "Startdetectie ingeschakeld",
      tap_disabled: "Dubbeltik uitgeschakeld", timezone: "Tijdzone", update_fail: "Fout: update mislukt",
      update_ok: "Update geslaagd", updating: "bezig met updaten", vario_disabled: "Vario uitgeschakeld", version: "Versie:",
      voice_auto_off: "Automatische meldingen uitgeschakeld", voice_auto_on: "Automatische meldingen ingeschakeld", voice_pack: "Spraakpakket",
      voice_update: "Werk het spraakpakket bij", voice_updated: "Spraakpakket bijgewerkt", vol_0: "Vario gedempt",
      vol_1: "Volume: zacht", vol_2: "Volume: gemiddeld", vol_3: "Volume: luid", vol_4: "Volume: zeer luid",
      vz_max: "Maximaal stijgen", wait: "even geduld", warning: "Let op", weak_lift_off: "Zwakke stijgtoon uitgeschakeld",
      weak_lift_on: "Zwakke stijgtoon ingeschakeld", battery: "Batterij", bluetooth: "Bluetooth",
      bluetooth_pair: "Bluetooth-koppeling ingeschakeld", bluetooth_pin: "De Bluetooth-pincode is",
      bluetooth_remote_event: "Bluetooth-afstandsbedieningsmodus", gps: "GPS", landing: "Landing", memory: "Geheugen",
      sensors_imu: "Versnellingssensor", sensors_pressure: "Barometer", solar_cell: "Zonnecel",
      vario_sink_off: "Daaltoon uitgeschakeld", vz_min: "Maximaal dalen",
      glide_ratio: "Glijgetal", g_load: "G-kracht", bank_angle: "Rolhoek", thermal_exit: "Thermiek verlaten",
    },
  },
};

/* A language that differs from another in a few words only. */
function voicepackVariant(base, label, words, units = {}) {
  const b = VOICEPACK_LANGUAGES[base];
  return { ...b, ...units, label, words: { ...b.words, ...words } };
}
VOICEPACK_LANGUAGES.br = voicepackVariant("pt", "Português (Brasil)", {
  "nav/e": "Leste", bat_charging: "Bateria carregando", contact: "Por favor, contate o suporte", takeoff: "Decolagem",
  loading_config: "Carregando a configuração", no_gps: "Procurando GPS", wait_for_takeoff: "Aguardando a decolagem", press_button_to_confirm: "Pressione o botão para confirmar",
  reset_hold: "Mantenha o botão pressionado para restaurar as configurações de fábrica",
  reset_wait: "Carregando as configurações de fábrica, aguarde", takeoff_off: "Detecção de decolagem desativada",
  takeoff_on: "Detecção de decolagem ativada", updating: "atualizando", bluetooth_remote_event: "Modo de controle remoto Bluetooth",
  landing: "Pouso", sensors_imu: "Acelerômetro", sensors_pressure: "Barômetro",
}, { kmh: (v) => (v === 1 ? "1 quilômetro por hora" : `${v} quilômetros por hora`) });
VOICEPACK_LANGUAGES.us = voicepackVariant("en", "English (US)", {}, {
  metres: (m) => `${m} meters`, kmh: (v) => `${v} kilometers per hour`,
  ms: (v) => `${decimal(v, ".")} meters per second`, sink: (v) => `minus ${decimal(v, ".")} meters per second`,
  cms: (v) => `${v} centimeters per second`,
});

/*
 * The manufacturer's voice packs: voices/<id>.ubpack, the id is the value of system_language_string.
 * lang: the texts above used for generating clips in that language (null: none yet, the pack can
 * only be used); tts: the preferred locale of the voices.
 */
const VOICEPACKS = [
  { id: "fr", label: "French", lang: "fr", tts: "fr-FR" },
  { id: "en", label: "English (UK)", lang: "en", tts: "en-GB" },
  { id: "us", label: "English (US)", lang: "us", tts: "en-US" },
  { id: "av", label: "English (avionics)", lang: "en", tts: "en-GB" },
  { id: "de", label: "German", lang: "de", tts: "de-DE" },
  { id: "it", label: "Italian", lang: "it", tts: "it-IT" },
  { id: "es", label: "Spanish", lang: "es", tts: "es-ES" },
  { id: "pt", label: "Portuguese", lang: "pt", tts: "pt-PT" },
  { id: "br", label: "Portuguese (Brazil)", lang: "br", tts: "pt-BR" },
  { id: "pl", label: "Polish", lang: null, tts: "pl-PL" },
  { id: "fi", label: "Finnish", lang: null, tts: "fi-FI" },
  { id: "cs", label: "Czech", lang: null, tts: "cs-CZ" },
  { id: "ja", label: "Japanese", lang: null, tts: "ja-JP" },
  { id: "el", label: "Greek", lang: null, tts: "el-GR" },
  { id: "sv", label: "Swedish", lang: null, tts: "sv-SE" },
  { id: "tr", label: "Turkish", lang: null, tts: "tr-TR" },
  { id: "zh", label: "Mandarin", lang: null, tts: "zh-CN" },
  { id: "nl", label: "Dutch", lang: "nl", tts: "nl-NL" },
  { id: "sk", label: "Slovak", lang: null, tts: "sk-SK" },
  { id: "hu", label: "Hungarian", lang: null, tts: "hu-HU" },
  { id: "lv", label: "Latvian", lang: null, tts: "lv-LV" },
  { id: "ro", label: "Romanian", lang: null, tts: "ro-RO" },
  { id: "ko", label: "Korean", lang: null, tts: "ko-KR" },
  { id: "ru", label: "Russian", lang: null, tts: "ru-RU" },
];

/*
 * The pack behind a system_language_string value: {id, label, lang, tts, language (the two-letter
 * code of the voices)}. An unknown name (a pack of one's own) counts by its first two letters.
 */
function voicepackInfo(id) {
  const key = (id || "de").trim().toLowerCase();
  const known = VOICEPACKS.find((p) => p.id === key);
  if (known) return { ...known, language: known.tts.split("-")[0] };
  const two = key.slice(0, 2);
  const lang = VOICEPACK_LANGUAGES[two] ? two : null;
  return { id: key, label: key, lang, tts: lang ? VOICEPACKS.find((p) => p.lang === two)?.tts || two : "", language: two };
}

/*
 * Clips the manufacturer's packs do not have: UltraBeep says them for its own features, so they have to be
 * generated (single files in voices/custom, or a pack built here). Checked against the stock de.ubpack
 * (973 clips). profile_custom_<n>, the own profile names, are extension clips too but live outside the
 * catalogue (the profile's voice line). So are the clips of the folders in VOICEPACK_EXTENSION_FOLDERS, which
 * the device says instead of single numbers once it has them: g/g_<tenths>, the load factor 0.0 to 10.0 g as
 * one clip each ("2,3 G" instead of "2" "dot" "3"), and bank/bank_<deg>, the bank angle 0 to 90 degrees in
 * steps of 5 ("60 Grad"; a size without a side: it comes from the load of a coordinated turn).
 */
const VOICEPACK_EXTENSION = new Set(["ready", "wait_for_takeoff", "press_button_to_confirm", "glide_ratio", "g_load",
  "bank_angle", "thermal_exit"]);
const VOICEPACK_EXTENSION_FOLDERS = ["g", "bank"];
const voicepackExtension = (name) => VOICEPACK_EXTENSION.has(name) || VOICEPACK_EXTENSION_FOLDERS.includes(name.split("/")[0]);

/*
 * All clips of a language: [{name, text, stock}], name as the firmware asks for it ("al/al_1230", "takeoff");
 * stock is false for the extension clips above.
 * Every clip is generated at the same level: the device sets the volume by its mode (on the ground, in
 * flight), so a pack needs no quieter clips.
 */
function voicepackClips(lang) {
  const L = VOICEPACK_LANGUAGES[lang] || VOICEPACK_LANGUAGES.en;
  const out = [];
  const add = (name, text) => out.push({ name, text, stock: !voicepackExtension(name) });
  for (let h = 0; h < 24; h++) add(`time/hour_${h}`, L.hour(h));
  for (let h = 0; h < 24; h++) add(`time/duration_${h}`, L.hours(h));
  for (let m = 0; m < 60; m++) add(`time/min_${m}`, L.minutes(m));
  for (let a = -100; a < 5000; a += 10) add(`al/al_${a}`, L.metres(a));
  for (let a = 5000; a <= 7000; a += 100) add(`al/al_${a}`, L.metres(a));
  for (let v = 0; v < 80; v++) add(`sp/sp_${v}`, L.kmh(v));
  for (let v = 85; v <= 200; v += 5) add(`sp/sp_${v}`, L.kmh(v));
  for (let n = 0; n <= 60; n++) add(`${n}`, L.number(n));
  for (let p = 0; p <= 100; p += 25) add(`${p}_percent`, L.percent(p));
  add("vario_lift_0", L.ms(0));
  for (let c = 10; c <= 50; c += 10) add(`vario_lift_${c}`, L.ms(c / 100));
  for (let c = 100; c <= 1000; c += 50) add(`vario_lift_${c}`, L.ms(c / 100));
  for (let c = 10; c <= 50; c += 10) add(`vario_sink_${c}`, L.sink(c / 100));
  for (let c = 100; c <= 1050; c += 50) add(`vario_sink_${c}`, L.sink(c / 100));
  for (let c = 1150; c <= 1950; c += 100) add(`vario_sink_${c}`, L.sink(c / 100));
  for (const c of [5, 10, 15, 20, 30, 40, 50]) add(`settings_${c}`, L.cms(c));
  /* the load factor in tenths of a g: the accelerometer measures to 8 g and is good to about 0.1 g */
  for (let t = 0; t <= 100; t++) add(`g/g_${t}`, L.g(t / 10));
  /* the bank angle, acos(1/load): 0.1 g is 2 degrees at 60 and more at less, so steps of 5 */
  for (let d = 0; d <= 90; d += 5) add(`bank/bank_${d}`, L.deg(d));
  for (const [name, text] of Object.entries(L.words)) add(name, text);
  return out;
}

/* The text of one clip in a language, or "" when the pack has no such clip. */
function voicepackText(lang, name) {
  const c = voicepackClips(lang).find((x) => x.name === name);
  return c ? c.text : "";
}

/* Sentences to judge a voice by: spoken as single clips, as the device plays them. */
function voicepackSample() {
  return ["profile_select", "profile_paragliding", "gps_ok", "takeoff", "time/duration", "time/duration_1", "time/min_2",
    "al/al_1230", "sp/sp_42", "vario_lift_150"];
}

if (typeof module !== "undefined") {
  module.exports = { voicepackClips, voicepackText, voicepackSample, voicepackInfo, voicepackExtension, VOICEPACK_LANGUAGES,
    VOICEPACKS, VOICEPACK_EXTENSION };
}
