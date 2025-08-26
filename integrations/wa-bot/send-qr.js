const { default: makeWASocket, useMultiFileAuthState, fetchLatestBaileysVersion } = require('@whiskeysockets/baileys');
const QRCode = require('qrcode');
const fs = require('fs');
const path = require('path');
const P = require('pino');

const HOME = process.env.HOME || '/data/data/com.termux/files/home';
const localPng = path.join(HOME, 'wa-bot', 'wa-qr.png');
const dlPng = '/sdcard/Download/wa-qr.png';

(async () => {
  try {
    console.log('🟡 Starting Baileys QR generator (with version fetch)...');
    const { version } = await fetchLatestBaileysVersion();
    console.log('ℹ️  Using WA Web version:', version.join('.'));

    const authDir = path.join(HOME, 'wa-bot', 'auth');
    const logger = P({ level: 'info' }); // show reasons if it fails
    const { state, saveCreds } = await useMultiFileAuthState(authDir);

    const sock = makeWASocket({
      version,
      printQRInTerminal: false,
      auth: state,
      logger,
      browser: ['Termux','Chrome','1.0'],
      connectTimeoutMs: 30000
    });

    sock.ev.on('creds.update', saveCreds);

    let saved = false;

    sock.ev.on('connection.update', async (u) => {
      const { connection, lastDisconnect, qr } = u;
      if (qr) {
        console.log('🟠 QR received, writing PNG...');
        try {
          await QRCode.toFile(localPng, qr, { width: 520, margin: 1 });
          console.log('✅ Saved:', localPng);
          try {
            fs.copyFileSync(localPng, dlPng);
            console.log('✅ Also copied to:', dlPng, '— open Downloads/Gallery to scan');
          } catch (e) {
            console.log('⚠️ Could not copy to', dlPng, '-', e.message);
            console.log('  Tip: run once: termux-setup-storage');
          }
          saved = true;
        } catch (e) {
          console.error('❌ QR to PNG failed:', e.message);
        }
      }
      if (connection === 'open') {
        console.log('🟢 Already logged in — no QR needed.');
        process.exit(0);
      }
      if (connection === 'close') {
        const err = lastDisconnect?.error;
        const msg = err?.message || err?.stack || String(err) || 'unknown';
        console.log('🔴 Connection closed:', msg);
        // Common hints
        console.log('💡 Hints: check internet, correct time, TLS certs, or try another network/VPN.');
        setTimeout(() => process.exit(saved ? 0 : 1), 800);
      }
    });

    // Safety timer
    setTimeout(() => {
      if (!saved) {
        console.log('⏳ Timeout: no QR received. Try again after checking network.');
        process.exit(1);
      } else {
        process.exit(0);
      }
    }, 90000);

  } catch (err) {
    console.error('❌ Fatal:', err.message);
    process.exit(1);
  }
})();
