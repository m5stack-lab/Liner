#pragma once

// The dashboard page, served as-is from flash. Everything dynamic (status,
// now playing, settings) comes from /api/status as JSON and is inserted with
// textContent, so device data such as SSIDs or track titles is never parsed
// as HTML. Mobile first: one column, thumb-reach bottom tabs, 16px inputs (no
// iOS zoom on focus), 44px+ tap targets, dark mode, no external assets.
static const char DASHBOARD_HTML[] PROGMEM = R"LINERHTML(<!DOCTYPE html>
<html lang="en"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#f5f4f0" media="(prefers-color-scheme: light)">
<meta name="theme-color" content="#141413" media="(prefers-color-scheme: dark)">
<title>Liner</title>
<style>
:root{--bg:#f5f4f0;--card:#fff;--ink:#1a1a19;--muted:#6b6a65;--line:#e4e2dc;--fill:#efeee9;
--accent:#1a1a19;--on-accent:#fff;--ok:#2f7d4f;--warn:#b4561c;--bad:#b3261e;--r:16px;}
@media (prefers-color-scheme:dark){:root{--bg:#141413;--card:#1e1e1c;--ink:#f2f1ec;--muted:#9c9b95;
--line:#2e2e2b;--fill:#272725;--accent:#f2f1ec;--on-accent:#141413;--ok:#5cc489;--warn:#e39a5b;--bad:#f2867d;}}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html{-webkit-text-size-adjust:100%}
body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.45 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
padding-bottom:calc(76px + env(safe-area-inset-bottom))}
header{position:sticky;top:0;z-index:5;background:var(--bg);display:flex;align-items:center;justify-content:space-between;
padding:calc(12px + env(safe-area-inset-top)) 16px 12px}
header h1{font-size:22px;margin:0;letter-spacing:-.01em}
.pill{display:inline-flex;align-items:center;gap:6px;font-size:13px;font-weight:600;padding:6px 12px;border-radius:99px;background:var(--fill)}
.pill i{width:8px;height:8px;border-radius:50%;background:var(--muted)}
.pill.ok i{background:var(--ok)}.pill.warn i{background:var(--warn)}.pill.bad i{background:var(--bad)}
main{max-width:560px;margin:0 auto;padding:0 16px}
section[data-tab]{display:none}section[data-tab].on{display:block}
.card{background:var(--card);border-radius:var(--r);padding:16px;margin:0 0 14px;border:1px solid var(--line)}
.card h2{font-size:13px;text-transform:uppercase;letter-spacing:.06em;color:var(--muted);margin:0 0 10px;font-weight:700}
.np-art{width:100%;aspect-ratio:1;border-radius:12px;background:var(--fill);object-fit:cover;display:block}
.qrs{display:flex;flex-wrap:wrap;justify-content:center;gap:16px}
.qr{flex:1 1 150px;max-width:220px;text-align:center;margin:0}
.qr img{display:block;width:100%;aspect-ratio:1;background:#fff;border-radius:12px;padding:6px;border:1px solid var(--line)}
.qr figcaption{font-size:14px;font-weight:600;margin-top:8px}.qr figcaption small{display:block;color:var(--muted);font-weight:400}
.np-ph{display:flex;align-items:center;justify-content:center;font-size:64px;color:var(--muted)}
.np-title{font-size:21px;font-weight:700;margin:14px 0 2px;line-height:1.25}
.np-artist{font-size:17px}.np-album{color:var(--muted);font-size:15px}
.np-meta{display:inline-block;margin-top:10px;font-size:13px;font-weight:600;padding:4px 10px;border-radius:8px;background:var(--fill)}
.np-idle{text-align:center;color:var(--muted);padding:36px 0}
.np-idle b{display:block;color:var(--ink);font-size:18px;margin-bottom:4px}
.banner{border-radius:12px;padding:12px 14px;margin:0 0 14px;font-size:15px;background:var(--fill);border-left:4px solid var(--warn)}
.rows{margin:0;padding:0;list-style:none}
.rows li{display:flex;justify-content:space-between;gap:12px;padding:11px 0;border-top:1px solid var(--line);font-size:15px}
.rows li:first-child{border-top:0;padding-top:2px}
.rows span{color:var(--muted)}.rows b{font-weight:600;text-align:right;overflow-wrap:anywhere}
.tog{display:flex;align-items:center;gap:14px;min-height:56px;padding:6px 0;border-top:1px solid var(--line)}
.tog:first-of-type{border-top:0}
.tog div{flex:1;min-width:0}.tog strong{display:block;font-size:16px}.tog small{display:block;color:var(--muted);font-size:13px}
.sw{position:relative;width:52px;height:32px;flex-shrink:0}
.sw input{position:absolute;opacity:0;inset:0;margin:0;cursor:pointer;z-index:1}
.sw span{position:absolute;inset:0;border-radius:32px;background:var(--line);transition:background .2s}
.sw span:before{content:"";position:absolute;left:3px;top:3px;width:26px;height:26px;border-radius:50%;background:#fff;
box-shadow:0 1px 3px rgba(0,0,0,.25);transition:transform .2s}
.sw input:checked+span{background:var(--ok)}.sw input:checked+span:before{transform:translateX(20px)}
.sw input:focus-visible+span{outline:2px solid var(--accent);outline-offset:2px}
.sw input:disabled+span{opacity:.5}
code{font:13px ui-monospace,SFMono-Regular,Menlo,monospace;background:var(--fill);padding:2px 6px;border-radius:6px;overflow-wrap:anywhere}
label{display:block;font-weight:600;font-size:14px;margin:14px 0 6px}
input[type=text],input[type=password]{width:100%;font-size:16px;padding:12px 14px;border-radius:12px;border:1px solid var(--line);
background:var(--bg);color:var(--ink);min-height:48px}
input:focus{outline:2px solid var(--accent);outline-offset:-1px;border-color:transparent}
.pw{position:relative}.pw input{padding-right:64px}
.pw button{position:absolute;right:4px;top:4px;bottom:4px;width:auto;margin:0;padding:0 12px;font-size:14px;background:none;color:var(--muted);min-height:0}
button{font:inherit;font-weight:600;width:100%;min-height:50px;margin-top:16px;border:0;border-radius:12px;background:var(--accent);color:var(--on-accent);cursor:pointer}
button.ghost{background:var(--fill);color:var(--ink)}
button:disabled{opacity:.55}
.sub{margin-top:8px;padding:2px 0 4px}
.nets{list-style:none;margin:10px 0 0;padding:0;max-height:280px;overflow:auto;border-radius:12px;border:1px solid var(--line)}
.nets li{display:flex;justify-content:space-between;align-items:center;padding:0 14px;min-height:48px;border-top:1px solid var(--line);cursor:pointer}
.nets li:first-child{border-top:0}.nets li small{color:var(--muted)}
.hint{color:var(--muted);font-size:13px;margin:8px 0 0}
details{border-top:1px solid var(--line)}details:first-of-type{border-top:0}
summary{list-style:none;cursor:pointer;font-weight:600;min-height:52px;display:flex;align-items:center;justify-content:space-between}
summary::-webkit-details-marker{display:none}
summary:after{content:"+";font-size:22px;color:var(--muted);font-weight:400}details[open] summary:after{content:"\2212"}
details p{margin:0 0 14px;color:var(--muted);font-size:15px}
a{color:var(--ink)}
nav{position:fixed;left:0;right:0;bottom:0;z-index:6;display:flex;background:var(--card);border-top:1px solid var(--line);
padding-bottom:env(safe-area-inset-bottom)}
nav a{flex:1;display:flex;flex-direction:column;align-items:center;gap:2px;padding:9px 0 10px;font-size:12px;font-weight:600;
color:var(--muted);text-decoration:none}
nav a svg{width:24px;height:24px;fill:none;stroke:currentColor;stroke-width:1.8;stroke-linecap:round;stroke-linejoin:round}
nav a.on{color:var(--ink)}
#toast{position:fixed;left:50%;bottom:calc(86px + env(safe-area-inset-bottom));transform:translate(-50%,20px);opacity:0;
background:var(--accent);color:var(--on-accent);padding:10px 16px;border-radius:12px;font-size:14px;font-weight:600;
transition:.25s;pointer-events:none;max-width:calc(100% - 32px);z-index:7}
#toast.show{opacity:1;transform:translate(-50%,0)}
[hidden]{display:none!important}
.bar{display:flex;align-items:center;justify-content:space-between;gap:16px;width:100%;max-width:1032px;margin:0 auto}
/* Tablet and up: tabs move into the header, cards flow into two columns. */
@media (min-width:760px){
body{padding-bottom:32px}
header{padding:16px 24px;border-bottom:1px solid var(--line);margin-bottom:24px}
nav{position:static;flex:0 1 auto;background:var(--fill);border:0;border-radius:12px;padding:4px;gap:4px;margin-left:auto}
nav a{flex:0 0 auto;flex-direction:row;gap:8px;padding:8px 16px;font-size:14px;border-radius:9px}
nav a svg{width:18px;height:18px}
nav a.on{background:var(--card);box-shadow:0 1px 2px rgba(0,0,0,.08)}
main{max-width:1080px;padding:0 24px}
section[data-tab].on{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);gap:20px;align-items:start}
section[data-tab] .card,.banner{margin:0}
.banner,section[data-tab="help"] .card{grid-column:1/-1}
section[data-tab="help"] .card{columns:2;column-gap:40px}
section[data-tab="help"] .card h2{column-span:all}
section[data-tab="help"] details{break-inside:avoid}
#toast{bottom:24px}
}
/* Desktop: now playing becomes art beside text. */
@media (min-width:1100px){
section[data-tab="now"].on{grid-template-columns:minmax(0,3fr) minmax(0,2fr)}
#np:not(:has(.np-idle)){display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);column-gap:24px;align-content:start}
#np .np-art{grid-row:1/span 5}
#np .np-title{margin-top:4px}
#np .np-meta{justify-self:start;align-self:start}
}
</style></head><body>
<header><div class="bar"><h1>Liner</h1>
<nav>
  <a href="#now" data-nav="now"><svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="2.5"/></svg>Now</a>
  <a href="#settings" data-nav="settings"><svg viewBox="0 0 24 24"><path d="M4 7h10M18 7h2M4 17h4M12 17h8"/><circle cx="16" cy="7" r="2"/><circle cx="10" cy="17" r="2"/></svg>Settings</a>
  <a href="#help" data-nav="help"><svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M9.5 9.5a2.5 2.5 0 1 1 3.3 2.4c-.5.2-.8.6-.8 1.1v.5M12 17h.01"/></svg>Help</a>
</nav>
<span class="pill" id="pill"><i></i><span id="pillText">Loading</span></span></div></header>
<main>
<section data-tab="now">
  <div class="banner" id="banner" hidden></div>
  <div class="card" id="np"><div class="np-idle"><b>Loading&hellip;</b></div></div>
  <div class="card"><h2>Device</h2><ul class="rows" id="dev"></ul></div>
</section>

<section data-tab="settings">
  <div class="card"><h2>Options</h2>
    <div class="tog"><div><strong>OTA updates</strong><small id="otaNote">Push firmware over Wi-Fi</small></div>
      <label class="sw"><input type="checkbox" id="ota" aria-label="OTA updates"><span></span></label></div>
    <div class="tog"><div><strong>Idle screensaver</strong><small>Patterns after 5 min of nothing playing</small></div>
      <label class="sw"><input type="checkbox" id="screensaver" aria-label="Idle screensaver"><span></span></label></div>
    <div class="tog"><div><strong>Password protection</strong><small id="authNote">Anyone on your network can open this page</small></div>
      <label class="sw"><input type="checkbox" id="auth" aria-label="Password protection"><span></span></label></div>
    <form class="sub" id="authForm" hidden>
      <label for="pw1" id="pw1Label">New password</label>
      <div class="pw"><input type="password" id="pw1" autocomplete="new-password" minlength="4" maxlength="64" required><button type="button" data-show="pw1">Show</button></div>
      <label for="pw2">Confirm password</label>
      <input type="password" id="pw2" autocomplete="new-password" minlength="4" maxlength="64" required>
      <p class="hint">Username is <code>liner</code>. Forgot it? Hold the device's top button for 5 seconds to remove the password.</p>
      <button type="submit" id="authBtn">Set password</button>
      <button type="button" class="ghost" id="authCancel">Cancel</button>
    </form>
  </div>

  <div class="card" id="wifiCard"><h2>Wi-Fi &amp; Volumio</h2>
    <form id="netForm">
      <button type="button" class="ghost" id="scanBtn" style="margin-top:0">Scan for networks</button>
      <ul class="nets" id="nets" hidden></ul>
      <label for="ssid">Network name</label>
      <input type="text" id="ssid" autocomplete="off" autocapitalize="none" spellcheck="false" required>
      <label for="pass">Wi-Fi password</label>
      <div class="pw"><input type="password" id="pass" placeholder="Leave blank to keep current" autocomplete="off"><button type="button" data-show="pass">Show</button></div>
      <label for="vhost">Volumio host</label>
      <input type="text" id="vhost" autocomplete="off" autocapitalize="none" spellcheck="false" placeholder="volumio.local">
      <button type="submit" id="netBtn">Save network</button>
    </form>
  </div>

  <div class="card" id="qrCard" hidden><h2>Scan to open</h2><div class="qrs" id="qrs"></div></div>
</section>

<section data-tab="help">
  <div class="card"><h2>Troubleshooting</h2>
    <details><summary>The screen isn't updating</summary><p>A full e-paper refresh takes 15&ndash;30 seconds and only happens when the track changes. A pause is normal, not a hang.</p></details>
    <details><summary>The screen shows a QR code</summary><p>Liner couldn't reach Wi-Fi or Volumio. Scan the code with your phone to get back to this page. It keeps retrying on its own and returns to Now Playing once the network is back.</p></details>
    <details><summary>No album art is showing</summary><p>Only Tidal's album art is fully verified so far. Local files and Spotify may not resolve art yet; playback info still works.</p></details>
    <details><summary>Wi-Fi won't connect</summary><p>Double-check the password. Press the device's top button any time to reopen this page over its own <code>Liner-Setup</code> Wi-Fi network.</p></details>
    <details><summary>Updating firmware over the air</summary><p>Turn on OTA updates in Settings, then run <code>pio run -t upload --upload-port liner.local</code></p></details>
    <details><summary>Forgot the web password</summary><p>Hold the device's top button for 5 seconds. The password is removed and the screen confirms it.</p></details>
    <details><summary>Reporting an issue</summary><p>Include the firmware version from the Now tab and what the device's screen shows. <a href="https://github.com/m5stack-lab/Liner/issues">Open an issue on GitHub</a>.</p></details>
  </div>
</section>
</main>

<div id="toast" role="status" aria-live="polite"></div>

<script>
const $=id=>document.getElementById(id);
let S=null,formLoaded=false,paused=false,authEditing=false,toastT;

function el(tag,cls,text){const e=document.createElement(tag);if(cls)e.className=cls;if(text!=null)e.textContent=text;return e}
function toast(msg){const t=$('toast');t.textContent=msg;t.classList.add('show');clearTimeout(toastT);toastT=setTimeout(()=>t.classList.remove('show'),2600)}
async function api(path,body){
  const o={headers:{'X-Liner':'1'},cache:'no-store'};
  if(body){o.method='POST';o.body=new URLSearchParams(body)}
  const r=await fetch(path,o);
  const txt=await r.text();
  if(!r.ok)throw new Error(txt||('HTTP '+r.status));
  return txt?JSON.parse(txt):{};
}

function tab(name){
  if(!['now','settings','help'].includes(name))name='now';
  document.querySelectorAll('section[data-tab]').forEach(s=>s.classList.toggle('on',s.dataset.tab===name));
  document.querySelectorAll('nav a').forEach(a=>a.classList.toggle('on',a.dataset.nav===name));
  scrollTo(0,0);
}
addEventListener('hashchange',()=>tab(location.hash.slice(1)));

function fmtUptime(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);
  return d?d+'d '+h+'h':h?h+'h '+m+'m':m+'m'}
function signal(r){return r>=-60?'Strong':r>=-70?'Good':r>=-80?'Fair':'Weak'}

// Rebuilt only when the track (or mode) changes, so the art isn't re-fetched every poll.
function renderNowPlaying(s){
  const np=$('np'),key=JSON.stringify([s.np,s.mode]);
  if(np.dataset.key===key)return;
  np.dataset.key=key;np.replaceChildren();
  if(s.np.playing){
    const ph=()=>el('div','np-art np-ph','\u266A');
    let art=ph();
    if(s.np.art){art=el('img','np-art');art.alt='';art.src=s.np.art;art.onerror=()=>art.replaceWith(ph())}
    np.append(art,el('div','np-title',s.np.title),el('div','np-artist',s.np.artist),el('div','np-album',s.np.album));
    if(s.np.meta)np.append(el('span','np-meta',s.np.meta));
  }else{
    const d=el('div','np-idle');d.append(el('b',null,'Nothing playing'),
      document.createTextNode(s.mode==='online'?'Start something in Volumio and it shows up here.':'Waiting for the network.'));
    np.append(d);
  }
}

// Liner's address as a QR code: its setup-network IP while no network is
// assigned, its LAN IP once connected. Rebuilt only when mode or IP changes.
function renderQr(s){
  const card=$('qrCard'),key=s.mode+'|'+s.ip;
  if(card.dataset.key===key)return;
  card.dataset.key=key;
  card.hidden=!s.ip;
  if(!s.ip)return;
  const f=el('figure','qr'),img=el('img');
  img.alt='QR code for http://'+s.ip+'/';img.src='/api/qr.svg?t='+Date.now(); // fresh URL: never show a cached code from an older address or firmware
  const c=el('figcaption',null,s.mode==='setup'?'Open setup on another device':'Open on your phone');
  c.append(el('small',null,'http://'+s.ip+'/'+(s.mode==='setup'?' on '+s.ssid:'')));
  f.append(img,c);$('qrs').replaceChildren(f);
}

function render(s){
  S=s;
  const pill=$('pill');let cls='ok',txt='Connected';
  if(s.mode==='setup'){cls='warn';txt='Setup mode'}
  else if(s.mode!=='online'){cls='bad';txt='Offline'}
  else if(!s.volumio){cls='warn';txt='No Volumio'}
  else if(s.np.playing){txt='Playing'}
  pill.className='pill '+cls;$('pillText').textContent=txt;

  const b=$('banner');b.hidden=true;
  if(s.mode==='setup'){b.hidden=false;b.textContent='Liner isn’t on your Wi-Fi yet. Choose a network in Settings to finish setup.'}
  else if(s.mode==='online'&&!s.volumio){b.hidden=false;b.textContent='Can’t reach Volumio at '+s.vhost+'. Check the host in Settings, or that Volumio is running.'}

  renderNowPlaying(s);
  renderQr(s);

  const rows=[['Network',s.ssid||'—'],['IP address',s.ip||'—']];
  if(s.mode==='online'){rows.push(['Address',s.hostname]);if(s.rssi!=null)rows.push(['Signal',signal(s.rssi)+' ('+s.rssi+' dBm)'])}
  rows.push(['Volumio',s.vhost],['Firmware','v'+s.version],['Uptime',fmtUptime(s.uptime)]);
  const ul=$('dev');ul.replaceChildren(...rows.map(([k,v])=>{const li=el('li');li.append(el('span',null,k),el('b',null,v));return li}));

  $('ota').checked=s.ota;$('screensaver').checked=s.screensaver;
  $('otaNote').textContent=s.ota?(s.otaActive?'On: pio run -t upload --upload-port '+s.hostname:'On, starts once Liner is on Wi-Fi'):'Off: USB flashing only';
  if(!authEditing){$('auth').checked=s.auth}
  $('authNote').textContent=s.auth?'On: username liner':'Anyone on your network can open this page';
  if(!formLoaded){formLoaded=true;$('ssid').value=s.savedSsid;$('vhost').value=s.vhost;
    if(s.mode==='setup'&&!location.hash)tab('settings')}
}

async function poll(){
  if(paused||document.hidden)return;
  try{render(await api('/api/status'))}catch(e){$('pill').className='pill bad';$('pillText').textContent='Unreachable'}
}

async function setOption(key,input){
  const v=input.checked;input.disabled=true;
  try{render(await api('/api/options',{[key]:v?'1':'0'}));toast((key==='ota'?'OTA updates ':'Screensaver ')+(v?'on':'off'))}
  catch(e){input.checked=!v;toast('Couldn’t save: '+e.message)}
  finally{input.disabled=false}
}
$('ota').onchange=e=>setOption('ota',e.target);
$('screensaver').onchange=e=>setOption('screensaver',e.target);

function showAuthForm(on){authEditing=on;$('authForm').hidden=!on;$('pw1').value=$('pw2').value='';
  if(on){$('pw1Label').textContent=S&&S.auth?'New password':'Password';$('authBtn').textContent=S&&S.auth?'Change password':'Set password';$('pw1').focus()}}
$('auth').onchange=async e=>{
  if(e.target.checked){showAuthForm(true);return}
  if(!S||!S.auth){showAuthForm(false);return}
  if(!confirm('Remove the password? Anyone on your network will be able to open this page.')){e.target.checked=true;return}
  e.target.disabled=true;
  try{await api('/api/auth',{enabled:'0'});showAuthForm(false);toast('Password removed');poll()}
  catch(err){e.target.checked=true;toast('Couldn’t save: '+err.message)}
  finally{e.target.disabled=false}
};
$('authNote').parentElement.onclick=()=>{if(S&&S.auth&&!authEditing)showAuthForm(true)};
$('authCancel').onclick=()=>{showAuthForm(false);$('auth').checked=!!(S&&S.auth)};
$('authForm').onsubmit=async e=>{
  e.preventDefault();
  const a=$('pw1').value,b=$('pw2').value;
  if(a.length<4){toast('Use at least 4 characters');return}
  if(a!==b){toast('Passwords don’t match');return}
  $('authBtn').disabled=true;
  try{await api('/api/auth',{enabled:'1',password:a});showAuthForm(false);$('auth').checked=true;
    toast('Password set. Sign in as “liner”');setTimeout(poll,600)}
  catch(err){toast('Couldn’t save: '+err.message)}
  finally{$('authBtn').disabled=false}
};

document.querySelectorAll('[data-show]').forEach(btn=>btn.onclick=()=>{
  const i=$(btn.dataset.show);const show=i.type==='password';i.type=show?'text':'password';btn.textContent=show?'Hide':'Show'});

$('scanBtn').onclick=async()=>{
  const btn=$('scanBtn'),list=$('nets');btn.disabled=true;btn.textContent='Scanning…';
  try{
    const nets=await api('/api/scan');
    list.replaceChildren(...nets.sort((a,b)=>b.rssi-a.rssi).map(n=>{
      const li=el('li');li.append(el('span',null,n.ssid),el('small',null,(n.secure?'🔒 ':'')+signal(n.rssi)));
      li.onclick=()=>{$('ssid').value=n.ssid;list.hidden=true;$('pass').focus()};return li}));
    list.hidden=!nets.length;if(!nets.length)toast('No networks found');
  }catch(e){toast('Scan failed: '+e.message)}
  finally{btn.disabled=false;btn.textContent='Scan again'}
};

$('netForm').onsubmit=async e=>{
  e.preventDefault();
  const btn=$('netBtn');btn.disabled=true;
  try{
    const r=await api('/save',{ssid:$('ssid').value.trim(),password:$('pass').value,vhost:$('vhost').value.trim()});
    $('pass').value='';
    if(r.reconnecting){paused=true;
      $('banner').hidden=false;$('banner').textContent='Liner is joining “'+r.ssid+'”. Reconnect your phone to that network, then open http://liner.local';
      tab('now');toast('Saved. Reconnecting…')}
    else{toast('Saved');poll()}
  }catch(err){toast('Couldn’t save: '+err.message)}
  finally{btn.disabled=false}
};

tab(location.hash.slice(1));
poll();setInterval(poll,5000);
document.addEventListener('visibilitychange',poll);
</script>
</body></html>)LINERHTML";
