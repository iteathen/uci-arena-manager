import {
  IconActivityHeartbeat,
  IconAlertTriangle,
  IconChess,
  IconCpu,
  IconDatabase,
  IconPlayerPlay,
} from '@tabler/icons-react'
import {
  Button,
  Grid,
  Group,
  Paper,
  SimpleGrid,
  Stack,
  Text,
  Title,
} from '@mantine/core'
import { useQuery } from '@tanstack/react-query'
import { api } from '../api/client'
import type { ManagedServiceData } from '../api/types'
import { Chessboard } from '../components/Chessboard'
import { MetricCard } from '../components/MetricCard'
import { PageHeader } from '../components/PageHeader'

export function OverviewPage({ navigate }: { navigate: (page: string) => void }) {
  const status = useQuery({ queryKey: ['status'], queryFn: api.status, refetchInterval: 10_000 })
  const components = useQuery({ queryKey: ['components'], queryFn: api.components })
  const services = useQuery({
    queryKey: ['resources', 'managed_service'],
    queryFn: () => api.resources<ManagedServiceData>('managed_service'),
  })
  const counts = status.data?.counts ?? {}
  const resources =
    (counts.engine ?? 0) +
    (counts.bot ?? 0) +
    (counts.managed_service ?? 0)
  const componentIssues = (components.data?.components ?? []).filter(
    (component) => component.package_state !== 'installed' && !component.optional,
  )
  const serviceIssues = (services.data ?? []).filter(
    (service) =>
      service.data.observed_state === 'degraded' ||
      service.data.integration_state === 'incompatible',
  )
  const attention = componentIssues.length + serviceIssues.length + (status.isError ? 1 : 0)
  const evidence = (services.data ?? []).find(
    (service) => service.data.profile_id === 'uci_arena_evidence',
  )
  const initialLoading = status.isLoading || components.isLoading || services.isLoading

  return (
    <>
      <PageHeader
        title="Control room"
        description="Local engines, services, games, and operational signals in one place."
        actions={
          <Group>
            <Button variant="default" onClick={() => navigate('engines')}>
              Add engine
            </Button>
            <Button leftSection={<IconPlayerPlay size={17} />} onClick={() => navigate('planner')}>
              Plan local match
            </Button>
          </Group>
        }
      />
      <SimpleGrid cols={{ base: 1, xs: 2, lg: 4 }} mb="lg">
        <MetricCard
          label="Managed resources"
          value={initialLoading ? 'checking' : resources}
          detail="Engines, bots, and services"
          icon={<IconCpu size={22} />}
        />
        <MetricCard
          label="Live games"
          value={counts.game ?? 0}
          detail="Canonical state retained"
          icon={<IconChess size={22} />}
          tone="violet"
        />
        <MetricCard
          label="Attention"
          value={initialLoading ? 'checking' : attention}
          detail={initialLoading ? 'Loading current component and service state' : attention ? 'Review missing or unhealthy components' : 'No known blockers'}
          icon={<IconAlertTriangle size={22} />}
          tone="orange"
        />
        <MetricCard
          label="Persistence"
          value={status.data?.database ?? 'checking'}
          detail="SQLite WAL · synchronous"
          icon={<IconDatabase size={22} />}
          tone="teal"
        />
      </SimpleGrid>
      <Grid>
        <Grid.Col span={{ base: 12, lg: 7 }}>
          <Paper withBorder className="panel" h="100%">
            <Group justify="space-between" mb="lg">
              <div>
                <Text fw={700}>Operational posture</Text>
                <Text size="sm" c="dimmed">
                  Highest priority conditions appear first.
                </Text>
              </div>
              <IconActivityHeartbeat className="panel-icon" />
            </Group>
            <Stack gap="sm">
              <Group className="attention-row" justify="space-between">
                <div>
                  <Text fw={600}>
                    {status.isLoading ? 'Checking control plane status' : status.data ? 'Control plane ready' : 'Control plane status unavailable'}
                  </Text>
                  <Text size="sm" c="dimmed">
                    {status.isLoading
                      ? 'Waiting for the first current Manager status sample.'
                      : status.data
                      ? 'Loopback API and durable event stream are available.'
                      : 'The dashboard could not obtain a current Manager status sample.'}
                  </Text>
                </div>
                <Text size="sm" c={status.isLoading ? 'dimmed' : status.data ? 'teal' : 'red'} fw={700}>
                  {status.isLoading ? 'CHECKING' : status.data ? 'HEALTHY' : 'UNAVAILABLE'}
                </Text>
              </Group>
              <Group className="attention-row" justify="space-between">
                <div>
                  <Text fw={600}>No tournament currently running</Text>
                  <Text size="sm" c="dimmed">
                    Register engines and create a deterministic schedule.
                  </Text>
                </div>
                <Button size="xs" variant="subtle" onClick={() => navigate('planner')}>
                  Plan
                </Button>
              </Group>
              <Group className="attention-row" justify="space-between">
                <div>
                  <Text fw={600}>
                    {services.isLoading
                      ? 'Checking Evidence service'
                      : evidence
                      ? `Evidence service ${evidence.data.observed_state}`
                      : 'Evidence service is not configured'}
                  </Text>
                  <Text size="sm" c="dimmed">
                    {services.isLoading
                      ? 'Waiting for the first current service sample.'
                      : evidence?.data.health.reason ??
                      'Install or detect the optional service, then validate its public contract.'}
                  </Text>
                </div>
                <Button size="xs" variant="subtle" onClick={() => navigate('services')}>
                  Review
                </Button>
              </Group>
            </Stack>
          </Paper>
        </Grid.Col>
        <Grid.Col span={{ base: 12, lg: 5 }}>
          <Paper withBorder className="panel live-preview">
            <Group justify="space-between" mb="md">
              <div>
                <Text fw={700}>Live board preview</Text>
                <Text size="sm" c="dimmed">
                  Backend FEN authority
                </Text>
              </div>
              <Text className="eyebrow">IDLE</Text>
            </Group>
            <Chessboard />
          </Paper>
        </Grid.Col>
      </Grid>
      <Paper withBorder className="panel quick-start" mt="lg">
        <Grid align="center">
          <Grid.Col span={{ base: 12, md: 8 }}>
            <Text className="eyebrow">QUICK START</Text>
            <Title order={2}>Registered engine round robin</Title>
            <Text c="dimmed" mt={6}>
              Register each executable once, then build head-to-head, round-robin, gauntlet,
              Swiss, or SPRT schedules without engine-specific code.
            </Text>
          </Grid.Col>
          <Grid.Col span={{ base: 12, md: 4 }}>
            <Button fullWidth size="md" onClick={() => navigate('planner')}>
              Open match planner
            </Button>
          </Grid.Col>
        </Grid>
      </Paper>
    </>
  )
}
