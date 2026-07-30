import {
  IconActivity,
  IconClock,
  IconDatabase,
  IconServer,
} from '@tabler/icons-react'
import { Alert, Paper, SimpleGrid, Stack, Text } from '@mantine/core'
import { useQuery } from '@tanstack/react-query'
import { api } from '../api/client'
import type { ManagedServiceData } from '../api/types'
import { MetricCard } from '../components/MetricCard'
import { PageHeader } from '../components/PageHeader'
import { StatusPill } from '../components/StatusPill'

export function StatisticsPage() {
  const status = useQuery({ queryKey: ['status'], queryFn: api.status })
  const services = useQuery({
    queryKey: ['resources', 'managed_service'],
    queryFn: () => api.resources<ManagedServiceData>('managed_service'),
  })
  const events = useQuery({
    queryKey: ['events', 'health'],
    queryFn: () => api.events(0),
  })
  const resources = Object.values(status.data?.counts ?? {}).reduce(
    (total, count) => total + Number(count ?? 0),
    0,
  )
  const unhealthy = (services.data ?? []).filter(
    (service) =>
      service.data.observed_state === 'degraded' ||
      service.data.integration_state === 'incompatible',
  )

  return (
    <>
      <PageHeader
        title="Statistics & health"
        description="Backend-authoritative state and provider-observed service health."
      />
      <SimpleGrid cols={{ base: 1, xs: 2, lg: 4 }} mb="lg">
        <MetricCard
          label="Manager"
          value={status.data ? 'Ready' : status.isError ? 'Unavailable' : 'Checking'}
          detail={status.data ? `Version ${status.data.version}` : 'Loopback control plane'}
          icon={<IconServer size={21} />}
        />
        <MetricCard
          label="Resources"
          value={String(resources)}
          detail="Durable Manager records"
          icon={<IconDatabase size={21} />}
          tone="violet"
        />
        <MetricCard
          label="Services"
          value={`${services.data?.length ?? 0}`}
          detail={`${unhealthy.length} degraded or incompatible`}
          icon={<IconActivity size={21} />}
          tone={unhealthy.length ? 'orange' : 'teal'}
        />
        <MetricCard
          label="Events"
          value={`${events.data?.length ?? 0}`}
          detail="Current bounded audit snapshot"
          icon={<IconClock size={21} />}
          tone="orange"
        />
      </SimpleGrid>
      <Stack>
        {status.error && (
          <Alert color="red" title="Manager status unavailable">
            {status.error.message}
          </Alert>
        )}
        {(services.data ?? []).map((service) => (
          <Paper withBorder className="panel" key={service.id}>
            <Text fw={700}>{service.data.display_name}</Text>
            <SimpleGrid cols={{ base: 1, sm: 3 }} mt="md">
              <div>
                <Text size="xs" c="dimmed">DESIRED</Text>
                <StatusPill value={service.data.desired_state} />
              </div>
              <div>
                <Text size="xs" c="dimmed">OBSERVED</Text>
                <StatusPill value={service.data.observed_state} />
              </div>
              <div>
                <Text size="xs" c="dimmed">INTEGRATION</Text>
                <StatusPill value={service.data.integration_state} />
              </div>
            </SimpleGrid>
            <Text size="sm" c="dimmed" mt="md">
              {service.data.health.reason ?? 'No provider status detail is available.'}
            </Text>
          </Paper>
        ))}
        {!services.data?.length && (
          <Alert color="gray">
            No optional provider service is configured. Manager readiness is independent of
            Book Forge and Evidence Service.
          </Alert>
        )}
      </Stack>
    </>
  )
}
