#pragma once

const char* INDEX_HTML = R"=====(
<!DOCTYPE html>
<html>
<head>
  <title>Draupnir Config</title>
  <style>
    body { font-family: sans-serif; max-width: 800px; margin: 40px auto; padding: 20px; background: #111; color: #eee; }
    h1 { color: #00FFCC; }
    textarea { width: 100%; height: 400px; background: #222; color: #fff; border: 1px solid #444; font-family: monospace; padding: 10px; }
    button { background: #00FFCC; color: #000; border: none; padding: 10px 20px; cursor: pointer; font-size: 16px; margin-top: 10px; }
    button:hover { background: #00CCAA; }
    #status { margin-top: 20px; color: #ff5555; }
  </style>
</head>
<body>
  <h1>Draupnir Config (M6)</h1>
  <p>Edit your profiles.json below and hit Save.</p>
  <textarea id="json-editor">Loading...</textarea>
  <br>
  <button onclick="saveJson()">Save to Draupnir</button>
  <div id="status"></div>

  <script>
    // Fetch the current profiles.json from the device
    fetch('/api/profiles')
      .then(res => res.text())
      .then(text => {
        try {
          const obj = JSON.parse(text);
          document.getElementById('json-editor').value = JSON.stringify(obj, null, 2);
        } catch(e) {
          document.getElementById('json-editor').value = text;
        }
      })
      .catch(err => document.getElementById('status').innerText = 'Error loading profiles');

    function saveJson() {
      const text = document.getElementById('json-editor').value;
      try {
        JSON.parse(text); // Validate JSON
      } catch(e) {
        document.getElementById('status').innerText = 'Invalid JSON: ' + e.message;
        return;
      }
      
      document.getElementById('status').innerText = 'Saving...';
      
      fetch('/api/profiles', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: text
      })
      .then(res => res.text())
      .then(text => {
        document.getElementById('status').innerText = 'Saved! Draupnir is rebooting...';
        document.getElementById('status').style.color = '#00ffcc';
      })
      .catch(err => {
        document.getElementById('status').innerText = 'Error saving';
        document.getElementById('status').style.color = '#ff5555';
      });
    }
  </script>
</body>
</html>
)=====";
