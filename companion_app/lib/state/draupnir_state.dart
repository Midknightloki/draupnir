import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'package:flutter/foundation.dart';
import 'package:http/http.dart' as http;
import 'package:shared_preferences/shared_preferences.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

class DraupnirState extends ChangeNotifier {
  String deviceIp = 'draupnir.local';
  
  bool isLoading = false;
  String? error;
  
  Map<String, dynamic>? profilesData;
  int activeProfileIdx = 0;
  bool isEditorMode = false;
  
  String? authToken;
  bool needsPairing = false;
  bool isPairing = false;

  bool isBluetooth = false;
  BluetoothDevice? connectedDevice;
  BluetoothCharacteristic? rxChar;
  BluetoothCharacteristic? txChar;
  bool isScanningBle = false;
  Completer<Map<String, dynamic>>? _bleResponseCompleter;
  String _bleBuffer = '';

  Future<void> init() async {
    final prefs = await SharedPreferences.getInstance();
    authToken = prefs.getString('authToken');
  }

  Map<String, String> get _headers {
    if (authToken != null && authToken!.isNotEmpty) {
      return {'Authorization': 'Bearer $authToken', 'Content-Type': 'text/plain'};
    }
    return {'Content-Type': 'text/plain'};
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

  void _onBleDataReceived(List<int> value) {
    String text = utf8.decode(value);
    for (int i = 0; i < text.length; i++) {
      String char = text[i];
      if (char == '\n') {
        final responseStr = _bleBuffer.trim();
        _bleBuffer = '';
        if (responseStr.isEmpty) continue;
        if (_bleResponseCompleter != null && !_bleResponseCompleter!.isCompleted) {
          try {
            final responseJson = jsonDecode(responseStr) as Map<String, dynamic>;
            _bleResponseCompleter!.complete(responseJson);
          } catch (e) {
            _bleResponseCompleter!.completeError(e);
          }
        }
      } else {
        _bleBuffer += char;
      }
    }
  }

  Future<Map<String, dynamic>> _sendBleRequest(Map<String, dynamic> request) async {
    if (rxChar == null) {
      throw Exception('Not connected to BLE device');
    }
    
    _bleResponseCompleter = Completer<Map<String, dynamic>>();
    _bleBuffer = '';
    
    String jsonStr = '${jsonEncode(request)}\n';
    List<int> bytes = utf8.encode(jsonStr);
    
    int offset = 0;
    while (offset < bytes.length) {
      int chunkSize = (bytes.length - offset) < 180 ? (bytes.length - offset) : 180;
      List<int> chunk = bytes.sublist(offset, offset + chunkSize);
      await rxChar!.write(chunk, withoutResponse: false);
      offset += chunkSize;
      await Future.delayed(const Duration(milliseconds: 20));
    }
    
    return _bleResponseCompleter!.future.timeout(const Duration(seconds: 10));
  }

  Future<void> connectBluetooth() async {
    isLoading = true;
    isScanningBle = true;
    error = null;
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
      
      BluetoothDevice? targetDevice;
      List<String> foundNames = [];
      final subscription = FlutterBluePlus.scanResults.listen((results) {
        for (final r in results) {
          final name = r.device.platformName;
          if (name.isNotEmpty && !foundNames.contains(name)) {
            foundNames.add(name);
          }
          if (name.toLowerCase().contains('draupnir') ||
              r.advertisementData.serviceUuids.contains(Guid('6E400001-B5A3-F393-E0A9-E50E24DCCA9E'))) {
            targetDevice = r.device;
            FlutterBluePlus.stopScan();
            break;
          }
        }
      });

      await FlutterBluePlus.startScan(
        timeout: const Duration(seconds: 4),
      );
      
      await subscription.cancel();
      isScanningBle = false;
      
      if (targetDevice == null) {
        final listStr = foundNames.isEmpty ? "None" : foundNames.take(5).join(", ");
        throw Exception('No Draupnir device found. Nearby devices: $listStr. Ensure BLE/GPS is active.');
      }
      
      await targetDevice!.connect(license: License.nonprofit);
      connectedDevice = targetDevice;
      isBluetooth = true;
      
      List<BluetoothService> services = await targetDevice!.discoverServices();
      BluetoothService? uartService;
      for (var s in services) {
        if (s.uuid == Guid('6E400001-B5A3-F393-E0A9-E50E24DCCA9E')) {
          uartService = s;
          break;
        }
      }
      
      if (uartService == null) {
        throw Exception('Nordic UART Service not found on device');
      }
      
      for (var c in uartService.characteristics) {
        if (c.uuid == Guid('6E400002-B5A3-F393-E0A9-E50E24DCCA9E')) {
          rxChar = c;
        } else if (c.uuid == Guid('6E400003-B5A3-F393-E0A9-E50E24DCCA9E')) {
          txChar = c;
        }
      }
      
      if (rxChar == null || txChar == null) {
        throw Exception('UART characteristics not found');
      }
      
      await txChar!.setNotifyValue(true);
      txChar!.onValueReceived.listen((value) {
        _onBleDataReceived(value);
      });
      
      connectedDevice!.connectionState.listen((state) {
        if (state == BluetoothConnectionState.disconnected) {
          isBluetooth = false;
          connectedDevice = null;
          rxChar = null;
          txChar = null;
          error = 'Bluetooth disconnected';
          notifyListeners();
        }
      });
      
      await init();
      await fetchProfiles();
      
    } catch (e) {
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

  Future<void> connect(String ip) async {
    deviceIp = ip;
    await init();
    await fetchProfiles();
  }

  Future<void> fetchProfiles() async {
    isLoading = true;
    error = null;
    notifyListeners();

    try {
      if (isBluetooth) {
        final response = await _sendBleRequest({
          'cmd': 'get_profiles',
          'token': authToken,
        });
        if (response['status'] == 'ok') {
          profilesData = response['profiles'];
          needsPairing = false;
        } else if (response['message'] == 'Unauthorized') {
          needsPairing = true;
          error = 'Pairing required. Swipe down on Draupnir to enter Config Mode and press Pair.';
        } else {
          error = 'Failed to load profiles: ${response['message']}';
        }
      } else {
        final response = await http.get(Uri.parse('http://$deviceIp/api/profiles'), headers: _headers);
        if (response.statusCode == 200) {
          profilesData = jsonDecode(response.body);
          needsPairing = false;
        } else if (response.statusCode == 401 || response.statusCode == 403) {
          needsPairing = true;
          error = 'Pairing required. Swipe down on Draupnir to enter Config Mode and press Pair.';
        } else {
          error = 'Failed to load profiles (HTTP ${response.statusCode})';
        }
      }
    } catch (e) {
      error = isBluetooth 
        ? 'Bluetooth request failed: $e'
        : 'Connection failed. Ensure Draupnir is on the same network.';
    }

    isLoading = false;
    notifyListeners();
  }

  Future<void> pair() async {
    isPairing = true;
    notifyListeners();
    try {
      if (isBluetooth) {
        final response = await _sendBleRequest({
          'cmd': 'pair',
        });
        if (response['status'] == 'ok' && response['token'] != null) {
          authToken = response['token'];
          final prefs = await SharedPreferences.getInstance();
          await prefs.setString('authToken', authToken!);
          needsPairing = false;
          await fetchProfiles();
        } else {
          error = 'Failed to pair. Make sure Draupnir is in Config Mode (swipe down).';
        }
      } else {
        final response = await http.post(Uri.parse('http://$deviceIp/api/pair'));
        if (response.statusCode == 200) {
          final data = jsonDecode(response.body);
          if (data['token'] != null) {
            authToken = data['token'];
            final prefs = await SharedPreferences.getInstance();
            await prefs.setString('authToken', authToken!);
            needsPairing = false;
            await fetchProfiles();
          }
        } else {
          error = 'Failed to pair. Make sure Draupnir is in Config Mode (swipe down).';
        }
      }
    } catch (e) {
      error = 'Pairing connection failed: $e';
    }
    isPairing = false;
    notifyListeners();
  }

  Future<void> saveProfiles() async {
    if (profilesData == null) return;
    
    isLoading = true;
    notifyListeners();

    try {
      if (isBluetooth) {
        final response = await _sendBleRequest({
          'cmd': 'save_profiles',
          'token': authToken,
          'profiles': profilesData,
        });
        if (response['status'] != 'ok') {
          error = 'Failed to save profiles: ${response['message']}';
        }
      } else {
        final response = await http.post(
          Uri.parse('http://$deviceIp/api/profiles'),
          headers: _headers,
          body: jsonEncode(profilesData),
        );
        if (response.statusCode != 200) {
          error = 'Failed to save profiles (HTTP ${response.statusCode})';
        }
      }
    } catch (e) {
      error = 'Failed to save profiles: $e';
    }

    isLoading = false;
    notifyListeners();
  }

  Future<void> triggerMacro(int macroIdx) async {
    try {
      if (isBluetooth) {
        await _sendBleRequest({
          'cmd': 'trigger',
          'token': authToken,
          'profile': activeProfileIdx,
          'macro': macroIdx,
        });
      } else {
        await http.post(
          Uri.parse('http://$deviceIp/api/trigger'),
          headers: _headers,
          body: jsonEncode({
            'profile': activeProfileIdx,
            'macro': macroIdx,
          }),
        );
      }
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

  List<dynamic> get currentMacros {
    if (profilesData == null) return [];
    final profiles = profilesData!['profiles'] as List;
    if (profiles.isEmpty || activeProfileIdx >= profiles.length) return [];
    return profiles[activeProfileIdx]['macros'] ?? [];
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
