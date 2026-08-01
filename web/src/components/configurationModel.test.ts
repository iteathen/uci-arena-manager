import { describe, expect, it } from 'vitest'
import { managerEndpointLabel } from './configurationModel'

describe('managerEndpointLabel', () => {
  it('uses the active page endpoint instead of a default port', () => {
    expect(managerEndpointLabel({ host: '127.0.0.1:48992' })).toBe('127.0.0.1:48992')
    expect(managerEndpointLabel({ host: '' })).toBe('unavailable')
  })
})
