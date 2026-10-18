import { readFileSync } from 'fs'
import { findProperty, mapEntry, parseGvas } from '../../Documents/HeroicGamesLauncher/src/backend/utils/macos/gvas'
import {
  GRAPHICS_PRESET_PROFILES,
  upgradeGraphicsSave
} from '../../Documents/HeroicGamesLauncher/src/backend/utils/macos/graphics_presets'

const [before, live] = process.argv.slice(2)
const result = upgradeGraphicsSave(
  readFileSync(before),
  GRAPHICS_PRESET_PROFILES[0],
  { dlss: true, frameGeneration: true },
  { width: 5120, height: 1440 }
)
console.log('changes:', result?.changes)
const dlss = (data: Buffer) =>
  JSON.stringify(
    mapEntry(findProperty(parseGvas(data).properties, 'UpscalerSettings'), 'DLSS')
  )
console.log(
  'DLSS entry identical to the one the game wrote:',
  !!result && dlss(result.data) === dlss(readFileSync(live))
)
