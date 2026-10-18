import { readFileSync, writeFileSync } from 'fs'
import { addExports, D3DKMT_FUNCTIONS, readExports, peChecksum } from '../../Documents/HeroicGamesLauncher/src/backend/utils/macos/d3dkmt'
const stock = readFileSync(process.argv[2])
const out = addExports(stock, Object.fromEntries(Object.entries(D3DKMT_FUNCTIONS).map(([n, f]) => [n, f()])))
writeFileSync(process.argv[3], out)
for (const [n, f] of Object.entries(D3DKMT_FUNCTIONS)) writeFileSync(`${process.argv[3]}.${n}.bin`, f())
const before = readExports(stock), after = readExports(out)
console.log('exports', before.length, '->', after.length, 'checksum', peChecksum(out).toString(16), 'stock checksum ok', peChecksum(stock) === stock.readUInt32LE(stock.readUInt32LE(0x3c) + 24 + 64))
