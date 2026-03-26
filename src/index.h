#ifndef INDEX_H
#define INDEX_H

#include <Arduino.h>

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<html><head><title>NEEO Border Router</title>
<style>
    body{font-family:sans-serif;margin:20px;background:#f0f2f5;}
    .card{background:white;padding:20px;border-radius:12px;box-shadow:0 4px 6px rgba(0,0,0,0.1);margin-bottom:20px;}
    .grid{display:grid;grid-template-columns:repeat(auto-fit, minmax(180px, 1fr));gap:10px;}
    .config-row { display: flex; flex-wrap: wrap; gap: 20px; align-items: flex-end; margin-bottom: 15px; }
    .config-field { flex: 1; min-width: 200px; }
    select, input[type=text]{padding:8px;border-radius:4px;border:1px solid #ccc;margin:5px 0;width:100%;box-sizing:border-box;}
    button, input[type=submit]{padding:12px;cursor:pointer;border-radius:6px;border:none;background:#007bff;color:white;font-weight:bold;}
    button.red{background:#dc3545;} button.white{background:#e9ecef;color:#333;border:1px solid #ccc;}
    pre{background:#222;color:#00ff00;padding:15px;border-radius:8px;height:250px;overflow-y:auto;font-size:12px;}
</style></head><body>
    <h1>NEEO Dashboard</h1>
    
    <div class='card'><h3>Blink & Diag</h3><div class='grid'>
        <button class='red' onclick="fetch('/blink?mode=red')">RED</button>
        <button class='white' onclick="fetch('/blink?mode=white')">WHITE</button>
        <button onclick="fetch('/diag')">Run Diag</button>
        <button onclick="fetch('/neighbors')">Neighbors</button>
    </div></div>

    <div class='card'><h3>Press Simulation</h3><div class='grid'>
        <button onclick="fetch('/ShortPress')">ShortPress</button>
        <button onclick="fetch('/LongPress')">LongPress</button>
    </div></div>

    <div class='card'><h3>Configuration</h3><form action='/save' method='POST'>
        <div class='config-row'>
          <div class='config-field'>Brain Name:<br><input type='text' name='brain' value='[[BRAIN]]'></div>
          <div class='config-field'>Location:<br><select id='tzSelect' onchange='updateTZFields(this)'><option>Loading...</option></select></div>
          <div class='config-field'>POSIX String:<br><input type='text' name='tz_posix' id='tzPosix' value='[[POSIX]]'></div>
        </div>
        <input type='hidden' name='tz_name' id='tzName' value='[[TZNAME]]'>
        Debug Logs: <input type='checkbox' name='debug' [[DEBUG_CHECKED]]><br><br>
        <input type='submit' value='Save & Restart Settings'></form></div>

    <div class='card'><h3>System Log</h3><pre id='log'>[[LOG_CONTENT]]</pre>
    <button onclick="fetch('/clearlog').then(()=>location.reload())">Clear Log</button></div>

    <div class='card'><h3>Network Settings</h3><p>Current SSID: <b>[[WIFI_SSID]]</b></p>
    <button class='red' onclick="if(confirm('Reset WiFi?')) fetch('/reset_wifi', {method:'POST'})">Change WiFi Network</button></div>

<script>
    function updateTZFields(sel) {
        document.getElementById('tzPosix').value = sel.value;
        document.getElementById('tzName').value = sel.options[sel.selectedIndex].text;
    }
    async function loadTZ() {
        try {
            const res = await fetch('https://raw.githubusercontent.com');
            const text = await res.text();
            const lines = text.split('\n');
            const select = document.getElementById('tzSelect');
            const savedName = "[[TZNAME]]";
            select.innerHTML = '';
            lines.forEach(line => {
                const parts = line.split('","');
                if(parts.length >= 2) {
                    const name = parts[0].replace(/"/g, '');
                    const posix = parts[1].replace(/"/g, '').trim();
                    const opt = document.createElement('option');
                    opt.value = posix; opt.innerText = name;
                    if(name === savedName) opt.selected = true;
                    select.appendChild(opt);
                }
            });
        } catch(e) { console.error(e); }
    }
    loadTZ();
    setInterval(()=>{
        fetch('/log').then(r=>r.text()).then(t=>{
            const e=document.getElementById('log');
            if(e){e.innerText=t;e.scrollTop=e.scrollHeight;}
        });
    },2000);
</script></body></html>
)rawliteral";

#endif
