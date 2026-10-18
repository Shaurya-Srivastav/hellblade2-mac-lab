import { syncGraphicsPreset } from '../../Documents/HeroicGamesLauncher/src/backend/utils/macos/graphics_presets'

const [prefix, gameRoot] = process.argv.slice(2)
syncGraphicsPreset(prefix, [gameRoot], true, { dlss: true }).then((result) =>
  console.log(JSON.stringify(result, null, 1))
)
