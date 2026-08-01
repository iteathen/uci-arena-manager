import {
  IconBook2,
  IconClockHour4,
  IconDatabase,
  IconFileAnalytics,
  IconRefresh,
} from '@tabler/icons-react'
import {
  Alert,
  Badge,
  Button,
  Code,
  Group,
  Paper,
  Select,
  SimpleGrid,
  Stack,
  Table,
  Text,
  TextInput,
  Title,
} from '@mantine/core'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { useMemo, useState } from 'react'
import { api, makeCommand } from '../api/client'
import type { ManagedServiceData, MatchPlanData, ReportData, Resource } from '../api/types'
import { PageHeader } from '../components/PageHeader'
import { StatusPill } from '../components/StatusPill'

function shortHash(value: string | undefined) {
  return value ? value.slice(0, 12) : 'unavailable'
}

export function ReportsPage() {
  const queryClient = useQueryClient()
  const [reportName, setReportName] = useState('UCI Arena Manager report')
  const [reportScope, setReportScope] = useState<'current_state' | 'competition'>('current_state')
  const [competitionId, setCompetitionId] = useState<string | null>(null)
  const services = useQuery({
    queryKey: ['resources', 'managed_service'],
    queryFn: () => api.resources<ManagedServiceData>('managed_service'),
  })
  const reports = useQuery({
    queryKey: ['resources', 'report'],
    queryFn: () => api.resources<ReportData>('report'),
  })
  const matches = useQuery({
    queryKey: ['resources', 'match_plan'],
    queryFn: () => api.resources<MatchPlanData>('match_plan'),
  })
  const tournaments = useQuery({
    queryKey: ['resources', 'tournament'],
    queryFn: () => api.resources<MatchPlanData>('tournament'),
  })
  const competitionOptions = useMemo(
    () => [...(matches.data ?? []), ...(tournaments.data ?? [])].map((competition) => ({
      value: competition.id,
      label: `${competition.data.display_name} · ${competition.kind} ${competition.id.slice(0, 13)}…`,
    })),
    [matches.data, tournaments.data],
  )
  const generate = useMutation({
    mutationFn: () => api.command<Resource<ReportData>>(
      makeCommand(
        'report.generate',
        'report',
        {
          display_name: reportName.trim(),
          scope: reportScope,
          competition_id: reportScope === 'competition' ? competitionId : null,
          formats: ['html', 'pdf', 'csv', 'json', 'pgn'],
        },
      ),
    ),
    onSuccess: async () => {
      await queryClient.invalidateQueries({ queryKey: ['resources', 'report'] })
    },
  })
  const refresh = useMutation({
    mutationFn: async (items: Array<Resource<ManagedServiceData>>) => {
      const results = await Promise.allSettled(
        items
          .filter(
            (service) =>
              service.data.profile_id === 'book_forge' ||
              service.data.profile_id === 'uci_arena_evidence',
          )
          .map((service) =>
            api.command(
              makeCommand('service.status', 'managed_service', {}, false, service.id),
            ),
          ),
      )
      const failed = results.filter((result) => result.status === 'rejected').length
      if (failed > 0) {
        throw new Error(
          `${failed} provider output refresh ${failed === 1 ? 'request' : 'requests'} failed.`,
        )
      }
    },
    onSettled: async () => {
      await queryClient.invalidateQueries({ queryKey: ['resources', 'managed_service'] })
    },
  })

  const book = services.data?.find((service) => service.data.profile_id === 'book_forge')
  const evidence = services.data?.find(
    (service) => service.data.profile_id === 'uci_arena_evidence',
  )
  const snapshots = book?.data.output_integration?.snapshots ?? []
  const datasets = evidence?.data.output_integration?.datasets ?? []
  const profiles = evidence?.data.output_integration?.timing_profiles ?? []
  const selectedProfile = evidence?.data.output_integration?.selected_timing_profile

  return (
    <>
      <PageHeader
        title="Reports and service outputs"
        description="Read immutable Manager reports and bounded public projections from Book Forge and Evidence."
        actions={
          <Button
            variant="default"
            leftSection={<IconRefresh size={16} />}
            loading={refresh.isPending}
            disabled={!services.data?.length}
            onClick={() => refresh.mutate(services.data ?? [])}
          >
            Refresh provider outputs
          </Button>
        }
      />
      {services.error && <Alert color="red" mb="lg">{services.error.message}</Alert>}
      {refresh.error && <Alert color="red" mb="lg">{refresh.error.message}</Alert>}
      <SimpleGrid cols={{ base: 1, xl: 2 }}>
        <Paper withBorder className="panel">
          <Group justify="space-between" align="flex-start">
            <Group>
              <IconBook2 size={24} />
              <div>
                <Title order={2}>Book Forge snapshots</Title>
                <Text size="sm" c="dimmed">
                  Validated opening-book publications only
                </Text>
              </div>
            </Group>
            <StatusPill value={book?.data.integration_state ?? 'not configured'} />
          </Group>
          {!book ? (
            <Alert color="gray" mt="lg">
              Book Forge is not configured. Install or detect it from Services.
            </Alert>
          ) : book.data.integration_state !== 'available' ? (
            <Alert color="orange" mt="lg">
              {book.data.health.reason ?? 'Book Forge output integration is unavailable.'}
            </Alert>
          ) : snapshots.length === 0 ? (
            <Alert color="blue" mt="lg">
              Book Forge is compatible but has not published a snapshot.
            </Alert>
          ) : (
            <Stack mt="lg">
              {snapshots.map((snapshot) => (
                <Paper withBorder p="md" key={snapshot.snapshot_id}>
                  <Group justify="space-between">
                    <Text fw={700}>{snapshot.channel} snapshot</Text>
                    <Group gap="xs">
                      {snapshot.current && <Badge color="blue">current</Badge>}
                      <Badge color={snapshot.qualified ? 'teal' : 'orange'}>
                        {snapshot.qualified ? 'strength qualified' : 'integration only'}
                      </Badge>
                    </Group>
                  </Group>
                  <SimpleGrid cols={{ base: 1, sm: 3 }} mt="md">
                    <div>
                      <Text size="xs" c="dimmed">Records</Text>
                      <Text fw={650}>{snapshot.record_count.toLocaleString()}</Text>
                    </div>
                    <div>
                      <Text size="xs" c="dimmed">Files</Text>
                      <Text fw={650}>{snapshot.artifact_files.length}</Text>
                    </div>
                    <div>
                      <Text size="xs" c="dimmed">Manifest</Text>
                      <Code>{shortHash(snapshot.manifest_sha256)}</Code>
                    </div>
                  </SimpleGrid>
                  <Text size="xs" c="dimmed" mt="sm">{snapshot.published_at}</Text>
                </Paper>
              ))}
            </Stack>
          )}
        </Paper>

        <Paper withBorder className="panel">
          <Group justify="space-between" align="flex-start">
            <Group>
              <IconClockHour4 size={24} />
              <div>
                <Title order={2}>Timing evidence</Title>
                <Text size="sm" c="dimmed">
                  Identity-bound datasets and qualified curve profiles
                </Text>
              </div>
            </Group>
            <StatusPill value={evidence?.data.integration_state ?? 'not configured'} />
          </Group>
          {!evidence ? (
            <Alert color="gray" mt="lg">
              Evidence Service is not configured. Install or detect it from Services.
            </Alert>
          ) : evidence.data.integration_state !== 'available' ? (
            <Alert color="orange" mt="lg">
              {evidence.data.health.reason ?? 'Evidence output integration is unavailable.'}
            </Alert>
          ) : datasets.length === 0 && profiles.length === 0 ? (
            <Alert color="blue" mt="lg">
              Evidence Service is compatible but has not published data.
            </Alert>
          ) : (
            <Stack mt="lg">
              <SimpleGrid cols={{ base: 1, sm: 2 }}>
                <Paper withBorder p="md">
                  <Group>
                    <IconDatabase size={20} />
                    <div>
                      <Text size="xs" c="dimmed">Datasets</Text>
                      <Text fw={700} size="xl">{datasets.length}</Text>
                    </div>
                  </Group>
                </Paper>
                <Paper withBorder p="md">
                  <Group>
                    <IconFileAnalytics size={20} />
                    <div>
                      <Text size="xs" c="dimmed">Qualified profiles</Text>
                      <Text fw={700} size="xl">{profiles.length}</Text>
                    </div>
                  </Group>
                </Paper>
              </SimpleGrid>
              {selectedProfile && (
                <Alert color="teal" title="Selected timing profile">
                  {selectedProfile.profile_schema} · {selectedProfile.source} ·{' '}
                  <Code>{shortHash(selectedProfile.sha256)}</Code>
                </Alert>
              )}
              {datasets.length > 0 && (
                <Table>
                  <Table.Thead>
                    <Table.Tr>
                      <Table.Th>Campaign</Table.Th>
                      <Table.Th>State</Table.Th>
                      <Table.Th>Observations</Table.Th>
                    </Table.Tr>
                  </Table.Thead>
                  <Table.Tbody>
                    {datasets.map((dataset) => (
                      <Table.Tr key={dataset.id}>
                        <Table.Td>{dataset.campaign || 'unspecified'}</Table.Td>
                        <Table.Td><StatusPill value={dataset.state} /></Table.Td>
                        <Table.Td>{dataset.observation_count.toLocaleString()}</Table.Td>
                      </Table.Tr>
                    ))}
                  </Table.Tbody>
                </Table>
              )}
            </Stack>
          )}
        </Paper>
      </SimpleGrid>

      <Paper withBorder className="panel" mt="xl">
        <Group justify="space-between">
          <div>
            <Title order={2}>Manager reports</Title>
            <Text size="sm" c="dimmed">
              Immutable HTML, PDF, CSV, JSON, and PGN artifacts generated by Manager
            </Text>
          </div>
          <Badge variant="light">{reports.data?.length ?? 0}</Badge>
        </Group>
        <Paper withBorder p="md" mt="lg">
          <Stack gap="sm">
            <SimpleGrid cols={{ base: 1, md: 3 }}>
              <TextInput
                label="Report name"
                value={reportName}
                maxLength={120}
                onChange={(event) => setReportName(event.currentTarget.value)}
              />
              <Select
                label="Report scope"
                value={reportScope}
                allowDeselect={false}
                data={[
                  { value: 'current_state', label: 'All current Manager state' },
                  { value: 'competition', label: 'One saved competition' },
                ]}
                onChange={(value) => {
                  setReportScope(value === 'competition' ? 'competition' : 'current_state')
                  if (value !== 'competition') setCompetitionId(null)
                }}
              />
              {reportScope === 'competition' && (
                <Select
                  searchable
                  label="Competition"
                  placeholder="Select a saved competition"
                  value={competitionId}
                  data={competitionOptions}
                  onChange={setCompetitionId}
                  nothingFoundMessage="No saved competitions"
                />
              )}
            </SimpleGrid>
            <Group justify="space-between">
              <Text size="sm" c="dimmed">
                Generates immutable HTML, PDF, CSV, JSON, and PGN artifacts from one backend snapshot.
              </Text>
              <Button
                loading={generate.isPending}
                disabled={!reportName.trim() || (reportScope === 'competition' && !competitionId)}
                onClick={() => generate.mutate()}
              >
                Generate report
              </Button>
            </Group>
          </Stack>
        </Paper>
        {generate.error && <Alert color="red" mt="lg">{generate.error.message}</Alert>}
        {generate.isSuccess && (
          <Alert color="teal" mt="lg">Report generated and verified in all five formats.</Alert>
        )}
        {reports.error && <Alert color="red" mt="lg">{reports.error.message}</Alert>}
        {!reports.isLoading && !reports.data?.length && (
          <Alert color="gray" mt="lg">
            No Manager reports have been generated yet.
          </Alert>
        )}
        {reports.data?.map((report) => (
          <Paper withBorder p="sm" mt="sm" key={report.id}>
            <Group justify="space-between" align="flex-start">
              <div>
                <Text fw={650}>{report.data.display_name}</Text>
                <Text size="xs" c="dimmed">
                  {report.data.scope.replaceAll('_', ' ')} · cutoff {report.data.data_cutoff} · Manager {report.data.provenance.producer_version}
                </Text>
                <Text size="xs" c="dimmed" ff="monospace">
                  {report.data.content_hashes.source_projection_sha256.slice(0, 24)}…
                </Text>
              </div>
              <Group gap="xs">
                {report.data.artifacts.map((artifact) => (
                  <Button
                    key={artifact.format}
                    component="a"
                    href={artifact.download_url}
                    download={artifact.filename}
                    variant="default"
                    size="xs"
                  >
                    {artifact.format.toUpperCase()}
                  </Button>
                ))}
              </Group>
            </Group>
          </Paper>
        ))}
      </Paper>
    </>
  )
}
