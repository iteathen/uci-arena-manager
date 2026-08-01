import { describe, expect, it } from 'vitest'
import {
  availableCompetitionActions,
  competitionPlanTarget,
  competitionScheduleSize,
} from './competitionModel'

describe('competition operator model', () => {
  it('stores every multi-engine format as a tournament resource', () => {
    for (const format of ['round_robin', 'gauntlet', 'swiss', 'sprt']) {
      expect(competitionPlanTarget(format)).toEqual({
        action: 'tournament.plan',
        kind: 'tournament',
      })
    }
    expect(competitionPlanTarget('head_to_head')).toEqual({
      action: 'match.plan',
      kind: 'match_plan',
    })
  })

  it('offers only lifecycle-valid controls', () => {
    expect(availableCompetitionActions('planned')).toEqual(['start'])
    expect(availableCompetitionActions('running')).toEqual(['pause', 'cancel'])
    expect(availableCompetitionActions('paused')).toEqual(['resume', 'cancel'])
    for (const terminal of ['completed', 'cancelled', 'failed']) {
      expect(availableCompetitionActions(terminal)).toEqual([])
    }
  })

  it('projects the complete scheduled game count for every format', () => {
    expect(competitionScheduleSize('head_to_head', 2, 10, 1)).toBe(10)
    expect(competitionScheduleSize('round_robin', 4, 2, 1)).toBe(12)
    expect(competitionScheduleSize('gauntlet', 4, 2, 1)).toBe(6)
    expect(competitionScheduleSize('swiss', 5, 1, 3)).toBe(6)
    expect(competitionScheduleSize('sprt', 2, 20, 1)).toBe(20)
  })
})
