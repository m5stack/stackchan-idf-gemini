// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Gemini customer-build behaviour for the BLE / Wi-Fi settings pages.
// Loaded as a sibling on Pages (settings.html) and inlined into the
// firmware's settings_wifi.html. The Gemini-only build translates the full
// page at build time; this overlay only adds product fields and hides
// non-Gemini providers after the device reports gemini_only. Keeping the
// translation out of runtime DOM traversal prevents late or repeated
// rewrites from missing dynamically generated strings.

(function () {
  'use strict';

  const MODELS = [
    { id: 'gemini-3.8-live', label: 'gemini-3.8-live (recommended, GA)' },
    { id: 'gemini-2.5-flash-native-audio-preview-12-2025',
      label: 'gemini-2.5-flash-native-audio-preview-12-2025 (legacy; shutdown 2026-11-17)' },
  ];
  const VOICES = ['Aoede', 'Puck', 'Charon', 'Kore', 'Fenrir', 'Leda', 'Orus', 'Zephyr'];

  // Translation is a build-time transform now. Keep this compatibility entry
  // point for settings.html, which calls it after a Gemini-only device is
  // identified, but never rewrite dynamic text from a partial runtime table.
  function tr(value) {
    return value == null ? value : String(value);
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
      if (!s.gemini_only) return;
      applyGeminiOnlyUi({
        gemini_only: !!s.gemini_only,
        gemini_model: s.gemini_model,
        gemini_voice: s.gemini_voice,
        update_catalog: s.update_catalog,
      });
    } catch (_) { /* Keep the translated page usable if status is unavailable. */ }
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
    wrapWifi();
    // The device status decides whether this is the Gemini-only product.
    // Do not hide providers or inject product fields for upstream firmware.
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
