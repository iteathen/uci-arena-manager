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
  ThemeIcon,
  Title,
} from '@mantine/core'
import { useDisclosure } from '@mantine/hooks'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { useState } from 'react'
import { api, makeCommand } from '../api/client'
import type { ManagedServiceData } from '../api/types'
import { PageHeader } from '../components/PageHeader'
import { PathPickerInput } from '../components/PathPickerInput'
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
  const [evidenceNodePath, setEvidenceNodePath] = useState('')
  const [evidenceCliPath, setEvidenceCliPath] = useState('')
  const [evidenceWorkspace, setEvidenceWorkspace] = useState('')
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
              OPTIONAL · AVAILABLE
            </Badge>
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
            <List.Item>Status, health, pause, resume, and restart</List.Item>
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
            {evidenceService ? 'Edit locator' : 'Detect or configure'}
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
          <PathPickerInput
            label="Node executable"
            description="Prefer the component-private Node runtime beside the installed service."
            placeholder="C:\Program Files\UCI Arena\Node\node.exe"
            value={nodePath}
            onChange={setNodePath}
            mode="file"
            filters={[{ name: 'Node executable', patterns: ['node.exe', 'node'] }]}
          />
          <PathPickerInput
            label="Book Forge CLI entrypoint"
            description="Select the packaged Book Forge CLI, not a source-checkout-relative file."
            placeholder="C:\Program Files\UCI Arena\Book Forge\app\src\cli.mjs"
            value={cliPath}
            onChange={setCliPath}
            mode="file"
            filters={[{ name: 'Node CLI modules', patterns: ['*.mjs', '*.js'] }]}
          />
          <PathPickerInput
            label="Book Forge workspace"
            description="Select a writable volume with at least 75 GiB free after installation."
            placeholder="E:\book-forge"
            value={workspace}
            onChange={setWorkspace}
            mode="directory"
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
            This adapter invokes the Evidence Service’s public CLI with native arguments and
            reads only its JSON status. The service remains independently installed and owns its
            catalog, workspace, datasets, and resource governor.
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
          <PathPickerInput
            label="Node executable"
            description="Prefer the component-private Node runtime beside the installed service."
            placeholder="C:\Program Files\UCI Arena\Node\node.exe"
            value={evidenceNodePath}
            onChange={setEvidenceNodePath}
            mode="file"
            filters={[{ name: 'Node executable', patterns: ['node.exe', 'node'] }]}
          />
          <PathPickerInput
            label="Evidence Service CLI entrypoint"
            description="Select the packaged Evidence Service CLI, not a repository file."
            placeholder="C:\Program Files\UCI Arena\Evidence Service\app\src\cli.mjs"
            value={evidenceCliPath}
            onChange={setEvidenceCliPath}
            mode="file"
            filters={[{ name: 'Node CLI modules', patterns: ['*.mjs', '*.js'] }]}
          />
          <PathPickerInput
            label="Evidence Service workspace"
            description="Select a writable volume with at least 75 GiB free after installation."
            placeholder="E:\uci-arena-evidence"
            value={evidenceWorkspace}
            onChange={setEvidenceWorkspace}
            mode="directory"
          />
          {save.error && <Text c="red">{save.error.message}</Text>}
          <Group justify="flex-end">
            <Button variant="default" onClick={evidenceModal.close}>
              Cancel
            </Button>
            <Button
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
