import {
  IconBook2,
  IconBracketsContain,
  IconCircleCheck,
  IconClockHour4,
  IconPlugConnected,
  IconSearch,
  IconSettings,
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
  SimpleGrid,
  Stack,
  Text,
  TextInput,
  ThemeIcon,
  Title,
} from '@mantine/core'
import { useDisclosure } from '@mantine/hooks'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { useState } from 'react'
import { api, makeCommand } from '../api/client'
import type { ManagedServiceData } from '../api/types'
import { PageHeader } from '../components/PageHeader'
import { StatusPill } from '../components/StatusPill'

export function ServicesPage() {
  const queryClient = useQueryClient()
  const profiles = useQuery({ queryKey: ['service-profiles'], queryFn: api.serviceProfiles })
  const services = useQuery({
    queryKey: ['resources', 'managed_service'],
    queryFn: () => api.resources<ManagedServiceData>('managed_service'),
  })
  const [bookOpened, bookModal] = useDisclosure(false)
  const [evidenceOpened, evidenceModal] = useDisclosure(false)
  const [nodePath, setNodePath] = useState('')
  const [cliPath, setCliPath] = useState('')
  const [workspace, setWorkspace] = useState('')
  const [evidenceEndpoint, setEvidenceEndpoint] = useState('')
  const [discoveryMessage, setDiscoveryMessage] = useState('')
  const serviceByProfile = new Map(
    services.data?.map((service) => [service.data.profile_id, service]),
  )
  const bookService = serviceByProfile.get('book_forge')
  const evidenceService = serviceByProfile.get('uci_arena_evidence')
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
        setEvidenceEndpoint(candidate.configuration.endpoint ?? '')
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
    setEvidenceEndpoint(evidenceService?.data.endpoint ?? '')
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
    onSuccess: async () => {
      await queryClient.invalidateQueries({ queryKey: ['resources', 'managed_service'] })
    },
  })

  return (
    <>
      <PageHeader
        title="Managed services"
        description="Control external producers through explicit capability contracts and narrow adapters."
      />
      <SimpleGrid cols={{ base: 1, md: 2 }} mb="xl">
        <Paper withBorder className="service-profile profile-live">
          <Group justify="space-between" align="flex-start">
            <ThemeIcon size={46} radius="md" variant="light">
              <IconBook2 size={24} />
            </ThemeIcon>
            <Badge color="teal" variant="light">
              OPTIONAL · AVAILABLE
            </Badge>
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
            <List.Item>Status, health, pause, resume, and restart</List.Item>
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
            {bookService ? 'Edit locator' : 'Detect or configure'}
          </Button>
        </Paper>
        <Paper withBorder className="service-profile profile-planned">
          <Group justify="space-between" align="flex-start">
            <ThemeIcon size={46} radius="md" variant="light" color="violet">
              <IconClockHour4 size={24} />
            </ThemeIcon>
            <Badge color="violet" variant="light">
              OPTIONAL · IN DEVELOPMENT
            </Badge>
          </Group>
          <Title order={2} mt="lg">
            UCI Arena Evidence Service
          </Title>
          <Text c="dimmed" mt={5}>
            Arena evidence, immutable performance curves, confidence intervals, and
            provenance—currently being built as an independent service.
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
            <List.Item>Versioned capabilities, health, curves, and snapshots API</List.Item>
            <List.Item>Hardware and engine fingerprints in every artifact</List.Item>
            <List.Item>Unavailable state—never synthetic data or a fake process</List.Item>
            <List.Item>Not required for arena scheduling or reports</List.Item>
          </List>
          <Button
            mt="xl"
            color="violet"
            variant={evidenceService ? 'default' : 'light'}
            leftSection={<IconSettings size={17} />}
            onClick={openEvidenceSetup}
          >
            {evidenceService ? 'Edit endpoint' : 'Detect or configure'}
          </Button>
        </Paper>
      </SimpleGrid>

      <Title order={2} mb="md">
        Service instances
      </Title>
      <Stack>
        {services.data?.map((service) => (
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
                {service.data.profile_id === 'book_forge' ? (
                  <Group gap="xs" justify="flex-end">
                    <Button
                      size="xs"
                      variant="default"
                      loading={control.isPending}
                      onClick={() => control.mutate({ id: service.id, action: 'status' })}
                    >
                      Probe
                    </Button>
                    {service.data.observed_state === 'running' ? (
                      <>
                        <Button
                          size="xs"
                          variant="light"
                          onClick={() => control.mutate({ id: service.id, action: 'pause' })}
                        >
                          Pause
                        </Button>
                        <Button
                          size="xs"
                          color="red"
                          variant="light"
                          onClick={() => control.mutate({ id: service.id, action: 'stop' })}
                        >
                          Stop
                        </Button>
                      </>
                    ) : (
                      <Button
                        size="xs"
                        onClick={() => control.mutate({ id: service.id, action: 'start' })}
                      >
                        Start
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
          </Paper>
        ))}
        {!services.data?.length && (
          <Alert color="gray">
            No service instances configured. The catalog above remains available even when an
            external implementation is missing.
          </Alert>
        )}
      </Stack>

      <Modal opened={bookOpened} onClose={bookModal.close} title="Configure Book Forge" size="lg">
        <Stack>
          <Alert color="blue">
            This adapter invokes Book Forge’s public CLI with native arguments and reads only its
            JSON status. Its catalog, workspace, resource governor, and credentials remain
            Book Forge-owned.
          </Alert>
          <Group justify="space-between">
            <Text size="sm" c="dimmed">
              Optional integration · no resource is created until you save.
            </Text>
            <Button
              variant="default"
              leftSection={<IconSearch size={16} />}
              loading={discover.isPending}
              onClick={() => discover.mutate('book_forge')}
            >
              Auto-detect
            </Button>
          </Group>
          {discoveryMessage && <Alert color="gray">{discoveryMessage}</Alert>}
          <TextInput
            label="Node executable"
            placeholder="C:\Program Files\nodejs\node.exe"
            value={nodePath}
            onChange={(event) => setNodePath(event.currentTarget.value)}
          />
          <TextInput
            label="Book Forge CLI entrypoint"
            placeholder="C:\projects\engine\tools\book_forge\src\cli.mjs"
            value={cliPath}
            onChange={(event) => setCliPath(event.currentTarget.value)}
          />
          <TextInput
            label="Book Forge workspace"
            placeholder="E:\book-forge"
            value={workspace}
            onChange={(event) => setWorkspace(event.currentTarget.value)}
          />
          {save.error && <Text c="red">{save.error.message}</Text>}
          <Group justify="flex-end">
            <Button variant="default" onClick={bookModal.close}>
              Cancel
            </Button>
            <Button
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
            </Button>
          </Group>
        </Stack>
      </Modal>
      <Modal
        opened={evidenceOpened}
        onClose={evidenceModal.close}
        title="Configure UCI Arena Evidence Service"
        size="lg"
      >
        <Stack>
          <Alert color="violet">
            This optional integration uses the Evidence Service’s versioned HTTP capability
            document. The service remains independently installed and owns its evidence store.
          </Alert>
          <Group justify="space-between">
            <Text size="sm" c="dimmed">
              Omit this integration without affecting arena health or local matches.
            </Text>
            <Button
              variant="default"
              leftSection={<IconSearch size={16} />}
              loading={discover.isPending}
              onClick={() => discover.mutate('uci_arena_evidence')}
            >
              Auto-detect
            </Button>
          </Group>
          {discoveryMessage && <Alert color="gray">{discoveryMessage}</Alert>}
          <TextInput
            label="Evidence Service endpoint"
            description="Loopback HTTP endpoint only in v1"
            placeholder="http://127.0.0.1:47900"
            value={evidenceEndpoint}
            onChange={(event) => setEvidenceEndpoint(event.currentTarget.value)}
          />
          {save.error && <Text c="red">{save.error.message}</Text>}
          <Group justify="flex-end">
            <Button variant="default" onClick={evidenceModal.close}>
              Cancel
            </Button>
            <Button
              color="violet"
              loading={save.isPending}
              onClick={() =>
                save.mutate({
                  parameters: {
                    profile_id: 'uci_arena_evidence',
                    display_name: 'UCI Arena Evidence Service',
                    endpoint: evidenceEndpoint,
                  },
                  existing: evidenceService,
                })
              }
            >
              {evidenceEndpoint ? 'Save endpoint' : 'Track without endpoint'}
            </Button>
          </Group>
        </Stack>
      </Modal>
      <Text size="xs" c="dimmed" mt="lg">
        {profiles.data?.length ?? 0} built-in capability profiles loaded.
      </Text>
    </>
  )
}
