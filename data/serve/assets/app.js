'use strict';

const $ = (id) => document.getElementById(id);
let lastStatus = null;
let configLoaded = false;
let modalAction = null;
let scanTimer = 0;

function iconHtml(id, cls = 'icon icon-sm') { return `<svg class="${cls}"><use href="#${id}"/></svg>`; }
function num(v, digits = 1) { const n = Number(v); return Number.isFinite(n) ? n.toFixed(digits) : '--'; }
function setText(id, value) { const el = $(id); if (el) el.textContent = value; }

function showToast(message, type = 'ok', title = 'DMK SOLUÇÕES SMART') {
  const wrap = $('toastWrap');
  wrap.innerHTML = '';
  const el = document.createElement('div');
  el.className = `toast-msg ${type}`;
  el.innerHTML = `${iconHtml(type === 'ok' ? 'i-check' : type === 'err' ? 'i-x' : 'i-info')}<div><div class="toast-title"></div><div class="toast-text"></div></div><button class="toast-close" aria-label="Fechar">${iconHtml('i-x')}</button>`;
  el.querySelector('.toast-title').textContent = title;
  el.querySelector('.toast-text').textContent = message;
  el.querySelector('.toast-close').onclick = () => el.remove();
  wrap.appendChild(el);
  window.setTimeout(() => el.remove(), 5200);
}

async function api(url, options = {}) {
  // Se a senha do painel foi digitada (aba OTA), envia junto; senão o
  // navegador pede usuário/senha sozinho na primeira ação protegida.
  const pass = $('otaAuthPass')?.value || '';
  const headers = pass ? otaAuthHeaders(options.headers || {}) : (options.headers || {});
  const res = await fetch(url, { cache: 'no-store', ...options, headers });
  const raw = await res.text();
  let data = {};

  try {
    data = raw ? JSON.parse(raw) : {};
  } catch {
    // Algumas rotas antigas do firmware podem responder texto puro.
    // Mostramos esse texto ao usuário em vez de esconder a causa do erro.
    data = { ok: res.ok, message: raw || `Resposta HTTP ${res.status}` };
  }

  if (!res.ok) throw new Error(data.message || `Erro HTTP ${res.status}`);
  return data;
}

function setView(name) {
  document.querySelectorAll('.view').forEach(v => v.classList.toggle('active', v.dataset.view === name));
  document.querySelectorAll('.nav-btn').forEach(b => b.classList.toggle('active', b.dataset.target === name));
  history.replaceState(null, '', `#${name}`);
  window.scrollTo({ top: 0, behavior: 'smooth' });
}

document.querySelectorAll('.nav-btn').forEach(btn => btn.addEventListener('click', () => setView(btn.dataset.target)));

function stateLabel(value, on = 'LIGADA', off = 'DESLIGADA') { return value ? on : off; }
function stateClass(value) { return value ? 'text-success' : 'text-danger'; }

function updateStatus(s) {
  lastStatus = s;
  document.body.classList.toggle('config-mode', !!s.config_mode);
  setText('deviceSubtitle', `${s.device || 'ESP32'} · ${s.config_mode ? 'modo configuração' : 'modo normal'}`);
  setText('firmwareBadge', `v${s.version || '--'}`);
  setText('otaInstalledBadge', `${s.firmware_name || 'Firmware'} v${s.version || '--'}`);
  setText('vR', num(s.vR, 1)); setText('vS', num(s.vS, 1)); setText('vT', num(s.vT, 1));
  setText('currentA', num(s.current_a, 2)); setText('tempC', num(s.temperature_c, 1));
  setText('flowL', num(s.flow_l_min, 2)); setText('rainP', num(s.rain_pct, 0));
  setText('adcR', `${num(s.adcVrmsR, 3)} V`); setText('adcS', `${num(s.adcVrmsS, 3)} V`); setText('adcT', `${num(s.adcVrmsT, 3)} V`);

  const p = $('pumpState'); p.textContent = stateLabel(s.pump); p.className = `state-value ${stateClass(s.pump)}`;
  const l = $('lightState'); l.textContent = stateLabel(s.light); l.className = `state-value ${stateClass(s.light)}`;
  $('pumpBtn').className = `btn ${s.pump ? 'btn-danger' : 'btn-success'} control-btn`;
  $('pumpBtn').querySelector('span').textContent = s.pump ? 'Desligar bomba' : 'Ligar bomba';
  $('lightBtn').className = `btn ${s.light ? 'btn-danger' : 'btn-primary'} control-btn`;
  $('lightBtn').querySelector('span').textContent = s.light ? 'Desligar luz' : 'Ligar luz';

  const online = s.wifi || s.config_mode;
  $('onlineDot').classList.toggle('ok', online);
  setText('onlineText', s.config_mode ? 'Configuração' : s.wifi ? 'Online' : 'Offline');
  setText('wifiStatus', s.wifi ? 'Conectado' : 'Desconectado');
  setText('ipStatus', s.wifi ? s.ip : '--');
  setText('rssiStatus', s.wifi ? `${s.rssi} dBm` : '--');
  setText('apStatus', s.config_mode ? `${s.ap_ssid} · ${s.ap_ip}` : 'Inativo');
  setText('mqttBadge', s.mqtt ? 'MQTT ONLINE' : 'MQTT OFFLINE');
  $('mqttBadge').className = `badge ${s.mqtt ? 'badge-success' : 'badge-danger'}`;
  setText('fwInfo', `${s.firmware_name || 'Firmware'} v${s.version || '--'}`);
  setText('buildInfo', s.build || '--');
  setText('uptimeInfo', `${Math.floor((Number(s.uptime_s) || 0) / 60)} min`);

  $('passwordWarn')?.classList.toggle('d-none', !s.default_password);
  const banner = $('modeBanner');
  if (s.config_mode) { banner.className = 'alert alert-info'; banner.textContent = `Modo de configuração ativo: ${s.ap_ssid} (${s.ap_ip})`; }
  else { banner.className = 'alert alert-info d-none'; banner.textContent = ''; }

  document.querySelectorAll('.config-field').forEach(el => el.disabled = !s.config_mode);
  document.querySelectorAll('.save-config-btn').forEach(el => el.disabled = !s.config_mode);
  $('configLockWifi').classList.toggle('d-none', !!s.config_mode);
  if (s.config_mode && !configLoaded) loadConfig();
}

async function refreshStatus() {
  try { updateStatus(await api('/api/status')); }
  catch (err) { $('onlineDot').classList.remove('ok'); setText('onlineText', 'Sem resposta'); }
}

function assignValue(id, value) { const el = $(id); if (el && document.activeElement !== el) el.value = value ?? ''; }
function assignCheck(id, value) { const el = $(id); if (el) el.checked = !!value; }

async function loadConfig() {
  try {
    const c = await api('/api/config');
    assignValue('devName', c.device_name); assignValue('ssid', c.wifi_ssid);
    assignValue('mqttHost', c.mqtt_host); assignValue('mqttPort', c.mqtt_port); assignValue('mqttUser', c.mqtt_user); assignValue('topic', c.topic);
    assignValue('calR', c.cal_r); assignValue('calS', c.cal_s); assignValue('calT', c.cal_t);
    assignValue('vMin', c.vmin); assignValue('vMax', c.vmax); assignValue('currentCal', c.current_cal); assignValue('flowCal', c.flow_cal); assignValue('rainDry', c.rain_dry); assignValue('rainWet', c.rain_wet);
    assignCheck('relayLow', c.relay_low); assignCheck('lightLow', c.light_low); assignCheck('alarmLow', c.alarm_low); assignCheck('protection', c.protection);
    configLoaded = true;
  } catch (err) { showToast(err.message, 'err', 'Configuração'); }
}

function signalLevel(rssi) { if (rssi >= -55) return 4; if (rssi >= -65) return 3; if (rssi >= -75) return 2; return 1; }
function renderNetworks(networks) {
  const list = $('networkList'); list.innerHTML = '';
  if (!networks.length) { list.innerHTML = '<div class="alert alert-warning">Nenhuma rede encontrada. Tente buscar novamente.</div>'; return; }
  const unique = new Map();
  networks.filter(n => n.ssid).sort((a, b) => b.rssi - a.rssi).forEach(n => { if (!unique.has(n.ssid)) unique.set(n.ssid, n) });
  [...unique.values()].forEach(n => {
    const b = document.createElement('button'); b.type = 'button'; b.className = 'net-item';
    b.innerHTML = `<div class="signal s${signalLevel(n.rssi)}"><i></i><i></i><i></i><i></i></div><div class="net-main"><div class="net-ssid"></div><div class="net-meta">${n.rssi} dBm · ${n.secure ? 'Protegida' : 'Aberta'}</div></div>${n.secure ? iconHtml('i-lock') : ''}`;
    b.querySelector('.net-ssid').textContent = n.ssid;
    b.onclick = () => { $('ssid').value = n.ssid; showToast(`Rede selecionada: ${n.ssid}`, 'ok', 'Wi‑Fi'); };
    list.appendChild(b);
  });
}

async function pollScan() {
  try {
    const data = await api('/api/wifi/scan');
    if (data.scanning) { scanTimer = window.setTimeout(pollScan, 700); return; }
    renderNetworks(data.networks || []);
    $('scanBtn').disabled = false; $('scanBtn').innerHTML = `${iconHtml('i-search')}<span>Buscar redes Wi‑Fi</span>`;
  } catch (err) { $('scanBtn').disabled = false; $('scanBtn').innerHTML = `${iconHtml('i-search')}<span>Buscar redes Wi‑Fi</span>`; showToast(err.message, 'err', 'Wi‑Fi'); }
}

$('scanBtn').addEventListener('click', () => {
  if (scanTimer) clearTimeout(scanTimer);
  $('scanBtn').disabled = true; $('scanBtn').innerHTML = '<span class="spinner"></span><span>Buscando redes...</span>';
  $('networkList').innerHTML = '<div class="text-muted small">Busca em andamento sem bloquear o loop principal.</div>';
  pollScan();
});

$('pumpBtn').addEventListener('click', async () => {
  const state = lastStatus?.pump ? 'OFF' : 'ON';
  try { const d = await api(`/api/pump?state=${state}`, { method: 'POST' }); showToast(d.message, 'ok', 'Bomba'); setTimeout(refreshStatus, 250); }
  catch (err) { showToast(err.message, 'err', 'Bomba'); }
});
$('lightBtn').addEventListener('click', async () => {
  const state = lastStatus?.light ? 'OFF' : 'ON';
  try { const d = await api(`/api/light?state=${state}`, { method: 'POST' }); showToast(d.message, 'ok', 'Luz'); setTimeout(refreshStatus, 200); }
  catch (err) { showToast(err.message, 'err', 'Luz'); }
});

function buildConfigBody() {
  const body = new URLSearchParams();

  // Coleta somente os campos persistentes da configuração geral.
  // Checkboxes são enviados explicitamente como 1/0 para permitir desativá-los.
  document.querySelectorAll('#configForm .config-field[name]').forEach(el => {
    if (el.type === 'checkbox') body.set(el.name, el.checked ? '1' : '0');
    else body.set(el.name, el.value ?? '');
  });

  return body;
}

function validateConfigBeforeSave() {
  const port = Number($('mqttPort')?.value || 0);
  if ($('mqttPort')?.value && (!Number.isInteger(port) || port < 1 || port > 65535)) {
    throw new Error('Porta MQTT inválida. Use um valor entre 1 e 65535.');
  }

  const vMin = Number($('vMin')?.value);
  const vMax = Number($('vMax')?.value);
  if (Number.isFinite(vMin) && Number.isFinite(vMax) && vMin >= vMax) {
    throw new Error('A tensão mínima deve ser menor que a tensão máxima.');
  }

  const rainDry = Number($('rainDry')?.value);
  const rainWet = Number($('rainWet')?.value);
  if (!Number.isInteger(rainDry) || rainDry < 0 || rainDry > 4095 ||
      !Number.isInteger(rainWet) || rainWet < 0 || rainWet > 4095 || rainDry === rainWet) {
    throw new Error('Chuva seco/molhado devem ser valores diferentes entre 0 e 4095.');
  }

  const otaPass = $('otaPass')?.value || '';
  if (otaPass && otaPass.length < 8) {
    throw new Error('A nova senha OTA deve ter no mínimo 8 caracteres.');
  }
}

async function saveConfig(btn) {
  // Não bloqueamos o POST apenas por um estado antigo do navegador.
  // A autorização definitiva é feita pelo próprio ESP na rota /save.
  const originalHtml = btn?.innerHTML || '';
  document.querySelectorAll('.save-config-btn').forEach(b => b.disabled = true);
  if (btn) btn.innerHTML = '<span class="spinner"></span><span>Salvando...</span>';

  try {
    validateConfigBeforeSave();
    const body = buildConfigBody();
    const d = await api('/save', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
      body
    });
    showToast(d.message || 'Configurações salvas. O ESP32 será reiniciado.', 'ok', 'Configuração salva');
    configLoaded = false;
  } catch (err) {
    showToast(err.message, 'err', 'Falha ao salvar');
  } finally {
    document.querySelectorAll('.save-config-btn').forEach(b => b.disabled = false);
    if (btn) btn.innerHTML = originalHtml;
  }
}

document.querySelectorAll('.save-config-btn').forEach(btn => {
  btn.addEventListener('click', () => saveConfig(btn));
});

function openConfirm(title, text, action) { $('modalTitle').textContent = title; $('modalText').textContent = text; modalAction = action; $('confirmModal').classList.remove('d-none'); }
function closeConfirm() { $('confirmModal').classList.add('d-none'); modalAction = null; }
$('modalCancel').onclick = closeConfirm;
$('confirmModal').addEventListener('click', (e) => { if (e.target === $('confirmModal')) closeConfirm(); });
$('modalConfirm').onclick = async () => { const action = modalAction; closeConfirm(); if (action) await action(); };
$('factoryBtn').addEventListener('click', () => openConfirm('Restaurar configuração', 'Esta ação apaga Wi‑Fi, MQTT e calibrações salvas. O ESP32 será reiniciado.', async () => {
  try { const d = await api('/factory', { method: 'POST' }); showToast(d.message, 'ok', 'Restauração'); } catch (err) { showToast(err.message, 'err', 'Restauração'); }
}));



// ---------------------------------------------------------------------------
// OTA LOCAL + OTA REMOTA SEGURA
// ---------------------------------------------------------------------------
function otaAuthHeaders(extra = {}) {
  const user = $('otaAuthUser')?.value || 'admin';
  const pass = $('otaAuthPass')?.value || '';
  let auth = '';

  try { auth = 'Basic ' + btoa(`${user}:${pass}`); }
  catch { auth = ''; }

  return { ...(auth ? { Authorization: auth } : {}), ...extra };
}

async function otaApi(url, options = {}) {
  const headers = otaAuthHeaders(options.headers || {});
  const res = await fetch(url, { cache: 'no-store', ...options, headers });

  let data = {};
  try { data = await res.json(); }
  catch { data = { ok: false, message: `Resposta inválida (${res.status})` }; }

  if (!res.ok) throw new Error(data.message || `Erro HTTP ${res.status}`);
  return data;
}

function otaResult(id, type, text) {
  const el = $(id);
  if (!el) return;
  const cls = type === 'ok' ? 'alert-success' : type === 'warn' ? 'alert-warning' : 'alert-danger';
  el.innerHTML = `<div class="alert ${cls}"></div>`;
  el.firstElementChild.textContent = text;
}

async function loadRemoteOtaConfig(showMessage = false) {
  try {
    const c = await otaApi('/api/ota/remote/config');
    $('otaRepo').value = c.repo || '';
    $('otaManifestUrl').value = c.manifest_url || '';
    $('otaLocalHttp').checked = !!c.allow_local_http;
    $('otaAutoInstall').checked = !!c.auto_install;

    if (showMessage)
      showToast(c.configured ? 'Servidor OTA remoto carregado.' : 'OTA remota ainda não configurada.', c.configured ? 'ok' : 'warn', 'OTA remota');
  } catch (err) {
    if (showMessage) showToast(err.message, 'err', 'OTA remota');
    throw err;
  }
}

$('otaLocalForm')?.addEventListener('submit', async (e) => {
  e.preventDefault();

  const file = $('otaLocalFile').files[0];
  if (!file) return;

  if (!file.name.toLowerCase().endsWith('.bin')) {
    otaResult('otaLocalResult', 'err', 'Selecione um arquivo .bin.');
    return;
  }

  const btn = $('otaLocalBtn');
  btn.disabled = true;
  btn.innerHTML = '<span class="spinner"></span> Enviando e gravando...';
  otaResult('otaLocalResult', 'warn', 'OTA local em andamento. Não desligue o ESP32.');

  try {
    const fd = new FormData();
    fd.append('firmware', file, file.name);

    const r = await fetch('/update', {
      method: 'POST',
      headers: otaAuthHeaders(),
      body: fd
    });

    let d = {};
    try { d = await r.json(); } catch {}

    if (!r.ok) throw new Error(d.message || `Erro HTTP ${r.status}`);

    otaResult('otaLocalResult', 'ok', d.message || 'Atualização local concluída. O ESP32 vai reiniciar.');
  } catch (err) {
    otaResult('otaLocalResult', 'err', err.message || 'Falha na OTA local.');
  } finally {
    btn.disabled = false;
    btn.innerHTML = `${iconHtml('i-upload')} Enviar OTA local`;
  }
});

$('otaRemoteConfigForm')?.addEventListener('submit', async (e) => {
  e.preventDefault();

  const btn = $('otaRemoteSaveBtn');
  btn.disabled = true;
  btn.innerHTML = '<span class="spinner"></span> Salvando...';

  try {
    const body = new URLSearchParams({
      repo: $('otaRepo').value.trim(),
      manifestUrl: $('otaManifestUrl').value.trim(),
      localHttp: $('otaLocalHttp').checked ? '1' : '0',
      autoInstall: $('otaAutoInstall').checked ? '1' : '0',
      token: '',
      caCert: ''
    });

    const d = await otaApi('/api/ota/remote/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
      body
    });

    showToast(d.message || 'Servidor OTA salvo.', 'ok', 'OTA remota');
    $('otaRemoteInstallBtn').disabled = true;
    $('otaRemoteInfo').innerHTML = '';
  } catch (err) {
    showToast(err.message, 'err', 'OTA remota');
  } finally {
    btn.disabled = false;
    btn.innerHTML = `${iconHtml('i-save')} Salvar`;
  }
});

$('otaRemoteLoadBtn')?.addEventListener('click', () => loadRemoteOtaConfig(true).catch(() => {}));

$('otaRemoteCheckBtn')?.addEventListener('click', async () => {
  const btn = $('otaRemoteCheckBtn');
  btn.disabled = true;
  btn.innerHTML = '<span class="spinner"></span> Verificando...';

  try {
    const d = await otaApi('/api/ota/remote/check', { method: 'POST' });

    const state = d.available ? 'Nova versão disponível' :
                  d.same_version ? 'Mesma versão instalada' :
                  'Versão remota não é mais nova';

    $('otaRemoteInfo').innerHTML = `
      <div class="alert ${d.available ? 'alert-success' : 'alert-info'}">
        <b>${state}</b><br>
        Versão remota: ${String(d.version || '--')}<br>
        Tamanho: ${Number(d.size || 0).toLocaleString('pt-BR')} bytes<br>
        SHA‑256: <span style="word-break:break-all">${String(d.sha256 || '--')}</span>
      </div>`;

    $('otaRemoteInstallBtn').disabled = !(d.available || d.same_version);
  } catch (err) {
    $('otaRemoteInstallBtn').disabled = true;
    otaResult('otaRemoteInfo', 'err', err.message);
  } finally {
    btn.disabled = false;
    btn.textContent = 'Verificar atualização';
  }
});

$('otaRemoteInstallBtn')?.addEventListener('click', () => {
  const force = $('otaForceSame').checked;

  openConfirm(
    'Instalar firmware remoto',
    'O ESP32 vai desligar bomba e luz, baixar o firmware do servidor autorizado, conferir SHA-256 e reiniciar se tudo estiver correto.',
    async () => {
      const btn = $('otaRemoteInstallBtn');
      btn.disabled = true;
      btn.innerHTML = '<span class="spinner"></span> Baixando e instalando...';
      otaResult('otaRemoteInfo', 'warn', 'Download/validação em andamento. Não desligue o ESP32.');

      try {
        const body = new URLSearchParams({ force: force ? '1' : '0' });
        await otaApi('/api/ota/remote/install', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
          body
        });
        watchOtaProgress(btn);
      } catch (err) {
        otaResult('otaRemoteInfo', 'err', err.message);
        btn.disabled = false;
        btn.innerHTML = `${iconHtml('i-cloud')} Baixar e instalar versão remota`;
      }
    }
  );
});

// Acompanha a instalação em segundo plano (download, SHA-256, reinício).
function watchOtaProgress(btn) {
  const started = Date.now();
  let sawRestart = false;
  const timer = setInterval(async () => {
    try {
      const s = await api('/api/ota-mqtt/status');
      const o = s.ota || {};
      if (o.phase === 'error' || s.error) {
        clearInterval(timer);
        otaResult('otaRemoteInfo', 'err', o.error || s.error || 'Falha na atualização.');
        btn.disabled = false;
        btn.innerHTML = `${iconHtml('i-cloud')} Baixar e instalar versão remota`;
        return;
      }
      if (o.phase === 'restarting') sawRestart = true;
      otaResult('otaRemoteInfo', 'warn', `${o.message || 'Preparando'} · ${Number(o.progress || 0)}%`);
    } catch {
      if (sawRestart || Date.now() - started > 20000) {
        otaResult('otaRemoteInfo', 'ok', 'Firmware instalado. A placa está reiniciando… a página volta sozinha.');
      }
    }
    if (Date.now() - started > 300000) clearInterval(timer);
  }, 1500);
}

// ---------------------------------------------------------------------------
// CONTROLES REMOTOS (ESP-NOW / REDE LOCAL)
// ---------------------------------------------------------------------------
function escapeHtml(v) { return String(v ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c])); }

async function loadLink() {
  try {
    const d = await api('/api/link');
    setText('linkEspnow', d.espnow ? (d.udp ? 'Ativo · rede local ativa' : 'Ativo') : 'Indisponível');
    setText('linkChannel', `canal ${d.channel || '--'} · ${d.mac || '--'}`);
    setText('linkPairing', d.pairing ? `ABERTO · ${d.pair_remaining_s}s` : 'fechado');
    $('linkPairBtn').innerHTML = d.pairing ? `${iconHtml('i-x')} Encerrar pareamento` : `${iconHtml('i-refresh')} Parear controle`;
    $('linkPairBtn').dataset.open = d.pairing ? '1' : '0';
    if (document.activeElement !== $('linkShare')) $('linkShare').checked = !!d.share_config;
    const box = $('linkControls');
    if (!d.controls || !d.controls.length) {
      box.textContent = 'Nenhum controle pareado ainda.';
    } else {
      box.innerHTML = d.controls.map(c => `<div class="ctl-item"><div><b>${escapeHtml(c.name || 'Controle')}</b><small>${escapeHtml(c.mac)} · ${c.last_seen_s == null ? 'sem contato desde o boot' : c.last_seen_s < 15 ? 'online' : 'visto há ' + c.last_seen_s + ' s'}</small></div><button class="btn btn-light" data-remove="${escapeHtml(c.mac)}">Remover</button></div>`).join('');
      box.querySelectorAll('[data-remove]').forEach(b => b.onclick = () => openConfirm('Remover controle', `Remover ${b.dataset.remove} da lista?`, async () => {
        try { const r = await api('/api/link/remove', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: 'mac=' + encodeURIComponent(b.dataset.remove) }); showToast(r.message, 'ok', 'Controles'); loadLink(); }
        catch (err) { showToast(err.message, 'err', 'Controles'); }
      }));
    }
  } catch { /* status principal ja mostra offline */ }
}

$('linkPairBtn')?.addEventListener('click', async () => {
  const closing = $('linkPairBtn').dataset.open === '1';
  try { const d = await api('/api/link/pair', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: 'state=' + (closing ? 'OFF' : 'ON') }); showToast(d.message, 'ok', 'Pareamento'); loadLink(); }
  catch (err) { showToast(err.message, 'err', 'Pareamento'); }
});

$('linkShare')?.addEventListener('change', async (e) => {
  try { const d = await api('/api/link/share', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: 'enabled=' + (e.target.checked ? '1' : '0') }); showToast(d.message, 'ok', 'Pareamento'); }
  catch (err) { e.target.checked = !e.target.checked; showToast(err.message, 'err', 'Pareamento'); }
});

$('linkRekeyBtn')?.addEventListener('click', () => openConfirm('Trocar chave do sistema', 'Todos os controles deixam de funcionar até parear de novo. Use se perdeu um controle.', async () => {
  try { const d = await api('/api/link/rekey', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: 'confirm=TROCAR' }); showToast(d.message, 'ok', 'Segurança'); loadLink(); }
  catch (err) { showToast(err.message, 'err', 'Segurança'); }
}));

document.querySelectorAll('[data-open-ota]').forEach(btn => {
  btn.addEventListener('click', () => setView('ota'));
});


const initial = location.hash.replace('#', '');
if (['home', 'wifi', 'mqtt', 'sensors', 'ota', 'system'].includes(initial)) setView(initial);
refreshStatus();
setInterval(refreshStatus, 2000);
loadLink();
setInterval(loadLink, 4000);
