import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';
import 'package:shared_preferences/shared_preferences.dart';
import '../services/profile_transfer.dart';

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

  // Set when a save failed for a reason the user can simply retry. Unlike [needsPairing] and
  // [needsConfigMode] this is an event, not a state: the caller shows it in place and it is
  // cleared on the next save attempt. See [saveProfiles].
  String? lastSaveFailure;

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

  // One key, overwritten on every import. Survives an app restart, which an in-memory undo
  // would not — and a wrong import is the only irreversible action in this app.
  static const String _snapshotKey = 'pre_import_config';
  static const String _snapshotAtKey = 'pre_import_config_at';

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
        // Two eras of Android BLE permissions, and the app supports both.
        //
        // API 31+: BLUETOOTH_SCAN and BLUETOOTH_CONNECT. Location is NOT required and must not
        // be demanded -- the manifest declares BLUETOOTH_SCAN with neverForLocation and bounds
        // ACCESS_FINE_LOCATION to maxSdkVersion=30 precisely so this app never asks for
        // location on a modern phone.
        //
        // API 30 and below: BLUETOOTH/BLUETOOTH_ADMIN are install-time, and a BLE scan returns
        // nothing without ACCESS_FINE_LOCATION granted at runtime.
        //
        // So the gate accepts EITHER set, and requiring both is a bug: this check used to AND
        // location into the condition, which made it unsatisfiable on API 31+. ACCESS_FINE_
        // LOCATION is not in the merged manifest there, so permission_handler can only ever
        // report it denied, and every connection attempt failed with "Permissions denied" while
        // both Bluetooth permissions sat granted. It hid for a while because Android remembers
        // a grant from a build whose manifest did declare location; a clean install is what
        // surfaces it.
        final statuses = await [
          Permission.bluetoothScan,
          Permission.bluetoothConnect,
          Permission.location,
        ].request();

        final modern = statuses[Permission.bluetoothScan]?.isGranted == true &&
            statuses[Permission.bluetoothConnect]?.isGranted == true;
        final legacy = statuses[Permission.location]?.isGranted == true;

        if (!modern && !legacy) {
          _log('[ERR] permissions: scan=${statuses[Permission.bluetoothScan]} '
              'connect=${statuses[Permission.bluetoothConnect]} '
              'location=${statuses[Permission.location]}');
          throw Exception(
              'Draupnir needs permission to find nearby Bluetooth devices. '
              'Grant it in Settings > Apps > Draupnir Forge > Permissions.');
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
      error = "Couldn't connect to the Draupnir. Check it's powered on and in range.";
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
  // system Bluetooth settings and the PIN on the device's screen. Both boards enforce bonding
  // now, and this is shown without knowing which one refused, so it names both.
  static const String pairingRequiredMessage =
      'This Draupnir is not paired with your phone yet.\n\n'
      'Open your phone\'s Bluetooth settings, pair with it ("Draupnir" for the Waveshare knob, '
      '"Draupnir_Mini" for the M5Dial), and enter the PIN shown on the device\'s screen. '
      'Then come back and connect again.';

  // LEGACY FALLBACK — do not delete, and do not "restore" the firmware gate to match it.
  // The M5Dial's CONFIG_MODE check was removed when it gained real BLE pairing; an encrypted,
  // authenticated link is the authorization now. This message can therefore only come from an
  // M5Dial still running pre-gate firmware, which is exactly why it stays: on that board it is
  // still the correct instruction. Note the gesture named here is the OPPOSITE direction from
  // that board's kill-all (swipe up), so naming the wrong one would be worse than saying nothing.
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
        _log('[ERR] get_profiles refused: ${response['message']}');
        error = 'The device refused the request.';
      }
    } catch (e) {
      // A write/subscribe rejected for insufficient authentication surfaces here as a platform
      // GATT error rather than as a device response — the device never sees the request at all,
      // because the GATT permission flags stop it at the link layer.
      if (_looksLikeAuthFailure(e)) {
        needsPairing = true;
        error = pairingRequiredMessage;
      } else {
        _log('[ERR] get_profiles failed: $e');
        error = "Couldn't reach the device. Check it's powered on and in range.";
      }
    }

    isLoading = false;
    notifyListeners();
  }

  /// Fetches a document WITH icon bitmaps, for export only.
  ///
  /// Deliberately does not touch [profilesData]. The on-screen document was fetched without
  /// icons and the UI has no use for them; more importantly, exporting from the in-memory copy
  /// would silently produce an icon-less file, which is the exact failure this whole feature
  /// exists to prevent. Export always asks the device fresh.
  Future<Map<String, dynamic>?> fetchProfilesForExport() async {
    isLoading = true;
    error = null;
    notifyListeners();

    Map<String, dynamic>? result;
    try {
      final response =
          await _sendBleRequest({'cmd': 'get_profiles', 'include_icons': true});
      if (response['status'] == 'ok') {
        result = Map<String, dynamic>.from(response['profiles'] as Map);
        needsPairing = false;
        needsConfigMode = false;
      } else if (_looksLikeConfigModeRefusal(response['message'])) {
        needsConfigMode = true;
        error = configModeRequiredMessage;
      } else {
        _log('[ERR] export read refused: ${response['message']}');
        error = 'The device refused the request.';
      }
    } catch (e) {
      if (_looksLikeAuthFailure(e)) {
        needsPairing = true;
        error = pairingRequiredMessage;
      } else {
        _log('[ERR] export read failed: $e');
        error = "Couldn't reach the device. Check it's powered on and in range.";
      }
    }

    isLoading = false;
    notifyListeners();
    return result;
  }

  bool _hasSnapshot = false;
  DateTime? _snapshotAt;

  bool get hasImportSnapshot => _hasSnapshot;
  DateTime? get importSnapshotTakenAt => _snapshotAt;

  /// Call once at startup so the Undo affordance survives an app restart.
  Future<void> loadImportSnapshotState() async {
    final prefs = await SharedPreferences.getInstance();
    _hasSnapshot = prefs.getString(_snapshotKey) != null;
    final at = prefs.getString(_snapshotAtKey);
    _snapshotAt = at == null ? null : DateTime.tryParse(at);
    notifyListeners();
  }

  Future<void> _takeImportSnapshot() async {
    if (profilesData == null) return;
    final prefs = await SharedPreferences.getInstance();
    final now = DateTime.now().toUtc();
    await prefs.setString(_snapshotKey, jsonEncode(profilesData));
    await prefs.setString(_snapshotAtKey, now.toIso8601String());
    _hasSnapshot = true;
    _snapshotAt = now;
  }

  /// Applies a parsed transfer to the device.
  ///
  /// A snapshot is taken before EVERY import, not only the destructive one. An append is not
  /// destructive, but undoing one is just as useful, and a rule that fires on every import
  /// cannot be got wrong about which case it covers.
  Future<bool> importTransfer(ParsedTransfer t) async {
    if (profilesData == null) {
      error = 'Connect to a Draupnir before importing.';
      notifyListeners();
      return false;
    }

    await _takeImportSnapshot();

    if (t.isConfig) {
      // Replaces everything. This is restore.
      profilesData = Map<String, dynamic>.from(t.payload);
    } else {
      // Appends. Never overwrites, and never renumbers pos — pos is a stable identifier and the
      // ring-order key, unique only WITHIN a profile, so an appended profile cannot collide.
      final profiles = profilesData!['profiles'] as List;
      final incoming = Map<String, dynamic>.from(t.payload);
      incoming['name'] =
          uniqueProfileName(incoming['name'].toString(), profiles);
      profiles.add(incoming);
    }

    notifyListeners();
    await saveProfiles();
    if (error != null) return false;

    // Re-read so the UI reflects what the device actually stored, including any icon merge it
    // performed on the way in.
    await fetchProfiles();
    return error == null;
  }

  /// Re-sends the config captured immediately before the last import.
  Future<bool> undoImport() async {
    final prefs = await SharedPreferences.getInstance();
    final saved = prefs.getString(_snapshotKey);
    if (saved == null) {
      error = 'There is no pre-import snapshot to restore.';
      notifyListeners();
      return false;
    }
    profilesData = Map<String, dynamic>.from(jsonDecode(saved) as Map);
    notifyListeners();
    await saveProfiles();
    if (error != null) return false;
    await fetchProfiles();
    return error == null;
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

  /// Writes the current document to the device. Returns true if the device accepted it.
  ///
  /// Two kinds of failure, deliberately reported differently.
  ///
  /// Pairing and Config Mode are STATES: nothing can be saved until the user does something
  /// away from this screen, so they raise [needsPairing] / [needsConfigMode] and the dashboard
  /// hands over to a screen that says which gesture to make.
  ///
  /// Everything else is an EVENT -- a dropped write, a timeout, a device-side refusal. The deck
  /// is still valid, the document is still in memory, and retrying is one tap. Those return
  /// false with [lastSaveFailure] set, and the caller reports them in place. They deliberately
  /// do NOT set [error]: the dashboard renders `error != null` as a full-screen takeover, so
  /// routing a transient write failure through it used to wipe the deck off the screen and
  /// leave the user staring at a reconnect button with a perfectly healthy link.
  Future<bool> saveProfiles() async {
    if (profilesData == null) return false;

    // Clear stale state on entry, the way fetchProfiles() does. Without this a single failed
    // save left `error` set forever: nothing on the save path ever cleared it, so every LATER
    // save -- including successful ones -- still rendered the error screen, and the only way
    // out was a refresh or a reconnect.
    isLoading = true;
    error = null;
    needsPairing = false;
    needsConfigMode = false;
    lastSaveFailure = null;
    notifyListeners();

    bool saved = false;
    try {
      final response = await _sendBleRequest({
        'cmd': 'save_profiles',
        'profiles': profilesData,
      });
      if (response['status'] == 'ok') {
        saved = true;
      } else if (_looksLikeConfigModeRefusal(response['message'])) {
        needsConfigMode = true;
        error = configModeRequiredMessage;
      } else {
        _log('[ERR] save_profiles refused: ${response['message']}');
        lastSaveFailure = 'The device refused the save.';
      }
    } catch (e) {
      if (_looksLikeAuthFailure(e)) {
        needsPairing = true;
        error = pairingRequiredMessage;
      } else {
        // The raw exception is a PlatformException dump. It belongs in the debug log, which
        // already exists and is already copyable, not in front of the user.
        _log('[ERR] save_profiles failed: $e');
        lastSaveFailure = "Couldn't reach the device. Check it's powered on and in range.";
      }
    }

    isLoading = false;
    notifyListeners();
    return saved;
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

  Future<bool> deleteMacro(int pos) async {
    if (profilesData == null) return false;
    List profiles = profilesData!['profiles'] as List;
    if (activeProfileIdx >= profiles.length) return false;

    List macros = profiles[activeProfileIdx]['macros'] ?? [];
    macros.removeWhere((m) => m['pos'] == pos);
    profiles[activeProfileIdx]['macros'] = macros;
    return await saveProfiles();
  }

  /// Writes one macro into the document and pushes it. Returns the device's verdict, so the
  /// caller can confirm the save rather than assume it.
  Future<bool> updateMacro(int pos, Map<String, dynamic> macroData) async {
    if (profilesData == null) return false;
    List profiles = profilesData!['profiles'] as List;
    if (activeProfileIdx >= profiles.length) return false;
    
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
    return await saveProfiles();
  }
}
