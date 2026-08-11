import { readFile, readdir } from 'node:fs/promises'
import { extname, join, relative } from 'node:path'
import { fileURLToPath } from 'node:url'

import { isNamedModelTestFixture } from './model-product-naming-policy.mjs'

const root = fileURLToPath(new URL('..', import.meta.url))
const self = fileURLToPath(import.meta.url)
const roots = ['README.md', 'CMakeLists.txt', 'apps', 'include', 'src', 'web/src', 'tests', 'specs']
const textExtensions = new Set([
  '',
  '.cpp',
  '.h',
  '.hpp',
  '.json',
  '.md',
  '.mjs',
  '.ps1',
  '.ts',
  '.tsx',
  '.yaml',
  '.yml',
])
const modelName = /flashfork|latticeknight/i
const requiredIdentities = new Map([
  ['src/application/engine_onboarding.cpp', ['"uci_arena_engine.exe"', '"uci_arena_engine"']],
  ['specs/product_contract_v1.yaml', ['product_engine_executable: uci_arena_engine']],
])

async function filesUnder(path) {
  const entries = await readdir(path, { withFileTypes: true })
  const files = []
  for (const entry of entries) {
    const child = join(path, entry.name)
    if (entry.isDirectory()) {
      files.push(...await filesUnder(child))
    } else if (entry.isFile() && textExtensions.has(extname(entry.name))) {
      files.push(child)
    }
  }
  return files
}

const files = []
for (const candidate of roots) {
  const path = join(root, candidate)
  if (extname(candidate) || candidate.endsWith('.md')) {
    files.push(path)
  } else {
    files.push(...await filesUnder(path))
  }
}

const violations = []
for (const [relativePath, requiredValues] of requiredIdentities) {
  const text = await readFile(join(root, relativePath), 'utf8')
  for (const required of requiredValues) {
    if (!text.includes(required)) {
      violations.push(`${relativePath}: required neutral identity missing: ${required}`)
    }
  }
}
for (const path of files) {
  if (path === self) {
    continue
  }
  const relativePath = relative(root, path)
  const lines = (await readFile(path, 'utf8')).split(/\r?\n/u)
  lines.forEach((line, index) => {
    if (modelName.test(line) && !isNamedModelTestFixture(relativePath)) {
      violations.push(`${relativePath}:${index + 1}: ${line.trim()}`)
    }
  })
}

if (violations.length > 0) {
  process.stderr.write(`${violations.join('\n')}\n`)
  process.exitCode = 1
} else {
  process.stdout.write('model/product naming boundary: ok\n')
}
