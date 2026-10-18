import { readFileSync } from 'fs'
import { parseGvas } from '../../Documents/HeroicGamesLauncher/src/backend/utils/macos/gvas'

const show = (value: unknown): unknown =>
  JSON.parse(
    JSON.stringify(value, (key, v) =>
      key === 'structGuid' ? undefined : v?.type === 'Buffer' ? `<${v.data.length} bytes>` : v
    )
  )

const save = parseGvas(readFileSync(process.argv[2]))
const names = process.argv.slice(3)
for (const property of save.properties) {
  if (!names.length || names.includes(property.name)) {
    console.log(JSON.stringify(show(property), null, 1))
  }
}
