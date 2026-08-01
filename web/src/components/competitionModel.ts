import type { MatchPlanData, Resource } from '../api/types'

export type LifecycleAction = 'start' | 'pause' | 'resume' | 'cancel'

type Competition = Resource<MatchPlanData>

function isRevisionConflict(error: unknown): boolean {
  return typeof error === 'object' && error !== null &&
    'code' in error && error.code === 'resource_revision_conflict'
}

export function availableCompetitionActions(lifecycle: string): LifecycleAction[] {
  if (lifecycle === 'planned') return ['start']
  if (lifecycle === 'running') return ['pause', 'cancel']
  if (lifecycle === 'paused') return ['resume', 'cancel']
  return []
}

export async function submitCompetitionLifecycleAction<T>(
  resource: Competition,
  action: LifecycleAction,
  operations: {
    submit: (current: Competition, action: LifecycleAction) => Promise<T>
    refresh: () => Promise<Competition | undefined>
  },
): Promise<T> {
  let current = resource
  for (let attempt = 0; attempt < 3; attempt += 1) {
    if (!availableCompetitionActions(current.data.lifecycle).includes(action)) {
      throw new Error(
        `Competition changed to ${current.data.lifecycle}; ${action} is no longer available.`,
      )
    }
    try {
      return await operations.submit(current, action)
    } catch (error) {
      if (!isRevisionConflict(error) || attempt === 2) throw error
      const refreshed = await operations.refresh()
      if (!refreshed) {
        throw new Error('Competition no longer exists; refresh the tournament list.')
      }
      current = refreshed
    }
  }
  throw new Error('Competition lifecycle retry bound was exhausted.')
}

export function competitionPlanTarget(format: string) {
  return format === 'head_to_head'
    ? { action: 'match.plan', kind: 'match_plan' as const }
    : { action: 'tournament.plan', kind: 'tournament' as const }
}

export function competitionScheduleSize(
  format: string,
  participants: number,
  gamesPerPair: number,
  rounds: number,
) {
  if (format === 'swiss') return Math.floor(participants / 2) * rounds
  if (format === 'gauntlet') return Math.max(0, participants - 1) * gamesPerPair
  const pairs = format === 'head_to_head' || format === 'sprt'
    ? participants === 2 ? 1 : 0
    : (participants * (participants - 1)) / 2
  return pairs * gamesPerPair
}

export function competitionPairingPreview(
  format: string,
  scheduledGames: number,
  participants: number,
) {
  if (format === 'swiss') {
    return {
      label: 'Games / round',
      value: String(Math.floor(participants / 2)),
    }
  }
  const completeSets = Math.floor(scheduledGames / 2)
  return {
    label: 'Color-reversed sets',
    value: scheduledGames % 2 === 0
      ? String(completeSets)
      : `${completeSets} + 1 unpaired game`,
  }
}
