/* ============================================================
   嵌入式综合环境监测台 · 交互逻辑
   数据源：Spring Boot /api/sensor/* 、/api/device/*
   ============================================================ */
(function () {
  'use strict';

  /* ---------------- 配置 ---------------- */
  var POLL_MS = 2000;
  var SPARK_POINTS = 30;

  var DEVICES = [
    {
      id: 'ct107d', name: 'CT107D-V40', img: 'assets/board-ct107d.webp',
      live: true, tag: '运行中', status: '在线', note: '实时数据已接入'
    },
    {
      id: 'ct117e', name: 'CT117E-M4', img: 'assets/board-ct117e.webp',
      live: false, tag: '未接入', status: '未接入', note: '传感器通道开发中'
    }
  ];

  var METRICS = [
    {
      key: 'temperature', name: '温度', unit: '℃', dec: 2,
      color: '#FF8F1F', tint: 'rgba(255,143,31,.12)', range: [15, 35],
      icon: '<path d="M12 3.6a2.4 2.4 0 012.4 2.4v7.7a4.4 4.4 0 11-4.8 0V6A2.4 2.4 0 0112 3.6z"/><path d="M12 8.8v5.6"/>'
    },
    {
      key: 'light', name: '光照', unit: 'V', dec: 3,
      color: '#00B578', tint: 'rgba(0,181,120,.12)', range: [0, 5],
      icon: '<circle cx="12" cy="12" r="3.6"/><path d="M12 2.8v2.2M12 19v2.2M4.8 12H2.6M21.4 12h-2.2M6.5 6.5L5 5M19 19l-1.5-1.5M17.5 6.5L19 5M5 19l1.5-1.5"/>'
    },
    {
      key: 'pot', name: '电位器', unit: 'V', dec: 3,
      color: '#0085FF', tint: 'rgba(0,133,255,.10)', range: [0, 5],
      icon: '<circle cx="12" cy="12" r="7.6"/><path d="M12 12l3.6-2.6"/><circle cx="12" cy="12" r="1.4"/>'
    },
    {
      key: 'frequency', name: '频率', unit: 'Hz', dec: 0,
      color: '#7C5CFC', tint: 'rgba(124,92,252,.12)', range: [0, 3000],
      icon: '<path d="M2.6 12h3.2l2-6.4 3.2 12.8 2.8-9.6 1.8 3.2h5.8"/>'
    }
  ];

  /* ---------------- 状态 ---------------- */
  var state = {
    device: localStorage.getItem('monitor.device') || 'ct107d',
    history: [],
    latest: null,
    online: false,
    lastUpdate: 0,
    ctrl: { relay: false, buzzer: false }
  };
  if (!DEVICES.some(function (d) { return d.id === state.device; })) state.device = 'ct107d';

  var $ = function (id) { return document.getElementById(id); };
  var devName = $('devName'), devDot = $('devDot'), devSub = $('devSub'), subTime = $('subTime');
  var heroImg = $('heroImg'), heroTag = $('heroTag'), heroTagText = $('heroTagText');
  var connPill = $('connPill'), connText = $('connText');
  var grid = $('metricGrid'), devLive = $('devLive'), devPending = $('devPending');
  var mask = $('mask'), sheetDevices = $('sheetDevices'), sheetInfo = $('sheetInfo');
  var deviceList = $('deviceList'), toastEl = $('toast');

  /* ---------------- 工具 ---------------- */
  function pad(n) { return n < 10 ? '0' + n : '' + n; }

  function toDate(t) {
    if (!t) return null;
    if (typeof t === 'string') return new Date(t.replace(' ', 'T'));
    return new Date(t);
  }

  function fmtClock(t) {
    var d = toDate(t);
    if (!d || isNaN(d.getTime())) return '--:--:--';
    return pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds());
  }

  function fmtDate(t) {
    var d = toDate(t);
    if (!d || isNaN(d.getTime())) return '';
    return (d.getMonth() + 1) + '月' + d.getDate() + '日';
  }

  function num(v) {
    if (v === null || v === undefined || v === '') return null;
    var n = Number(v);
    return isNaN(n) ? null : n;
  }

  function fmtVal(v, dec) {
    var n = num(v);
    if (n === null) return '--';
    return dec === 0 ? String(Math.round(n)) : n.toFixed(dec);
  }

  function relativeTime(ts) {
    if (!ts) return '等待数据';
    var s = Math.max(0, Math.round((Date.now() - ts) / 1000));
    if (s < 3) return '刚刚更新';
    if (s < 60) return s + ' 秒前更新';
    var m = Math.floor(s / 60);
    if (m < 60) return m + ' 分钟前更新';
    return fmtClock(ts) + ' 更新';
  }

  var toastTimer = null;
  function toast(msg) {
    toastEl.textContent = msg;
    toastEl.classList.add('is-show');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { toastEl.classList.remove('is-show'); }, 2200);
  }

  /* ---------------- 迷你曲线 ---------------- */
  function buildRange(vals, def) {
    if (!vals.length) return def;
    var mn = Math.min.apply(null, vals);
    var mx = Math.max.apply(null, vals);
    var span = mx - mn;
    if (span < (def[1] - def[0]) * 0.08) {
      var mid = (mn + mx) / 2;
      var p = Math.max((def[1] - def[0]) * 0.1, 0.4);
      return [mid - p, mid + p];
    }
    var pad = span * 0.2;
    return [mn - pad, mx + pad];
  }

  function sparkPaths(vals, range) {
    var w = 100, h = 30, padY = 2.5;
    var n = vals.length;
    if (n < 2) return { line: '', area: '' };
    var span = (range[1] - range[0]) || 1;
    var pts = vals.map(function (v, i) {
      var x = (i / (n - 1)) * w;
      var p = (v - range[0]) / span;
      p = Math.max(0, Math.min(1, p));
      var y = h - padY - p * (h - padY * 2);
      return x.toFixed(2) + ',' + y.toFixed(2);
    });
    return {
      line: 'M' + pts.join('L'),
      area: 'M' + pts.join('L') + 'L' + w + ',' + h + 'L0,' + h + 'Z'
    };
  }

  /* ---------------- 构建指标卡 ---------------- */
  function buildMetrics() {
    var html = '';
    METRICS.forEach(function (m) {
      html +=
        '<div class="metric" id="m-' + m.key + '">' +
          '<div class="m-head">' +
            '<span class="m-ico" style="background:' + m.tint + ';color:' + m.color + '">' +
              '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round">' + m.icon + '</svg>' +
            '</span>' +
            '<span class="m-name">' + m.name + '</span>' +
          '</div>' +
          '<div class="m-val" id="v-' + m.key + '">--<span class="u">' + m.unit + '</span></div>' +
          '<svg class="m-spark" viewBox="0 0 100 30" preserveAspectRatio="none">' +
            '<defs><linearGradient id="g-' + m.key + '" x1="0" y1="0" x2="0" y2="1">' +
              '<stop offset="0" stop-color="' + m.color + '" stop-opacity=".24"/>' +
              '<stop offset="1" stop-color="' + m.color + '" stop-opacity="0"/>' +
            '</linearGradient></defs>' +
            '<path class="area" fill="url(#g-' + m.key + ')" d=""/>' +
            '<path class="line" stroke="' + m.color + '" vector-effect="non-scaling-stroke" d=""/>' +
          '</svg>' +
        '</div>';
    });
    grid.innerHTML = html;
  }

  /* ---------------- 渲染 ---------------- */
  function renderMetrics() {
    METRICS.forEach(function (m) {
      var el = $('v-' + m.key);
      if (!el) return;
      var v = state.latest ? state.latest[m.key] : null;
      el.innerHTML = fmtVal(v, m.dec) + '<span class="u">' + m.unit + '</span>';

      var vals = state.history.map(function (r) { return num(r[m.key]); })
        .filter(function (v2) { return v2 !== null; })
        .slice(-SPARK_POINTS);

      var card = $('m-' + m.key);
      card.classList.toggle('is-stale', vals.length === 0);

      var svg = card.querySelector('svg.m-spark');
      var p = sparkPaths(vals, buildRange(vals, m.range));
      svg.querySelector('path.area').setAttribute('d', p.area);
      svg.querySelector('path.line').setAttribute('d', p.line);
    });
  }

  function renderRecords() {
    var list = $('recList');
    var rows = state.history.slice(-50).reverse();
    $('recCount').textContent = state.history.length + ' 条';
    $('menuRecCount').textContent = state.history.length + ' 条';
    $('statCount').innerHTML = state.history.length + '<small>条</small>';

    if (!rows.length) {
      list.innerHTML = '<p class="rec-empty">暂无记录，等板子上报…</p>';
      return;
    }
    list.innerHTML = rows.map(function (r) {
      return '<div class="rec-row">' +
        '<span class="t">' + fmtClock(r.createTime) + '</span>' +
        '<span class="v">' + fmtVal(r.temperature, 2) + '</span>' +
        '<span class="v">' + fmtVal(r.light, 3) + '</span>' +
        '<span class="v">' + fmtVal(r.pot, 3) + '</span>' +
        '<span class="v">' + fmtVal(r.frequency, 0) + '</span>' +
        '</div>';
    }).join('');
  }

  function setOnline(on, msg) {
    state.online = on;
    connPill.classList.toggle('is-off', !on && !msg);
    connPill.classList.toggle('is-err', !on && !!msg);
    connText.textContent = on ? '已连接' : (msg || '未连接');
    devDot.classList.toggle('is-off', !on && !msg);
    devDot.classList.toggle('is-warn', !on && !!msg);
    heroTag.classList.toggle('is-off', !on && !msg);
    heroTag.classList.toggle('is-warn', !on && !!msg);
    if (!on) heroTagText.textContent = msg ? '连接中断' : '未接入';
    else heroTagText.textContent = '运行中';
  }

  function tickSubTime() {
    if (state.device !== 'ct107d') {
      subTime.textContent = '暂无数据';
      return;
    }
    subTime.textContent = state.online ? relativeTime(state.lastUpdate) : (state.lastUpdate ? '连接中断 · ' + fmtClock(state.lastUpdate) : '等待数据');
  }

  /* ---------------- 数据拉取 ---------------- */
  function applyDevice() {
    var d = DEVICES.filter(function (x) { return x.id === state.device; })[0] || DEVICES[0];
    devName.textContent = d.name;
    heroImg.src = d.img;
    heroImg.alt = d.name + ' 开发板';
    heroTagText.textContent = d.tag;
    devSub.firstElementChild.textContent = d.status;

    devLive.hidden = !d.live;
    devPending.hidden = d.live;
    $('page-device').classList.toggle('is-pending', !d.live);

    if (d.live) {
      if (state.online) {
        setOnline(true);
      } else if (state.lastUpdate) {
        setOnline(false, '连接失败');
      } else {
        heroTag.classList.remove('is-off', 'is-warn');
        heroTagText.textContent = '运行中';
        connPill.classList.remove('is-off', 'is-err');
        connText.textContent = '连接中';
        devDot.classList.remove('is-off', 'is-warn');
      }
      tickSubTime();
    } else {
      connPill.classList.remove('is-err');
      connPill.classList.add('is-off');
      connText.textContent = '未接入';
      devDot.classList.remove('is-warn');
      devDot.classList.add('is-off');
      heroTag.classList.remove('is-warn');
      heroTag.classList.add('is-off');
      heroTagText.textContent = '未接入';
      subTime.textContent = '暂无数据';
    }
    renderSheetActive();
  }

  async function refresh() {
    if (state.device !== 'ct107d') return;
    try {
      var res = await Promise.all([
        fetch('/api/sensor/latest', { cache: 'no-store' }),
        fetch('/api/sensor/history?limit=100', { cache: 'no-store' })
      ]);
      if (!res[0].ok || !res[1].ok) throw new Error('http ' + res[0].status);
      var latest = await res[0].json();
      var history = await res[1].json();

      state.history = Array.isArray(history) ? history : [];
      var hasData = latest && num(latest.temperature) !== null;
      state.latest = hasData ? latest : (state.history.length ? state.history[state.history.length - 1] : null);
      if (hasData) state.lastUpdate = toDate(latest.createTime) ? toDate(latest.createTime).getTime() : Date.now();

      setOnline(hasData);
      renderMetrics();
      renderRecords();
      tickSubTime();
    } catch (e) {
      setOnline(false, '连接失败');
      tickSubTime();
    }
  }

  async function refreshCtrlStatus() {
    if (state.device !== 'ct107d') return;
    try {
      var r = await fetch('/api/device/status', { cache: 'no-store' });
      if (!r.ok) return;
      var s = await r.json();
      var map = { PENDING: '待下发', SENT: '已下发串口' };
      var label = { R1: '继电器开', R0: '继电器关', B1: '蜂鸣器开', B0: '蜂鸣器关' };
      if (s && s.lastCommand) {
        $('ctrlMeta').textContent = (label[s.lastCommand] || s.lastCommand) + ' · ' +
          (map[s.status] || '') + (s.time ? ' ' + fmtClock(s.time) : '');
      } else {
        $('ctrlMeta').textContent = '经串口桥下发';
      }
    } catch (e) { /* 后端未启动时静默 */ }
  }

  /* ---------------- 设备切换 ---------------- */
  function renderSheetActive() {
    var rows = deviceList.querySelectorAll('.dev-row');
    for (var i = 0; i < rows.length; i++) {
      rows[i].classList.toggle('is-active', rows[i].getAttribute('data-dev') === state.device);
    }
    document.querySelectorAll('.dev-card').forEach(function (c) {
      c.classList.toggle('is-active', c.getAttribute('data-dev') === state.device);
    });
  }

  function pickedDevice(id, opts) {
    if (id === state.device) {
      if (opts && opts.jump) switchTab('device');
      return;
    }
    state.device = id;
    localStorage.setItem('monitor.device', id);
    applyDevice();
    if (opts && opts.jump) switchTab('device');
    if (id === 'ct107d') refresh();
    toast(id === 'ct117e' ? '已切换到 CT117E-M4' : '已切换到 CT107D-V40');
  }

  function buildMeDevCards() {
    $('meDevCards').innerHTML = DEVICES.map(function (d) {
      return '<button class="dev-card' + (d.id === state.device ? ' is-active' : '') + '" data-dev="' + d.id + '">' +
        '<span class="dc-thumb"><img src="' + d.img + '" alt=""></span>' +
        '<span class="dc-name">' + d.name + '</span>' +
        '<span class="dc-status"><span class="dot' + (d.live ? ' on' : '') + '"></span>' + d.status + '</span>' +
        '</button>';
    }).join('');
  }

  function buildDeviceList() {
    deviceList.innerHTML = DEVICES.map(function (d) {
      return '<button class="dev-row' + (d.id === state.device ? ' is-active' : '') + '" data-dev="' + d.id + '">' +
        '<span class="dev-thumb"><img src="' + d.img + '" alt=""></span>' +
        '<span class="r-txt">' +
          '<span class="r-name">' + d.name + '</span>' +
          '<span class="r-sub"><span class="dot' + (d.live ? ' on' : '') + '"></span>' + d.note + '</span>' +
        '</span>' +
        '<span class="check"><svg viewBox="0 0 24 24" width="12" height="12" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><path d="M5 12.5l4.5 4.5L19 7"/></svg></span>' +
        '</button>';
    }).join('');

    deviceList.addEventListener('click', function (e) {
      var row = e.target.closest('.dev-row');
      if (!row) return;
      var id = row.getAttribute('data-dev');
      closeSheets();
      pickedDevice(id);
    });
  }

  /* ---------------- 面板 ---------------- */
  function openSheet(el) {
    mask.classList.add('is-open');
    el.classList.add('is-open');
  }
  function closeSheets() {
    mask.classList.remove('is-open');
    sheetDevices.classList.remove('is-open');
    sheetInfo.classList.remove('is-open');
  }
  mask.addEventListener('click', closeSheets);

  function openInfo(title, html) {
    $('infoTitle').textContent = title;
    $('infoBody').innerHTML = html;
    openSheet(sheetInfo);
  }

  /* ---------------- 控制 ---------------- */
  function ctrlText(type, on, suffix) {
    var el = $(type === 'relay' ? 'relayState' : 'buzzerState');
    el.textContent = (on ? '已开启' : '已关闭') + ' · ' + suffix;
  }

  async function sendCmd(type, on) {
    var body = {};
    body[type] = on ? 'on' : 'off';
    try {
      var r = await fetch('/api/device/command', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body)
      });
      var txt = await r.text();
      var ok = /已加入队列/.test(txt);
      ctrlText(type, on, ok ? '指令已下发' : '下发失败');
      if (!ok) toast(txt.slice(0, 40));
    } catch (e) {
      ctrlText(type, on, '下发失败');
      toast('网络异常，指令未发送');
    }
  }

  function bindSwitch(id, type) {
    var btn = $(id);
    btn.addEventListener('click', function () {
      if (state.device !== 'ct107d') return;
      var on = btn.getAttribute('aria-checked') !== 'true';
      btn.setAttribute('aria-checked', on ? 'true' : 'false');
      state.ctrl[type] = on;
      ctrlText(type, on, '下发中…');
      sendCmd(type, on);
    });
  }

  /* ---------------- Tab ---------------- */
  var pages = { device: $('page-device'), me: $('page-me') };
  function switchTab(name) {
    Object.keys(pages).forEach(function (k) { pages[k].classList.toggle('is-active', k === name); });
    document.querySelectorAll('.tab-item').forEach(function (t) {
      t.classList.toggle('is-active', t.getAttribute('data-tab') === name);
    });
  }

  /* ---------------- 绑定 ---------------- */
  function bind() {
    document.querySelectorAll('.tab-item').forEach(function (t) {
      t.addEventListener('click', function () { switchTab(t.getAttribute('data-tab')); });
    });

    $('btnSwitch').addEventListener('click', function () {
      buildDeviceList();
      openSheet(sheetDevices);
    });
    $('menuDevices').addEventListener('click', function () {
      buildDeviceList();
      openSheet(sheetDevices);
    });

    $('meDevCards').addEventListener('click', function (e) {
      var c = e.target.closest('.dev-card');
      if (!c) return;
      pickedDevice(c.getAttribute('data-dev'), { jump: true });
    });

    var rec = $('pageRecords');
    $('menuRecords').addEventListener('click', function () {
      rec.classList.add('is-open');
      renderRecords();
    });
    $('btnBackRecords').addEventListener('click', function () { rec.classList.remove('is-open'); });

    $('menuGuide').addEventListener('click', function () {
      openInfo('使用说明',
        '<p><b>数据从哪来</b><br>CT107D-V40 每 2 秒通过串口上报一帧数据（温度、光敏电压、电位器电压、NE555 频率），串口桥换算单位后写入后端数据库。</p>' +
        '<p><b>曲线怎么读</b><br>每张指标卡下方的迷你曲线是最近 1 分钟的走势，横轴为时间，纵轴自适应数值范围。</p>' +
        '<p><b>怎么控制板子</b><br>「板载控制」里的两个开关点一下即下发指令：继电器、蜂鸣器。指令先进入后端队列，串口桥每 0.5 秒取走一次并写入串口。</p>' +
        '<p><b>换一块板子</b><br>点页面顶部的设备名即可切换。CT117E-M4 目前只完成外观接入，数据通道仍在开发中。</p>');
    });

    $('menuAbout').addEventListener('click', function () {
      openInfo('关于监测台',
        '<p><b>嵌入式综合环境监测台</b><br>版本 v2.0 · 2026</p>' +
        '<p><b>系统构成</b><br>CT107D 开发板（IAP15F2K61S2）→ Python 串口桥 → Spring Boot 后端 → 本页面。</p>' +
        '<p><b>技术栈</b><br>板端 Keil C51，后端 Spring Boot + MyBatis + MySQL，前端原生 HTML / CSS / JavaScript，曲线为自绘 SVG，不依赖任何第三方图表库。</p>' +
        '<p><b>用途</b><br>蓝桥杯单片机实训项目，用于演示传感器采集与远程控制的完整链路。</p>');
    });

    bindSwitch('swRelay', 'relay');
    bindSwitch('swBuzzer', 'buzzer');
  }

  /* ---------------- 启动 ---------------- */
  function start() {
    var hash = (location.hash || '').replace('#', '');
    if (hash === 'ct117e' || hash === 'ct107d') state.device = hash;

    buildMetrics();
    buildDeviceList();
    buildMeDevCards();
    bind();
    applyDevice();
    if (hash === 'me') switchTab('me');
    renderMetrics();
    renderRecords();
    refresh();
    refreshCtrlStatus();

    setInterval(function () {
      if (document.hidden) return;
      refresh();
    }, POLL_MS);

    setInterval(function () {
      if (document.hidden) return;
      refreshCtrlStatus();
    }, 3000);

    setInterval(tickSubTime, 1000);
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', start);
  } else {
    start();
  }
})();
