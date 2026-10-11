import {
  IconBook2,
  IconClockHour4,
  IconDatabase,
  IconFileAnalytics,
  IconRefresh,
} from '@tabler/icons-react'
import {
  Alert,
  Anchor,
  Badge,
  Button,
  Code,
  Group,
  Paper,
  Select,
  SimpleGrid,
  ScrollArea,
  Stack,
  Table,
  Text,
  TextInput,
  Title,
} from '@mantine/core'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { useMemo, useState } from 'react'
import { api, discoverLocalAssets, localSetupAvailable, makeCommand } from '../api/client'
import type { EngineData, ManagedServiceData, MatchPlanData, ProbeTimeControl, ReportData, Resource } from '../api/types'
import { EngineProbeClockInputs } from '../components/EngineProbeClockInputs'
import { LocalSetupImportButton } from '../components/LocalSetupImportButton'
import { PageHeader } from '../components/PageHeader'
import { StatusPill } from '../components/StatusPill'
import {
  activeEngineBookPath,
  bookBindingState,
  candidateRegistration,
  engineRegistrationParameters,
} from './engineRegistration'

function shortHash(value: string | undefined) {
  return value ? value.slice(0, 12) : 'unavailable'
}

function sameLocalPath(left: string | undefined, right: string | undefined) {
  if (!left || !right) return false
  if (left === right) return true
  const windows = /^(?:[a-z]:[\\/]|\\\\)/iu
  return windows.test(left) && windows.test(right)
    ? left.replaceAll('/', '\\').toLowerCase() === right.replaceAll('/', '\\').toLowerCase()
    : false
}

export function ReportsPage() {
  const queryClient = useQueryClient()
  const [refreshClock, setRefreshClock] = useState<ProbeTimeControl>()
  const [reportName, setReportName] = useState('UCI Arena Manager report')
  const [reportScope, setReportScope] = useState<'current_state' | 'competition'>('current_state')
  const [competitionId, setCompetitionId] = useState<string | null>(null)
  const [bookSetupNotice, setBookSetupNotice] = useState<{
    color: 'green' | 'orange'
    text: string
  } | null>(null)
  const services = useQuery({
    queryKey: ['resources', 'managed_service'],
    queryFn: () => api.resources<ManagedServiceData>('managed_service'),
  })
  const engines = useQuery({
    queryKey: ['resources', 'engine'],
    queryFn: () => api.resources<EngineData>('engine'),
  })
  const nativeSetup = localSetupAvailable()
  const locatorInventory = useQuery({
    queryKey: ['local-setup', 'opening-book-selection'],
    queryFn: () => discoverLocalAssets([]),
    enabled: nativeSetup,
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
  const bookSelection = locatorInventory.data?.discovery?.opening_book?.selection
  const configuredMode = !bookSelection
    ? nativeSetup ? 'not_selected' : 'desktop_helper_unavailable'
    : typeof bookSelection.details?.authority_mode === 'string'
      ? bookSelection.details.authority_mode
      : bookSelection.source === 'saved_locator'
        ? 'immutable_pinned_snapshot'
        : 'installed_component'
  const activeEngine = engines.data?.find(
    (engine) => engine.data.component_id === 'uci_arena.engine',
  ) ?? engines.data?.[0]
  const refreshManagedEngine = async () => {
    try {
      await queryClient.invalidateQueries({ queryKey: ['local-setup', 'opening-book-selection'] })
      const [registered, available] = await Promise.all([
        api.resources<EngineData>('engine'),
        api.engineCandidates(),
      ])
      const candidate = available.candidates.find(
        (entry) => entry.source === 'install_receipt' && entry.component_id === 'uci_arena.engine',
      )
      let refreshed = false
      if (candidate) {
        const registration = candidateRegistration(candidate, registered)
        if (registration.state === 'refresh_required' && registration.engine) {
          await api.command(
            makeCommand(
              'engine.refresh',
              'engine',
              engineRegistrationParameters(candidate, refreshClock),
              false,
              registration.engine.id,
              registration.engine.revision,
            ),
          )
          refreshed = true
        }
      }
      setBookSetupNotice({
        color: 'green',
        text: refreshed
          ? 'Opening-book selection committed and the managed Engine registration was re-probed.'
          : 'Opening-book selection committed. No stale managed Engine registration required refresh.',
      })
    } catch (error) {
      setBookSetupNotice({
        color: 'orange',
        text: `Opening-book selection committed, but Engine registration refresh remains required: ${error instanceof Error ? error.message : 'unknown failure'}`,
      })
    } finally {
      await Promise.all([
        queryClient.invalidateQueries({ queryKey: ['resources', 'engine'] }),
        queryClient.invalidateQueries({ queryKey: ['engine-candidates'] }),
      ])
    }
  }

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
      {bookSetupNotice && (
        <Alert color={bookSetupNotice.color} mb="lg">{bookSetupNotice.text}</Alert>
      )}
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
              Book Forge is not configured. <Anchor href="#page=services">Install or detect it from Services.</Anchor>
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
              <SimpleGrid cols={{ base: 1, sm: 2 }}>
                <Paper withBorder p="sm">
                  <Text size="xs" c="dimmed">Configured authority</Text>
                  <Text fw={650}>{configuredMode.replaceAll('_', ' ')}</Text>
                  <Code block mt="xs">{bookSelection?.path ?? 'No explicit selection'}</Code>
                </Paper>
                <Paper withBorder p="sm">
                  <Text size="xs" c="dimmed">Engine active book route</Text>
                  <Code block mt="xs">{activeEngineBookPath(activeEngine) || 'Engine not registered'}</Code>
                </Paper>
              </SimpleGrid>
              {activeEngine && <EngineProbeClockInputs onChange={setRefreshClock} />}
              {snapshots.map((snapshot) => (
                <Paper withBorder p="md" key={snapshot.snapshot_id}>
                  <Group justify="space-between">
                    <Text fw={700}>{snapshot.channel} snapshot</Text>
                    <Group gap="xs">
                      {snapshot.current && <Badge color="blue">current</Badge>}
                      {(sameLocalPath(bookSelection?.path, snapshot.path) ||
                        sameLocalPath(bookSelection?.path, snapshot.live_channel_root ?? undefined)) && (
                        <Badge color="violet">configured</Badge>
                      )}
                      {(bookBindingState(activeEngine, snapshot.path) === 'active' ||
                        (snapshot.live_channel_root &&
                          bookBindingState(activeEngine, snapshot.live_channel_root) === 'active')) && (
                        <Badge color="teal">Engine active</Badge>
                      )}
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
                  <Group mt="md">
                    {snapshot.current && snapshot.live_channel_root && (
                      <LocalSetupImportButton
                        assetKind="opening_book"
                        path={snapshot.live_channel_root}
                        authorityMode="service_managed_live_channel"
                        label="Follow live channel"
                        onCompleted={refreshManagedEngine}
                      />
                    )}
                    <LocalSetupImportButton
                      assetKind="opening_book"
                      path={snapshot.path}
                      authorityMode="immutable_pinned_snapshot"
                      label="Pin this snapshot"
                      onCompleted={refreshManagedEngine}
                    />
                  </Group>
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
              Evidence Service is not configured.{' '}
              <Anchor href="#page=services">Install or detect it from Services.</Anchor>
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
                  <Code>{shortHash(selectedProfile.sha256)}</Code>{' '}
                  <Anchor href="#page=configuration">Review configuration</Anchor>
                </Alert>
              )}
              {datasets.length > 0 && (
                <ScrollArea h={320} type="auto" offsetScrollbars>
                  <Table stickyHeader>
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
                </ScrollArea>
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
