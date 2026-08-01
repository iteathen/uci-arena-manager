import type { ResourceKind } from '../api/types'

export function resourcePageForKind(kind: ResourceKind): string {
  switch (kind) {
    case 'engine': return 'engines'
    case 'bot': return 'bots'
    case 'managed_service': return 'services'
    case 'match_plan':
    case 'tournament': return 'tournaments'
    case 'game': return 'history'
    case 'report':
    case 'artifact': return 'reports'
    case 'secret_reference': return 'configuration'
    case 'host':
    case 'health_probe':
    case 'health_snapshot': return 'statistics'
    case 'operation':
    case 'log_source': return 'logs'
  }
}
