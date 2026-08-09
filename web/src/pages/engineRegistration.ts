import type { EngineCandidate, EngineData, Resource } from '../api/types'

export function engineRegistrationParameters(candidate: EngineCandidate) {
  return {
    display_name: candidate.display_name,
    executable: candidate.executable,
    working_directory: candidate.working_directory,
    arguments: candidate.arguments,
    uci_options: candidate.uci_options,
    candidate_id: candidate.candidate_id,
  }
}

function samePath(left: string | undefined, right: string | undefined) {
  if (!left || !right) return false
  if (left === right) return true
  const windowsPath = /^(?:[a-z]:[\\/]|\\\\)/i
  return windowsPath.test(left) && windowsPath.test(right)
    ? left.replaceAll('/', '\\').toLowerCase() === right.replaceAll('/', '\\').toLowerCase()
    : false
}

function sameValue(left: unknown, right: unknown): boolean {
  if (left === right) return true
  if (Array.isArray(left) || Array.isArray(right)) {
    return Array.isArray(left) && Array.isArray(right) &&
      left.length === right.length && left.every((value, index) => sameValue(value, right[index]))
  }
  if (left && right && typeof left === 'object' && typeof right === 'object') {
    const leftRecord = left as Record<string, unknown>
    const rightRecord = right as Record<string, unknown>
    const leftKeys = Object.keys(leftRecord).sort()
    const rightKeys = Object.keys(rightRecord).sort()
    return sameValue(leftKeys, rightKeys) &&
      leftKeys.every((key) => sameValue(leftRecord[key], rightRecord[key]))
  }
  return false
}

export type CandidateRegistration = {
  state: 'unregistered' | 'registered' | 'refresh_required'
  engine?: Resource<EngineData>
}

export function candidateRegistration(
  candidate: EngineCandidate,
  engines: Resource<EngineData>[],
): CandidateRegistration {
  const exact = engines.filter(
    (engine) => engine.data.candidate_id === candidate.candidate_id,
  )
  const pathMatches = engines.filter(
    (engine) => samePath(engine.data.executable, candidate.executable),
  )
  const engine = exact[0] ?? (
    candidate.source === 'install_receipt'
      ? pathMatches.find(
          (entry) => entry.data.onboarding_source === 'install_receipt' &&
            entry.data.component_id === candidate.component_id,
        )
      : pathMatches[0]
  )
  if (!engine) return { state: 'unregistered' }
  if (candidate.source !== 'install_receipt' ||
      engine.data.onboarding_source !== 'install_receipt') {
    return { state: 'registered', engine }
  }
  const current = engine.data
  const currentMatches =
    current.candidate_id === candidate.candidate_id &&
    current.component_id === candidate.component_id &&
    current.component_version === candidate.component_version &&
    samePath(current.working_directory, candidate.working_directory) &&
    sameValue(current.arguments ?? [], candidate.arguments) &&
    sameValue(current.option_overrides ?? {}, candidate.uci_options) &&
    sameValue(current.launch_profile ?? null, candidate.launch_profile ?? null) &&
    sameValue(current.default_model ?? null, candidate.default_model ?? null)
  return { state: currentMatches ? 'registered' : 'refresh_required', engine }
}
