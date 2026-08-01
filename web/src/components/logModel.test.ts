import { describe, expect, it } from 'vitest'
import { logResourceTypeOptions } from './logModel'

describe('logResourceTypeOptions', () => {
  it('covers the complete public ResourceKind contract exactly once', () => {
    const values = logResourceTypeOptions.map((option) => option.value)

    expect(values).toEqual([
      'all',
      'host',
      'engine',
      'bot',
      'managed_service',
      'health_probe',
      'match_plan',
      'tournament',
      'game',
      'operation',
      'report',
      'artifact',
      'secret_reference',
      'log_source',
      'health_snapshot',
    ])
    expect(new Set(values).size).toBe(values.length)
  })
})
