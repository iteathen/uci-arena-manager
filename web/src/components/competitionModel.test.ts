import { describe, expect, it } from 'vitest'
import {
  availableCompetitionActions,
  competitionPairingPreview,
  competitionPlanTarget,
  competitionScheduleSize,
  submitCompetitionLifecycleAction,
} from './competitionModel'
import type { MatchPlanData, Resource } from '../api/types'

function competition(lifecycle: string, revision: number): Resource<MatchPlanData> {
  return {
    id: 'competition-1',
    host_id: 'local',
    kind: 'match_plan',
    revision,
    created_at: '2026-01-01T00:00:00Z',
    updated_at: '2026-01-01T00:00:00Z',
    labels: {},
    data: { lifecycle } as MatchPlanData,
  }
}

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

  it('refreshes and retries a lifecycle action after bounded revision churn', async () => {
    const submitted: number[] = []
    const submit = async (current: Resource<MatchPlanData>) => {
      submitted.push(current.revision)
      if (submitted.length === 1) {
        throw Object.assign(new Error('changed'), { code: 'resource_revision_conflict' })
      }
      return 'cancelled'
    }
    await expect(submitCompetitionLifecycleAction(
      competition('running', 4),
      'cancel',
      { submit, refresh: async () => competition('running', 5) },
    )).resolves.toBe('cancelled')
    expect(submitted).toEqual([4, 5])
  })

  it('does not retry an action that became invalid after refresh', async () => {
    const submit = async () => {
      throw Object.assign(new Error('changed'), { code: 'resource_revision_conflict' })
    }
    await expect(submitCompetitionLifecycleAction(
      competition('running', 4),
      'cancel',
      { submit, refresh: async () => competition('completed', 5) },
    )).rejects.toThrow('cancel is no longer available')
  })

  it('stops after three revision-conflict attempts', async () => {
    let attempts = 0
    const submit = async () => {
      attempts += 1
      throw Object.assign(new Error('changed'), { code: 'resource_revision_conflict' })
    }
    await expect(submitCompetitionLifecycleAction(
      competition('running', 4),
      'cancel',
      {
        submit,
        refresh: async () => competition('running', 4 + attempts),
      },
    )).rejects.toMatchObject({ code: 'resource_revision_conflict' })
    expect(attempts).toBe(3)
  })

  it('projects the complete scheduled game count for every format', () => {
    expect(competitionScheduleSize('head_to_head', 2, 10, 1)).toBe(10)
    expect(competitionScheduleSize('round_robin', 4, 2, 1)).toBe(12)
    expect(competitionScheduleSize('gauntlet', 4, 2, 1)).toBe(6)
    expect(competitionScheduleSize('swiss', 5, 1, 3)).toBe(6)
    expect(competitionScheduleSize('sprt', 2, 20, 1)).toBe(20)
  })

  it('describes Swiss and color-reversed capacity truthfully', () => {
    expect(competitionPairingPreview('swiss', 6, 5)).toEqual({
      label: 'Games / round',
      value: '2',
    })
    expect(competitionPairingPreview('round_robin', 12, 4)).toEqual({
      label: 'Color-reversed sets',
      value: '6',
    })
    expect(competitionPairingPreview('head_to_head', 3, 2)).toEqual({
      label: 'Color-reversed sets',
      value: '1 + 1 unpaired game',
    })
  })
})
