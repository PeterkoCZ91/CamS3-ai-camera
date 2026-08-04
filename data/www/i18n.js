// CamS3 i18n — lightweight client-side translation.
// Default language: English. Czech overlay via data-i18n keys.
// Persist user choice in localStorage; auto-detect browser language on first visit.
(function () {
    const T = {
        en: {
            // Navigation
            'nav.overview': 'Overview',
            'nav.settings': 'Settings',
            'nav.zones': 'Zones',
            'nav.wifi': 'Wi-Fi',
            'nav.admin': 'Admin',
            'nav.gallery': 'Gallery',
            'nav.back': 'Back',

            // Common
            'common.save': 'Save settings',
            'common.cancel': 'Cancel',
            'common.confirm': 'Confirm',
            'common.close': 'Close',
            'common.delete': 'Delete',
            'common.download': 'Download',
            'common.connect': 'Connect',
            'common.scan': 'Scan',
            'common.loading': 'Loading...',
            'common.connecting': 'Connecting...',
            'common.online': 'Online',
            'common.offline': 'Disconnected',
            'common.language': 'Language',
            'common.error': 'Error',
            // Short dynamic state words used by index.html tiles and buttons.
            'state.on': 'ON',
            'state.off': 'OFF',
            'state.detected': 'DETECTED',
            'state.idle': 'Idle',
            'state.none': 'None',
            'state.connected': 'Connected',
            'state.disconnected': 'Disconnected',
            'index.stream.error': 'Stream unavailable (too many clients?)',
            // Accessibility strings (aria-label / alt), applied via data-i18n-aria-label
            // and data-i18n-alt.
            'aria.nav': 'Main navigation',
            'aria.path': 'Current folder path',
            'aria.grid': 'Motion detection grid',
            'index.stream.alt': 'Live camera stream',

            // Index / overview
            'index.title': 'CamS3 5MP Camera',
            'index.stream.start': 'Start stream',
            'index.stream.stop': 'Stop stream',
            'index.stream.placeholder': 'Click to start stream',
            'index.snapshot': 'Snapshot',
            'index.info.ip': 'IP address',
            'index.info.uptime': 'Uptime',
            'index.info.heap': 'Free heap',
            'index.info.psram': 'Free PSRAM',
            'index.info.fps': 'Capture FPS',
            'index.info.rssi': 'Wi-Fi RSSI',
            'index.info.version': 'Firmware',
            'index.info.clients': 'Stream clients',
            'index.info.temp': 'Temperature',
            'index.info.profile': 'Profile',
            'index.info.sd': 'SD card',
            'index.profile.day': 'DAY',
            'index.profile.dusk': 'DUSK',
            'index.profile.night': 'NIGHT',
            'index.tools.status': 'Status JSON',
            'index.tools.health': 'Health',
            'index.tools.events': 'Events',
            'index.tools.telemetry': 'Telemetry',
            'index.tools.stream_stats': 'Stream Stats',
            'index.tools.psram_stats': 'PSRAM Stats',
            'index.tools.sd_files': 'SD files',
            'index.banner.defaultpw': 'Warning: device still uses the default password',
            'index.banner.defaultpw.action': 'Change it in Settings.',
            'index.tools.log': 'Live Log',
            'index.tools.motion_debug': 'Motion debug',

            // Settings
            'settings.title': 'Camera settings',
            'settings.basic': 'Basic settings',
            'settings.image': 'Image',
            'settings.exposure': 'Exposure & gain',
            'settings.whitebalance': 'White balance',
            'settings.motion': 'Motion detection',
            'settings.face': 'Face detection',
            'settings.person': 'Person detection',
            'settings.timelapse': 'Time-lapse',
            'settings.telegram': 'Telegram',
            'settings.mqtt': 'MQTT',
            'settings.security': 'Security',
            'settings.perf': 'Performance',

            'settings.jpeg_quality': 'JPEG quality (1–63, lower = better)',
            'settings.frame_size': 'Resolution',
            'settings.vflip': 'Flip vertically (V-Flip)',
            'settings.hmirror': 'Mirror horizontally (H-Mirror)',
            'settings.brightness': 'Brightness (-2 to 2)',
            'settings.contrast': 'Contrast (-2 to 2)',
            'settings.saturation': 'Saturation (-2 to 2)',
            'settings.sharpness': 'Sharpness (-3 to 3)',
            'settings.denoise': 'Denoise (0–8)',
            'settings.security.user': 'Admin user (admin UI / OTA / WebSocket)',
            'settings.security.pass': 'New password (min. 4 chars)',
            'settings.security.pass.hint': 'After change you will need the new password on next login.',
            'settings.security.pass.placeholder': 'Leave empty to keep current',
            'settings.perf.active_fps': 'FPS while streaming',
            'settings.perf.idle_fps': 'Idle FPS',
            'settings.perf.idle_fps.hint': 'Lower = less power, slower detection',

            'settings.aec': 'Auto Exposure (AEC)',
            'settings.ae_level': 'AE Level (-2 to 2)',
            'settings.ae_level.hint': '-2 = darker, +2 = brighter',
            'settings.manual_exposure': 'Manual Exposure (0–1200)',
            'settings.agc': 'Auto Gain (AGC)',
            'settings.manual_gain': 'Manual Gain (0–30)',
            'settings.gainceiling': 'Gain Ceiling',
            'settings.awb': 'Auto White Balance',
            'settings.wb_mode': 'White Balance Mode',
            'settings.sensor_corrections': 'Sensor corrections',
            'settings.bpc': 'Black Pixel Correction (BPC)',
            'settings.wpc': 'White Pixel Correction (WPC)',
            'settings.gamma': 'Gamma Correction',
            'settings.lens': 'Lens Correction',
            'settings.motion.enabled': 'Motion detection enabled',
            // motion_threshold is a per-block pixel difference threshold, so a
            // HIGHER value means LESS sensitive. The old label said the opposite.
            'settings.motion.sensitivity': 'Pixel threshold (5–80, lower = more sensitive)',
            'settings.motion.min_area': 'Min area % to trigger',
            'settings.person.enabled': 'Person detection enabled',
            'settings.person.confidence': 'Uncertain threshold % (MQTT verification)',
            'settings.person.confident': 'Confident threshold % (direct alert)',
            'settings.person.temporal': 'Consecutive frames required',
            'settings.person.cooldown': 'Cooldown between detections (s)',
            'settings.person.tg_notify': 'Telegram notification',
            'settings.person.tg_photo': 'Telegram photo',
            'settings.person.cascade_hint': 'Person detection runs only after motion is detected (cascade).',
            'settings.mqtt.user': 'Username (leave empty to keep)',
            'settings.mqtt.pass': 'Password (leave empty to keep)',
            'settings.mqtt.tls': 'TLS (needs /ca.pem for validation)',
            'settings.stored': 'stored on device',
            'settings.notloaded': 'Current settings not loaded yet — reload the page',
            'settings.loadfailed': 'Could not load current settings',
            'settings.motion.cooldown': 'Cooldown between detections (s)',
            'settings.motion.max_area': 'Max area % (rejects lighting changes)',
            'settings.motion.temporal': 'Temporal filter (2+ frames)',
            'settings.motion.spatial': 'Spatial filter (reject isolated blocks)',
            // "Night suppression" read as if it suppressed detection at night. What it
            // actually does (motion_detect.cpp) is raise the pixel threshold and require
            // denser clusters, i.e. suppress sensor noise — detection stays active.
            'settings.motion.night': 'Night noise suppression',
            'settings.save_sd': 'Save to SD card',
            'settings.face.enabled': 'Face detection enabled',
            'settings.face.two_stage': 'Two-stage detection (more accurate)',
            'settings.face.cooldown': 'Cooldown between detections (s)',
            'settings.face.save_sd': 'Save face to SD',
            'settings.face.cascade_hint': 'Face detection runs only when motion is detected (cascade).',
            'settings.timelapse.enabled': 'Time-lapse enabled',
            'settings.timelapse.interval': 'Interval (seconds)',
            'settings.timelapse.interval_hint': '60s = every minute, 3600s = every hour',
            'settings.telegram.enabled': 'Telegram enabled',
            'settings.telegram.bot_token': 'Bot Token',
            'settings.telegram.bot_token_hint': 'Get from @BotFather on Telegram',
            'settings.telegram.chat_id': 'Chat ID',
            'settings.telegram.chat_id_hint': 'Get from @userinfobot',
            'settings.telegram.notify_motion': 'Notify on motion',
            'settings.telegram.photo_motion': 'Photo on motion',
            'settings.telegram.notify_face': 'Notify on face',
            'settings.telegram.photo_face': 'Photo on face',
            'settings.telegram.cooldown': 'Cooldown (5-300s)',
            'settings.telegram.active_start': 'Active hours - start (0–23)',
            'settings.telegram.active_end': 'Active hours - end (0–23)',
            'settings.telegram.active_hint': 'Overnight wrap supported (e.g. 22–7)',
            'settings.mqtt.enabled': 'MQTT enabled',
            'settings.mqtt.server': 'MQTT server',
            'settings.mqtt.port': 'Port',
            'settings.mqtt.topic_prefix': 'Topic prefix',
            'settings.wb.auto': 'Auto',
            'settings.wb.sunny': 'Sunny',
            'settings.wb.cloudy': 'Cloudy',
            'settings.wb.office': 'Office',
            'settings.wb.home': 'Home',

            // Wi-Fi
            'wifi.title': 'Wi-Fi configuration',
            'wifi.current': 'Current connection',
            'wifi.ip': 'IP',
            'wifi.rssi': 'RSSI',
            'wifi.mac': 'MAC',
            'wifi.hostname': 'Hostname',
            'wifi.scan.title': 'Available networks',
            'wifi.scan.hint': 'Click "Scan" to load networks',
            'wifi.scan.searching': 'Scanning...',
            'wifi.scan.none': 'No networks found',
            'wifi.scan.error': 'Scan error',
            'wifi.manual.title': 'Manual connect',
            'wifi.manual.ssid': 'SSID',
            'wifi.manual.ssid.ph': 'Network name',
            'wifi.manual.pass': 'Password',
            'wifi.manual.pass.ph': 'Network password',
            'wifi.manual.ssid.required': 'Enter SSID',
            'wifi.connecting.to': 'Connecting to',
            'wifi.connecting.note': 'Camera is connecting, this page may become unreachable...',
            'wifi.strength.excellent': 'Excellent',
            'wifi.strength.good': 'Good',
            'wifi.strength.weak': 'Weak',

            // Gallery
            'gallery.title': 'SD Gallery',
            'gallery.loading': 'Loading...',
            'gallery.empty': 'Empty folder',
            'gallery.unavailable': 'SD card unavailable',
            'gallery.folder': 'folder',
            'gallery.delete.confirm': 'Delete file',
            'gallery.delete.ok': 'File deleted',
            'gallery.delete.err': 'Deletion error',

            // Admin
            'admin.title': 'Administration',
            'admin.sys': 'System info',
            'admin.camera': 'Camera',
            'admin.sd': 'SD card',
            'admin.ota': 'OTA update',
            'admin.actions': 'System actions',
            'admin.api': 'API endpoints',
            'admin.cam.count': 'Total frames',
            'admin.cam.errors': 'Errors',
            'admin.cam.clients': 'Stream clients',
            'admin.cam.reinit': 'Reinitialize camera',
            'admin.sd.status': 'Status',
            'admin.sd.total': 'Total',
            'admin.sd.used': 'Used',
            'admin.sd.mounted': 'Mounted',
            'admin.sd.missing': 'Not mounted',
            'admin.ota.hint': 'Firmware update via web interface. Upload a .bin for firmware or a .bin for the filesystem (LittleFS).',
            'admin.ota.open': 'Open OTA page',
            'admin.action.reboot': 'Reboot device',
            'admin.action.reset': 'Factory reset',
            'admin.action.reset.hint': 'Factory reset erases all configuration and reboots into AP mode.',
            'admin.psram.total': 'Total PSRAM',
            'admin.confirm.generic': 'Confirmation',
            'admin.confirm.reboot.title': 'Reboot device',
            'admin.confirm.reboot.text': 'Really reboot the camera? Stream will be interrupted.',
            'admin.confirm.reset.title': 'Factory reset',
            'admin.confirm.reset.text': 'WARNING: all configuration will be erased. The device will reboot into AP mode. Continue?',
            'admin.rebooting': 'Rebooting...',
            'admin.resetting': 'Resetting...',

            // Zones / ROI editor
            'zones.title': 'Detection zones & ROI',
            'zones.mode': 'Editor mode',
            'zones.tab.roi': 'ROI mask',
            'zones.tab.zones': 'Zones',
            'zones.preview.refresh': 'Refresh snapshot',
            'zones.preview.live': 'Live stream',
            'zones.preview.failed': 'Preview unavailable',
            'zones.grid.label': 'Motion grid',
            'zones.roi.hint': 'Click or drag over the image to toggle blocks. Red = ignored by motion detection.',
            'zones.roi.active': 'Active blocks',
            'zones.roi.all': 'Select all',
            'zones.roi.none': 'Clear all',
            'zones.roi.invert': 'Invert',
            'zones.roi.save': 'Save mask',
            'zones.roi.reset': 'Whole frame (no mask)',
            'zones.roi.saved': 'ROI mask saved',
            'zones.roi.cleared': 'ROI mask cleared, the whole frame is active',
            'zones.roi.loadfailed': 'Could not load the ROI mask',
            'zones.roi.savefailed': 'Saving the ROI mask failed',
            'zones.roi.unsaved': 'Unsaved changes',
            'zones.zone.hint': 'Drag over the image to draw a rectangle for the zone being edited.',
            'zones.zone.list': 'Saved zones',
            'zones.zone.empty': 'No zones defined yet',
            'zones.zone.new': 'New zone',
            'zones.zone.editing': 'Zone being edited',
            'zones.zone.edit': 'Edit',
            'zones.zone.name': 'Zone name (max 23 characters)',
            'zones.zone.color': 'Colour',
            'zones.zone.alert': 'Trigger an alert (event / Telegram)',
            'zones.zone.rects': 'Rectangles',
            'zones.zone.rects.hint': 'Up to 4 rectangles per zone, up to 8 zones per device.',
            'zones.zone.rects.empty': 'No rectangle yet, draw one on the image',
            'zones.zone.add_rect': 'Add rectangle',
            'zones.zone.remove_rect': 'Remove rectangle',
            'zones.zone.save': 'Save zone',
            'zones.zone.cancel': 'Cancel editing',
            'zones.zone.saved': 'Zone saved',
            'zones.zone.deleted': 'Zone deleted',
            'zones.zone.delete.confirm': 'Delete zone',
            'zones.zone.name.required': 'Enter a zone name',
            'zones.zone.rect.required': 'Draw at least one rectangle',
            'zones.zone.limit': 'Maximum of 8 zones reached',
            'zones.zone.rectlimit': 'Maximum of 4 rectangles per zone',
            'zones.zone.loadfailed': 'Could not load zones',
            'zones.zone.savefailed': 'Saving the zone failed',
            'zones.zone.deletefailed': 'Deleting the zone failed',

            // Gallery paging / thumbnails
            'gallery.thumbs': 'Show thumbnails',
            'gallery.thumbs.hint': 'The device cannot generate thumbnails, so every preview is the full JPEG. They load only once scrolled into view.',
            'gallery.page': 'Page',
            'gallery.prev': 'Previous',
            'gallery.next': 'Next',
            'gallery.items': 'items',
            'gallery.preview.title': 'Image preview',
        },

        cs: {
            'nav.overview': 'Přehled',
            'nav.settings': 'Nastavení',
            'nav.zones': 'Zóny',
            'nav.wifi': 'Wi-Fi',
            'nav.admin': 'Admin',
            'nav.gallery': 'Galerie',
            'nav.back': 'Zpět',

            'common.save': 'Uložit nastavení',
            'common.cancel': 'Zrušit',
            'common.confirm': 'Potvrdit',
            'common.close': 'Zavřít',
            'common.delete': 'Smazat',
            'common.download': 'Stáhnout',
            'common.connect': 'Připojit',
            'common.scan': 'Vyhledat',
            'common.loading': 'Načítání...',
            'common.connecting': 'Připojování...',
            'common.error': 'Chyba',
            'state.on': 'ZAP',
            'state.off': 'VYP',
            'state.detected': 'DETEKOVÁNO',
            'state.idle': 'Klid',
            'state.none': 'Žádné',
            'state.connected': 'Připojeno',
            'state.disconnected': 'Odpojeno',
            'aria.nav': 'Hlavní navigace',
            'aria.path': 'Cesta k aktuální složce',
            'aria.grid': 'Mřížka detekce pohybu',
            'index.stream.alt': 'Živý přenos z kamery',
            'index.stream.error': 'Stream není dostupný (příliš mnoho klientů?)',
            'common.online': 'Online',
            'common.offline': 'Odpojeno',
            'common.language': 'Jazyk',

            'index.title': 'Kamera CamS3 5MP',
            'index.stream.start': 'Spustit stream',
            'index.stream.stop': 'Zastavit stream',
            'index.stream.placeholder': 'Kliknutím spustíte stream',
            'index.snapshot': 'Snímek',
            'index.info.ip': 'IP adresa',
            'index.info.uptime': 'Doba běhu',
            'index.info.heap': 'Volná paměť (heap)',
            'index.info.psram': 'Volná PSRAM',
            'index.info.fps': 'Snímků/s',
            'index.info.rssi': 'Wi-Fi RSSI',
            'index.info.version': 'Firmware',
            'index.info.clients': 'Klientů streamu',
            'index.info.temp': 'Teplota',
            'index.info.profile': 'Profil',
            'index.info.sd': 'SD karta',
            'index.profile.day': 'DEN',
            'index.profile.dusk': 'ŠERO',
            'index.profile.night': 'NOC',
            'index.tools.status': 'Stav (JSON)',
            'index.tools.health': 'Zdraví',
            'index.tools.events': 'Události',
            'index.tools.telemetry': 'Telemetrie',
            'index.tools.stream_stats': 'Statistiky streamu',
            'index.tools.psram_stats': 'Statistiky PSRAM',
            'index.tools.sd_files': 'Soubory na SD',
            'index.banner.defaultpw': 'Pozor: zařízení stále používá výchozí heslo',
            'index.banner.defaultpw.action': 'Změňte ho v Nastavení.',
            'index.tools.log': 'Živý log',
            'index.tools.motion_debug': 'Diagnostika pohybu',

            'settings.title': 'Nastavení kamery',
            'settings.basic': 'Základní nastavení',
            'settings.image': 'Obraz',
            'settings.exposure': 'Expozice a zesílení',
            'settings.whitebalance': 'Vyvážení bílé',
            'settings.motion': 'Detekce pohybu',
            'settings.face': 'Detekce obličejů',
            'settings.person': 'Detekce osob',
            'settings.timelapse': 'Časosběr',
            'settings.telegram': 'Telegram',
            'settings.mqtt': 'MQTT',
            'settings.security': 'Zabezpečení',
            'settings.perf': 'Výkon',

            'settings.jpeg_quality': 'Kvalita JPEG (1–63, nižší = lepší)',
            'settings.frame_size': 'Rozlišení',
            'settings.vflip': 'Překlopit svisle (V-Flip)',
            'settings.hmirror': 'Zrcadlit vodorovně (H-Mirror)',
            'settings.brightness': 'Jas (-2 až 2)',
            'settings.contrast': 'Kontrast (-2 až 2)',
            'settings.saturation': 'Saturace (-2 až 2)',
            'settings.sharpness': 'Ostrost (-3 až 3)',
            'settings.denoise': 'Potlačení šumu (0–8)',
            'settings.security.user': 'Uživatelské jméno správce (admin UI / OTA / WebSocket)',
            'settings.security.pass': 'Nové heslo (min. 4 znaky)',
            'settings.security.pass.hint': 'Po změně budete při dalším přihlášení potřebovat nové heslo.',
            'settings.security.pass.placeholder': 'Prázdné = ponechat stávající',
            'settings.perf.active_fps': 'FPS při streamování',
            'settings.perf.idle_fps': 'FPS v klidovém režimu',
            'settings.perf.idle_fps.hint': 'Nižší = menší spotřeba, pomalejší detekce',

            'settings.aec': 'Automatická expozice (AEC)',
            'settings.ae_level': 'Úroveň expozice (-2 až 2)',
            'settings.ae_level.hint': '-2 = tmavší, +2 = světlejší',
            'settings.manual_exposure': 'Ruční expozice (0–1200)',
            'settings.agc': 'Automatické zesílení (AGC)',
            'settings.manual_gain': 'Ruční zesílení (0–30)',
            'settings.gainceiling': 'Strop zesílení',
            'settings.awb': 'Automatické vyvážení bílé',
            'settings.wb_mode': 'Režim vyvážení bílé',
            'settings.sensor_corrections': 'Korekce senzoru',
            'settings.bpc': 'Korekce černých pixelů (BPC)',
            'settings.wpc': 'Korekce bílých pixelů (WPC)',
            'settings.gamma': 'Korekce gama',
            'settings.lens': 'Korekce objektivu',
            'settings.motion.enabled': 'Detekce pohybu zapnuta',
            'settings.motion.sensitivity': 'Práh pixelu (5–80, nižší = citlivější)',
            'settings.motion.min_area': 'Min. plocha pro spuštění (%)',
            'settings.person.enabled': 'Detekce osob zapnuta',
            'settings.person.confidence': 'Práh UNCERTAIN (%) – ověření přes MQTT',
            'settings.person.confident': 'Práh CONFIDENT (%) – přímé upozornění',
            'settings.person.temporal': 'Počet po sobě jdoucích snímků',
            'settings.person.cooldown': 'Prodleva mezi detekcemi (s)',
            'settings.person.tg_notify': 'Upozornění přes Telegram',
            'settings.person.tg_photo': 'Foto přes Telegram',
            'settings.person.cascade_hint': 'Detekce osob se spustí až po detekci pohybu (kaskáda).',
            'settings.mqtt.user': 'Uživatelské jméno (prázdné = ponechat)',
            'settings.mqtt.pass': 'Heslo (prázdné = ponechat)',
            'settings.mqtt.tls': 'TLS (pro ověření je potřeba /ca.pem)',
            'settings.stored': 'uloženo v zařízení',
            'settings.notloaded': 'Aktuální nastavení ještě nejsou načtena – obnovte stránku',
            'settings.loadfailed': 'Aktuální nastavení se nepodařilo načíst',
            'settings.motion.cooldown': 'Prodleva mezi detekcemi (s)',
            'settings.motion.max_area': 'Max. plocha (%) – odfiltruje změny osvětlení',
            'settings.motion.temporal': 'Časový filtr (2 a více snímků)',
            'settings.motion.spatial': 'Prostorový filtr (odfiltruje izolované bloky)',
            'settings.motion.night': 'Noční potlačení šumu',
            'settings.save_sd': 'Ukládat na SD kartu',
            'settings.face.enabled': 'Detekce obličejů zapnuta',
            'settings.face.two_stage': 'Dvoufázová detekce (přesnější)',
            'settings.face.cooldown': 'Prodleva mezi detekcemi (s)',
            'settings.face.save_sd': 'Ukládat obličeje na SD',
            'settings.face.cascade_hint': 'Detekce obličejů se spustí až po detekci pohybu (kaskáda).',
            'settings.timelapse.enabled': 'Časosběr zapnut',
            'settings.timelapse.interval': 'Interval (sekundy)',
            'settings.timelapse.interval_hint': '60 s = každou minutu, 3600 s = každou hodinu',
            'settings.telegram.enabled': 'Telegram zapnut',
            'settings.telegram.bot_token': 'Token bota',
            'settings.telegram.bot_token_hint': 'Získáte od @BotFather na Telegramu',
            'settings.telegram.chat_id': 'Chat ID',
            'settings.telegram.chat_id_hint': 'Získáte od @userinfobot',
            'settings.telegram.notify_motion': 'Upozornit na pohyb',
            'settings.telegram.photo_motion': 'Foto při pohybu',
            'settings.telegram.notify_face': 'Upozornit na obličej',
            'settings.telegram.photo_face': 'Foto při detekci obličeje',
            'settings.telegram.cooldown': 'Prodleva (5–300 s)',
            'settings.telegram.active_start': 'Aktivní hodiny – začátek (0–23)',
            'settings.telegram.active_end': 'Aktivní hodiny – konec (0–23)',
            'settings.telegram.active_hint': 'Podporuje přechod přes půlnoc (např. 22–7)',
            'settings.mqtt.enabled': 'MQTT zapnuto',
            'settings.mqtt.server': 'MQTT server',
            'settings.mqtt.port': 'Port',
            'settings.mqtt.topic_prefix': 'Prefix topicu',
            'settings.wb.auto': 'Auto',
            'settings.wb.sunny': 'Slunečno',
            'settings.wb.cloudy': 'Zataženo',
            'settings.wb.office': 'Kancelář',
            'settings.wb.home': 'Domov',

            'wifi.title': 'Konfigurace Wi-Fi',
            'wifi.current': 'Aktuální připojení',
            'wifi.ip': 'IP',
            'wifi.rssi': 'RSSI',
            'wifi.mac': 'MAC',
            'wifi.hostname': 'Hostname',
            'wifi.scan.title': 'Dostupné sítě',
            'wifi.scan.hint': 'Sítě načtete kliknutím na „Vyhledat“',
            'wifi.scan.searching': 'Vyhledávání...',
            'wifi.scan.none': 'Nenalezeny žádné sítě',
            'wifi.scan.error': 'Chyba při vyhledávání',
            'wifi.manual.title': 'Ruční připojení',
            'wifi.manual.ssid': 'SSID',
            'wifi.manual.ssid.ph': 'Název sítě',
            'wifi.manual.pass': 'Heslo',
            'wifi.manual.pass.ph': 'Heslo sítě',
            'wifi.manual.ssid.required': 'Zadejte SSID',
            'wifi.connecting.to': 'Připojování k',
            'wifi.connecting.note': 'Kamera se připojuje, tato stránka může přestat být dostupná...',
            'wifi.strength.excellent': 'Výborný',
            'wifi.strength.good': 'Dobrý',
            'wifi.strength.weak': 'Slabý',

            'gallery.title': 'Galerie SD karty',
            'gallery.loading': 'Načítání...',
            'gallery.empty': 'Prázdná složka',
            'gallery.unavailable': 'SD karta není dostupná',
            'gallery.folder': 'složka',
            'gallery.delete.confirm': 'Smazat soubor',
            'gallery.delete.ok': 'Soubor smazán',
            'gallery.delete.err': 'Chyba při mazání',

            'admin.title': 'Administrace',
            'admin.sys': 'Systémové informace',
            'admin.camera': 'Kamera',
            'admin.sd': 'SD karta',
            'admin.ota': 'Aktualizace OTA',
            'admin.actions': 'Systémové akce',
            'admin.api': 'Endpointy API',
            'admin.cam.count': 'Celkem snímků',
            'admin.cam.errors': 'Chyby',
            'admin.cam.clients': 'Klientů streamu',
            'admin.cam.reinit': 'Reinicializovat kameru',
            'admin.sd.status': 'Stav',
            'admin.sd.total': 'Celkem',
            'admin.sd.used': 'Použito',
            'admin.sd.mounted': 'Připojena',
            'admin.sd.missing': 'Nepřipojena',
            'admin.ota.hint': 'Aktualizace firmwaru přes webové rozhraní. Nahrajte .bin s firmwarem nebo .bin se souborovým systémem (LittleFS).',
            'admin.ota.open': 'Otevřít stránku OTA',
            'admin.action.reboot': 'Restartovat zařízení',
            'admin.action.reset': 'Tovární reset',
            'admin.action.reset.hint': 'Tovární reset smaže veškerou konfiguraci a restartuje zařízení do režimu AP.',
            'admin.psram.total': 'PSRAM celkem',
            'admin.confirm.generic': 'Potvrzení',
            'admin.confirm.reboot.title': 'Restart zařízení',
            'admin.confirm.reboot.text': 'Opravdu restartovat kameru? Stream bude přerušen.',
            'admin.confirm.reset.title': 'Tovární reset',
            'admin.confirm.reset.text': 'POZOR: veškerá konfigurace bude smazána. Zařízení se restartuje do režimu AP. Pokračovat?',
            'admin.rebooting': 'Restartování...',
            'admin.resetting': 'Resetování...',

            'zones.title': 'Detekční zóny a ROI',
            'zones.mode': 'Režim editoru',
            'zones.tab.roi': 'Maska ROI',
            'zones.tab.zones': 'Zóny',
            'zones.preview.refresh': 'Obnovit snímek',
            'zones.preview.live': 'Živý stream',
            'zones.preview.failed': 'Náhled není dostupný',
            'zones.grid.label': 'Detekční mřížka',
            'zones.roi.hint': 'Kliknutím nebo tažením po obrázku přepnete bloky. Červená = ignorováno detekcí pohybu.',
            'zones.roi.active': 'Aktivní bloky',
            'zones.roi.all': 'Vybrat vše',
            'zones.roi.none': 'Zrušit výběr',
            'zones.roi.invert': 'Invertovat',
            'zones.roi.save': 'Uložit masku',
            'zones.roi.reset': 'Celý obraz (bez masky)',
            'zones.roi.saved': 'Maska ROI uložena',
            'zones.roi.cleared': 'Maska ROI zrušena, aktivní je celý obraz',
            'zones.roi.loadfailed': 'Masku ROI se nepodařilo načíst',
            'zones.roi.savefailed': 'Masku ROI se nepodařilo uložit',
            'zones.roi.unsaved': 'Neuložené změny',
            'zones.zone.hint': 'Tažením po obrázku nakreslete obdélník pro upravovanou zónu.',
            'zones.zone.list': 'Uložené zóny',
            'zones.zone.empty': 'Zatím nejsou definovány žádné zóny',
            'zones.zone.new': 'Nová zóna',
            'zones.zone.editing': 'Upravovaná zóna',
            'zones.zone.edit': 'Upravit',
            'zones.zone.name': 'Název zóny (max. 23 znaků)',
            'zones.zone.color': 'Barva',
            'zones.zone.alert': 'Vyvolat upozornění (událost / Telegram)',
            'zones.zone.rects': 'Obdélníky',
            'zones.zone.rects.hint': 'Nejvýše 4 obdélníky na zónu, nejvýše 8 zón na zařízení.',
            'zones.zone.rects.empty': 'Zatím žádný obdélník, nakreslete ho v obrázku',
            'zones.zone.add_rect': 'Přidat obdélník',
            'zones.zone.remove_rect': 'Odebrat obdélník',
            'zones.zone.save': 'Uložit zónu',
            'zones.zone.cancel': 'Zrušit úpravy',
            'zones.zone.saved': 'Zóna uložena',
            'zones.zone.deleted': 'Zóna smazána',
            'zones.zone.delete.confirm': 'Smazat zónu',
            'zones.zone.name.required': 'Zadejte název zóny',
            'zones.zone.rect.required': 'Nakreslete alespoň jeden obdélník',
            'zones.zone.limit': 'Dosažen maximální počet 8 zón',
            'zones.zone.rectlimit': 'Maximálně 4 obdélníky na zónu',
            'zones.zone.loadfailed': 'Zóny se nepodařilo načíst',
            'zones.zone.savefailed': 'Zónu se nepodařilo uložit',
            'zones.zone.deletefailed': 'Zónu se nepodařilo smazat',

            'gallery.thumbs': 'Zobrazovat miniatury',
            'gallery.thumbs.hint': 'Zařízení neumí generovat miniatury, každý náhled je proto JPEG v plné velikosti. Načítají se teprve při zobrazení.',
            'gallery.page': 'Stránka',
            'gallery.prev': 'Předchozí',
            'gallery.next': 'Další',
            'gallery.items': 'položek',
            'gallery.preview.title': 'Náhled obrázku',
        },
    };

    const SUPPORTED = Object.keys(T);

    function detectLang() {
        const stored = localStorage.getItem('cams3_lang');
        if (stored && SUPPORTED.indexOf(stored) >= 0) return stored;
        const nav = (navigator.language || 'en').toLowerCase();
        if (nav.indexOf('cs') === 0 || nav.indexOf('sk') === 0) return 'cs';
        return 'en';
    }

    function t(key, lang) {
        lang = lang || window.I18N.lang;
        const table = T[lang] || T.en;
        return table[key];
    }

    function apply(root) {
        root = root || document;
        const lang = window.I18N.lang;
        root.querySelectorAll('[data-i18n]').forEach(el => {
            const key = el.getAttribute('data-i18n');
            const val = t(key, lang);
            if (val !== undefined) el.textContent = val;
        });
        root.querySelectorAll('[data-i18n-placeholder]').forEach(el => {
            const key = el.getAttribute('data-i18n-placeholder');
            const val = t(key, lang);
            if (val !== undefined) el.setAttribute('placeholder', val);
        });
        root.querySelectorAll('[data-i18n-title]').forEach(el => {
            const key = el.getAttribute('data-i18n-title');
            const val = t(key, lang);
            if (val !== undefined) el.setAttribute('title', val);
        });
        // aria-label and alt are user-facing text too — a screen reader user was
        // getting English landmark names and image descriptions regardless of the
        // selected language, because only textContent/placeholder/title were handled.
        root.querySelectorAll('[data-i18n-aria-label]').forEach(el => {
            const key = el.getAttribute('data-i18n-aria-label');
            const val = t(key, lang);
            if (val !== undefined) el.setAttribute('aria-label', val);
        });
        root.querySelectorAll('[data-i18n-alt]').forEach(el => {
            const key = el.getAttribute('data-i18n-alt');
            const val = t(key, lang);
            if (val !== undefined) el.setAttribute('alt', val);
        });
        document.documentElement.lang = lang;
    }

    function setLang(lang) {
        if (SUPPORTED.indexOf(lang) < 0) return;
        window.I18N.lang = lang;
        localStorage.setItem('cams3_lang', lang);
        apply();
        refreshSelector();
        // Notify listeners (dynamic content may need re-render)
        window.dispatchEvent(new CustomEvent('i18n:change', { detail: { lang } }));
    }

    // Flag glyphs via regional indicator symbols (renders as flag on most platforms).
    const FLAGS = { en: '\uD83C\uDDEC\uD83C\uDDE7', cs: '\uD83C\uDDE8\uD83C\uDDFF' };

    function injectSelector() {
        const host = document.querySelector('.nav-tabs');
        if (!host || document.getElementById('lang-toggle')) return;

        const group = document.createElement('div');
        group.id = 'lang-toggle';
        group.setAttribute('role', 'radiogroup');
        group.setAttribute('aria-label', 'Language / Jazyk');
        group.style.cssText =
            'margin-left:auto;display:inline-flex;gap:4px;align-self:center;' +
            'padding:3px;background:rgba(255,255,255,.04);' +
            'border:1px solid var(--border,#2a2a3e);border-radius:999px';

        SUPPORTED.forEach(l => {
            const btn = document.createElement('button');
            btn.type = 'button';
            btn.dataset.lang = l;
            btn.setAttribute('role', 'radio');
            btn.title = l === 'en' ? 'English' : (l === 'cs' ? 'Čeština' : l);
            btn.innerHTML = '<span style="font-size:1.05em;line-height:1">' +
                (FLAGS[l] || '') + '</span>&nbsp;' +
                '<span style="letter-spacing:.04em;font-weight:600">' + l.toUpperCase() + '</span>';
            const active = l === window.I18N.lang;
            btn.setAttribute('aria-checked', active);
            btn.style.cssText =
                'display:inline-flex;align-items:center;gap:2px;' +
                'padding:5px 10px;font-size:.8em;cursor:pointer;' +
                'border:0;border-radius:999px;transition:all .15s ease;' +
                'background:' + (active ? 'var(--accent,#6366f1)' : 'transparent') + ';' +
                'color:' + (active ? '#fff' : 'var(--text,#cbd5e1)') + ';';
            btn.addEventListener('click', () => setLang(l));
            btn.addEventListener('mouseenter', () => {
                if (btn.dataset.lang !== window.I18N.lang) {
                    btn.style.background = 'rgba(255,255,255,.08)';
                }
            });
            btn.addEventListener('mouseleave', () => {
                if (btn.dataset.lang !== window.I18N.lang) btn.style.background = 'transparent';
            });
            group.appendChild(btn);
        });
        host.appendChild(group);
    }

    function refreshSelector() {
        const group = document.getElementById('lang-toggle');
        if (!group) return;
        group.querySelectorAll('button').forEach(btn => {
            const active = btn.dataset.lang === window.I18N.lang;
            btn.setAttribute('aria-checked', active);
            btn.style.background = active ? 'var(--accent,#6366f1)' : 'transparent';
            btn.style.color      = active ? '#fff' : 'var(--text,#cbd5e1)';
        });
    }

    window.I18N = {
        lang: detectLang(),
        t: t,
        apply: apply,
        setLang: setLang,
        supported: SUPPORTED,
    };

    document.addEventListener('DOMContentLoaded', () => {
        apply();
        injectSelector();
    });
})();
