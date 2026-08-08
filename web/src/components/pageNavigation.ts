import {
  IconActivity,
  IconAdjustments,
  IconArchive,
  IconRobot,
  IconChartHistogram,
  IconChess,
  IconClipboardData,
  IconCpu,
  IconFileAnalytics,
  IconHistory,
  IconHome,
  IconReportAnalytics,
  IconServer,
} from '@tabler/icons-react'

export const navigationItems = [
  { key: 'overview', label: 'Overview', icon: IconHome },
  { key: 'services', label: 'Services', icon: IconServer },
  { key: 'bots', label: 'Bots', icon: IconRobot },
  { key: 'engines', label: 'Engines', icon: IconCpu },
  { key: 'planner', label: 'Local match planner', icon: IconChess },
  { key: 'live', label: 'Live games', icon: IconActivity },
  { key: 'tournaments', label: 'Tournaments', icon: IconChartHistogram },
  { key: 'history', label: 'Game history & replay', icon: IconHistory },
  { key: 'logs', label: 'Logs', icon: IconClipboardData },
  { key: 'statistics', label: 'Statistics & health', icon: IconFileAnalytics },
  { key: 'reports', label: 'Reports', icon: IconReportAnalytics },
  { key: 'configuration', label: 'Configuration', icon: IconAdjustments },
  { key: 'audit', label: 'Audit', icon: IconArchive },
] as const

export function pageFromLocation(location: Pick<Location, 'hash'> = window.location) {
  const value = new URLSearchParams(location.hash.replace(/^#/, '')).get('page')
  return value && navigationItems.some((item) => item.key === value) ? value : 'overview'
}

export function locationHasPairingToken(location: Pick<Location, 'hash'> = window.location) {
  const value = new URLSearchParams(location.hash.replace(/^#/, '')).get('pair')
  return Boolean(value?.match(/^[a-f0-9]{64}$/))
}
