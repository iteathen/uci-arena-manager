import { describe, expect, it } from 'vitest'
import { makeCommand } from './client'

describe('command envelope', () => {
  it('uses one typed envelope for dashboard mutations', () => {
    const command = makeCommand('match.plan', 'match_plan', { engine_ids: ['a', 'b'] }, true)
    expect(command.schema_version).toBe(1)
    expect(command.request_id).toMatch(/^[0-9a-f-]{36}$/)
    expect(command.idempotency_key).toContain(command.request_id)
    expect(command.target).toEqual({ kind: 'match_plan', id: null })
    expect(command.dry_run).toBe(true)
  })
})
