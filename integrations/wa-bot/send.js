const qrcode = require('qrcode-terminal');
const makeWASocket = require('@whiskeysockets/baileys').default;
const { useMultiFileAuthState } = require('@whiskeysockets/baileys');
const P = require('pino');

// Usage: node send.js "+92300155681" "Message text"
const [ , , phoneRaw, ...msgParts ] = process.argv;
if (!phoneRaw || msgParts.length === 0) {
  console.error('Usage: node send.js "+92300155681" "Message text"');
  process.exit(1);
}
const message = msgParts.join(' ');
const jid = phoneRaw.replace(/\+/g,'').replace(/\D/g,'') + '@s.whatsapp.net';

(async () => {
  const authDir = process.env.WA_AUTH_DIR || (process.env.HOME + '/wa-bot/auth');
  const { state, saveCreds } = await useMultiFileAuthState(authDir);
  const sock = makeWASocket({
    printQRInTerminal: true,       // show QR in Termux on first login
    auth: state,
    logger: P({ level: 'silent' }),
    browser: ['Termux','Chrome','1.0'],
  });

  sock.ev.on('creds.update', saveCreds);

  sock.ev.on('connection.update', (u) => {
    const { connection, lastDisconnect, qr } = u;
    if (qr) {
      console.log('📱 Scan this QR code in WhatsApp (Linked Devices):');
      qrcode.generate(qr, { small: true });
    }
    if (connection === 'open') {
      sock.sendMessage(jid, { text: message })
        .then(() => {
          console.log('📤 Sent to', phoneRaw, ':', message);
          process.exit(0);
        })
        .catch(err => {
          console.error('❌ Send failed:', err);
          process.exit(2);
        });
    }
    if (connection === 'close') {
      const reason = (lastDisconnect && lastDisconnect.error && (lastDisconnect.error.output?.statusCode || lastDisconnect.error.message)) || 'unknown';
      console.error('Connection closed:', reason);
      process.exit(1);
    }
  });
})();
