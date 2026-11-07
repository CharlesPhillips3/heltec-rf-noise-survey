#pragma once
// Live page served at http://192.168.4.1 — open in iPhone Safari after joining the RF-Survey Wi-Fi.
static const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RF Survey 902-928</title>
<style>
body{font-family:-apple-system,Helvetica,sans-serif;margin:0;padding:12px;background:#0f1419;color:#e6e6e6}
h1{font-size:18px;margin:0 0 8px}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:6px;margin-bottom:8px}
.t{background:#1b232c;border-radius:8px;padding:8px}
.t b{display:block;font-size:20px}
.t span{font-size:11px;color:#9aa5b1}
canvas{width:100%;height:180px;background:#1b232c;border-radius:8px}
button{font-size:16px;padding:10px 12px;margin:4px 4px 0 0;border:0;border-radius:8px;background:#2f6fde;color:#fff}
button.warn{background:#a33}
#msg{font-size:13px;color:#9aa5b1;margin-top:6px;min-height:18px}
.ok{color:#5c5}.bad{color:#e66}
.stn{background:#1b232c;border-radius:8px;padding:10px;margin-bottom:8px}
.stn input{width:100%;font-size:16px;padding:9px;border-radius:8px;border:1px solid #33404d;background:#0f1419;color:#e6e6e6;margin-bottom:8px;box-sizing:border-box}
button.go{width:100%;font-size:22px;padding:16px;margin:0;background:#2ea043}
button.go.run{background:#d29922}
#stnInfo{font-size:13px;color:#9aa5b1;margin-top:8px}
</style></head><body>
<h1>RF Survey · 902–928 MHz</h1>
<div class="stn">
 <input id="note" placeholder="Station label (optional), e.g. Main St & 5th NE">
 <button id="go" class="go" onclick="go()">GO · baseline</button>
 <div id="stnInfo">Park, then press GO. Keep the antenna still until it finishes.</div>
</div>
<div class="grid">
 <div class="t"><span>Noise floor (P20)</span><b id="floor">–</b></div>
 <div class="t"><span>Median</span><b id="med">–</b></div>
 <div class="t"><span>Peak</span><b id="peak">–</b></div>
 <div class="t"><span>Busy channels</span><b id="busy">–</b></div>
 <div class="t"><span>Clock</span><b id="clk">–</b></div>
 <div class="t"><span>GPS module</span><b id="gps">–</b></div>
 <div class="t"><span>Log rows / storage</span><b id="log">–</b></div>
 <div class="t"><span>Logging</span><b id="lg">–</b></div>
</div>
<canvas id="c" width="600" height="300"></canvas>
<div>
 <button onclick="sync()">Sync clock</button>
 <button onclick="mark()">Mark spot</button>
 <button onclick="location.href='/log.csv'">Download CSV</button>
 <button onclick="toggleLog()">Pause/Resume</button>
 <button class="warn" onclick="clr()">Clear log</button>
</div>
<div id="msg"></div>
<script>
let st=null;
const $=id=>document.getElementById(id);
function say(t){$('msg').textContent=t}
async function sync(){try{let r=await fetch('/api/sync?t='+Date.now());say(await r.text())}catch(e){say('sync failed')}}
async function go(){let r=await fetch('/api/go?note='+encodeURIComponent($('note').value));say(await r.text());if(st&&!st.stnActive)$('note').value=''}
async function mark(){let n=prompt('Note for this spot (optional):','')||'';let r=await fetch('/api/mark?note='+encodeURIComponent(n));say(await r.text())}
async function toggleLog(){let r=await fetch('/api/log?on='+(st&&st.logging?0:1));say(await r.text())}
async function clr(){if(!confirm('Erase the whole log on the device?'))return;let r=await fetch('/api/clear',{method:'POST'});say(await r.text())}
function draw(s){
 const c=$('c'),g=c.getContext('2d'),W=c.width,H=c.height;g.clearRect(0,0,W,H);
 const lo=-130,hi=-50,n=s.spec.length;if(!n)return;
 const y=v=>H-20-(Math.max(lo,Math.min(hi,v))-lo)/(hi-lo)*(H-30);
 g.strokeStyle='#33404d';g.fillStyle='#9aa5b1';g.font='12px sans-serif';
 for(let d=lo;d<=hi;d+=20){g.beginPath();g.moveTo(30,y(d));g.lineTo(W,y(d));g.stroke();g.fillText(d,0,y(d)+4)}
 const bw=(W-32)/n;
 for(let i=0;i<n;i++){const v=s.spec[i];g.fillStyle=v>s.floor+10?'#e8a33d':'#2f9e8f';g.fillRect(31+i*bw,y(v),Math.max(1,bw-0.5),H-20-y(v))}
 g.fillStyle='#9aa5b1';g.fillText(s.f0.toFixed(0)+' MHz',32,H-4);g.fillText((s.f0+s.step*(n-1)).toFixed(0)+' MHz',W-60,H-4);
 g.strokeStyle='#e66';g.setLineDash([4,4]);g.beginPath();g.moveTo(30,y(s.floor));g.lineTo(W,y(s.floor));g.stroke();g.setLineDash([]);
}
async function poll(){
 try{let r=await fetch('/api/status');st=await r.json();
  $('floor').textContent=st.floor.toFixed(1)+' dBm';$('med').textContent=st.median.toFixed(1)+' dBm';
  $('peak').textContent=st.peak.toFixed(1)+' @'+st.peakFreq.toFixed(1);$('busy').textContent=st.busy+' / '+st.spec.length;
  $('clk').innerHTML=st.synced?'<span class="ok">synced</span>':'<span class="bad">NOT synced</span>';
  $('gps').textContent=st.gpsChars?(st.gpsFix?('fix '+st.sats+' sats'):'no fix'):'none';
  $('log').textContent=st.rows+' / '+Math.round(100*st.fsUsed/st.fsTotal)+'%';
  $('lg').innerHTML=st.logging?'<span class="ok">ON</span>':'<span class="bad">PAUSED</span>';
  const b=$('go');
  if(st.stnActive){b.textContent='STOP & SAVE · '+st.stnLeft+' s left';b.className='go run';
   $('stnInfo').textContent='Station '+st.stnId+' running ('+st.stnSweeps+' sweeps). Keep still.';}
  else{b.textContent='GO · '+st.stnSecs+' s baseline';b.className='go';
   if(st.last)$('stnInfo').innerHTML='Last station <b>'+st.last.id+'</b>: floor <b>'+st.last.floor.toFixed(1)+'</b> dBm, median '+st.last.median.toFixed(1)+', peak '+st.last.peak.toFixed(1)+' @ '+st.last.peakFreq.toFixed(1)+' MHz, busy '+st.last.busy+' ch ('+st.last.sweeps+' sweeps)';}
  draw(st);
 }catch(e){}
 setTimeout(poll,st&&st.stnActive?1000:1500);
}
sync();poll();
</script></body></html>)HTML";
