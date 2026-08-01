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
  IconMoon,
  IconReportAnalytics,
  IconServer,
  IconSun,
} from '@tabler/icons-react'
import {
  ActionIcon,
  AppShell,
  Box,
  Burger,
  Button,
  Center,
  Group,
  Loader,
  NavLink,
  ScrollArea,
  Stack,
  Text,
  ThemeIcon,
  Title,
  useMantineColorScheme,
} from '@mantine/core'
import { useDisclosure } from '@mantine/hooks'
import { useQueryClient } from '@tanstack/react-query'
import { lazy, Suspense, useCallback, useEffect, useMemo, useState } from 'react'
import { ApiError, api, connectEvents, exchangePairingToken } from './api/client'
import { ConfigurationPage } from './pages/ConfigurationPage'
import { BotsPage } from './pages/BotsPage'
import { EnginesPage } from './pages/EnginesPage'
import { LogsPage } from './pages/LogsPage'
import { MatchPlannerPage } from './pages/MatchPlannerPage'
import { TournamentPage } from './pages/TournamentPage'
import { OverviewPage } from './pages/OverviewPage'
import { ResourcePage } from './pages/ResourcePage'
import { ServicesPage } from './pages/ServicesPage'
import { AuditPage } from './pages/AuditPage'
import { ReportsPage } from './pages/ReportsPage'

const StatisticsPage = lazy(() =>
  import('./pages/StatisticsPage').then((module) => ({ default: module.StatisticsPage })),
)

const nav = [
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

function pageFromLocation() {
  const value = new URLSearchParams(window.location.hash.replace(/^#/, '')).get('page')
  return nav.some((item) => item.key === value) ? value! : 'overview'
}

export default function App() {
  const queryClient = useQueryClient()
  const [opened, { toggle, close }] = useDisclosure()
  const [page, setPage] = useState(pageFromLocation)
  const [authState, setAuthState] = useState<'loading' | 'ready' | 'required' | 'failed'>(
    'loading',
  )
  const [authDetail, setAuthDetail] = useState('')
  const [eventsConnected, setEventsConnected] = useState(false)
  const { colorScheme, toggleColorScheme } = useMantineColorScheme()

  const authenticate = useCallback(() => {
    setAuthState('loading')
    setAuthDetail('')
    void (async () => {
      try {
        await exchangePairingToken()
        await api.status()
        setAuthState('ready')
      } catch (error) {
        if (error instanceof ApiError && error.status === 401) {
          setAuthState('required')
        } else {
          setAuthState('failed')
          setAuthDetail(error instanceof Error ? error.message : 'Unknown connection failure')
        }
      }
    })()
  }, [])

  useEffect(() => {
    const initialAuthentication = window.setTimeout(authenticate, 0)
    const handlePairingNavigation = () => authenticate()
    window.addEventListener('hashchange', handlePairingNavigation)
    return () => {
      window.clearTimeout(initialAuthentication)
      window.removeEventListener('hashchange', handlePairingNavigation)
    }
  }, [authenticate])

  useEffect(() => {
    if (authState !== 'ready') return
    return connectEvents(
      (events) => {
        if (events.length) {
          void queryClient.invalidateQueries({ queryKey: ['status'] })
          void queryClient.invalidateQueries({ queryKey: ['resources'] })
        }
      },
      setEventsConnected,
    )
  }, [authState, queryClient])

  const navigate = useCallback((next: string) => {
    setPage(next)
    history.replaceState(null, '', `#page=${next}`)
    close()
  }, [close])

  const content = useMemo(() => {
    switch (page) {
      case 'overview':
        return <OverviewPage navigate={navigate} />
      case 'services':
        return <ServicesPage navigate={navigate} />
      case 'engines':
        return <EnginesPage />
      case 'planner':
        return <MatchPlannerPage navigate={navigate} />
      case 'bots':
        return <BotsPage navigate={navigate} />
      case 'live':
        return (
          <ResourcePage
            title="Live games"
            description="Canonical positions, clocks, evaluations, and engine state."
            kind="game"
            emptyAction={{ label: 'Plan a match', onClick: () => navigate('planner') }}
          />
        )
      case 'tournaments':
        return <TournamentPage navigate={navigate} />
      case 'history':
        return (
          <ResourcePage
            title="Game history & replay"
            description="Replay persisted games from backend-authoritative FEN snapshots."
            kind="game"
            emptyAction={{ label: 'Plan a match', onClick: () => navigate('planner') }}
          />
        )
      case 'logs':
        return <LogsPage />
      case 'statistics':
        return (
          <Suspense fallback={<Center mih={360}><Loader size="sm" /></Center>}>
            <StatisticsPage />
          </Suspense>
        )
      case 'reports':
        return <ReportsPage />
      case 'configuration':
        return <ConfigurationPage />
      case 'audit':
        return <AuditPage />
      default:
        return <OverviewPage navigate={navigate} />
    }
  }, [navigate, page])

  if (authState !== 'ready') {
    return (
      <Center mih="100vh" className="auth-screen">
        <Stack align="center" gap="md" maw={480} p="xl">
          <ThemeIcon size={58} radius="lg" variant="gradient" gradient={{ from: 'indigo', to: 'cyan' }}>
            <IconChess size={31} />
          </ThemeIcon>
          <Title order={1}>UCI Arena Manager</Title>
          {authState === 'loading' ? (
            <>
              <Loader size="sm" />
              <Text c="dimmed">Establishing local session…</Text>
            </>
          ) : (
            <>
              <Text c="dimmed" ta="center">
                {authState === 'required'
                  ? 'This dashboard needs a new local pairing session. Reopen UCI Arena Manager from the Start menu or application launcher.'
                  : `The local control plane is unavailable: ${authDetail}`}
              </Text>
              <Button variant="default" onClick={authenticate}>
                Retry connection
              </Button>
              {authState === 'required' && (
                <Text size="xs" c="dimmed" ta="center">
                  Optional external-browser troubleshooting: <Box component="code" className="pair-command">arena ui</Box>
                </Text>
              )}
            </>
          )}
        </Stack>
      </Center>
    )
  }

  return (
    <AppShell
      header={{ height: 64 }}
      navbar={{ width: 264, breakpoint: 'md', collapsed: { mobile: !opened } }}
      padding={{ base: 'md', sm: 'xl' }}
    >
      <AppShell.Header className="app-header">
        <Group h="100%" px="md" justify="space-between">
          <Group>
            <Burger opened={opened} onClick={toggle} hiddenFrom="md" size="sm" />
            <ThemeIcon size={36} radius="md" variant="gradient" gradient={{ from: 'indigo', to: 'cyan' }}>
              <IconChess size={21} />
            </ThemeIcon>
            <div>
              <Text fw={750} lh={1.1}>
                UCI Arena
              </Text>
              <Text size="xs" c="dimmed">
                LOCAL CONTROL PLANE
              </Text>
            </div>
          </Group>
          <Group>
            <Group gap={7} className="connection-state">
              <span className={eventsConnected ? 'status-dot connected' : 'status-dot'} />
              <Text size="xs" fw={650}>
                {eventsConnected ? 'LIVE' : 'RECONNECTING'}
              </Text>
            </Group>
            <ActionIcon
              variant="default"
              size="lg"
              aria-label="Toggle color scheme"
              onClick={() => toggleColorScheme()}
            >
              {colorScheme === 'dark' ? <IconSun size={18} /> : <IconMoon size={18} />}
            </ActionIcon>
          </Group>
        </Group>
      </AppShell.Header>
      <AppShell.Navbar className="app-navbar">
        <ScrollArea p="sm" scrollbarSize={6}>
          <Text className="nav-section">OPERATE</Text>
          {nav.slice(0, 8).map((item) => (
            <NavLink
              key={item.key}
              component="a"
              href={`#page=${item.key}`}
              label={item.label}
              leftSection={<item.icon size={18} stroke={1.8} />}
              active={page === item.key}
              onClick={() => navigate(item.key)}
            />
          ))}
          <Text className="nav-section" mt="lg">
            OBSERVE & GOVERN
          </Text>
          {nav.slice(8).map((item) => (
            <NavLink
              key={item.key}
              component="a"
              href={`#page=${item.key}`}
              label={item.label}
              leftSection={<item.icon size={18} stroke={1.8} />}
              active={page === item.key}
              onClick={() => navigate(item.key)}
            />
          ))}
        </ScrollArea>
        <Box className="host-card">
          <Group gap="sm">
            <span className="host-mark">L</span>
            <div>
              <Text size="sm" fw={650}>
                Local host
              </Text>
              <Text size="xs" c="dimmed">
                127.0.0.1 · authenticated session
              </Text>
            </div>
          </Group>
        </Box>
      </AppShell.Navbar>
      <AppShell.Main>
        <main className="page-container">{content}</main>
      </AppShell.Main>
    </AppShell>
  )
}
