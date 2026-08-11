import { describe, expect, it } from 'vitest'
import {
  availableCompetitionActions,
  competitionEngineOptions,
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
  it('makes duplicate engine registrations distinguishable without hiding preference', () => {
    const engines = [
      {
        id: '019fe5c3-first-engine',
        data: {
          display_name: 'UCI Arena Engine',
          component_id: 'uci_arena.engine',
          component_version: '0.1.6',
          recommended: true,
        },
      },
      {
        id: '019e0000-stale-engine',
        data: {
          display_name: 'UCI Arena Engine',
          component_id: 'uci_arena.engine',
          component_version: '0.1.4',
          recommended: true,
        },
      },
    ] as Resource<import('../api/types').EngineData>[]
    expect(competitionEngineOptions(engines)).toEqual([
      {
        value: '019fe5c3-first-engine',
        label: 'UCI Arena Engine · preferred · 0.1.6 · 019fe5c3',
      },
      {
        value: '019e0000-stale-engine',
        label: 'UCI Arena Engine · preferred · 0.1.4 · 019e0000',
      },
    ])
  })

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
