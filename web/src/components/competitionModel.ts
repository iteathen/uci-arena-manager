export type LifecycleAction = 'start' | 'pause' | 'resume' | 'cancel'

export function availableCompetitionActions(lifecycle: string): LifecycleAction[] {
  if (lifecycle === 'planned') return ['start']
  if (lifecycle === 'running') return ['pause', 'cancel']
  if (lifecycle === 'paused') return ['resume', 'cancel']
  return []
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
