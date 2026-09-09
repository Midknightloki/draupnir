import re

with open('M5_M6_config.ino', 'r') as f:
    content = f.read()

# 1. Update Enums and add pairingToken
content = content.replace(
    'enum AppMode { RUN_MODE, CONFIG_MODE };\nAppMode currentMode = RUN_MODE;',
    'enum AppMode { RUN_MODE, CONFIG_MODE, WIFI_SETUP_MODE };\nAppMode currentMode = RUN_MODE;\nString pairingToken = "";\n'
)

# 2. Add isAuthorized helper function before setupWebServer
auth_helper = '''bool isAuthorized() {
  if (currentMode == CONFIG_MODE) return true;
  if (server.hasHeader("Authorization")) {
    String auth = server.header("Authorization");
    if (auth == "Bearer " + pairingToken && pairingToken.length() > 0) return true;
  }
  return false;
}

'''
content = content.replace('void setupWebServer() {', auth_helper + 'void setupWebServer() {')

# 3. Modify setupWebServer to collect headers and add auth checks
setup_server_mod = '''void setupWebServer() {
  server.enableCORS(true);
  
  const char * headerkeys[] = {"Authorization"};
  size_t headerkeyssize = sizeof(headerkeys)/sizeof(char*);
  server.collectHeaders(headerkeys, headerkeyssize);
  
  server.on("/", HTTP_GET, [](){
    if (currentMode != CONFIG_MODE) {
      server.send(403, "text/plain", "Forbidden. Swipe down on Draupnir to enter Config Mode.");
      return;
    }
    server.send(200, "text/html", INDEX_HTML);
  });
  
  server.on("/api/pair", HTTP_POST, []() {
    if (currentMode != CONFIG_MODE) {
      server.send(403, "application/json", "{\\"status\\":\\"error\\",\\"message\\":\\"Not in Config Mode\\"}");
      return;
    }
    pairingToken = String(esp_random(), HEX) + String(esp_random(), HEX) + String(esp_random(), HEX);
    prefs.putString("pairingToken", pairingToken);
    server.send(200, "application/json", "{\\"token\\":\\"" + pairingToken + "\\"}");
  });

  server.on("/api/profiles", HTTP_GET, [](){
    if (!isAuthorized()) {
      server.send(401, "application/json", "{\\"status\\":\\"error\\",\\"message\\":\\"Unauthorized\\"}");
      return;
    }'''

content = content.replace('''void setupWebServer() {
  server.enableCORS(true);
  
  server.on("/", HTTP_GET, [](){
    server.send(200, "text/html", INDEX_HTML);
  });
  
  server.on("/api/profiles", HTTP_GET, [](){''', setup_server_mod)

# Add auth check to /api/profiles POST
content = content.replace('''  server.on("/api/profiles", HTTP_POST, [](){
    if(server.hasArg("plain")) {''', '''  server.on("/api/profiles", HTTP_POST, [](){
    if (!isAuthorized()) {
      server.send(401, "application/json", "{\\"status\\":\\"error\\",\\"message\\":\\"Unauthorized\\"}");
      return;
    }
    if(server.hasArg("plain")) {''')

# Add auth check to /api/trigger POST
content = content.replace('''  server.on("/api/trigger", HTTP_POST, [](){
    if(server.hasArg("plain")) {''', '''  server.on("/api/trigger", HTTP_POST, [](){
    if (!isAuthorized()) {
      server.send(401, "application/json", "{\\"status\\":\\"error\\",\\"message\\":\\"Unauthorized\\"}");
      return;
    }
    if(server.hasArg("plain")) {''')

# 4. In enterConfigMode, change to WIFI_SETUP_MODE rename
content = content.replace('void enterConfigMode() {\n  currentMode = CONFIG_MODE;', 'void enterWiFiSetupMode() {\n  currentMode = WIFI_SETUP_MODE;')

# In loop(), change enterConfigMode() call to enterWiFiSetupMode()
content = content.replace('enterConfigMode();\n    }\n    \n    auto touch', 'enterWiFiSetupMode();\n    }\n    \n    auto touch')

# Fix the CONFIG_MODE logic in loop (now WIFI_SETUP_MODE)
content = content.replace('} else if (currentMode == CONFIG_MODE) {', '} else if (currentMode == WIFI_SETUP_MODE) {')

# Add a real enterConfigMode and loop handling
config_mode_funcs = '''void enterConfigMode() {
  currentMode = CONFIG_MODE;
  killAllMacros();
  
  auto& d = M5Dial.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(middle_center);
  d.setFont(&fonts::Orbitron_Light_24);
  d.setTextColor(TFT_GREEN, TFT_BLACK);
  d.drawString("CONFIG MODE", 120, 60);
  
  d.setFont(&fonts::Orbitron_Light_16);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  d.drawString("IP Address:", 120, 110);
  d.drawString(WiFi.localIP().toString(), 120, 140);
  
  d.setTextColor(TFT_DARKGRAY, TFT_BLACK);
  d.drawString("TAP TO EXIT", 120, 190);
}

void enterWiFiSetupMode() {'''

content = content.replace('void enterWiFiSetupMode() {', config_mode_funcs)

# 5. In loop, add touch handling for enterConfigMode
# Swipe Down -> touch.distanceY() > 40 && abs(touch.distanceX()) < 30
# Existing:
#       if (touch.distanceY() < -40 && abs(touch.distanceX()) < 30) {
#         // Swipe Up -> Kill All

swipe_down_logic = '''      if (touch.distanceY() < -40 && abs(touch.distanceX()) < 30) {
        // Swipe Up -> Kill All
        M5Dial.Speaker.tone(1000, 50);
        delay(50);
        M5Dial.Speaker.tone(800, 50);
        killAllMacros();
      } else if (touch.distanceY() > 40 && abs(touch.distanceX()) < 30) {
        // Swipe Down -> Enter Config Mode
        M5Dial.Speaker.tone(1500, 50);
        enterConfigMode();
      } else if (abs(touch.distanceX()) < 10 && touch.y > 60 && touch.y < 180) { '''

content = content.replace('''      if (touch.distanceY() < -40 && abs(touch.distanceX()) < 30) {
        // Swipe Up -> Kill All
        M5Dial.Speaker.tone(1000, 50);
        delay(50);
        M5Dial.Speaker.tone(800, 50);
        killAllMacros();
      } else if (abs(touch.distanceX()) < 10 && touch.y > 60 && touch.y < 180) { ''', swipe_down_logic)

# 6. Add CONFIG_MODE loop handling
config_mode_loop = '''  } else if (currentMode == CONFIG_MODE) {
    server.handleClient();
    auto touch = M5Dial.Touch.getDetail();
    if (touch.wasReleased() || M5Dial.BtnA.wasReleased()) {
      currentMode = RUN_MODE;
      M5Dial.Speaker.tone(2000, 30);
      requestRedraw();
    }
  } else if (currentMode == WIFI_SETUP_MODE) {'''

content = content.replace('  } else if (currentMode == WIFI_SETUP_MODE) {', config_mode_loop)

# 7. Load pairingToken in setup()
setup_load = '''  prefs.begin("draupnir", false);
  pairingToken = prefs.getString("pairingToken", "");
'''
content = content.replace('  prefs.begin("draupnir", false);\n  \n  loadProfiles();', setup_load + '  \n  loadProfiles();')

with open('M5_M6_config.ino', 'w') as f:
    f.write(content)

print("Firmware file updated successfully!")
