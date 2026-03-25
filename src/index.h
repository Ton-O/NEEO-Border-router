String html = "<html><head><title>NEEO Border Router</title>";
html += "<style>body{font-family:sans-serif;margin:20px;background:#f0f2f5;}";
html += ".card{background:white;padding:20px;border-radius:12px;box-shadow:0 4px 6px rgba(0,0,0,0.1);margin-bottom:20px;}";
html += ".grid{display:grid;grid-template-columns:repeat(auto-fit, minmax(180px, 1fr));gap:10px;}";
html += ".config-row { display: flex; flex-wrap: wrap; gap: 20px; align-items: flex-end; margin-bottom: 15px; }";
html += ".config-field { flex: 1; min-width: 200px; }";
html += "select, input[type=text]{padding:8px;border-radius:4px;border:1px solid #ccc;margin:5px 0;width:100%;box-sizing:border-box;}";
html += "button, input[type=submit]{padding:12px;cursor:pointer;border-radius:6px;border:none;background:#007bff;color:white;font-weight:bold;}";
html += "button.red{background:#dc3545;} button.white{background:#e9ecef;color:#333;border:1px solid #ccc;}";
html += "pre{background:#222;color:#00ff00;padding:15px;border-radius:8px;height:250px;overflow-y:auto;font-size:12px;}</style></head><body>";
html += "<h1>NEEO Dashboard</h1>";

html += "<div class='card'><h3>Blink & Diag</h3><div class='grid'>";
html += "<button class='red' onclick=\"fetch('/blink?mode=red')\">RED</button>";
html += "<button class='white' onclick=\"fetch('/blink?mode=white')\">WHITE</button>";
html += "<button onclick=\"fetch('/diag')\">Run Diag</button>";
html += "<button onclick=\"fetch('/neighbors')\">Neighbors</button></div></div>";

html += "<div class='card'><h3>Press Simulation</h3><div class='grid'>";
html += "<button onclick=\"fetch('/ShortPress')\">ShortPress</button>";
html += "<button onclick=\"fetch('/LongPress')\">LongPress</button></div></div>";

html += "<div class='card'><h3>Configuration</h3><form action='/save' method='POST'>";
html += "<div class='config-row'>";
html += "  <div class='config-field'>Brain Name:<br><input type='text' name='brain' value='" + brainName + "'></div>";
html += "  <div class='config-field'>Location:<br><select id='tzSelect' onchange='updateTZFields(this)'><option>Loading...</option></select></div>";
html += "  <div class='config-field'>POSIX String:<br><input type='text' name='tz_posix' id='tzPosix' value='" + timezonePosix + "'></div>";
html += "</div>";
html += "<input type='hidden' name='tz_name' id='tzName' value='" + timezoneName + "'>";
html += "Debug Logs: <input type='checkbox' name='debug' " + String(debugEnabled ? "checked" : "") + "><br><br>";
html += "<input type='submit' value='Save & Restart Settings'></form></div>";

html += "<div class='card'><h3>System Log</h3><pre id='log'>" + getFullLog() + "</pre>";
html += "<button onclick=\"fetch('/clearlog').then(()=>location.reload())\">Clear Log</button></div>";

html += "<div class='card'><h3>Network Settings</h3><p>Current SSID: <b>" + WiFi.SSID() + "</b></p>";
html += "<button class='red' onclick=\"if(confirm('Reset WiFi?')) fetch('/reset_wifi', {method:'POST'})\">Change WiFi Network</button></div>";

html += "<script>";
html += "function updateTZFields(sel) {";
html += "  document.getElementById('tzPosix').value = sel.value;";
html += "  document.getElementById('tzName').value = sel.options[sel.selectedIndex].text;";
html += "}";
html += "async function loadTZ() {";
html += "  try {";
html += "    const res = await fetch('https://raw.githubusercontent.com');";
html += "    const text = await res.text();";
html += "    const lines = text.split('\\n');";
html += "    const select = document.getElementById('tzSelect');";
html += "    const savedName = \"" + timezoneName + "\";";
html += "    select.innerHTML = '';";
html += "    lines.forEach(line => {";
html += "      const parts = line.split('\",\"');";
html += "      if(parts.length >= 2) {";
html += "        const name = parts[0].replace(/\"/g, '');";
html += "        const posix = parts[1].replace(/\"/g, '').trim();";
html += "        const opt = document.createElement('option');";
html += "        opt.value = posix; opt.innerText = name;";
html += "        if(name === savedName) opt.selected = true;";
html += "        select.appendChild(opt);";
html += "      }";
html += "    });";
html += "  } catch(e) { console.error(e); }";
html += "}";
html += "loadTZ();";
html += "setInterval(()=>{fetch('/log').then(r=>r.text()).then(t=>{const e=document.getElementById('log');if(e){e.innerText=t;e.scrollTop=e.scrollHeight;}});},2000);";
html += "</script></body></html>";
