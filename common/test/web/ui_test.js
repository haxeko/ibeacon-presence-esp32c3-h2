// Headless check of main/web/index.html against a mocked device API.
// Usage: node ui_test.js [path/to/index.html] [screenshot-dir]   (needs the `playwright` package + Chromium)
const { chromium } = require('playwright');
const fs = require('fs');
const path = require('path');

const html = fs.readFileSync(process.argv[2] || path.join(__dirname, '../../main/web/index.html'), 'utf8');
const out = process.argv[3];
let failures = 0;
function check(cond, msg) { if (!cond) { failures++; console.log('FAIL', msg); } else console.log('ok  ', msg); }

const slot = (i, extra = {}) => Object.assign({ enabled: false, label: 'Presence ' + (i + 1), mac: '', addr_type: 'any',
  timeout_s: 45, rssi_filter: false, rssi_threshold: -80, rssi_hysteresis: 5, model: '',
  watch: { mac: '', state: 'none', watched_s: 0, total_s: 1800, new_mac: '' } }, extra);
const slots = n => Array.from({ length: n }, (_, i) =>
  i === 0 ? slot(0, { enabled: true, label: 'Ví của Bố', mac: 'C0:11:22:33:44:01', addr_type: 'public', model: 'CP101',
    watch: { mac: 'C0:11:22:33:44:01', state: 'fixed', watched_s: 1800, total_s: 1800, new_mac: '' } }) : slot(i));
const config = {
  scan: { interval_ms: 160, window_ms: 40 },
  slots: slots(5),
  limits: { label_max_bytes: 48, model_max_bytes: 32, timeout_min: 10, timeout_max: 3600, rssi_min: -100, rssi_max: -20,
            hysteresis_max: 20, interval_min: 20, interval_max: 10240, window_min: 10 },
  defaults: { timeout_s: 45, rssi_threshold: -80, rssi_hysteresis: 5, interval_ms: 160, window_ms: 40 },
  allow_duplicates: false, storage: { status: 'ok', detail: '' }, network: 'thread',
};
const status = {
  mode: 'config', network: 'thread', firmware: '1.0.181', idf: 'v6.0.2', uptime_s: 75, idle_left_s: 587, idle_timeout_s: 600,
  heap: { free: 123456, min: 100000, largest: 80000 }, ap: { ssid: 'VN-Presence-3A7C', stations: 1, channel: 1 },
  scan: { state: 'running', paused_by: '', active: true, interval_ms: 160, window_ms: 40, adv_reports: 1234,
          ibeacon_reports: 210, starts: 1, start_failures: 0, unexpected_stops: 0, watchdog_restarts: 0, last_error: 0,
          list: 3, capacity: 32, queue_drops: 0, evictions: 0 },
  storage: { status: 'ok', detail: '', nvs_main: 'ESP_OK' },
};
const evil = '<img src=x onerror="window.__xss=1">';
const beacons = [
  { mac: 'C0:11:22:33:44:01', addr_type: 'public', kind: 'public', stable: true, rssi: -58, uuid: 'E2C56DB5-DFFB-48D2-B060-D0F5A71096E0',
    major: 1, minor: 1, tx_power: -59, name: 'Ví', battery_mv: 2950, battery_pct: 94, battery_age_ms: 3000, age_ms: 800, count: 120,
    same_id_other_mac: false },
  { mac: 'D4:AA:BB:CC:DD:02', addr_type: 'random', kind: 'static', stable: true, rssi: -71, uuid: 'E2C56DB5-DFFB-48D2-B060-D0F5A71096E0',
    major: 1, minor: 2, tx_power: -59, name: evil, battery_mv: 0, battery_pct: -1, battery_age_ms: -1, age_ms: 2100, count: 40,
    same_id_other_mac: false },
  { mac: '5A:01:02:03:04:05', addr_type: 'random', kind: 'rpa', stable: false, rssi: -84, uuid: 'FDA50693-A4E2-4FB1-AFCF-C6EB07647825',
    major: 10, minor: 7, tx_power: -60, name: '', battery_mv: 0, battery_pct: -1, battery_age_ms: -1, age_ms: 9000, count: 5,
    same_id_other_mac: true },
  { mac: 'E1:22:33:44:55:66', addr_type: 'public', kind: 'public', stable: true, rssi: -64, uuid: 'E2C56DB5-DFFB-48D2-B060-D0F5A71096E0',
    major: 1, minor: 3, tx_power: -59, name: 'CP101', battery_mv: 2400, battery_pct: 25, battery_age_ms: 1000, age_ms: 500, count: 60,
    same_id_other_mac: false },
];

// A page of the device with the API answered by the mock; network: 'thread' (ESP32-C6) or 'wifi' (ESP32-C3).
// opts.scanDelayMs: the device answers the beacon list that late (slow Wi-Fi); opts.stats counts the requests.
async function openPage(browser, network, posts, opts = {}) {
  const page = await browser.newPage({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 2, isMobile: true, hasTouch: true });
  page.on('pageerror', e => { failures++; console.log('FAIL page error', e.message); });
  await page.route('http://device.test/**', async route => {
    const req = route.request();
    const url = new URL(req.url());
    const json = (obj, code = 200) => route.fulfill({ status: code, contentType: 'application/json', body: JSON.stringify(obj) });
    if (url.pathname === '/') return route.fulfill({ status: 200, contentType: 'text/html; charset=utf-8', body: html });
    if (url.pathname === '/api/status') return json(Object.assign({}, status, { network },
      network === 'h2' ? { h2: Object.assign({ seen: true, ago_s: 1, commissioned: false, network_up: false, window_open: true,
        fabrics: 0, heap_kb: 52, heap_min_kb: 41,
        bridged: Array.from({ length: 20 }, (_, i) => (i === 0 ? 2 : 0)),
        waiting: Array.from({ length: 20 }, (_, i) => i === 3),
        errors: Array.from({ length: 20 }, () => false),
        pairing: { manual: '34970112332', qr: 'MT:Y.K9042C00KA0648G00' } }, opts.h2 || {}) } : {}));
    if (url.pathname === '/api/scan' && req.method() === 'GET') {
      const st = opts.stats || {};
      st.scanInFlight = (st.scanInFlight || 0) + 1;
      st.scanMaxInFlight = Math.max(st.scanMaxInFlight || 0, st.scanInFlight);
      st.scanRequests = (st.scanRequests || 0) + 1;
      if (opts.scanDelayMs) await new Promise(r => setTimeout(r, opts.scanDelayMs));
      st.scanInFlight--;
      return json({ beacons });
    }
    if (url.pathname === '/api/config' && req.method() === 'GET') {
      // The two-chip version has 20 slots; slot 4 has a saved beacon the H2 has not added yet
      const c = Object.assign({}, config, { network });
      if (opts.watch) c.slots = c.slots.map((x, i) => opts.watch[i] ? Object.assign({}, x, opts.watch[i]) : x);
      if (network === 'h2') {
        c.slots = slots(20);
        c.slots[3] = slot(3, { enabled: true, mac: 'E1:22:33:44:55:66', addr_type: 'public', model: 'CP101' });
      }
      return json(c);
    }
    if (req.method() === 'POST') {
      posts.push({ path: url.pathname, body: req.postData(), type: req.headers()['content-type'] || '' });
      if (url.pathname === '/api/config') return json({ ok: true, message: 'Đã lưu và đọc lại kiểm tra thành công.' });
      return json({ ok: true, message: 'ok', idle_left_s: 600 });
    }
    return route.fulfill({ status: 404, body: '' });
  });
  await page.goto('http://device.test/');
  await page.waitForSelector('.beacon');
  return page;
}

(async () => {
  const browser = await chromium.launch();
  const posts = [];
  let dialogs = 0;
  const page = await openPage(browser, 'thread', posts);
  page.on('dialog', async d => { dialogs++; await d.accept(); });
  await page.waitForTimeout(300);

  // Texts of the ESP32-C6 version (Matter over Thread)
  check((await page.textContent('#statusGrid')).includes('Matter/Thread'), 'Thread version: mode shows Matter/Thread');
  check((await page.textContent('#coexNote')).includes('C1'), 'Thread version: coexistence note (C1)');
  check((await page.textContent('#coexNote')).includes('40/160 ms (25%)'), 'scan defaults from the device');

  // Rendering and escaping
  const names = await page.$$eval('.beacon b', els => els.map(e => e.textContent));
  check(names.includes(evil), 'malicious beacon name shown as plain text');
  check(await page.evaluate(() => window.__xss === undefined), 'no script from a beacon name ran');
  check((await page.$$('.beacon img')).length === 0, 'no <img> element injected');
  check(!(await page.textContent('#beaconList')).includes('thay đổi theo thời gian') && (await page.$$('#beaconList .warnt')).length === 0,
    'no address warning in the list from the address bits');
  check((await page.textContent('#wslot0')) === '', 'MAC found fixed: no warning, nothing shown again');
  check((await page.inputValue('#mac0')) === 'C0:11:22:33:44:01', 'stored MAC loaded into slot 1');
  check((await page.textContent('#seen0')).includes('-58 dBm'), 'slot 1 shows its beacon is heard');
  check((await page.textContent('#hdrSsid')).includes('VN-Presence-3A7C'), 'SSID in header');

  // Assign the second beacon to Presence 2 from the list
  const selects = await page.$$('.beacon select');
  await selects[1].selectOption('1');
  check((await page.inputValue('#mac1')) === 'D4:AA:BB:CC:DD:02', 'assign from list fills the MAC');
  check((await page.inputValue('#type1')) === 'random', 'assign from list fills the address type');
  check(await page.isChecked('#en1'), 'assign from list enables the slot');
  check((await page.textContent('#dirtyMsg')).length > 0, 'unsaved changes flagged');

  // Assigning the beacon of slot 1 to slot 3 is refused
  await selects[0].selectOption('2');
  check((await page.inputValue('#mac2')) === '', 'duplicate assign refused');
  check((await page.textContent('#banner')).includes('Presence 1'), 'duplicate assign explains which slot');

  // Invalid manual MAC blocks saving
  await page.check('#en2');
  await page.fill('#mac2', 'C0:11:22:33:44');
  await page.click('#btnSave');
  check((await page.textContent('#emac2')).length > 0, 'invalid MAC error shown');
  check(posts.filter(p => p.path === '/api/config').length === 0, 'nothing sent while invalid');

  // An invalid field in the folded RSSI filter section opens that section too
  const setRt1 = v => page.$eval('#rt1', (e, v) => { e.value = v; e.dispatchEvent(new Event('input')); }, v);
  await setRt1('-5');
  check(!(await page.isVisible('#rt1')), 'RSSI filter section folded before the save');
  await page.click('#btnSave');
  check(await page.isVisible('#rt1') && (await page.textContent('#ert1')).length > 0, 'invalid RSSI threshold: its sections opened');
  await setRt1('-80');

  // Manual duplicate (same MAC as slot 1, "any") is an error too
  await page.fill('#mac2', 'c0-11-22-33-44-01');
  check((await page.textContent('#emac2')).includes('Presence 1'), 'manual duplicate MAC flagged');
  // Fix it with a new beacon in another notation; out-of-range timeout flagged
  await page.fill('#mac2', 'aabbccddeeff');
  await page.fill('#to2', '5');
  check((await page.textContent('#eto2')).length > 0, 'timeout below 10 s flagged');
  await page.fill('#to2', '60');
  await page.fill('#label2', 'Phòng ngủ <b>x</b>');
  await page.fill('#scanWin', '120');
  check((await page.textContent('#scanInfo')).includes('75%'), 'scan duty shown with warning above 50%');
  await page.fill('#scanWin', '40');

  await page.click('#btnSave');
  await page.waitForTimeout(300);
  const saves = posts.filter(p => p.path === '/api/config');
  check(saves.length === 1, 'one save request sent');
  if (saves.length) {
    const body = JSON.parse(saves[0].body);
    check(body.slots.length === 5, 'save body has 5 slots');
    check(body.slots[0].model === 'CP101', 'model sent with the slot');
    check(body.slots[2].mac === 'AA:BB:CC:DD:EE:FF', 'manual MAC normalised to AA:BB:CC:DD:EE:FF');
    check(body.slots[1].mac === 'D4:AA:BB:CC:DD:02' && body.slots[1].addr_type === 'random', 'assigned slot saved with type');
    check(body.slots[2].label === 'Phòng ngủ <b>x</b>', 'label sent as typed (escaped by the device on output)');
    check(body.scan.interval_ms === 160 && body.scan.window_ms === 40, 'scan settings sent');
    check(body.slots[2].timeout_s === 60, 'timeout sent as number');
  }
  check((await page.textContent('#banner')).includes('Đã lưu'), 'save result shown');

  // Keepalive happens on interaction (not on the periodic polls)
  check(posts.some(p => p.path === '/api/keepalive'), 'keepalive sent on user interaction');
  // The device refuses a POST that is not application/json (blocks requests from pages of other sites)
  check(posts.every(p => p.type.startsWith('application/json')), 'every POST is sent as application/json');

  // A scan refresh while a "Gán vào…" picker is open must not rebuild the list (it closes the picker on phones)
  const picker = (await page.$$('.beacon select'))[2];
  await picker.focus();
  await page.waitForTimeout(2500); // more than one scan poll (2 s)
  check(await picker.evaluate(e => e.isConnected), 'beacon list not rebuilt while a picker has focus');
  await page.evaluate(() => document.activeElement.blur());
  await page.waitForTimeout(100);
  check(!(await picker.evaluate(e => e.isConnected)), 'beacon list refreshed once the picker is left');

  // Battery of the beacons in the list (Eddystone-TLM), and the one-chip version shows no model field
  check((await page.textContent('#beaconList')).includes('94% · 2950 mV'), 'battery of a beacon shown in the list');
  check((await page.textContent('#beaconList')).includes('chưa thấy gói Eddystone-TLM'), 'beacon without TLM: no battery');
  check((await page.textContent('#seen0')).includes('pin 94%'), 'slot shows the battery of its beacon');
  check(!(await page.isVisible('#modelBox0')), 'one-chip version: no model field');

  if (out) await page.screenshot({ path: path.join(out, 'config_page_mobile.png'), fullPage: true });

  // Restart
  await page.click('#btnRestart');
  await page.waitForTimeout(200);
  check(posts.some(p => p.path === '/api/restart'), 'restart request sent');
  check(dialogs === 0, 'no confirm dialog after a successful save');

  // Texts of the ESP32-C3 version (Matter over Wi-Fi)
  const wifi = await openPage(browser, 'wifi', []);
  await wifi.waitForTimeout(300);
  check((await wifi.textContent('#statusGrid')).includes('Matter/Wi-Fi'), 'Wi-Fi version: mode shows Matter/Wi-Fi');
  check(!(await wifi.isVisible('#h2ResetBox')) && (await wifi.textContent('#home0')) === '', 'one-chip version: no bridge items');
  check((await wifi.textContent('#coexNote')).includes('mức Y'), 'Wi-Fi version: coexistence note (Y)');
  check(!(await wifi.textContent('#coexNote')).includes('Thread'), 'Wi-Fi version: no Thread in the scan note');
  await wifi.fill('#scanWin', '120');
  check((await wifi.textContent('#scanInfo')).includes('Wi-Fi'), 'Wi-Fi version: duty warning names Wi-Fi');

  // Two-chip version: the ESP32-C3 scanner's page (Matter on the ESP32-H2), opened in the seconds after the phone
  // joined, while BLE scanning is still held off
  status.scan = Object.assign({}, status.scan, { state: 'hold_off' });
  const h2 = await openPage(browser, 'h2', []);
  await h2.waitForTimeout(300);
  check((await h2.textContent('#scanChip')) === 'Sắp quét', 'scanning held off after the phone joined: "Sắp quét"');
  check((await h2.textContent('#statusGrid')).includes('Matter/Thread (ESP32-H2)'), 'two-chip: mode names the ESP32-H2');
  check((await h2.textContent('#statusGrid')).includes('3497-011-2332'), 'two-chip: Matter pairing code shown (no console needed)');
  check((await h2.textContent('#statusGrid')).includes('đang ở chế độ ghép'), 'two-chip: pairing mode of the H2 shown');
  check((await h2.textContent('#statusGrid')).includes('1/20 · 1 đang chờ · bộ nhớ H2 trống 52 KB'), 'two-chip: devices, waiting and H2 memory');
  check((await h2.$$('.slot')).length === 20, 'two-chip: 20 slots');
  check((await h2.textContent('#slotsTitle')) === 'Presence 1–20', 'two-chip: title with 20 slots');
  check(await h2.evaluate(() => document.getElementById('det0').open && !document.getElementById('det5').open), 'two-chip: slots in use open, empty ones folded');
  check((await h2.textContent('#sum5')).includes('trống'), 'two-chip: folded slot says it is empty');
  check((await h2.textContent('#home0')) === 'Trên Nhà: thiết bị con C0:11:22:33:44:01 (endpoint 2)', 'two-chip: slot 1 is a bridged device named by its MAC');
  check((await h2.textContent('#home1')).startsWith('Chưa có trên Nhà'), 'two-chip: inactive slot has no device');
  check((await h2.textContent('#home3')).startsWith('Chờ ghép Matter xong'), 'two-chip: device waiting for commissioning explained');
  check((await h2.textContent('#slotsNote')).includes('iBeacon Bridge') && (await h2.textContent('#slotsNote')).includes('Haxeko'), 'two-chip: slots explained as bridged devices');
  check(await h2.isVisible('#modelBox0') && (await h2.inputValue('#model0')) === 'CP101', 'two-chip: model field with the saved model');
  check(await h2.isVisible('#h2ResetBox') && await h2.isVisible('#btnH2Pair'), 'two-chip: H2 reset and pairing mode offered');
  const h2Posts = [];
  h2.on('request', r => { if (r.method() === 'POST') h2Posts.push({ path: new URL(r.url()).pathname, body: r.postData() }); });
  h2.on('dialog', async d => { await d.accept(); });
  await h2.click('#h2ResetBox summary');
  await h2.click('#btnH2Reset');
  await h2.waitForTimeout(300);
  const reset = h2Posts.find(p => p.path === '/api/h2/reset');
  check(reset && JSON.parse(reset.body).confirm === 'RESET-MATTER', 'two-chip: H2 reset sent with its confirmation word');
  await h2.click('#btnH2Pair');
  await h2.waitForTimeout(300);
  check(h2Posts.some(p => p.path === '/api/h2/pair'), 'two-chip: pairing mode asked on the page');

  // Assign the CP101 of the list to slot 6 (folded, empty): the model is its advertised name, 20 slots saved
  const h2Selects = await h2.$$('.beacon select');
  check((await h2Selects[3].$$('option')).length === 21, 'two-chip: the picker offers the 20 slots');
  await h2.fill('#mac3', ''); // free the CP101 from slot 4 first
  await h2Selects[3].selectOption('5');
  check(await h2.evaluate(() => document.getElementById('det5').open), 'two-chip: assigned slot unfolds');
  check((await h2.inputValue('#model5')) === 'CP101', 'two-chip: model taken from the advertised name');
  await h2.fill('#model5', 'ệ'.repeat(12)); // 12 characters, 36 bytes of UTF-8 (the field limits characters)
  check((await h2.textContent('#emodel5')).includes('32 byte'), 'two-chip: model longer than 32 bytes flagged');
  await h2.fill('#model5', 'CP101 phòng khách');
  await h2.click('#btnSave');
  await h2.waitForTimeout(300);
  const h2Save = h2Posts.find(p => p.path === '/api/config');
  if (h2Save) {
    const b = JSON.parse(h2Save.body);
    check(b.slots.length === 20, 'two-chip: save body has 20 slots');
    check(b.slots[5].mac === 'E1:22:33:44:55:66' && b.slots[5].model === 'CP101 phòng khách', 'two-chip: slot 6 saved with its model');
  } else check(false, 'two-chip: save sent');
  if (out) await h2.screenshot({ path: path.join(out, 'config_page_two_chip.png'), fullPage: true });

  // Not enough memory on the H2: explained per slot
  const h2mem = await openPage(browser, 'h2', [], { h2: { commissioned: true, fabrics: 1, network_up: true, window_open: false, pairing: null } });
  await h2mem.waitForTimeout(300);
  check((await h2mem.textContent('#home3')).includes('không đủ bộ nhớ'), 'two-chip: device waiting for memory explained');
  check((await h2mem.textContent('#statusGrid')).includes('ẩn vì H2 đã ghép'), 'two-chip: code hidden once paired and not in pairing mode');
  // The H2 could not build the device: told per slot, whatever the commissioning state
  const h2err = await openPage(browser, 'h2', [], { h2: { errors: Array.from({ length: 20 }, (_, i) => i === 3) } });
  await h2err.waitForTimeout(300);
  check((await h2err.textContent('#home3')).includes('không thêm được'), 'two-chip: device the H2 failed to add explained');
  check((await h2err.textContent('#home4')).startsWith('Chưa có trên Nhà'), 'two-chip: failure shown only on its slot');
  check((await h2.textContent('#statusGrid')).includes('chưa ghép · Thread chưa kết nối'), 'two-chip: H2 status shown');
  check((await h2.textContent('#coexNote')).includes('hai chip'), 'two-chip: scan note');
  await h2.fill('#scanWin', '120');
  check(!(await h2.textContent('#scanInfo')).includes('⚠'), 'two-chip: no warning above 50 % (own radio)');

  // MAC watch: running (minutes shown), changed (warning naming the new address), new MAC not watched yet
  const watched = await openPage(browser, 'thread', [], { watch: [
    { watch: { mac: 'C0:11:22:33:44:01', state: 'running', watched_s: 725, total_s: 1800, new_mac: '' } },
    { enabled: true, mac: 'D4:AA:BB:CC:DD:02', addr_type: 'random',
      watch: { mac: 'D4:AA:BB:CC:DD:02', state: 'changed', watched_s: 300, total_s: 1800, new_mac: '5A:01:02:03:04:05' } },
    { enabled: true, mac: 'AA:BB:CC:DD:EE:01', addr_type: 'any',
      watch: { mac: 'AA:BB:CC:DD:EE:99', state: 'fixed', watched_s: 1800, total_s: 1800, new_mac: '' } } ] });
  await watched.waitForTimeout(300);
  check((await watched.textContent('#wslot0')).includes('Đang theo dõi địa chỉ MAC: 12/30 phút'), 'MAC watch running: minutes shown');
  const changedText = await watched.textContent('#wslot1');
  check(changedText.startsWith('⚠ Beacon đổi địa chỉ MAC') && changedText.includes('5A:01:02:03:04:05'), 'MAC changed: warned, with the new address');
  check(await watched.evaluate(() => document.getElementById('wslot1').className === 'warnt' && document.getElementById('wslot0').className === 'small'),
    'only a change is shown as a warning');
  check((await watched.textContent('#wslot2')).includes('Sau khi lưu'), 'another MAC than the watched one: watched after saving');
  await watched.fill('#mac2', 'AA:BB:CC:DD:EE:99');
  check((await watched.textContent('#wslot2')) === '', 'typing back the MAC found fixed: nothing shown');

  // Slow link: the device takes 3 s per beacon list, longer than the 2 s poll period. One request at a time.
  const slowStats = {};
  const slow = await openPage(browser, 'wifi', [], { scanDelayMs: 3000, stats: slowStats });
  await slow.waitForTimeout(9000);
  check(slowStats.scanMaxInFlight === 1, `slow link: one beacon list request at a time (max ${slowStats.scanMaxInFlight})`);
  check(slowStats.scanRequests >= 2, `slow link: polling goes on (${slowStats.scanRequests} requests)`);

  await browser.close();
  console.log(failures ? `${failures} FAILED` : 'UI checks passed');
  process.exit(failures ? 1 : 0);
})();
