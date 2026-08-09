import type { EngineData, MatchPlanData, Resource } from '../api/types'

export type LifecycleAction = 'start' | 'pause' | 'resume' | 'cancel'

type Competition = Resource<MatchPlanData>

export function competitionEngineOptions(engines: Resource<EngineData>[]) {
  const displayNameCounts = new Map<string, number>()
  for (const engine of engines) {
    const key = engine.data.display_name.toLocaleLowerCase()
    displayNameCounts.set(key, (displayNameCounts.get(key) ?? 0) + 1)
  }
  return [...engines]
    .sort((left, right) => {
      const leftPreferred =
        left.data.component_id === 'uci_arena.engine' && left.data.recommended === true
      const rightPreferred =
        right.data.component_id === 'uci_arena.engine' && right.data.recommended === true
      if (leftPreferred !== rightPreferred) return leftPreferred ? -1 : 1
      return left.data.display_name.localeCompare(right.data.display_name)
    })
    .map((engine) => {
      const preferred =
        engine.data.component_id === 'uci_arena.engine' && engine.data.recommended === true
      const duplicated =
        (displayNameCounts.get(engine.data.display_name.toLocaleLowerCase()) ?? 0) > 1
      const identity = duplicated
        ? ` · ${engine.data.component_version ?? 'unversioned'} · ${engine.id.slice(0, 8)}`
        : ''
      return {
        value: engine.id,
        label: `${engine.data.display_name}${preferred ? ' · preferred' : ''}${identity}`,
      }
    })
}

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
        throw new Error('Competition no longer exists; refresh the tournament list.', {
          cause: error,
        })
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
