import {
  IconBook2,
  IconBracketsContain,
  IconCircleCheck,
  IconClockHour4,
  IconPlugConnected,
  IconSearch,
  IconSettings,
  IconTrash,
} from '@tabler/icons-react'
import {
  Alert,
  Badge,
  Button,
  Grid,
  Group,
  List,
  Modal,
  Paper,
  ScrollArea,
  Select,
  SimpleGrid,
  Stack,
  Table,
  Text,
  ThemeIcon,
  Title,
} from '@mantine/core'
import { useDisclosure } from '@mantine/hooks'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { useEffect, useRef, useState } from 'react'
import { api, makeCommand } from '../api/client'
import type { EngineData, ManagedServiceData, ProbeTimeControl, Resource } from '../api/types'
import { EngineProbeClockInputs } from '../components/EngineProbeClockInputs'
import type { ImportedEngineOutput } from './engineRegistration'
import { PageHeader } from '../components/PageHeader'
import { ComponentSetupButton } from '../components/ComponentSetupButton'
import { LocalSetupImportButton } from '../components/LocalSetupImportButton'
import { PathPickerInput } from '../components/PathPickerInput'
import { StatusPill } from '../components/StatusPill'
import {
  bookSnapshotActivationActions,
  canEnqueueEvidenceCampaign,
  evidenceCampaignBlocker,
  needsEvidenceTarget,
  selectPreferredService,
  evidenceEligibleEngines,
  evidenceCampaignChoices,
  selectedManagedOutputEngine,
  serviceCardStates,
  serviceLifecycleActions,
  serviceLocatorMode,
} from './serviceSelection'
import {
  activeEngineBookPath,
  activeEngineTimingIdentity,
  bookBindingState,
  candidateRegistration,
  candidateRepresentsOutput,
  engineRegistrationParameters,
  timingBindingState,
} from './engineRegistration'

export function ServicesPage({ navigate }: { navigate: (page: string) => void }) {
  const queryClient = useQueryClient()
  const [refreshClock, setRefreshClock] = useState<ProbeTimeControl>()
  const profiles = useQuery({ queryKey: ['service-profiles'], queryFn: api.serviceProfiles })
  const components = useQuery({
    queryKey: ['components'],
    queryFn: api.components,
    refetchInterval: (query) =>
      query.state.data?.verification_state === 'checking' ? 500 : 10_000,
  })
  const services = useQuery({
    queryKey: ['resources', 'managed_service'],
    queryFn: () => api.resources<ManagedServiceData>('managed_service'),
  })
  const engines = useQuery({
    queryKey: ['resources', 'engine'],
    queryFn: () => api.resources<EngineData>('engine'),
  })
  const automaticallyProbed = useRef(new Set<string>())
  useEffect(() => {
    const verificationByComponent = new Map(
      components.data?.components.map((component) => [
        component.component_id,
        component.verification_state,
      ]),
    )
    const pending = (services.data ?? []).filter(
      (service) =>
        !automaticallyProbed.current.has(service.id) &&
        service.data.observed_state === 'unknown' &&
        (!service.data.installed_component_id ||
          verificationByComponent.get(service.data.installed_component_id) === 'verified') &&
        (!service.data.runtime_component_id ||
          verificationByComponent.get(service.data.runtime_component_id) === 'verified'),
    )
    if (!pending.length) return
    pending.forEach((service) => automaticallyProbed.current.add(service.id))
    void Promise.allSettled(
      pending.map((service) =>
        api.command(
          makeCommand(
            'service.status',
            'managed_service',
            {},
            false,
            service.id,
          ),
        ),
      ),
    ).then(() =>
      queryClient.invalidateQueries({
        queryKey: ['resources', 'managed_service'],
      }),
    )
  }, [components.data, queryClient, services.data])
  const [bookOpened, bookModal] = useDisclosure(false)
  const [evidenceOpened, evidenceModal] = useDisclosure(false)
  const [nodePath, setNodePath] = useState('')
  const [cliPath, setCliPath] = useState('')
  const [workspace, setWorkspace] = useState('')
  const [evidenceNodePath, setEvidenceNodePath] = useState('')
  const [evidenceCliPath, setEvidenceCliPath] = useState('')
  const [evidenceWorkspace, setEvidenceWorkspace] = useState('')
  const [evidenceEngineId, setEvidenceEngineId] = useState<string | null>(null)
  const [outputEngineId, setOutputEngineId] = useState<string | null>(null)
  const [evidenceTargetId, setEvidenceTargetId] = useState<string | null>(null)
  const [evidenceCampaignName, setEvidenceCampaignName] = useState<string | null>(null)
  const [discoveryMessage, setDiscoveryMessage] = useState('')
  const [activationNotice, setActivationNotice] = useState<{
    serviceId: string
    color: 'green' | 'yellow' | 'red'
    detail: string
  } | null>(null)
  const bookService = selectPreferredService(
    services.data ?? [],
    'book_forge',
    'book_forge.service',
  )
  const evidenceService = selectPreferredService(
    services.data ?? [],
    'uci_arena_evidence',
    'uci_arena_evidence.service',
  )
  const duplicateProfiles = ['book_forge', 'uci_arena_evidence'].filter(
    (profileId) =>
      (services.data ?? []).filter(
        (service) => service.data.profile_id === profileId,
      ).length > 1,
  )
  const packageById = new Map(
    components.data?.components.map((component) => [component.component_id, component]),
  )
  const bookPackage = packageById.get('book_forge.service')
  const evidencePackage = packageById.get('uci_arena_evidence.service')
  const bookStates = serviceCardStates(
    bookPackage?.package_state,
    Boolean(bookService),
    bookService?.data.observed_state,
    components.isLoading,
    services.isLoading,
  )
  const evidenceStates = serviceCardStates(
    evidencePackage?.package_state,
    Boolean(evidenceService),
    evidenceService?.data.observed_state,
    components.isLoading,
    services.isLoading,
  )
  const bookLocatorMode = serviceLocatorMode(bookService)
  const evidenceLocatorMode = serviceLocatorMode(evidenceService)
  const eligibleEvidenceEngines = evidenceEligibleEngines(engines.data ?? [])
  const preferredEvidenceEngine = eligibleEvidenceEngines.find(engine => engine.id === evidenceEngineId) ??
    (eligibleEvidenceEngines.length === 1 ? eligibleEvidenceEngines[0] : undefined)
  const selectedEvidenceCampaign = evidenceService && evidenceCampaignChoices(evidenceService.data, evidenceTargetId)
    .find(campaign => campaign.name === evidenceCampaignName)
  const managedOutputEngines = (engines.data ?? []).filter(engine => engine.data.onboarding_source === 'install_receipt' && Boolean(engine.data.component_id))
  const activeManagedEngine = selectedManagedOutputEngine(engines.data ?? [], outputEngineId)
  const activeBookPath = activeEngineBookPath(activeManagedEngine)
  const activeTimingIdentity = activeEngineTimingIdentity(activeManagedEngine)
  const activateImportedOutput = async (serviceId: string, label: string, output: ImportedEngineOutput) => {
    try {
      if (!activeManagedEngine) {
        setActivationNotice({ serviceId, color: 'yellow', detail: `${label} was saved. Select a registered engine to activate its regenerated profile.` })
        return
      }
      const [discovery, currentEngines] = await Promise.all([
        api.engineCandidates(''),
        api.resources<EngineData>('engine'),
      ])
      const candidate = discovery.candidates.find(
        (entry) => entry.source === 'install_receipt' &&
          entry.component_id === activeManagedEngine.data.component_id,
      )
      if (!candidate || candidate.state !== 'ready_to_probe') {
        setActivationNotice({
          serviceId,
          color: 'yellow',
          detail: `${label} was saved, but the selected engine candidate is unavailable. Repair its package before activation.`,
        })
        return
      }
      if (!candidateRepresentsOutput(candidate, output)) {
        setActivationNotice({ serviceId, color: 'yellow', detail: `${label} was saved, but the selected engine does not advertise that output binding.` })
        return
      }
      const registration = candidateRegistration(candidate, currentEngines, activeManagedEngine.id)
      if (registration.state === 'unregistered' || !registration.engine) {
        setActivationNotice({
          serviceId,
          color: 'yellow',
          detail: `${label} was saved. Register the selected engine to activate its regenerated profile.`,
        })
        return
      }
      if (registration.state === 'refresh_required') {
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
      }
      const finalEngines = await api.resources<EngineData>('engine')
      const finalRegistration = candidateRegistration(candidate, finalEngines, activeManagedEngine.id)
      const outputActive = output.kind === 'book'
        ? bookBindingState(finalRegistration.engine, output.path) === 'active'
        : timingBindingState(finalRegistration.engine, output.identity) === 'active'
      if (finalRegistration.state !== 'registered' || !outputActive) {
        setActivationNotice({ serviceId, color: 'yellow', detail: `${label} was saved, but its binding is not confirmed in the selected Engine registration.` })
        await queryClient.invalidateQueries({ queryKey: ['resources', 'engine'] })
        return
      }
      await Promise.all([
        queryClient.invalidateQueries({ queryKey: ['resources', 'engine'] }),
        queryClient.invalidateQueries({ queryKey: ['engine-candidates'] }),
        queryClient.invalidateQueries({ queryKey: ['resources', 'managed_service'] }),
      ])
      setActivationNotice({
        serviceId,
        color: 'green',
        detail: registration.state === 'refresh_required'
          ? `${label} is active in the registered Engine after a complete runtime probe.`
          : `${label} is already represented by the current registered Engine profile.`,
      })
    } catch (error) {
      setActivationNotice({
        serviceId,
        color: 'red',
        detail: `${label} was saved, but the existing Engine registration was not changed: ${error instanceof Error ? error.message : String(error)}`,
      })
    }
  }
  const save = useMutation({
    mutationFn: ({
      parameters,
      existing,
    }: {
      parameters: Record<string, unknown>
      existing?: { id: string; revision: number }
    }) =>
      api.command(
        makeCommand(
          existing ? 'service.configure' : 'service.register',
          'managed_service',
          parameters,
          false,
          existing?.id ?? null,
          existing?.revision ?? null,
        ),
      ),
    onSuccess: async () => {
      await queryClient.invalidateQueries({ queryKey: ['resources', 'managed_service'] })
      bookModal.close()
      evidenceModal.close()
    },
  })
  const discover = useMutation({
    mutationFn: api.discoverService,
    onSuccess: (result) => {
      const candidate = result.candidates[0]
      if (!candidate) {
        setDiscoveryMessage('No local service was detected. Enter its location manually.')
        return
      }
      if (result.profile_id === 'book_forge') {
        setNodePath(candidate.configuration.node_executable ?? '')
        setCliPath(candidate.configuration.cli_entrypoint ?? '')
        setWorkspace(candidate.configuration.workspace ?? '')
      } else {
        setEvidenceNodePath(candidate.configuration.node_executable ?? '')
        setEvidenceCliPath(candidate.configuration.cli_entrypoint ?? '')
        setEvidenceWorkspace(candidate.configuration.workspace ?? '')
      }
      setDiscoveryMessage(
        candidate.complete
          ? 'A complete local candidate was found. Review it before saving.'
          : `A partial candidate was found. Enter: ${candidate.missing.join(', ')}.`,
      )
    },
    onError: (error) => setDiscoveryMessage(error.message),
  })
  const openBookSetup = () => {
    setNodePath(bookService?.data.node_executable ?? '')
    setCliPath(bookService?.data.cli_entrypoint ?? '')
    setWorkspace(bookService?.data.workspace ?? '')
    setDiscoveryMessage('')
    bookModal.open()
  }
  const openEvidenceSetup = () => {
    setEvidenceNodePath(evidenceService?.data.node_executable ?? '')
    setEvidenceCliPath(evidenceService?.data.cli_entrypoint ?? '')
    setEvidenceWorkspace(evidenceService?.data.workspace ?? '')
    setDiscoveryMessage('')
    evidenceModal.open()
  }
  const control = useMutation({
    mutationFn: ({ id, action }: { id: string; action: string }) =>
      api.command(
        makeCommand(
          `service.${action}`,
          'managed_service',
          {},
          false,
          id,
        ),
      ),
    onSuccess: async (_result, variables) => {
      if (
        variables.action === 'start' ||
        variables.action === 'restart' ||
        variables.action === 'recover' ||
        variables.action === 'pause' ||
        variables.action === 'resume'
      ) {
        await new Promise((resolve) => window.setTimeout(resolve, 350))
        try {
          await api.command(
            makeCommand(
              'service.status',
              'managed_service',
              {},
              false,
              variables.id,
            ),
          )
        } catch {
          // The subsequent resource refresh presents the launch/status failure.
        }
      }
      await queryClient.invalidateQueries({ queryKey: ['resources', 'managed_service'] })
    },
  })
  const evidenceControl = useMutation({
    mutationFn: ({
      service,
      action,
      jobId,
      engineId,
      targetId,
      campaign,
    }: {
      service: Resource<ManagedServiceData>
      action: 'target_register' | 'campaign_enqueue' | 'job_cancel' | 'job_retry'
      jobId?: string
      engineId?: string
      targetId?: string
      campaign?: string
    }) =>
      api.command(
        makeCommand(
          `service.evidence_${action}`,
          'managed_service',
          jobId
            ? { job_id: jobId }
            : engineId
              ? { engine_id: engineId }
              : campaign && targetId
                ? { target_id: targetId, campaign, configuration: {} }
                : { target_id: targetId ?? null },
          false,
          service.id,
          service.revision,
        ),
      ),
    onSuccess: async () => {
      await Promise.all([
        queryClient.invalidateQueries({ queryKey: ['resources', 'managed_service'] }),
        queryClient.invalidateQueries({ queryKey: ['resources', 'engine'] }),
      ])
    },
  })
  const [removeTarget, setRemoveTarget] =
    useState<Resource<ManagedServiceData> | null>(null)
  const removeLocator = useMutation({
    mutationFn: (service: Resource<ManagedServiceData>) =>
      api.command(
        makeCommand(
          'resource.remove',
          'managed_service',
          {},
          false,
          service.id,
          service.revision,
        ),
      ),
    onSuccess: async () => {
      setRemoveTarget(null)
      await queryClient.invalidateQueries({
        queryKey: ['resources', 'managed_service'],
      })
    },
  })

  return (
    <>
      <PageHeader
        title="Managed services"
        description="Control external producers through explicit capability contracts and narrow adapters."
      />
      {managedOutputEngines.length > 1 && <Select mb="md" label="Engine for book and timing outputs"
        placeholder="Choose a registered engine" value={outputEngineId} onChange={setOutputEngineId}
        data={managedOutputEngines.map(engine => ({ value: engine.id, label: engine.data.display_name }))} />}
      {activeManagedEngine && <EngineProbeClockInputs onChange={setRefreshClock} />}
      <SimpleGrid cols={{ base: 1, md: 2 }} mb="xl">
        <Paper withBorder className="service-profile profile-live">
          <Group justify="space-between" align="flex-start">
            <ThemeIcon size={46} radius="md" variant="light">
              <IconBook2 size={24} />
            </ThemeIcon>
            <Group gap={5} justify="flex-end">
              <Badge color={bookPackage?.package_state === 'installed' ? 'teal' : 'gray'} variant="light">{bookStates.package}</Badge>
              <Badge color={bookService ? 'blue' : 'gray'} variant="light">{bookStates.locator}</Badge>
              <Badge color={bookService?.data.observed_state === 'running' ? 'teal' : 'gray'} variant="light">{bookStates.runtime}</Badge>
            </Group>
          </Group>
          <Title order={2} mt="lg">
            Book Forge
          </Title>
          <Text c="dimmed" mt={5}>
            Opening-book acquisition, qualification, bounded maintenance, and immutable
            publication.
          </Text>
          <List
            mt="lg"
            spacing="xs"
            icon={
              <ThemeIcon size={20} radius="xl" color="teal" variant="light">
                <IconCircleCheck size={13} />
              </ThemeIcon>
            }
          >
            <List.Item>Status, health, pause, and resume</List.Item>
            <List.Item>Acquisition, validation, maintenance, and publication</List.Item>
            <List.Item>External workspace and token ownership preserved</List.Item>
            <List.Item>Not required for local matches or manager health</List.Item>
          </List>
          <Button
            mt="xl"
            variant={bookService ? 'default' : 'filled'}
            leftSection={<IconPlugConnected size={17} />}
            onClick={openBookSetup}
          >
            {bookLocatorMode === 'view_installed'
              ? 'View installed locator'
              : bookLocatorMode === 'edit_manual'
                ? 'Edit locator'
                : 'Detect or configure'}
          </Button>
          {bookPackage && bookPackage.package_state !== 'installed' && (
            <Stack gap="xs" mt="sm">
              <Text size="xs" c="dimmed">
                {bookPackage.downloadable
                  ? 'Install the signed Book Forge package and its declared private runtime.'
                  : 'Repair UCI Arena Suite Setup to restore retained verified installer media.'}
              </Text>
              <Group>
                <ComponentSetupButton
                  componentId="book_forge.service"
                  label="Book Forge"
                  available={bookPackage.downloadable}
                  onCompleted={async () => {
                    await new Promise((resolve) => window.setTimeout(resolve, 1000))
                    await Promise.all([components.refetch(), services.refetch()])
                  }}
                />
              </Group>
            </Stack>
          )}
        </Paper>
        <Paper withBorder className="service-profile profile-live">
          <Group justify="space-between" align="flex-start">
            <ThemeIcon size={46} radius="md" variant="light" color="violet">
              <IconClockHour4 size={24} />
            </ThemeIcon>
            <Group gap={5} justify="flex-end">
              <Badge color={evidencePackage?.package_state === 'installed' ? 'violet' : 'gray'} variant="light">{evidenceStates.package}</Badge>
              <Badge color={evidenceService ? 'blue' : 'gray'} variant="light">{evidenceStates.locator}</Badge>
              <Badge color={evidenceService?.data.observed_state === 'running' ? 'violet' : 'gray'} variant="light">{evidenceStates.runtime}</Badge>
            </Group>
          </Group>
          <Title order={2} mt="lg">
            UCI Arena Evidence Service
          </Title>
          <Text c="dimmed" mt={5}>
            Durable timing and strength evidence, identity-bound datasets, resource-aware
            scheduling, and qualified timing-profile publication.
          </Text>
          <List
            mt="lg"
            spacing="xs"
            icon={
              <ThemeIcon size={20} radius="xl" color="violet" variant="light">
                <IconBracketsContain size={13} />
              </ThemeIcon>
            }
          >
            <List.Item>Status, health, pause, and resume</List.Item>
            <List.Item>Hardware and engine fingerprints in every artifact</List.Item>
            <List.Item>Jobs, targets, datasets, exports, and audited pruning</List.Item>
            <List.Item>Not required for arena scheduling or reports</List.Item>
          </List>
          <Button
            mt="xl"
            color="violet"
            variant={evidenceService ? 'default' : 'light'}
            leftSection={<IconSettings size={17} />}
            onClick={openEvidenceSetup}
          >
            {evidenceLocatorMode === 'view_installed'
              ? 'View installed locator'
              : evidenceLocatorMode === 'edit_manual'
                ? 'Edit locator'
                : 'Detect or configure'}
          </Button>
          {evidencePackage && evidencePackage.package_state !== 'installed' && (
            <Stack gap="xs" mt="sm">
              <Text size="xs" c="dimmed">
                {evidencePackage.downloadable
                  ? 'Install the signed Evidence Service package and its declared runtime.'
                  : 'Repair UCI Arena Suite Setup to restore retained verified installer media.'}
              </Text>
              <Group>
                <ComponentSetupButton
                  componentId="uci_arena_evidence.service"
                  label="UCI Arena Evidence Service"
                  available={evidencePackage.downloadable}
                  onCompleted={async () => {
                    await new Promise((resolve) => window.setTimeout(resolve, 1000))
                    await Promise.all([components.refetch(), services.refetch()])
                  }}
                />
              </Group>
            </Stack>
          )}
        </Paper>
      </SimpleGrid>

      <Title order={2} mb="md">
        Service instances
      </Title>
      {duplicateProfiles.length > 0 && (
        <Alert color="yellow" mb="md">
          Multiple saved locators exist for {duplicateProfiles.join(' and ')}. The
          receipt-verified package is used by the catalog card. Stop and remove inactive manual
          locators below to restore one instance per profile.
        </Alert>
      )}
      <Stack>
        {services.data?.map((service) => {
          const removalBlocked = ['running', 'starting', 'paused', 'stopping'].includes(
            service.data.observed_state,
          )
          const receiptOwned = Boolean(service.data.installed_component_id)
          const lifecycleActions = serviceLifecycleActions(service.data)
          return (
            <Paper withBorder className="service-instance" key={service.id}>
            <Grid align="center">
              <Grid.Col span={{ base: 12, md: 5 }}>
                <Text fw={700}>{service.data.display_name}</Text>
                <Text size="sm" c="dimmed">
                  {service.data.profile_id} · revision {service.revision}
                </Text>
              </Grid.Col>
              <Grid.Col span={{ base: 6, md: 2 }}>
                <Text size="xs" c="dimmed" mb={4}>
                  INTEGRATION
                </Text>
                <StatusPill value={service.data.integration_state} />
              </Grid.Col>
              <Grid.Col span={{ base: 6, md: 2 }}>
                <Text size="xs" c="dimmed" mb={4}>
                  OBSERVED
                </Text>
                <StatusPill value={service.data.observed_state} />
              </Grid.Col>
              <Grid.Col span={{ base: 12, md: 3 }}>
                {service.data.profile_id === 'book_forge' ||
                service.data.profile_id === 'uci_arena_evidence' ? (
                  <Group gap="xs" justify="flex-end">
                    <Button
                      size="xs"
                      variant="default"
                      loading={control.isPending}
                      onClick={() => control.mutate({ id: service.id, action: 'status' })}
                    >
                      Probe
                    </Button>
                    {lifecycleActions.map((action) => (
                      <Button
                        key={action}
                        size="xs"
                        color={action === 'stop' ? 'red' : undefined}
                        variant="light"
                        loading={
                          control.isPending &&
                          control.variables?.id === service.id &&
                          control.variables.action === action
                        }
                        disabled={control.isPending}
                        onClick={() => control.mutate({ id: service.id, action })}
                      >
                        {action[0].toUpperCase() + action.slice(1)}
                      </Button>
                    ))}
                    {!receiptOwned && (
                      <Button
                        size="xs"
                        color="red"
                        variant="subtle"
                        leftSection={<IconTrash size={14} />}
                        disabled={removalBlocked}
                        title={
                          removalBlocked
                            ? 'Stop this manual service before removing its locator.'
                            : 'Remove this manual locator.'
                        }
                        onClick={() => setRemoveTarget(service)}
                      >
                        Remove
                      </Button>
                    )}
                  </Group>
                ) : (
                  <Text size="sm" c="dimmed">
                    {service.data.health.reason ?? `${service.data.capabilities.length} capabilities`}
                  </Text>
                )}
              </Grid.Col>
            </Grid>
            <Group justify="space-between" align="flex-start" mt="sm">
              <Text size="sm" c={service.data.health.state === 'healthy' ? 'dimmed' : 'orange'}>
                {service.data.health.reason ?? 'No provider status detail is available.'}
              </Text>
              <Text size="xs" c="dimmed">
                {service.data.last_successful_status_at
                  ? `Last successful status: ${new Date(service.data.last_successful_status_at).toLocaleString()}`
                  : 'No successful status sample yet'}
              </Text>
            </Group>
            {service.data.output_projection_stale && (
              <Alert color="yellow" mt="sm">
                Showing the last validated provider outputs while the current provider probe is unavailable.
              </Alert>
            )}
            {control.error && control.variables?.id === service.id && (
              <Alert color="red" mt="sm">{control.error.message}</Alert>
            )}
            {service.data.output_integration &&
              typeof service.data.output_integration === 'object' && (
                <Paper withBorder mt="md" p="sm" bg="var(--mantine-color-dark-7)">
                  <Text size="xs" c="dimmed" tt="uppercase" fw={700}>
                    Provider outputs
                  </Text>
                  {service.data.profile_id === 'book_forge' ? (
                    <Stack gap="xs" mt={4}>
                      <Text size="sm">
                        {Array.isArray(service.data.output_integration.snapshots)
                          ? `${service.data.output_integration.snapshots.length} validated opening-book snapshot(s)`
                          : 'No validated opening-book snapshots'}
                      </Text>
                      <Text size="xs" c="dimmed" ff="monospace" title={activeBookPath}>
                        Engine active book: {activeBookPath || 'Engine not registered'}
                      </Text>
                      {service.data.output_integration.snapshots?.map((snapshot) => {
                        const activationActions = bookSnapshotActivationActions(snapshot)
                        const binding = activationActions.some(
                          (action) => bookBindingState(activeManagedEngine, action.path) === 'active',
                        )
                          ? 'active'
                          : activeManagedEngine
                            ? 'available'
                            : 'missing'
                        return (
                          <Paper withBorder p="xs" key={snapshot.snapshot_id}>
                            <Group justify="space-between" align="center" wrap="nowrap">
                              <Stack gap={3} style={{ minWidth: 0 }}>
                                <Group gap="xs">
                                  <Text size="sm" fw={650}>{snapshot.channel}</Text>
                                  <Badge color={snapshot.current ? 'blue' : 'gray'} variant="light">
                                    {snapshot.current ? 'Provider current' : 'Provider retained'}
                                  </Badge>
                                  <Badge
                                    color={binding === 'active' ? 'teal' : binding === 'available' ? 'yellow' : 'gray'}
                                    variant="light"
                                  >
                                    {binding === 'active'
                                      ? 'Engine active'
                                      : binding === 'available'
                                        ? 'Available to switch'
                                        : 'Engine not registered'}
                                  </Badge>
                                </Group>
                                <Text size="xs" c="dimmed">
                                  {snapshot.record_count.toLocaleString()} records · {snapshot.qualified ? 'strength qualified' : 'validated'}
                                </Text>
                                {snapshot.current && snapshot.live_channel_root && (
                                  <Text
                                    size="xs"
                                    c="dimmed"
                                    ff="monospace"
                                    truncate
                                    title={snapshot.live_channel_root}
                                  >
                                    Provider updates: {snapshot.live_channel_root}
                                  </Text>
                                )}
                                <Text size="xs" c="dimmed" ff="monospace" truncate title={snapshot.path}>
                                  Immutable snapshot: {snapshot.path}
                                </Text>
                              </Stack>
                              <Group gap="xs" justify="flex-end">
                                {activationActions.map((action) => (
                                  <LocalSetupImportButton
                                    key={action.authorityMode}
                                    assetKind="opening_book"
                                    path={action.path}
                                    authorityMode={action.authorityMode}
                                    label={action.label}
                                    onCompleted={() => activateImportedOutput(service.id, 'Opening-book selection', { kind: 'book', path: action.path })}
                                  />
                                ))}
                              </Group>
                            </Group>
                          </Paper>
                        )
                      })}
                    </Stack>
                  ) : (
                    <Stack gap="xs" mt={4}>
                      <Text size="sm">
                        {Number(service.data.output_integration.dataset_count ?? 0)}{' '}
                        dataset(s) ·{' '}
                        {Number(service.data.output_integration.timing_profile_count ?? 0)}{' '}
                        qualified timing profile(s)
                      </Text>
                      <Text size="xs" c="dimmed" ff="monospace" title={activeTimingIdentity}>
                        Engine active timing identity: {activeTimingIdentity || 'Engine not registered'}
                      </Text>
                      {service.data.output_integration.timing_profiles?.map((profile) => {
                        const binding = timingBindingState(activeManagedEngine, profile.identity_sha256)
                        const providerCurrent = service.data.output_integration?.selected_timing_profile
                          ?.identity_sha256 === profile.identity_sha256
                        return (
                          <Paper withBorder p="xs" key={`${profile.source}-${profile.identity_sha256}`}>
                            <Group justify="space-between" align="center" wrap="nowrap">
                              <Stack gap={3} style={{ minWidth: 0 }}>
                                <Group gap="xs">
                                  <Text size="sm" fw={650}>Qualified timing evidence</Text>
                                  <Badge color={providerCurrent ? 'blue' : 'gray'} variant="light">
                                    {providerCurrent ? 'Provider current' : 'Provider retained'}
                                  </Badge>
                                  <Badge
                                    color={binding === 'active' ? 'teal' : binding === 'available' ? 'yellow' : 'gray'}
                                    variant="light"
                                  >
                                    {binding === 'active'
                                      ? 'Engine active'
                                      : binding === 'available'
                                        ? 'Available to switch'
                                        : 'Engine not registered'}
                                  </Badge>
                                </Group>
                                <Text size="xs" c="dimmed">
                                  {profile.target_identity.model_id ? String(profile.target_identity.model_id) : profile.source}
                                </Text>
                                <Text size="xs" c="dimmed" ff="monospace" truncate title={profile.path}>
                                  Provider file: {profile.path}
                                </Text>
                              </Stack>
                              <LocalSetupImportButton
                                assetKind="timing_profile"
                                path={profile.path}
                                label={binding === 'active' ? 'Reapply to Engine' : 'Use in Engine'}
                                onCompleted={() => activateImportedOutput(service.id, 'Timing profile', { kind: 'timing', identity: profile.identity_sha256 })}
                              />
                            </Group>
                          </Paper>
                        )
                      })}
                    </Stack>
                  )}
                </Paper>
              )}
            {activationNotice?.serviceId === service.id && (
              <Alert color={activationNotice.color} mt="sm">
                {activationNotice.detail}
              </Alert>
            )}
            {service.data.profile_id === 'uci_arena_evidence' &&
              service.data.orchestration_projection && (
                <Paper withBorder mt="md" p="sm">
                  <Group justify="space-between" align="flex-start">
                    <div>
                      <Text size="xs" c="dimmed" tt="uppercase" fw={700}>
                        Evidence collection
                      </Text>
                      <Text size="sm" mt={4}>
                        {service.data.orchestration_projection.targets.length} explicit target(s) ·{' '}
                        {service.data.orchestration_projection.jobs.length} job(s)
                      </Text>
                    </div>
                    {!service.data.orchestration_projection.targets.some(target => target.target_schema === 'uci_arena_evidence_target_v2') && <Button
                      size="xs"
                      color="violet"
                      loading={
                        evidenceControl.isPending &&
                        evidenceControl.variables?.service.id === service.id &&
                        evidenceControl.variables.action === 'campaign_enqueue'
                      }
                      title={
                        evidenceCampaignBlocker(service.data) ??
                        'Enqueue the provider-defined first evidence campaign.'
                      }
                      disabled={
                        evidenceControl.isPending ||
                        !canEnqueueEvidenceCampaign(service.data)
                      }
                      onClick={() =>
                        evidenceControl.mutate({ service, action: 'campaign_enqueue' })
                      }
                    >
                      Run first evidence campaign
                    </Button>}
                  </Group>
                  {service.data.orchestration_projection.targets.some(target => target.target_schema === 'uci_arena_evidence_target_v2') && (
                    <Group mt="sm" align="flex-end">
                      <Select label="Evidence target" placeholder="Choose an engine deployment" value={evidenceTargetId}
                        data={service.data.orchestration_projection.targets.map(target => ({ value: target.id, label: target.label }))}
                        onChange={value => { setEvidenceTargetId(value); setEvidenceCampaignName(null) }} />
                      <Select label="Campaign" placeholder="Choose an advertised campaign" value={evidenceCampaignName}
                        data={evidenceCampaignChoices(service.data, evidenceTargetId).map(campaign => ({ value: campaign.name, label: campaign.name }))}
                        onChange={setEvidenceCampaignName} />
                      {service.data.orchestration_projection.targets.some(target => target.id === evidenceTargetId && target.target_schema !== 'uci_arena_evidence_target_v2') ? (
                        <Button size="xs" color="violet" loading={evidenceControl.isPending}
                          onClick={() => { if (evidenceTargetId) evidenceControl.mutate({ service, action: 'campaign_enqueue', targetId: evidenceTargetId }) }}>
                          Run first evidence campaign
                        </Button>
                      ) : <Button size="xs" color="violet" loading={evidenceControl.isPending}
                        disabled={!evidenceTargetId || !selectedEvidenceCampaign || !Array.isArray(selectedEvidenceCampaign.parameters_schema.required) || selectedEvidenceCampaign.parameters_schema.required.length > 0}
                        onClick={() => { if (evidenceTargetId && selectedEvidenceCampaign) evidenceControl.mutate({ service, action: 'campaign_enqueue', targetId: evidenceTargetId, campaign: selectedEvidenceCampaign.name }) }}>
                        Run campaign
                      </Button>}
                    </Group>
                  )}
                  {(needsEvidenceTarget(service.data) || eligibleEvidenceEngines.length > 0) && (
                    <Alert color="yellow" mt="sm">
                      {eligibleEvidenceEngines.length > 1 && <Select mb="sm" label="Engine for Evidence" placeholder="Choose a registered engine"
                        value={evidenceEngineId} onChange={setEvidenceEngineId}
                        data={eligibleEvidenceEngines.map(engine => ({ value: engine.id, label: engine.data.display_name }))} />}
                      <Group justify="space-between" align="center">
                        <Text size="sm">
                          {needsEvidenceTarget(service.data)
                            ? service.data.status_projection?.collection?.reason ?? 'No compatible engine deployment target is available yet.'
                            : 'Add another registered engine deployment to Evidence.'}
                        </Text>
                        {engines.isLoading ? (
                          <Button size="xs" variant="default" loading disabled>
                            Checking engines
                          </Button>
                        ) : preferredEvidenceEngine ? (
                          <Button
                            size="xs"
                            color="violet"
                            loading={
                              evidenceControl.isPending &&
                              evidenceControl.variables?.action === 'target_register'
                            }
                            disabled={evidenceControl.isPending}
                            title="Repeat the complete UCI probe and register this exact deployment with Evidence Service."
                            onClick={() =>
                              evidenceControl.mutate({
                                service,
                                action: 'target_register',
                                engineId: preferredEvidenceEngine.id,
                              })
                            }
                          >
                            Use selected engine
                          </Button>
                        ) : (
                          <Button
                            component="a"
                            href="#page=engines"
                            size="xs"
                            variant="default"
                            title="Register a compatible engine, then return here."
                            onClick={() => navigate('engines')}
                          >
                            Register an engine
                          </Button>
                        )}
                      </Group>
                    </Alert>
                  )}
                  {evidenceControl.error &&
                    evidenceControl.variables?.service.id === service.id && (
                      <Alert color="red" mt="sm">{evidenceControl.error.message}</Alert>
                    )}
                  {service.data.orchestration_projection.jobs.length > 0 && (
                    <ScrollArea h={280} mt="md">
                      <Table verticalSpacing="xs" horizontalSpacing="sm">
                        <Table.Thead>
                          <Table.Tr>
                            <Table.Th>Work</Table.Th>
                            <Table.Th>State</Table.Th>
                            <Table.Th>Attempts</Table.Th>
                            <Table.Th>Action</Table.Th>
                          </Table.Tr>
                        </Table.Thead>
                        <Table.Tbody>
                          {service.data.orchestration_projection.jobs.map((job) => (
                            <Table.Tr key={job.id}>
                              <Table.Td>
                                <Text size="sm" fw={650}>{job.adapter}</Text>
                                <Text size="xs" c="dimmed">{job.shard_id}</Text>
                                {(job.last_error || job.wait_reason) && (
                                  <Text size="xs" c={job.last_error ? 'red' : 'dimmed'}>
                                    {job.last_error || job.wait_reason}
                                  </Text>
                                )}
                              </Table.Td>
                              <Table.Td><StatusPill value={job.state} /></Table.Td>
                              <Table.Td>{job.attempts}</Table.Td>
                              <Table.Td>
                                {['queued', 'waiting_for_target', 'waiting_for_resources'].includes(
                                  job.state,
                                ) ? (
                                  <Button
                                    size="compact-xs"
                                    variant="subtle"
                                    color="red"
                                    disabled={evidenceControl.isPending}
                                    onClick={() =>
                                      evidenceControl.mutate({
                                        service,
                                        action: 'job_cancel',
                                        jobId: job.id,
                                      })
                                    }
                                  >
                                    Cancel
                                  </Button>
                                ) : ['failed', 'cancelled'].includes(job.state) ? (
                                  <Button
                                    size="compact-xs"
                                    variant="subtle"
                                    disabled={evidenceControl.isPending}
                                    onClick={() =>
                                      evidenceControl.mutate({
                                        service,
                                        action: 'job_retry',
                                        jobId: job.id,
                                      })
                                    }
                                  >
                                    Retry
                                  </Button>
                                ) : null}
                              </Table.Td>
                            </Table.Tr>
                          ))}
                        </Table.Tbody>
                      </Table>
                    </ScrollArea>
                  )}
                </Paper>
              )}
            </Paper>
          )
        })}
        {!services.data?.length && (
          <Alert color="gray">
            No service instances configured. The catalog above remains available even when an
            external implementation is missing.
          </Alert>
        )}
      </Stack>
      <Modal
        opened={removeTarget !== null}
        onClose={() => !removeLocator.isPending && setRemoveTarget(null)}
        title="Remove manual service locator?"
      >
        <Stack>
          <Text>
            This removes the Manager registration for{' '}
            <strong>{removeTarget?.data.display_name}</strong>. It does not delete the
            provider workspace or uninstall any package.
          </Text>
          {removeLocator.error && (
            <Text c="red">{removeLocator.error.message}</Text>
          )}
          <Group justify="flex-end">
            <Button
              variant="default"
              disabled={removeLocator.isPending}
              onClick={() => setRemoveTarget(null)}
            >
              Cancel
            </Button>
            <Button
              color="red"
              loading={removeLocator.isPending}
              onClick={() => removeTarget && removeLocator.mutate(removeTarget)}
            >
              Remove locator
            </Button>
          </Group>
        </Stack>
      </Modal>

      <Modal
        opened={bookOpened}
        onClose={bookModal.close}
        title={bookLocatorMode === 'view_installed' ? 'Installed Book Forge locator' : 'Configure Book Forge'}
        size="lg"
      >
        <Stack>
          <Alert color="blue">
            This adapter invokes Book Forge’s public CLI with native arguments and reads only its
            JSON status. Its catalog, workspace, resource governor, and credentials remain
            Book Forge-owned.
          </Alert>
          <Group justify="space-between">
            <Text size="sm" c="dimmed">
              {bookLocatorMode === 'view_installed'
                ? 'Suite Setup owns this locator. Rerun Suite Setup and choose Modify to change it.'
                : bookLocatorMode === 'edit_manual'
                  ? 'The existing manual locator changes only after validation succeeds.'
                  : 'Optional integration · no resource is created until you save.'}
            </Text>
            {bookLocatorMode !== 'view_installed' && <Button
              variant="default"
              leftSection={<IconSearch size={16} />}
              loading={discover.isPending}
              onClick={() => discover.mutate('book_forge')}
            >
              Auto-detect
            </Button>}
          </Group>
          {discoveryMessage && <Alert color="gray">{discoveryMessage}</Alert>}
          <PathPickerInput
            label="Node executable"
            description="Prefer the component-private Node runtime beside the installed service."
            placeholder="C:\Program Files\UCI Arena\Node\node.exe"
            value={nodePath}
            onChange={setNodePath}
            mode="file"
            filters={[{ name: 'Node executable', patterns: ['node.exe', 'node'] }]}
            disabled={bookLocatorMode === 'view_installed'}
          />
          <PathPickerInput
            label="Book Forge CLI entrypoint"
            description="Select the packaged Book Forge CLI, not a source-checkout-relative file."
            placeholder="C:\Program Files\UCI Arena\Book Forge\app\src\cli.mjs"
            value={cliPath}
            onChange={setCliPath}
            mode="file"
            filters={[{ name: 'Node CLI modules', patterns: ['*.mjs', '*.js'] }]}
            disabled={bookLocatorMode === 'view_installed'}
          />
          <PathPickerInput
            label="Book Forge workspace"
            description="Select a writable volume with at least 75 GiB free after installation."
            placeholder="E:\book-forge"
            value={workspace}
            onChange={setWorkspace}
            mode="directory"
            disabled={bookLocatorMode === 'view_installed'}
          />
          {save.error && <Text c="red">{save.error.message}</Text>}
          <Group justify="flex-end">
            <Button variant="default" onClick={bookModal.close}>
              {bookLocatorMode === 'view_installed' ? 'Close' : 'Cancel'}
            </Button>
            {bookLocatorMode !== 'view_installed' && <Button
              loading={save.isPending}
              disabled={!nodePath || !cliPath || !workspace}
              onClick={() =>
                save.mutate({
                  parameters: {
                    profile_id: 'book_forge',
                    display_name: 'Book Forge',
                    node_executable: nodePath,
                    cli_entrypoint: cliPath,
                    workspace,
                  },
                  existing: bookService,
                })
              }
            >
              Validate and save
            </Button>}
          </Group>
        </Stack>
      </Modal>
      <Modal
        opened={evidenceOpened}
        onClose={evidenceModal.close}
        title={evidenceLocatorMode === 'view_installed'
          ? 'Installed UCI Arena Evidence Service locator'
          : 'Configure UCI Arena Evidence Service'}
        size="lg"
      >
        <Stack>
          <Alert color="violet">
            This adapter invokes the Evidence Service’s public CLI with native arguments and
            reads only its JSON status. The service remains independently installed and owns its
            catalog, workspace, datasets, and resource governor.
          </Alert>
          <Group justify="space-between">
            <Text size="sm" c="dimmed">
              {evidenceLocatorMode === 'view_installed'
                ? 'Suite Setup owns this locator. Rerun Suite Setup and choose Modify to change it.'
                : evidenceLocatorMode === 'edit_manual'
                  ? 'The existing manual locator changes only after validation succeeds.'
                  : 'Omit this integration without affecting arena health or local matches.'}
            </Text>
            {evidenceLocatorMode !== 'view_installed' && <Button
              variant="default"
              leftSection={<IconSearch size={16} />}
              loading={discover.isPending}
              onClick={() => discover.mutate('uci_arena_evidence')}
            >
              Auto-detect
            </Button>}
          </Group>
          {discoveryMessage && <Alert color="gray">{discoveryMessage}</Alert>}
          <PathPickerInput
            label="Node executable"
            description="Prefer the component-private Node runtime beside the installed service."
            placeholder="C:\Program Files\UCI Arena\Node\node.exe"
            value={evidenceNodePath}
            onChange={setEvidenceNodePath}
            mode="file"
            filters={[{ name: 'Node executable', patterns: ['node.exe', 'node'] }]}
            disabled={evidenceLocatorMode === 'view_installed'}
          />
          <PathPickerInput
            label="Evidence Service CLI entrypoint"
            description="Select the packaged Evidence Service CLI, not a repository file."
            placeholder="C:\Program Files\UCI Arena\Evidence Service\app\src\cli.mjs"
            value={evidenceCliPath}
            onChange={setEvidenceCliPath}
            mode="file"
            filters={[{ name: 'Node CLI modules', patterns: ['*.mjs', '*.js'] }]}
            disabled={evidenceLocatorMode === 'view_installed'}
          />
          <PathPickerInput
            label="Evidence Service workspace"
            description="Select a writable volume with at least 75 GiB free after installation."
            placeholder="E:\uci-arena-evidence"
            value={evidenceWorkspace}
            onChange={setEvidenceWorkspace}
            mode="directory"
            disabled={evidenceLocatorMode === 'view_installed'}
          />
          {save.error && <Text c="red">{save.error.message}</Text>}
          <Group justify="flex-end">
            <Button variant="default" onClick={evidenceModal.close}>
              {evidenceLocatorMode === 'view_installed' ? 'Close' : 'Cancel'}
            </Button>
            {evidenceLocatorMode !== 'view_installed' && <Button
              color="violet"
              loading={save.isPending}
              disabled={!evidenceNodePath || !evidenceCliPath || !evidenceWorkspace}
              onClick={() =>
                save.mutate({
                  parameters: {
                    profile_id: 'uci_arena_evidence',
                    display_name: 'UCI Arena Evidence Service',
                    node_executable: evidenceNodePath,
                    cli_entrypoint: evidenceCliPath,
                    workspace: evidenceWorkspace,
                  },
                  existing: evidenceService,
                })
              }
            >
              Validate and save
            </Button>}
          </Group>
        </Stack>
      </Modal>
      <Text size="xs" c="dimmed" mt="lg">
        {profiles.data?.length ?? 0} built-in capability profiles loaded.
      </Text>
    </>
  )
}
