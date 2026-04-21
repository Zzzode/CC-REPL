import { chmodSync, mkdirSync, writeFileSync } from 'fs'
import { dirname, resolve } from 'path'

const outfile = resolve('dist/cli.js')
mkdirSync(dirname(outfile), { recursive: true })

const wrapper = `#!/usr/bin/env bun
import '../src/entrypoints/cli.tsx'
`

writeFileSync(outfile, wrapper, 'utf8')
chmodSync(outfile, 0o755)

console.log(`Built ${outfile}`)
