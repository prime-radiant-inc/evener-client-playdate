// Explicit host qualification: bootstrap a relay pin over verified TLS.
import { readFile, writeFile, rename } from 'node:fs/promises';
import { connect } from 'node:tls';

const path = process.argv[2];
if (!path) throw new Error('Usage: node check/pin-relay.mjs ISOLATED-ENROLLMENT.json');
const config = JSON.parse(await readFile(path, 'utf8'));
if (!/^[a-z0-9.-]+$/i.test(config.derp_host) || !Number.isInteger(config.derp_port)) throw new Error('Invalid relay destination');
const key = await new Promise((resolve, reject) => {
  const socket = connect({ host: config.derp_host, port: config.derp_port, servername: config.derp_host });
  let input = Buffer.alloc(0);
  socket.setTimeout(15000, () => socket.destroy(new Error('Relay pin timed out')));
  socket.once('error', reject);
  socket.once('secureConnect', () => socket.write(`GET /derp HTTP/1.1\r\nHost: ${config.derp_host}\r\nConnection: Upgrade\r\nUpgrade: DERP\r\n\r\n`));
  socket.on('data', (bytes) => {
    input = Buffer.concat([input, bytes]);
    if (input.length > 8192) { socket.destroy(new Error('Relay response exceeds bound')); return; }
    const end = input.indexOf('\r\n\r\n');
    if (end < 0 || input.length < end + 4 + 45) return;
    const frame = input.subarray(end + 4);
    if (!/^HTTP\/1\.[01] 101 /.test(input.toString('ascii', 0, end)) || frame[0] !== 1 || frame.readUInt32BE(1) !== 40 || !frame.subarray(5, 13).equals(Buffer.from('DERP🔑'))) {
      socket.destroy(new Error('Invalid authenticated relay greeting')); return;
    }
    resolve(frame.subarray(13, 45).toString('hex')); socket.destroy();
  });
  socket.once('end', () => reject(new Error('Relay closed before greeting')));
});
config.derp_public_key = key;
await writeFile(`${path}.tmp`, JSON.stringify(config), { mode: 0o600 });
await rename(`${path}.tmp`, path);
console.log('PASS relay public key pinned through verified TLS');
