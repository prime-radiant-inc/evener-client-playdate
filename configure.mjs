import { randomBytes } from 'node:crypto';
import { chmodSync, mkdirSync, readFileSync, unlinkSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { parseArgs } from 'node:util';

const { values } = parseArgs({ options: {
  'hub-url': { type: 'string' },
  'hub-token-file': { type: 'string' },
  'tailnet-config': { type: 'string' },
  'setup-only': { type: 'boolean' },
  'output': { type: 'string', default: 'Source' },
} });
if (!process.env.OPENAI_API_KEY || (!values['setup-only'] && (!values['hub-url'] || !values['hub-token-file'])))
  throw new Error('Supply OPENAI_API_KEY and either --setup-only or --hub-url with --hub-token-file');
if (values['setup-only'] && (values['tailnet-config'] || values['hub-url'] || values['hub-token-file']))
  throw new Error('--setup-only creates a private build without Hub or tailnet credentials');
const url = values['setup-only'] ? null : new URL(values['hub-url']);
if (url && (!['http:', 'https:', 'ws:', 'wss:'].includes(url.protocol) || url.username || url.password || url.search || url.hash ||
    !/^[a-z0-9.-]{1,255}$/.test(url.hostname) || url.pathname.length > 255))
  throw new Error('Use an HTTP(S) or WS(S) Hub URL with an IPv4 address or hostname and no credentials, query or fragment');
const tls = url && ['https:', 'wss:'].includes(url.protocol);
const token = values['setup-only'] ? '' : readFileSync(values['hub-token-file'], 'utf8').trim();
if ((!token && !values['setup-only']) || token.length > 511 || process.env.OPENAI_API_KEY.length > 511 || /[\r\n]/.test(token + process.env.OPENAI_API_KEY))
  throw new Error('Credentials must be nonempty single-line values');
const config = {
  hubHost: url?.hostname || '', hubPort: url ? Number(url.port || (tls ? 443 : 80)) : 0,
  hubTLS: Boolean(tls), hubPath: url ? (url.pathname === '/' ? '/rpc' : url.pathname) : '',
  hubToken: token, openaiKey: process.env.OPENAI_API_KEY,
  transportSeed: randomBytes(32).toString('hex'),
  useTailnet: Boolean(values['tailnet-config']),
};
mkdirSync(values.output, { recursive: true, mode: 0o700 });
if (values['tailnet-config']) {
  if (tls) throw new Error('Use an HTTP or WS Hub URL inside the encrypted tailnet');
  const privateConfig = readFileSync(values['tailnet-config'], 'utf8');
  const tailnet = JSON.parse(privateConfig);
  if (!tailnet.machine_private_key || !tailnet.control_public_key || !tailnet.peer_id)
    throw new Error('Export the enrolled native control configuration before building');
  if (typeof tailnet.peer_ip !== 'string' || !/^\d+\.\d+\.\d+\.\d+$/.test(tailnet.peer_ip) ||
      !Number.isInteger(tailnet.peer_port) || tailnet.peer_port < 1 || tailnet.peer_port > 65535)
    throw new Error('Tailnet configuration has no valid Hub address');
  config.hubHost = tailnet.peer_ip;
  config.hubPort = tailnet.peer_port;
  writeFileSync(join(values.output, 'tailnet.json'), privateConfig, { mode: 0o600 });
  chmodSync(join(values.output, 'tailnet.json'), 0o600);
} else {
  // A setup-only or direct-network build must not carry an old node identity.
  try { unlinkSync(join(values.output, 'tailnet.json')); }
  catch (error) { if (error.code !== 'ENOENT') throw error; }
}
writeFileSync(join(values.output, 'config.json'), JSON.stringify(config), { mode: 0o600 });
chmodSync(join(values.output, 'config.json'), 0o600);
console.log('Private device configuration written. Credentials are not printed.');
