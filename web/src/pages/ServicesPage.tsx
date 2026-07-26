import {
  IconBook2,
  IconBracketsContain,
  IconCircleCheck,
  IconClockHour4,
  IconPlugConnected,
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
  const [nodePath, setNodePath] = useState('')
  const [cliPath, setCliPath] = useState('')
  const [workspace, setWorkspace] = useState('')
  const registered = new Set(services.data?.map((service) => service.data.profile_id))
  const register = useMutation({
    mutationFn: (parameters: Record<string, unknown>) =>
      api.command(makeCommand('service.register', 'managed_service', parameters)),
    onSuccess: async () => {
      await queryClient.invalidateQueries({ queryKey: ['resources', 'managed_service'] })
      bookModal.close()
    },
  })
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
              EXTERNAL · AVAILABLE
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
          </List>
          <Button
            mt="xl"
            variant={registered.has('book_forge') ? 'default' : 'filled'}
            leftSection={<IconPlugConnected size={17} />}
            onClick={bookModal.open}
            disabled={registered.has('book_forge')}
          >
            {registered.has('book_forge') ? 'Configured' : 'Configure adapter'}
          </Button>
        </Paper>
        <Paper withBorder className="service-profile profile-planned">
          <Group justify="space-between" align="flex-start">
            <ThemeIcon size={46} radius="md" variant="light" color="violet">
              <IconClockHour4 size={24} />
            </ThemeIcon>
            <Badge color="violet" variant="light">
            IN DEVELOPMENT · CONTRACT READY
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
          </List>
          <Button
            mt="xl"
            color="violet"
            variant={registered.has('uci_arena_evidence') ? 'default' : 'light'}
            leftSection={<IconSettings size={17} />}
            disabled={registered.has('uci_arena_evidence')}
            loading={register.isPending}
            onClick={() =>
              register.mutate({
                profile_id: 'uci_arena_evidence',
                display_name: 'UCI Arena Evidence Service',
              })
            }
          >
            {registered.has('uci_arena_evidence') ? 'Tracked' : 'Track integration'}
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
          {register.error && <Text c="red">{register.error.message}</Text>}
          <Group justify="flex-end">
            <Button variant="default" onClick={bookModal.close}>
              Cancel
            </Button>
            <Button
              loading={register.isPending}
              disabled={!nodePath || !cliPath || !workspace}
              onClick={() =>
                register.mutate({
                  profile_id: 'book_forge',
                  display_name: 'Book Forge',
                  node_executable: nodePath,
                  cli_entrypoint: cliPath,
                  workspace,
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
