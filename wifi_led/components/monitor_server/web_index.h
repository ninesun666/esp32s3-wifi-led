#pragma once

/* Served verbatim. Kept out of monitor_server.c so the page stays editable as
 * markup rather than as C escapes; raw string literal means no backslash
 * continuation and no % escaping. */
static const char k_index_html[] = R"HTML(<!doctype html><html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<link rel="icon" href="data:,">
<title>ESP32-S3 监控</title>
<style>
:root{--bg:#0e1116;--card:#161b22;--line:#232b36;--fg:#e6edf3;--dim:#8b949e}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.5 system-ui,"Microsoft YaHei",sans-serif;padding:18px}
h1{font-size:17px;margin:0 0 14px;font-weight:600}
.pill{display:inline-flex;align-items:center;gap:8px;padding:6px 14px;border-radius:999px;font-weight:600;font-size:13px}
.dot{width:9px;height:9px;border-radius:50%}
.on{background:#10331f}.on .dot{background:#3fb950;box-shadow:0 0 10px #3fb950}
.warn{background:#3a2c10}.warn .dot{background:#d29922;box-shadow:0 0 10px #d29922}
.off{background:#3a1414}.off .dot{background:#f85149;box-shadow:0 0 10px #f85149}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;margin-top:14px}
.c{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px}
.k{color:var(--dim);font-size:12px;margin-bottom:4px}
.v{font-size:19px;font-weight:600;font-variant-numeric:tabular-nums;word-break:break-all}
.v small{font-size:12px;color:var(--dim);font-weight:400}
.spark{display:flex;align-items:flex-end;gap:2px;height:46px;margin-top:8px}
.spark i{flex:1;background:#1f6feb;border-radius:2px 2px 0 0;min-height:2px;opacity:.85}
.foot{color:var(--dim);font-size:12px;margin-top:14px}
.btns{display:flex;flex-wrap:wrap;gap:8px;margin-top:8px}
.btns button{background:#21262d;color:var(--fg);border:1px solid var(--line);border-radius:8px;padding:7px 14px;font:inherit;font-size:13px;cursor:pointer}
.btns button:hover{background:#30363d}
</style></head><body>
<h1>ESP32-S3 &middot; Wi-Fi 状态监控</h1>
<div id="pill" class="pill off"><span class="dot"></span><span id="st">连接中</span></div>
<div class="grid">
<div class="c"><div class="k">IP 地址</div><div class="v"><span id="ip">-</span></div></div>
<div class="c"><div class="k">SSID / 加密</div><div class="v"><span id="ssid">-</span> <small id="auth"></small></div></div>
<div class="c"><div class="k">信号强度</div><div class="v"><span id="rssi">-</span> <small>dBm</small></div></div>
<div class="c"><div class="k">信道</div><div class="v"><span id="ch">-</span></div></div>
<div class="c"><div class="k">运行时长</div><div class="v"><span id="up">-</span></div></div>
<div class="c"><div class="k">内部内存空闲</div><div class="v"><span id="hi">-</span> <small>KiB</small></div></div>
<div class="c"><div class="k">PSRAM 空闲</div><div class="v"><span id="hp">-</span> <small>MiB</small></div></div>
<div class="c"><div class="k">外网连通</div><div class="v"><span id="net">-</span></div></div>
<div class="c"><div class="k">板子热点</div><div class="v"><span id="aps">-</span> <small id="api"></small></div></div>
<div class="c"><div class="k">接入板子的设备</div><div class="v"><span id="apc">-</span></div></div>
<div class="c"><div class="k">外接屏幕</div><div class="v"><span id="scr">-</span></div></div>
<div class="c" id="i2ccard" style="display:none"><div class="k">I2C 总线（GPIO 8/9）</div><div class="v"><span id="i2c">-</span> <small id="i2cv"></small></div></div>
</div>
<div class="c" style="margin-top:10px"><div class="k">信号历史（最近 60 次采样）</div><div class="spark" id="sp"></div></div>
<div class="c" style="margin-top:10px"><div class="k">连接到板子热点的终端</div><div id="cl" style="color:#8b949e;font-size:13px">无</div></div>
<div class="c" style="margin-top:10px">
<div class="k">手动点亮（POST /api/led，默认保持 5 秒后交还状态机）</div>
<div class="btns">
<button onclick="setLed(200,40,40)">红</button>
<button onclick="setLed(40,200,40)">绿</button>
<button onclick="setLed(40,80,220)">蓝</button>
<button onclick="setLed(200,200,40)">黄</button>
<button onclick="setLed(0,0,0,0)">交还自动</button>
</div>
<div id="ledmsg" style="color:#8b949e;font-size:12px;margin-top:6px"></div>
</div>
<div class="c" style="margin-top:10px">
<div class="k">手动刷屏（POST /api/screen，整屏纯色，一直保留到下一次刷）</div>
<div class="btns">
<button onclick="setScreen(0,0,0)">黑</button>
<button onclick="setScreen(255,255,255)">白</button>
<button onclick="setScreen(224,40,40)">红</button>
<button onclick="setScreen(40,200,40)">绿</button>
<button onclick="setScreen(40,90,240)">蓝</button>
<button onclick="setScreen(230,180,40)">黄</button>
<button onclick="paintScreen({pattern:'bars'})">红绿蓝三色条</button>
</div>
<div id="scrmsg" style="color:#8b949e;font-size:12px;margin-top:6px"></div>
</div>
<div class="c" style="margin-top:10px" id="emotecard">
<div class="k">屏上表情（esp_emote 动画，资源在 anim_icon 分区）</div>
<div class="btns">
<button onclick="setEmote('happy')">开心</button>
<button onclick="setEmote('laughing')">大笑</button>
<button onclick="setEmote('loving')">喜欢</button>
<button onclick="setEmote('relaxed')">放松</button>
<button onclick="setEmote('sad')">难过</button>
<button onclick="setEmote('crying')">大哭</button>
<button onclick="setEmote('angry')">生气</button>
<button onclick="setEmote('surprised')">惊讶</button>
<button onclick="setEmote('shocked')">震惊</button>
<button onclick="setEmote('thinking')">思考</button>
<button onclick="setEmote('confused')">困惑</button>
<button onclick="setEmote('sleepy')">犯困</button>
<button onclick="setEmote('winking')">眨眼</button>
<button onclick="setEmote('neutral')">平静</button>
<button onclick="setEmote('idle')">待机</button>
</div>
<div id="emsg" style="color:#8b949e;font-size:12px;margin-top:6px"></div>
</div>
<div class="foot" id="foot">等待数据…</div>
<script>
var hist=[];
function $(i){return document.getElementById(i)}
function h(v){return Math.max(2,Math.min(46,(v+95)*46/55))}
async function setLed(r,g,b,hold){
 if(hold===undefined)hold=5000;
 try{
  var res=await fetch('/api/led',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({r:r,g:g,b:b,hold_ms:hold})});
  var ok=res.ok?(hold>0?('已发送 rgb('+r+','+g+','+b+')，保持 '+(hold/1000)+' 秒'):'已交还状态机'):('失败 HTTP '+res.status);
  $('ledmsg').textContent=ok;
 }catch(e){$('ledmsg').textContent='发送失败：'+e}
}
async function paintScreen(body,target){
 try{
  var res=await fetch('/api/screen',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify(body)});
  var d=await res.json().catch(function(){return {}});
  $(target||'scrmsg').textContent=(res.ok&&d.ok)?('已刷屏 '+JSON.stringify(body)):('失败 HTTP '+res.status+' '+(d.error||''));
 }catch(e){$(target||'scrmsg').textContent='发送失败：'+e}
}
function setScreen(r,g,b){paintScreen({r:r,g:g,b:b})}
function setEmote(n){paintScreen({emote:n},'emsg')}
async function tick(){
 try{
  var r=await fetch('/api/status',{cache:'no-store'});var d=await r.json();
  var m={online:['已联网','on'],linked:['仅连上路由','warn'],connecting:['连接中','off']}[d.state];
  $('pill').className='pill '+m[1];$('st').textContent=m[0];
  $('ip').textContent=d.ip;
  $('ssid').textContent=d.ssid;$('auth').textContent=d.auth;
  $('rssi').textContent=d.rssi;$('ch').textContent=d.channel||'-';
  $('up').textContent=Math.floor(d.uptime_s/60)+' 分 '+(d.uptime_s%60)+' 秒';
  $('hi').textContent=Math.round(d.heap_internal/1024);
  $('hp').textContent=(d.heap_psram/1048576).toFixed(1);
  $('net').textContent=d.internet?'通':'不通';
  $('aps').textContent=d.ap_ssid;$('api').textContent=d.ap_ip;
  $('apc').textContent=d.ap_stas;
  $('scr').textContent=d.screen_ready?'ST7789 就绪':'未就绪';
  $('emotecard').style.display=d.emote_ready?'':'none';
  $('i2ccard').style.display=(d.i2c?'':'none');
  $('i2c').textContent=(d.i2c&&d.i2c.length)?d.i2c.join('、'):'无设备';
  $('i2cv').textContent=d.i2c_levels?('空闲电平 SDA '+(d.i2c_levels.sda?'高':'低')+' / SCL '+(d.i2c_levels.scl?'高':'低')):'';
  $('cl').innerHTML=(d.clients||[]).map(function(c){
    return '<div>'+c.mac+' &middot; '+c.rssi+' dBm</div>'}).join('')||'无';
  hist.push(d.rssi);if(hist.length>60)hist.shift();
  $('sp').innerHTML=hist.map(function(v){return '<i style="height:'+h(v)+'px"></i>'}).join('');
  $('foot').textContent='更新于 '+new Date().toLocaleTimeString()+' · '+d.host+' · MAC '+d.mac;
 }catch(e){$('pill').className='pill off';$('st').textContent='读不到板子';$('foot').textContent='请求失败：'+e}
}
tick();setInterval(tick,2000);
</script></body></html>)HTML";
