import { describe, expect, it } from 'vitest'
import { setupCommitNotice } from './ComponentSetupButton'

describe('component setup confirmation', () => {
  it('does not claim that a personal install requires elevation', () => {
    expect(setupCommitNotice({
      schema: 'arena_ui_setup_plan_v1',
      elevation_required: false,
      manager_service_restart: false,
    })).toContain('without UAC')
  })

  it('makes all-users elevation and restart explicit', () => {
    const notice = setupCommitNotice({
      schema: 'arena_ui_setup_plan_v1',
      elevation_required: true,
      manager_service_restart: true,
    })
    expect(notice).toContain('Administrator confirmation is required')
    expect(notice).toContain('restart and reconnect')
  })
})
