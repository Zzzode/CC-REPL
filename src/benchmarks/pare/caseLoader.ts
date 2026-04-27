import { createHash } from 'node:crypto'
import { readFile } from 'node:fs/promises'
import path from 'node:path'
import { CaseSetSchema, type PareCaseSet } from './schema.js'

export async function loadCaseSet(casesPath: string): Promise<{ caseSet: PareCaseSet; absolutePath: string; hash: string }> {
  const absolutePath = path.isAbsolute(casesPath)
    ? casesPath
    : path.resolve(process.cwd(), casesPath)
  const raw = await readFile(absolutePath, 'utf8')
  const parsed = CaseSetSchema.parse(JSON.parse(raw))
  const sorted = {
    ...parsed,
    cases: [...parsed.cases].sort((a, b) => a.id.localeCompare(b.id)),
  }
  const hashValue = createHash('sha256').update(JSON.stringify(sorted)).digest('hex')
  return { caseSet: sorted, absolutePath, hash: 'sha256:' + hashValue }
}
