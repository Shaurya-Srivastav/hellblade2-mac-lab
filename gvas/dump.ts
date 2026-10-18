import { readFileSync } from 'fs'
import { parseGvas } from '../../Documents/HeroicGamesLauncher/src/backend/utils/macos/gvas'
const file = parseGvas(readFileSync(process.argv[2]))
const show = (v: unknown): unknown =>
  Buffer.isBuffer(v) ? v.toString('hex') : v
console.log(JSON.stringify(file.properties, (_k, v) => (v && v.type === 'Buffer' ? Buffer.from(v.data).toString('hex') : show(v)), 1).slice(0, 20000))
