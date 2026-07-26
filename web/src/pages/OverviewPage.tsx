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
import { Chessboard } from '../components/Chessboard'
import { MetricCard } from '../components/MetricCard'
import { PageHeader } from '../components/PageHeader'

export function OverviewPage({ navigate }: { navigate: (page: string) => void }) {
  const status = useQuery({ queryKey: ['status'], queryFn: api.status, refetchInterval: 10_000 })
  const counts = status.data?.counts ?? {}
  const resources =
    (counts.engine ?? 0) +
    (counts.bot ?? 0) +
    (counts.managed_service ?? 0)

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
          value={resources}
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
          value={0}
          detail="No unhealthy resources"
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
                  <Text fw={600}>Control plane ready</Text>
                  <Text size="sm" c="dimmed">
                    Loopback API and durable event stream are available.
                  </Text>
                </div>
                <Text size="sm" c="teal" fw={700}>
                  HEALTHY
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
                  <Text fw={600}>Evidence service integration ready</Text>
                  <Text size="sm" c="dimmed">
                    UCI Arena Evidence Service is in development; capability discovery is ready.
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
            <Title order={2}>LC0 vs Stockfish vs LatticeKnight</Title>
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
