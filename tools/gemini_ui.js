// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Gemini customer-build overlay for the BLE / Wi-Fi settings pages.
// Loaded as a sibling on Pages (settings.html) and inlined into the
// firmware's settings_wifi.html. Keeps the Japanese HTML source intact
// (small upstream-merge surface) and rewrites the visible UI to English,
// hiding OpenAI / XiaoZhi when the firmware reports gemini_only.

(function () {
  'use strict';

  const MODELS = [
    { id: 'gemini-3.8-live', label: 'gemini-3.8-live (recommended, GA)' },
    { id: 'gemini-2.5-flash-native-audio-preview-12-2025',
      label: 'gemini-2.5-flash-native-audio-preview-12-2025 (legacy; shutdown 2026-11-17)' },
  ];
  const VOICES = ['Aoede', 'Puck', 'Charon', 'Kore', 'Fenrir', 'Leda', 'Orus', 'Zephyr'];

  const STR = {
    '🤖 Stack-chan 設定 (Wi-Fi)': '🤖 Stack-chan settings (Wi-Fi)',
    '🤖 Stack-chan 設定': '🤖 Stack-chan settings',
    'HTTP 経由で Wi-Fi と API キーを設定します': 'Configure Wi-Fi and the Gemini API key over HTTP',
    'Web Bluetooth 経由で Wi-Fi と API キーを設定します': 'Configure Wi-Fi and the Gemini API key over Web Bluetooth',
    '接続': 'Connect',
    'アバター': 'Avatar',
    '音声': 'Audio',
    '会話': 'Chat',
    'デバイス': 'Device',
    'メンテ': 'Maint',
    '状態を再取得': 'Refresh',
    '保存 & 再起動': 'Save & reboot',
    '保存 &amp; 再起動': 'Save & reboot',
    'BLE 接続': 'Connect BLE',
    '切断': 'Disconnect',
    '未接続': 'Disconnected',
    '状態取得中…': 'Loading…',
    '状態取得 OK': 'Status OK',
    '状態取得失敗': 'Status failed',
    '接続失敗': 'Connection failed',
    'HTTP 接続中': 'HTTP connected',
    '基板': 'Board',
    'バッテリー': 'Battery',
    '動作モード': 'Operation mode',
    '会話プロバイダ・API キー': 'Gemini Live',
    '会話プロバイダ': 'Conversation provider',
    'OpenAI API キー': 'OpenAI API key',
    'Gemini API キー': 'Gemini API key',
    'XiaoZhi サーバー URL': 'XiaoZhi server URL',
    'XiaoZhi トークン': 'XiaoZhi token',
    'システムプロンプト (会話の人格)': 'System prompt (persona)',
    '追加 HTTP ヘッダ (会話 API)': 'Extra HTTP headers (conversation API)',
    'エージェント サーバー URL (ask_agent)': 'Agent server URL (ask_agent)',
    'エージェント サーバー トークン': 'Agent server token',
    'タッチ操作': 'Touch',
    'ファームウェア更新 (OTA)': 'Firmware update (OTA)',
    'release を適用': 'Apply release',
    '書き込み': 'Write',
    'ファイル選択': 'Choose file',
    'ランダム生成': 'Generate',
    '表示': 'Show',
    'クリア': 'Clear',
    'キャンセル': 'Cancel',
    '接続パスワード': 'Connection password',
    'パスワード': 'Password',
    'パスワードを表示': 'Show password',
    'デバイス': 'Device',
    '設定済': 'set',
    '未設定': 'unset',
    'スタート': 'Start',
    'ストップ': 'Stop',
    '適用': 'Apply',
    'OpenAI Realtime': 'OpenAI Realtime',
    'Google Gemini Live': 'Google Gemini Live',
    'XiaoZhi サーバー': 'XiaoZhi server',
    '会話応答 (Conversation)': 'Conversation',
    'JTTS ランダム発話 (JttsRandom)': 'JTTS random speech',
    'マイクでリップシンク (MicLipSync)': 'Mic lip sync',
    'ローカル音声認識 (AsrLocal)': 'On-device ASR',
    'ESP-NOWリモコン (EspNowRemote)': 'ESP-NOW remote',
    'ESP-NOW姿勢配信 (EspNowSender)': 'ESP-NOW pose send',
    '空欄のままにすると現在の値を変更しません': 'Leave blank to keep the current value',
    '設定済 — 変更しない場合は空欄': 'set — leave blank to keep',
    '未設定 — 空欄なら認証なし': 'unset — blank means no auth',
    '（設定済み / 変更しない場合は空欄）': '(set / leave blank to keep)',
    '（未設定）': '(unset)',
    '（変更しない場合は空欄）': '(leave blank to keep)',
    'AIza… （変更しない場合は空欄）': 'AIza… (leave blank to keep)',
    'sk-… （変更しない場合は空欄）': 'sk-… (leave blank to keep)',
    '空欄にすると組み込みの既定プロンプトを使用します': 'Leave blank to use the built-in persona',
    'SSID 設定済み': 'SSID set',
    'Password 設定済み': 'Password set',
    'OpenAI key 設定済み': 'OpenAI key set',
    'OpenAI 有効': 'OpenAI on',
    'Gemini key 設定済み': 'Gemini key set',
    'Provider = Gemini': 'Provider = Gemini',
    'Wi-Fi: 接続中': 'Wi-Fi: up',
    'Wi-Fi 設定 (mDNS)': 'Wi-Fi settings (mDNS)',
    'Wi-Fi 設定 (IP)': 'Wi-Fi settings (IP)',
    'BLE 接続パスワード': 'BLE connection password',
    'このデバイスは認証パスワードが設定されています。': 'This device has an auth password set.',
    'ライセンス / 帰属表示': 'Licenses / attribution',
    '最新の release をローカル ファイルで書き込む手順 (iPhone / AP モードでも可)':
      'How to flash a release ZIP as a local file (works in iPhone / AP mode)',
  };

  function tr(s) {
    if (s == null) return s;
    if (Object.prototype.hasOwnProperty.call(STR, s)) return STR[s];
    const t = String(s).trim();
    if (Object.prototype.hasOwnProperty.call(STR, t)) {
      const lead = s.match(/^\s*/)[0];
      const trail = s.match(/\s*$/)[0];
      return lead + STR[t] + trail;
    }
    // Partial replacements used in log lines / mixed HTML.
    let out = s;
    const parts = [
      ['状態取得 OK', 'Status OK'],
      ['状態取得失敗', 'Status failed'],
      ['SSID 送信', 'SSID sent'],
      ['パスワード送信', 'Password sent'],
      ['OpenAI キー送信', 'OpenAI key sent'],
      ['Gemini キー送信', 'Gemini key sent'],
      ['プロバイダ', 'Provider'],
      ['書き込み完了', 'write complete'],
      ['保存中…', 'Saving…'],
      ['近接の閾値を適用', 'Applied proximity thresholds'],
      ['会話ステータスの LED 設定を適用', 'Applied conversation LED settings'],
      ['release tag', 'release tag'],
      ['件取得', ' fetched'],
      ['取得失敗', 'fetch failed'],
    ];
    for (const [ja, en] of parts) {
      if (out.indexOf(ja) >= 0) out = out.split(ja).join(en);
    }
    return out;
  }

  function walk(node, fn) {
    if (!node) return;
    fn(node);
    for (const c of Array.from(node.childNodes || [])) walk(c, fn);
  }

  function translateTree(root) {
    walk(root || document.body, (n) => {
      if (n.nodeType === 3) {
        const next = tr(n.nodeValue);
        if (next !== n.nodeValue) n.nodeValue = next;
      } else if (n.nodeType === 1) {
        for (const attr of ['placeholder', 'title', 'aria-label']) {
          if (n.hasAttribute && n.hasAttribute(attr)) {
            const v = n.getAttribute(attr);
            const nv = tr(v);
            if (nv !== v) n.setAttribute(attr, nv);
          }
        }
      }
    });
    document.documentElement.lang = 'en';
    if (document.title && document.title.indexOf('設定') >= 0) {
      document.title = document.title.replace('設定', 'settings');
    }
  }

  function setVisible(selector, visible) {
    document.querySelectorAll(selector).forEach((el) => {
      el.style.display = visible ? '' : 'none';
    });
  }

  function hideNonGemini() {
    setVisible('label[for="provider"], #provider', false);
    setVisible('#provider + p.hint', false);
    setVisible('label[for="apikey"], #apikey', false);
    setVisible('label[for="xiaozhi-url"], #xiaozhi-url', false);
    setVisible('label[for="xiaozhi-token"], #xiaozhi-token', false);
    const sel = document.getElementById('provider');
    if (sel) {
      sel.value = '1';
      for (const opt of sel.options) {
        opt.hidden = opt.value !== '1';
      }
    }
    const fk = document.getElementById('flag-key');
    const fe = document.getElementById('flag-en');
    if (fk) fk.style.display = 'none';
    if (fe) fe.style.display = 'none';
    const title = document.querySelector('#chat-section h2.section-title');
    if (title) title.textContent = 'Gemini Live';
  }

  function ensureSelect(el, items, current) {
    if (!el) return;
    const wanted = current || el.value || '';
    const have = new Set(Array.from(el.options).map((o) => o.value));
    for (const it of items) {
      const id = typeof it === 'string' ? it : it.id;
      const label = typeof it === 'string' ? it : it.label;
      if (!have.has(id)) {
        const opt = document.createElement('option');
        opt.value = id;
        opt.textContent = label;
        el.appendChild(opt);
        have.add(id);
      }
    }
    if (wanted && !have.has(wanted)) {
      const opt = document.createElement('option');
      opt.value = wanted;
      opt.textContent = wanted;
      el.appendChild(opt);
    }
    if (wanted) el.value = wanted;
  }

  function ensureFields() {
    const key = document.getElementById('gemini-apikey');
    if (!key) return;
    if (!document.getElementById('gemini-model')) {
      const lab = document.createElement('label');
      lab.htmlFor = 'gemini-model';
      lab.textContent = 'Gemini model';
      const sel = document.createElement('select');
      sel.id = 'gemini-model';
      const hint = document.createElement('p');
      hint.className = 'hint';
      hint.textContent =
        'Native-audio Live model. gemini-3.8-live is the current GA default; the 2.5 preview shuts down 2026-11-17.';
      key.insertAdjacentElement('afterend', lab);
      lab.insertAdjacentElement('afterend', sel);
      sel.insertAdjacentElement('afterend', hint);
    }
    if (!document.getElementById('gemini-voice')) {
      const after = document.getElementById('gemini-model') || key;
      const lab = document.createElement('label');
      lab.htmlFor = 'gemini-voice';
      lab.textContent = 'Gemini voice';
      const sel = document.createElement('select');
      sel.id = 'gemini-voice';
      const hint = document.createElement('p');
      hint.className = 'hint';
      hint.textContent = 'Prebuilt Gemini Live voice. Default is Aoede.';
      const anchor = after.nextElementSibling && after.nextElementSibling.classList &&
                     after.nextElementSibling.classList.contains('hint')
        ? after.nextElementSibling
        : after;
      anchor.insertAdjacentElement('afterend', lab);
      lab.insertAdjacentElement('afterend', sel);
      sel.insertAdjacentElement('afterend', hint);
    }
    ensureSelect(document.getElementById('gemini-model'), MODELS);
    ensureSelect(document.getElementById('gemini-voice'), VOICES, 'Aoede');
  }

  function hideReleaseIfUnset(updateCatalog) {
    const btn = document.getElementById('btn-ota-release');
    const sel = document.getElementById('ota-release-select');
    const ch = document.getElementById('ota-release-channel');
    const tag = document.getElementById('ota-release-tag');
    if (updateCatalog === false) {
      [btn, sel, ch, tag].forEach((el) => {
        if (!el) return;
        el.disabled = true;
      });
      if (btn) btn.title = 'In-app update catalog is not configured for this build';
    }
  }

  function rewritePagesLinks() {
    document.querySelectorAll('a[href*="ciniml.github.io/stackchan-idf"]').forEach((a) => {
      a.href = a.href.replace('ciniml.github.io/stackchan-idf', 'm5stack.github.io/stackchan-idf-gemini');
      if (a.textContent && a.textContent.indexOf('ciniml.github.io') >= 0) {
        a.textContent = a.textContent.replace('ciniml.github.io/stackchan-idf', 'm5stack.github.io/stackchan-idf-gemini');
      }
    });
    document.querySelectorAll('a[href*="github.com/ciniml/stackchan-idf"]').forEach((a) => {
      a.href = a.href.replace('github.com/ciniml/stackchan-idf', 'github.com/m5stack/stackchan-idf-gemini');
    });
  }

  function applyGeminiOnlyUi(opts) {
    const o = opts || {};
    const hide = o.gemini_only !== false; // product default: hide
    ensureFields();
    if (hide) hideNonGemini();
    if (o.gemini_model) ensureSelect(document.getElementById('gemini-model'), MODELS, o.gemini_model);
    if (o.gemini_voice) ensureSelect(document.getElementById('gemini-voice'), VOICES, o.gemini_voice);
    if ('update_catalog' in o) hideReleaseIfUnset(!!o.update_catalog);
    translateTree(document.body);
    rewritePagesLinks();
  }

  async function afterWifiRefresh() {
    try {
      const r = await fetch('/api/status', { cache: 'no-store' });
      if (!r.ok) return;
      const s = await r.json();
      applyGeminiOnlyUi({
        gemini_only: !!s.gemini_only,
        gemini_model: s.gemini_model,
        gemini_voice: s.gemini_voice,
        update_catalog: s.update_catalog,
      });
    } catch (_) { /* keep the English overlay even if status fails */ }
  }

  function wrapWifi() {
    if (typeof refresh === 'function') {
      const orig = refresh;
      window.refresh = async function () {
        await orig.apply(this, arguments);
        await afterWifiRefresh();
      };
    }
    if (typeof apply === 'function') {
      const orig = apply;
      window.apply = async function () {
        const model = document.getElementById('gemini-model');
        const voice = document.getElementById('gemini-voice');
        try {
          if (model && model.value && typeof postText === 'function') {
            await postText('/api/gemini-model', model.value);
          }
          if (voice && voice.value && typeof postText === 'function') {
            await postText('/api/gemini-voice', voice.value);
          }
        } catch (e) {
          if (typeof log === 'function') log('Gemini model/voice: ' + e.message, 'err');
        }
        return orig.apply(this, arguments);
      };
    }
    if (typeof log === 'function') {
      const origLog = log;
      window.log = function (msg, cls) {
        return origLog(tr(String(msg)), cls);
      };
    }
  }

  const api = {
    applyGeminiOnlyUi,
    afterWifiRefresh,
    hideNonGemini,
    ensureFields,
    translateTree,
    tr,
    MODELS,
    VOICES,
    CHR_GEMINI_MODEL: 'e3f0a032-7b1c-4d2a-9e6f-2c5a8d4b1f00',
    CHR_GEMINI_VOICE: 'e3f0a033-7b1c-4d2a-9e6f-2c5a8d4b1f00',
  };
  window.StackchanGeminiUi = api;

  function boot() {
    ensureFields();
    applyGeminiOnlyUi({ gemini_only: true });
    wrapWifi();
    // First Wi-Fi status fill happens at end of the page script; wrapWifi
    // covers later refreshes. Kick one extra pass in case refresh already ran.
    if (typeof fetch === 'function' && location.protocol !== 'file:') {
      afterWifiRefresh();
    }
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot);
  } else {
    boot();
  }
})();
