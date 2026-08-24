import { IconChess, IconMoon, IconSun } from '@tabler/icons-react'
import {
  ActionIcon,
  AppShell,
  Badge,
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
import { useQuery, useQueryClient } from '@tanstack/react-query'
import { lazy, Suspense, useCallback, useEffect, useMemo, useState } from 'react'
import { ApiError, api, connectEvents, exchangePairingToken } from './api/client'
import { ConfigurationPage } from './pages/ConfigurationPage'
import { BotsPage } from './pages/BotsPage'
import { EnginesPage } from './pages/EnginesPage'
import { LogsPage } from './pages/LogsPage'
import { MatchPlannerPage } from './pages/MatchPlannerPage'
import { TournamentPage } from './pages/TournamentPage'
import { GameResourcesPage } from './pages/GameResourcesPage'
import { OverviewPage } from './pages/OverviewPage'
import { ServicesPage } from './pages/ServicesPage'
import { AuditPage } from './pages/AuditPage'
import { ReportsPage } from './pages/ReportsPage'
import {
  locationHasPairingToken,
  navigationItems as nav,
  pageFromLocation,
} from './components/pageNavigation'

const StatisticsPage = lazy(() =>
  import('./pages/StatisticsPage').then((module) => ({ default: module.StatisticsPage })),
)

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
  const components = useQuery({
    queryKey: ['components'],
    queryFn: api.components,
    enabled: authState === 'ready',
    refetchInterval: (query) =>
      query.state.data?.verification_state === 'checking' ? 500 : 10_000,
  })
  const lichessAdapterInstalled = components.data?.components.some(
    (component) =>
      component.component_id === 'lichess.adapter' &&
      component.package_state === 'installed',
  ) ?? false
  const providerStatus = useQuery({
    queryKey: ['provider-status', 'lichess_bot'],
    queryFn: () => api.providerStatus('lichess_bot'),
    enabled: authState === 'ready' && lichessAdapterInstalled,
    retry: false,
    refetchInterval: 5_000,
    refetchIntervalInBackground: false,
  })
  const activeProviderIncidents =
    providerStatus.data && 'incidents' in providerStatus.data.status
      ? (providerStatus.data.status.incidents.active_count ?? 0)
      : 0
  const botIncidentBadge = activeProviderIncidents > 0
    ? (
        <Badge color="red" size="sm" circle aria-label={`${activeProviderIncidents} active Provider incidents`}>
          {activeProviderIncidents > 99 ? '99+' : activeProviderIncidents}
        </Badge>
      )
    : undefined

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
    const handleLocationChange = () => {
      setPage(pageFromLocation())
      if (locationHasPairingToken()) authenticate()
    }
    window.addEventListener('hashchange', handleLocationChange)
    window.addEventListener('popstate', handleLocationChange)
    return () => {
      window.clearTimeout(initialAuthentication)
      window.removeEventListener('hashchange', handleLocationChange)
      window.removeEventListener('popstate', handleLocationChange)
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
    if (pageFromLocation() !== next) {
      history.pushState(null, '', `#page=${next}`)
    }
    close()
  }, [close])

  const content = useMemo(() => {
    switch (page) {
      case 'overview':
        return <OverviewPage navigate={navigate} />
      case 'services':
        return <ServicesPage navigate={navigate} />
      case 'engines':
        return <EnginesPage navigate={navigate} />
      case 'planner':
        return <MatchPlannerPage navigate={navigate} />
      case 'bots':
        return <BotsPage navigate={navigate} />
      case 'live':
        return (
          <GameResourcesPage
            mode="live"
            navigate={navigate}
            providerEnabled={lichessAdapterInstalled}
          />
        )
      case 'tournaments':
        return <TournamentPage navigate={navigate} />
      case 'history':
        return (
          <GameResourcesPage
            mode="history"
            navigate={navigate}
            providerEnabled={lichessAdapterInstalled}
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
        return <ConfigurationPage navigate={navigate} />
      case 'audit':
        return <AuditPage />
      default:
        return <OverviewPage navigate={navigate} />
    }
  }, [lichessAdapterInstalled, navigate, page])

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
              rightSection={item.key === 'bots' ? botIncidentBadge : undefined}
              active={page === item.key}
              onClick={(event) => {
                event.preventDefault()
                navigate(item.key)
              }}
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
              rightSection={item.key === 'bots' ? botIncidentBadge : undefined}
              active={page === item.key}
              onClick={(event) => {
                event.preventDefault()
                navigate(item.key)
              }}
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
