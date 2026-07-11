import 'dart:convert';
import 'package:flutter/foundation.dart';
import 'package:http/http.dart' as http;
import 'package:shared_preferences/shared_preferences.dart';
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
    } catch (e) {
      error = 'Connection failed. Ensure Draupnir is on the same network.';
    }

    isLoading = false;
    notifyListeners();
  }

  Future<void> pair() async {
    isPairing = true;
    notifyListeners();
    try {
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
    } catch (e) {
      error = 'Pairing connection failed.';
    }
    isPairing = false;
    notifyListeners();
  }

  Future<void> saveProfiles() async {
    if (profilesData == null) return;
    
    isLoading = true;
    notifyListeners();

    try {
      final response = await http.post(
        Uri.parse('http://$deviceIp/api/profiles'),
        headers: _headers,
        body: jsonEncode(profilesData),
      );
      if (response.statusCode != 200) {
        error = 'Failed to save profiles (HTTP ${response.statusCode})';
      }
    } catch (e) {
      error = 'Failed to save profiles: $e';
    }

    isLoading = false;
    notifyListeners();
  }

  Future<void> triggerMacro(int macroIdx) async {
    try {
      await http.post(
        Uri.parse('http://$deviceIp/api/trigger'),
        headers: _headers,
        body: jsonEncode({
          'profile': activeProfileIdx,
          'macro': macroIdx,
        }),
      );
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
