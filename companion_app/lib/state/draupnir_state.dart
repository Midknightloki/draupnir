import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

class DraupnirState extends ChangeNotifier {
  bool isLoading = false;
  String? error;

  Map<String, dynamic>? profilesData;
  int activeProfileIdx = 0;
  bool isEditorMode = false;

  // Set when the device refuses a request because the BLE link is not paired/bonded. Pairing is
  // now the operating system's job — standard BLE passkey pairing, with the PIN shown on the
  // knob's screen — so there is deliberately no in-app "Pair" action to go with this. The app's
  // only job is to tell the user where to go. The old application-layer scheme (a `pair`
  // command, a `token` field on every request, an `authToken` persisted in SharedPreferences)
  // is removed: the firmware ignored the token entirely and never implemented `pair`, so it was
  // dead code that always reported failure, and it reimplemented — badly — what BLE bonding
  // already does correctly.
  bool needsPairing = false;

  // Set when the device answered but refused because it is not in Config Mode. Only the M5Dial
  // has this gate: it serves no config command outside CONFIG_MODE, which is entered by a
  // physical gesture on the dial (swipe down). The link is up and healthy in this case — the
  // request was answered, just with a refusal — so reporting it as a connection failure, which
  // is what the generic error path did, sent the user off diagnosing Bluetooth instead of
  // swiping the screen in front of them. Like needsPairing, there is no in-app action that can
  // fix it; the app's job is to say which gesture to make.
  bool needsConfigMode = false;

  // True once connected over BLE. BLE is now the only transport (the Wi-Fi/HTTP client half of
  // the cut web UI is gone), so this doubles as "connected".
  bool isBluetooth = false;
  BluetoothDevice? connectedDevice;
  BluetoothCharacteristic? rxChar;
  BluetoothCharacteristic? txChar;
  bool isScanningBle = false;
  Completer<Map<String, dynamic>>? _bleResponseCompleter;
  String _bleBuffer = '';
  int? _lastProcessedSeq;

  // Debug log — ring buffer, newest last
  final List<String> debugLog = [];
  static const int _maxLogLines = 200;

  void _log(String msg) {
    final ts = DateTime.now();
    final line = '[${ts.hour.toString().padLeft(2,'0')}:${ts.minute.toString().padLeft(2,'0')}:${ts.second.toString().padLeft(2,'0')}.${(ts.millisecond ~/ 10).toString().padLeft(2,'0')}] $msg';
    debugPrint(line);
    debugLog.add(line);
    if (debugLog.length > _maxLogLines) debugLog.removeAt(0);
    notifyListeners();
  }

  void clearDebugLog() {
    debugLog.clear();
    notifyListeners();
  }

  void toggleEditorMode() {
    isEditorMode = !isEditorMode;
    notifyListeners();
  }

  void setActiveProfile(int idx) {
    if (profilesData != null && profilesData!['profiles'] != null) {
      if (idx >= 0 && idx < (profilesData!['profiles'] as List).length) {
        activeProfileIdx = idx;
        notifyListeners();
      }
    }
  }

  Future<void> addProfile(String name) async {
    if (profilesData == null) return;
    List profiles = profilesData!['profiles'] as List;
    profiles.add({
      'name': name,
      'macros': [],
    });
    activeProfileIdx = profiles.length - 1;
    await saveProfiles();
  }

  Future<void> renameProfile(String name) async {
    if (profilesData == null) return;
    List profiles = profilesData!['profiles'] as List;
    if (activeProfileIdx < profiles.length) {
      profiles[activeProfileIdx]['name'] = name;
      await saveProfiles();
    }
  }

  Future<void> setProfileColor(String colorHex) async {
    if (profilesData == null) return;
    List profiles = profilesData!['profiles'] as List;
    if (activeProfileIdx < profiles.length) {
      profiles[activeProfileIdx]['color'] = colorHex;
      await saveProfiles();
    }
  }

  Future<void> deleteProfile() async {
    if (profilesData == null) return;
    List profiles = profilesData!['profiles'] as List;
    if (profiles.length > 1 && activeProfileIdx < profiles.length) {
      profiles.removeAt(activeProfileIdx);
      if (activeProfileIdx >= profiles.length) {
        activeProfileIdx = profiles.length - 1;
      }
      await saveProfiles();
    }
  }

  static const int _bleChunkAckMarker = 0xFE;

  void _onBleDataReceived(List<int> value) async {
    if (value.isEmpty) return;
    final seq = value[0];
    final payload = value.sublist(1);

    final preview = payload.length <= 40
        ? utf8.decode(payload, allowMalformed: true)
        : '${utf8.decode(payload.sublist(0, 40), allowMalformed: true)}…';
    _log('[RX] seq=$seq ${payload.length}B: ${preview.replaceAll('\n', '\\n')}');

    // notify() has no delivery guarantee, so the firmware resends a chunk when its ack doesn't
    // arrive in time. If the original actually got here and only the ack was slow, the resend
    // carries the same seq — skip re-appending it so the buffer doesn't get corrupted with
    // duplicate bytes, but still ack it below so the firmware unblocks.
    if (seq != _lastProcessedSeq) {
      _lastProcessedSeq = seq;
      String text = utf8.decode(payload, allowMalformed: true);
      for (int i = 0; i < text.length; i++) {
        String char = text[i];
        if (char == '\n') {
          final responseStr = _bleBuffer.trim();
          _bleBuffer = '';
          _log('[RX] \\n found — buf=${responseStr.length}B: ${responseStr.length > 60 ? '${responseStr.substring(0, 60)}…' : responseStr}');
          if (responseStr.isEmpty) continue;
          if (_bleResponseCompleter != null && !_bleResponseCompleter!.isCompleted) {
            try {
              final responseJson = jsonDecode(responseStr) as Map<String, dynamic>;
              _log('[RX] Parse OK: status=${responseJson['status']}');
              _bleResponseCompleter!.complete(responseJson);
            } catch (e) {
              _log('[RX] Parse ERROR: $e');
              _bleResponseCompleter!.completeError(e);
            }
          } else {
            _log('[RX] No active completer (isCompleted=${_bleResponseCompleter?.isCompleted})');
          }
        } else {
          _bleBuffer += char;
        }
      }
    } else {
      _log('[RX] duplicate seq=$seq, skipping re-append');
    }

    if (rxChar == null) {
      _log('[ACK] rxChar is null, cannot send ack');
      return;
    }
    try {
      await rxChar!.write([_bleChunkAckMarker, seq], withoutResponse: true);
      _log('[ACK] sent seq=$seq');
    } catch (e) {
      _log('[ACK] write failed: $e');
    }
  }

  Future<Map<String, dynamic>> _sendBleRequest(Map<String, dynamic> request) async {
    if (rxChar == null) {
      throw Exception('Not connected to BLE device');
    }

    // Retire any completer still outstanding from a previous request before installing a new one.
    //
    // Dart's Future.timeout() does NOT complete the underlying Completer -- it only rejects the
    // derived future. So after a request times out, its Completer stays open and eligible to
    // receive data. Without this, a late response for the timed-out request would satisfy the
    // NEXT request's completer with the wrong payload: a slow get_profiles landing during a
    // subsequent save_profiles reports {"status":"ok"} and the app claims the save succeeded when
    // it has no idea whether it did. Silently-wrong success is the worst failure mode for a
    // config app, so fail loudly instead.
    final stale = _bleResponseCompleter;
    if (stale != null && !stale.isCompleted) {
      _log('[TX] superseding an outstanding request (previous one never completed)');
      // Attach a swallowing handler BEFORE completing it. The original caller stopped listening
      // when its .timeout() wrapper threw, so completing the source with an error would otherwise
      // surface as an unhandled async error and could crash the zone in release builds.
      stale.future.catchError((_) => <String, dynamic>{});
      stale.completeError(StateError('Request superseded by a newer one'));
    }

    _bleResponseCompleter = Completer<Map<String, dynamic>>();
    _bleBuffer = '';
    _lastProcessedSeq = null;

    String jsonStr = '${jsonEncode(request)}\n';
    List<int> bytes = utf8.encode(jsonStr);
    _log('[TX] cmd=${request['cmd']} total=${bytes.length}B');

    int offset = 0;
    int chunkNum = 0;
    while (offset < bytes.length) {
      int chunkSize = (bytes.length - offset) < 180 ? (bytes.length - offset) : 180;
      List<int> chunk = bytes.sublist(offset, offset + chunkSize);
      _log('[TX] chunk$chunkNum: ${chunkSize}B offset=$offset');
      await rxChar!.write(chunk, withoutResponse: false);
      offset += chunkSize;
      chunkNum++;
      await Future.delayed(const Duration(milliseconds: 20));
    }
    _log('[TX] all chunks sent, awaiting response (30s timeout)');

    return _bleResponseCompleter!.future.timeout(
      const Duration(seconds: 30),
      onTimeout: () {
        _log('[ERR] Timeout — no response after 30s. Buffer was: "${_bleBuffer.length > 80 ? _bleBuffer.substring(0, 80) : _bleBuffer}"');
        throw TimeoutException('BLE response timeout', const Duration(seconds: 30));
      },
    );
  }

  // Nordic UART Service — the one Draupnir speaks. Matched as a fallback for boards whose
  // advertised name does not survive the scan (Android sometimes reports an empty platformName
  // until a name request completes).
  static final Guid nusServiceUuid = Guid('6E400001-B5A3-F393-E0A9-E50E24DCCA9E');

  // The boards advertise different names now ("Draupnir" = Waveshare knob, "Draupnir_Mini" =
  // M5Dial), but both match the scan filter — as any future board should. Whoever answers the
  // scan first is not necessarily the one the user meant, so instead of taking the first hit we
  // keep listening for a grace period after it to see whether a second board is on the air. One
  // match connects straight through; more than one asks. The grace window is what keeps the
  // common single-board case from paying the full 10s scan.
  static const Duration _scanGrace = Duration(seconds: 2);

  /// [chooseDevice] is called only when the scan turns up more than one Draupnir. Returning null
  /// (the user dismissed the picker) cancels the connect quietly — not an error. Omitting it
  /// falls back to the strongest signal, so callers without a UI to show still work.
  Future<void> connectBluetooth({
    Future<BluetoothDevice?> Function(List<ScanResult>)? chooseDevice,
  }) async {
    isLoading = true;
    isScanningBle = true;
    error = null;
    needsConfigMode = false;
    clearDebugLog();
    notifyListeners();

    try {
      if (await FlutterBluePlus.isSupported == false) {
        throw Exception('Bluetooth not supported on this device');
      }

      if (Platform.isAndroid) {
        Map<Permission, PermissionStatus> statuses = await [
          Permission.bluetoothScan,
          Permission.bluetoothConnect,
          Permission.location,
        ].request();

        if (statuses[Permission.bluetoothScan]?.isDenied == true ||
            statuses[Permission.bluetoothConnect]?.isDenied == true ||
            statuses[Permission.location]?.isDenied == true) {
          throw Exception('Permissions denied. Enable Bluetooth and Location in Settings.');
        }
      }

      _log('[SCAN] Starting (10s timeout)');
      final List<String> foundNames = [];
      // Keyed by remoteId so a board re-advertising during the scan updates its entry instead of
      // appearing twice in the picker.
      final Map<DeviceIdentifier, ScanResult> matches = {};
      final settled = Completer<void>();
      Timer? graceTimer;

      final subscription = FlutterBluePlus.scanResults.listen((results) {
        for (final r in results) {
          final name = r.device.platformName;
          if (name.isNotEmpty && !foundNames.contains(name)) {
            foundNames.add(name);
            _log('[SCAN] Found: "$name" rssi=${r.rssi}');
          }
          if (!name.toLowerCase().contains('draupnir') &&
              !r.advertisementData.serviceUuids.contains(nusServiceUuid)) {
            continue;
          }
          final isNew = !matches.containsKey(r.device.remoteId);
          matches[r.device.remoteId] = r;
          if (!isNew) continue;
          _log('[SCAN] Draupnir: "$name" (${r.device.remoteId}) rssi=${r.rssi}');
          // Restart the grace window on every NEW board, so a third one still gets counted.
          graceTimer?.cancel();
          graceTimer = Timer(_scanGrace, () {
            if (!settled.isCompleted) settled.complete();
          });
        }
      });

      // startScan's future resolves as soon as the platform scan call is issued, not when
      // scanning actually finishes — awaiting it alone races the results stream and only "works"
      // when the OS scan cache returns a hit instantly. Wait for the grace window to close after
      // a match, or for scanning to actually stop (10s timeout elapsed, or manually stopped).
      await FlutterBluePlus.startScan(timeout: const Duration(seconds: 10));
      await Future.any([
        settled.future,
        FlutterBluePlus.isScanning.where((scanning) => !scanning).first,
      ]);
      graceTimer?.cancel();
      await subscription.cancel();
      await FlutterBluePlus.stopScan();
      isScanningBle = false;
      notifyListeners();

      if (matches.isEmpty) {
        final listStr = foundNames.isEmpty ? 'none' : foundNames.take(8).join(', ');
        _log('[ERR] No Draupnir found. Seen: $listStr');
        throw Exception('No Draupnir device found. Nearby: $listStr');
      }

      // Strongest signal first: it is the best guess when nobody is choosing, and the most
      // useful order to show a human who is.
      final candidates = matches.values.toList()
        ..sort((a, b) => b.rssi.compareTo(a.rssi));

      BluetoothDevice? targetDevice;
      if (candidates.length == 1 || chooseDevice == null) {
        targetDevice = candidates.first.device;
        if (candidates.length > 1) {
          _log('[SCAN] ${candidates.length} Draupnirs, no picker — taking strongest signal');
        }
      } else {
        _log('[SCAN] ${candidates.length} Draupnirs found — asking');
        targetDevice = await chooseDevice(candidates);
        if (targetDevice == null) {
          _log('[SCAN] Cancelled at the picker');
          return;
        }
      }

      _log('[CONN] Connecting to "${targetDevice.platformName}" (${targetDevice.remoteId})…');
      await targetDevice.connect(license: License.nonprofit);
      connectedDevice = targetDevice;
      isBluetooth = true;
      _log('[CONN] Connected');

      // Explicit MTU negotiation so we know what we got
      try {
        final mtu = await connectedDevice!.requestMtu(512);
        _log('[MTU] Negotiated: ${mtu}B (payload=${mtu - 3}B)');
      } catch (e) {
        _log('[MTU] requestMtu failed: $e');
      }

      _log('[SVC] Discovering services…');
      List<BluetoothService> services = await targetDevice.discoverServices();
      _log('[SVC] Found ${services.length} services');

      BluetoothService? uartService;
      for (var s in services) {
        _log('[SVC] Service: ${s.uuid}');
        if (s.uuid == nusServiceUuid) {
          uartService = s;
        }
      }

      if (uartService == null) {
        _log('[ERR] NUS service not found');
        throw Exception('Nordic UART Service not found on device');
      }
      _log('[SVC] NUS found (${uartService.characteristics.length} chars)');

      for (var c in uartService.characteristics) {
        _log('[SVC] Char: ${c.uuid} props=${c.properties}');
        if (c.uuid == Guid('6E400002-B5A3-F393-E0A9-E50E24DCCA9E')) {
          rxChar = c;
          _log('[SVC] → RX char');
        } else if (c.uuid == Guid('6E400003-B5A3-F393-E0A9-E50E24DCCA9E')) {
          txChar = c;
          _log('[SVC] → TX char');
        }
      }

      if (rxChar == null || txChar == null) {
        _log('[ERR] Missing chars: rx=${rxChar != null} tx=${txChar != null}');
        throw Exception('UART characteristics not found');
      }

      // Attach the listener before enabling notifications (FBP-recommended order) — otherwise
      // a notification arriving in the gap between the CCCD write completing and the listener
      // being attached would be silently missed by this broadcast stream.
      txChar!.onValueReceived.listen((value) {
        _onBleDataReceived(value);
      });
      _log('[BLE] setNotifyValue(true) on TX…');
      await txChar!.setNotifyValue(true);
      _log('[BLE] Notifications enabled');

      connectedDevice!.connectionState.listen((state) {
        if (state == BluetoothConnectionState.disconnected) {
          _log('[CONN] Disconnected');
          isBluetooth = false;
          connectedDevice = null;
          rxChar = null;
          txChar = null;
          error = 'Bluetooth disconnected';
          notifyListeners();
        }
      });

      await fetchProfiles();

    } catch (e) {
      _log('[ERR] connectBluetooth: $e');
      error = 'Bluetooth connection failed: $e';
      isBluetooth = false;
      if (connectedDevice != null) {
        try {
          await connectedDevice!.disconnect();
        } catch (_) {}
        connectedDevice = null;
      }
    } finally {
      isScanningBle = false;
      isLoading = false;
      notifyListeners();
    }
  }

  Future<void> disconnectBluetooth() async {
    isBluetooth = false;
    needsConfigMode = false;
    if (connectedDevice != null) {
      try {
        await connectedDevice!.disconnect();
      } catch (_) {}
      connectedDevice = null;
    }
    rxChar = null;
    txChar = null;
    profilesData = null;
    notifyListeners();
  }

  // Shown whenever the device rejects a request for lack of an encrypted, bonded link. There is
  // no in-app action that can fix this — the OS owns pairing — so the message points at the
  // system Bluetooth settings and the PIN on the knob's screen.
  static const String pairingRequiredMessage =
      'This Draupnir is not paired with your phone yet.\n\n'
      'Open your phone\'s Bluetooth settings, pair with "Draupnir", and enter the PIN shown on '
      'the knob\'s screen. Then come back and connect again.';

  // Shown when the device answers with "Not in Config Mode". The gesture named here is the
  // M5Dial's, because it is the only board with the gate — and note it is the OPPOSITE
  // direction from that board's kill-all (swipe up), so naming the wrong one would be worse
  // than saying nothing.
  static const String configModeRequiredMessage =
      'Draupnir_Mini is in Run Mode and will not serve config requests.\n\n'
      'Swipe down on the dial\'s screen to enter Config Mode, then try again.';

  // The firmware reports this as a plain message string, not a status code, so match on the
  // text. Kept loose deliberately: the exact wording ("Not in Config Mode") lives in the M5Dial
  // sketch and is not a contract either side promises to keep.
  bool _looksLikeConfigModeRefusal(Object? message) =>
      message != null && message.toString().toLowerCase().contains('config mode');

  Future<void> fetchProfiles() async {
    isLoading = true;
    error = null;
    notifyListeners();

    try {
      final response = await _sendBleRequest({'cmd': 'get_profiles'});
      if (response['status'] == 'ok') {
        profilesData = response['profiles'];
        needsPairing = false;
        needsConfigMode = false;
      } else if (_looksLikeConfigModeRefusal(response['message'])) {
        needsConfigMode = true;
        error = configModeRequiredMessage;
      } else {
        error = 'Failed to load profiles: ${response['message']}';
      }
    } catch (e) {
      // A write/subscribe rejected for insufficient authentication surfaces here as a platform
      // GATT error rather than as a device response — the device never sees the request at all,
      // because the GATT permission flags stop it at the link layer.
      if (_looksLikeAuthFailure(e)) {
        needsPairing = true;
        error = pairingRequiredMessage;
      } else {
        error = 'Bluetooth request failed: $e';
      }
    }

    isLoading = false;
    notifyListeners();
  }

  // The firmware rejects unauthenticated access at the GATT layer, so there is no single
  // well-typed error to match on: Android surfaces ATT error 0x05/0x0F
  // (insufficient authentication / encryption) as a PlatformException whose text varies by
  // vendor, and iOS words it differently again. Match loosely and fall back to a generic error.
  bool _looksLikeAuthFailure(Object e) {
    final s = e.toString().toLowerCase();
    return s.contains('authent') ||
        s.contains('encrypt') ||
        s.contains('insufficient') ||
        s.contains('not paired') ||
        s.contains('bond');
  }

  Future<void> saveProfiles() async {
    if (profilesData == null) return;

    isLoading = true;
    notifyListeners();

    try {
      final response = await _sendBleRequest({
        'cmd': 'save_profiles',
        'profiles': profilesData,
      });
      if (response['status'] != 'ok') {
        if (_looksLikeConfigModeRefusal(response['message'])) {
          needsConfigMode = true;
          error = configModeRequiredMessage;
        } else {
          error = 'Failed to save profiles: ${response['message']}';
        }
      }
    } catch (e) {
      if (_looksLikeAuthFailure(e)) {
        needsPairing = true;
        error = pairingRequiredMessage;
      } else {
        error = 'Failed to save profiles: $e';
      }
    }

    isLoading = false;
    notifyListeners();
  }

  Future<void> triggerMacro(int macroIdx) async {
    try {
      await _sendBleRequest({
        'cmd': 'trigger',
        'profile': activeProfileIdx,
        'macro': macroIdx,
      });
    } catch (e) {
      debugPrint('Trigger failed: $e');
    }
  }

  int get orientation {
    if (profilesData == null || profilesData!['settings'] == null) return 0;
    return profilesData!['settings']['orientation'] ?? 0;
  }

  Future<void> setOrientation(int val) async {
    if (profilesData == null) return;
    if (profilesData!['settings'] == null) {
      profilesData!['settings'] = {};
    }
    profilesData!['settings']['orientation'] = val;
    await saveProfiles();
  }

  // Must match MAX_MACROS in both firmwares (macro_engine.h, M5_M6_config.ino). pos is a stable
  // identifier and ring-ordering key, NOT a grid slot -- see docs/Draupnir_Spec.md section 6.
  static const int maxMacros = 32;

  List<dynamic> get currentMacros {
    if (profilesData == null) return [];
    final profiles = profilesData!['profiles'] as List;
    if (profiles.isEmpty || activeProfileIdx >= profiles.length) return [];
    return profiles[activeProfileIdx]['macros'] ?? [];
  }

  /// Macros in ascending `pos` order -- the order the device's ring draws them in.
  ///
  /// The stored array is in whatever order edits left it; `pos` is the ordering key, so anything
  /// presenting macros to the user must sort. The firmware sorts for exactly the same reason (see
  /// scan_active_positions()).
  List<dynamic> get sortedMacros {
    final list = List<dynamic>.from(currentMacros);
    list.sort((a, b) => ((a['pos'] ?? 0) as int).compareTo((b['pos'] ?? 0) as int));
    return list;
  }

  /// Lowest `pos` not currently in use, or -1 when the profile is full.
  ///
  /// Lowest-free rather than highest-plus-one so that gaps left by deletion get reused before the
  /// ceiling is approached -- otherwise a long edit session of add/delete walks pos upward and
  /// hits maxMacros with a mostly-empty profile.
  int get lowestFreePos {
    final used = currentMacros.map((m) => m['pos'] as int?).whereType<int>().toSet();
    for (int p = 0; p < maxMacros; p++) {
      if (!used.contains(p)) return p;
    }
    return -1;
  }

  Future<void> deleteMacro(int pos) async {
    if (profilesData == null) return;
    List profiles = profilesData!['profiles'] as List;
    if (activeProfileIdx >= profiles.length) return;

    List macros = profiles[activeProfileIdx]['macros'] ?? [];
    macros.removeWhere((m) => m['pos'] == pos);
    profiles[activeProfileIdx]['macros'] = macros;
    await saveProfiles();
  }

  Future<void> updateMacro(int pos, Map<String, dynamic> macroData) async {
    if (profilesData == null) return;
    List profiles = profilesData!['profiles'] as List;
    if (activeProfileIdx >= profiles.length) return;
    
    List macros = profiles[activeProfileIdx]['macros'] ?? [];
    
    // Find if it exists
    int existingIdx = macros.indexWhere((m) => m['pos'] == pos);
    
    macroData['pos'] = pos;
    if (existingIdx != -1) {
      macros[existingIdx] = macroData;
    } else {
      macros.add(macroData);
    }
    
    profiles[activeProfileIdx]['macros'] = macros;
    await saveProfiles();
  }
}
