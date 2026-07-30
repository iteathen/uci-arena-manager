import { IconCpu, IconFolderSearch, IconPlus, IconRefresh } from '@tabler/icons-react'
import {
  Alert,
  Badge,
  Button,
  Code,
  Group,
  Modal,
  Paper,
  ScrollArea,
  SimpleGrid,
  Stack,
  Table,
  Text,
  TextInput,
  Title,
} from '@mantine/core'
import { useDisclosure } from '@mantine/hooks'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { useMemo, useState } from 'react'
import { api, makeCommand, pickLocalPath } from '../api/client'
import type { EngineCandidate, EngineData } from '../api/types'
import { EmptyState } from '../components/EmptyState'
import { ComponentSetupButton } from '../components/ComponentSetupButton'
import { PageHeader } from '../components/PageHeader'
import { PathPickerInput } from '../components/PathPickerInput'
import { StatusPill } from '../components/StatusPill'

function registrationParameters(candidate: EngineCandidate) {
  return {
    display_name: candidate.display_name,
    executable: candidate.executable,
    working_directory: candidate.working_directory,
    arguments: candidate.arguments,
    uci_options: candidate.uci_options,
    candidate_id: candidate.candidate_id,
  }
}

export function EnginesPage() {
  const queryClient = useQueryClient()
  const [scanRoot, setScanRoot] = useState('')
  const engines = useQuery({
    queryKey: ['resources', 'engine'],
    queryFn: () => api.resources<EngineData>('engine'),
  })
  const candidates = useQuery({
    queryKey: ['engine-candidates', scanRoot],
    queryFn: () => api.engineCandidates(scanRoot),
  })
  const components = useQuery({ queryKey: ['components'], queryFn: api.components })
  const arenaComponent = components.data?.components.find(
    (component) => component.component_id === 'uci_arena.engine',
  )
  const [opened, { open, close }] = useDisclosure(false)
  const [name, setName] = useState('')
  const [executable, setExecutable] = useState('')
  const [workingDirectory, setWorkingDirectory] = useState('')
  const [testedCandidates, setTestedCandidates] = useState<Record<string, string>>({})
  const [manualProbeId, setManualProbeId] = useState('')
  const [scanError, setScanError] = useState('')

  const registeredPaths = useMemo(
    () => new Set((engines.data ?? []).map((engine) => engine.data.executable.toLowerCase())),
    [engines.data],
  )

  const probeCandidate = useMutation({
    mutationFn: (candidate: EngineCandidate) =>
      api.probeEngine({
        ...registrationParameters(candidate),
      }),
    onSuccess: (result, candidate) => {
      setTestedCandidates((current) => ({
        ...current,
        [candidate.candidate_id]: result.probe_id,
      }))
    },
  })

  const registerCandidate = useMutation({
    mutationFn: (candidate: EngineCandidate) =>
      api.command(makeCommand('engine.register', 'engine', registrationParameters(candidate))),
    onSuccess: async (_, candidate) => {
      setTestedCandidates((current) => {
        const next = { ...current }
        delete next[candidate.candidate_id]
        return next
      })
      await Promise.all([
        queryClient.invalidateQueries({ queryKey: ['resources', 'engine'] }),
        queryClient.invalidateQueries({ queryKey: ['engine-candidates'] }),
      ])
    },
  })

  const manualParameters = {
    display_name: name,
    executable,
    working_directory: workingDirectory || executable.replace(/[\\/][^\\/]+$/, ''),
    arguments: [],
    uci_options: {},
  }

  const probeManual = useMutation({
    mutationFn: () =>
      api.probeEngine({
        candidate_id: 'manual',
        ...manualParameters,
      }),
    onSuccess: (result) => setManualProbeId(result.probe_id),
  })

  const registerManual = useMutation({
    mutationFn: () =>
      api.command(makeCommand('engine.register', 'engine', manualParameters)),
    onSuccess: async () => {
      await queryClient.invalidateQueries({ queryKey: ['resources', 'engine'] })
      close()
      setName('')
      setExecutable('')
      setWorkingDirectory('')
      setManualProbeId('')
    },
  })

  const refresh = async () => {
    setScanRoot('')
    setScanError('')
    await Promise.all([engines.refetch(), candidates.refetch()])
  }
  const scanFolder = async () => {
    setScanError('')
    try {
      const selected = await pickLocalPath({
        mode: 'directory',
        title: 'Select a folder containing UCI engines',
      })
      if (selected) setScanRoot(selected)
    } catch (error) {
      setScanError(error instanceof Error ? error.message : 'Folder scan is unavailable')
    }
  }

  return (
    <>
      <PageHeader
        title="Engines"
        description="Discover engines and register them through a Manager-owned UCI verification."
        actions={
          <Group>
            <Button
              variant="default"
              leftSection={<IconRefresh size={16} />}
              onClick={refresh}
            >
              Auto-detect
            </Button>
            <Button
              variant="default"
              leftSection={<IconFolderSearch size={16} />}
              onClick={() => void scanFolder()}
            >
              Scan folder
            </Button>
            <Button leftSection={<IconPlus size={16} />} onClick={open}>
              Add external engine
            </Button>
          </Group>
        }
      />

      <Stack gap="lg">
        <Paper withBorder className="panel" p="lg">
          <Group justify="space-between" mb="md">
            <div>
              <Title order={3}>Detected engines</Title>
              <Text size="sm" c="dimmed">
                Installation receipts take priority over imported locators and bounded discovery.
              </Text>
            </div>
            <Badge variant="light">
              {candidates.data?.candidates.length ?? 0} candidates
            </Badge>
          </Group>

          {candidates.error && (
            <Alert color="red" mb="md">
              {candidates.error.message}
            </Alert>
          )}
          {scanError && <Alert color="red" mb="md">{scanError}</Alert>}
          {scanRoot && (
            <Alert color="blue" mb="md">
              Showing normal discovery plus the selected bounded root: <Code>{scanRoot}</Code>
            </Alert>
          )}
          {arenaComponent?.package_state === 'missing' && (
            <Alert color="blue" mb="md" title="Install the preferred UCI Arena Engine">
              <Stack gap="sm">
                <Text>
                  External engines remain available, but UCI Arena Engine with its verified
                  default model is the preferred suite engine.
                </Text>
                {!arenaComponent.downloadable && (
                  <Text size="sm">
                    Repair UCI Arena Suite Setup to restore the retained verified installer
                    media.
                  </Text>
                )}
                <Group>
                  <ComponentSetupButton
                    componentId="uci_arena.engine"
                    label="UCI Arena Engine"
                    available={arenaComponent.downloadable}
                    onCompleted={async () => {
                      await new Promise((resolve) => window.setTimeout(resolve, 1000))
                      await Promise.all([components.refetch(), candidates.refetch()])
                    }}
                  />
                </Group>
              </Stack>
            </Alert>
          )}
          {!candidates.isLoading && !candidates.data?.candidates.length ? (
            <Alert color="gray">
              No installed engine receipts or saved engine locators were found. Scan a
              selected folder or add an external engine.
            </Alert>
          ) : (
            <SimpleGrid cols={{ base: 1, lg: 2 }}>
              {candidates.data?.candidates.map((candidate) => {
                const registered = registeredPaths.has(candidate.executable.toLowerCase())
                const tested = Boolean(testedCandidates[candidate.candidate_id])
                const probing =
                  probeCandidate.isPending &&
                  probeCandidate.variables?.candidate_id === candidate.candidate_id
                const registering =
                  registerCandidate.isPending &&
                  registerCandidate.variables?.candidate_id === candidate.candidate_id
                return (
                  <Paper key={candidate.candidate_id} withBorder p="md">
                    <Stack gap="sm">
                      <Group justify="space-between" align="flex-start">
                        <Group wrap="nowrap">
                          <IconCpu size={20} />
                          <div>
                            <Text fw={700}>{candidate.display_name}</Text>
                            <Text size="xs" c="dimmed">
                              {candidate.component_id ?? candidate.source}
                            </Text>
                          </div>
                        </Group>
                        <Badge
                          color={registered ? 'green' : tested ? 'teal' : candidate.recommended ? 'blue' : 'gray'}
                          variant="light"
                        >
                          {registered ? 'Registered' : tested ? 'Test passed' : candidate.recommended ? 'Recommended' : 'Not tested'}
                        </Badge>
                      </Group>
                      <Code block>{candidate.executable}</Code>
                      {candidate.default_model && (
                        <Text size="sm" c="dimmed">
                          Default model: {candidate.default_model.display_name} ·{' '}
                          {candidate.default_model.input_adapter_id}
                        </Text>
                      )}
                      {candidate.issues.map((issue) => (
                        <Alert color="red" key={issue.code}>
                          {issue.detail}
                        </Alert>
                      ))}
                      {probeCandidate.error &&
                        probeCandidate.variables?.candidate_id === candidate.candidate_id && (
                          <Alert color="red">{probeCandidate.error.message}</Alert>
                        )}
                      {registerCandidate.error &&
                        registerCandidate.variables?.candidate_id === candidate.candidate_id && (
                          <Alert color="red">{registerCandidate.error.message}</Alert>
                        )}
                      <Group justify="flex-end">
                        <Button
                          variant="default"
                          loading={probing}
                          disabled={candidate.state !== 'ready_to_probe' || registered}
                          onClick={() => probeCandidate.mutate(candidate)}
                        >
                          Test only
                        </Button>
                        <Button
                          loading={registering}
                          disabled={candidate.state !== 'ready_to_probe' || registered}
                          onClick={() => registerCandidate.mutate(candidate)}
                        >
                          {candidate.recommended ? 'Use this engine' : 'Register engine'}
                        </Button>
                      </Group>
                    </Stack>
                  </Paper>
                )
              })}
            </SimpleGrid>
          )}
        </Paper>

        <Paper withBorder className="panel" p={0}>
          {!engines.data?.length ? (
            <EmptyState
              title="No engines registered"
              detail="Register a detected engine above or add an external UCI executable."
              action="Add external engine"
              onAction={open}
            />
          ) : (
            <ScrollArea>
              <Table verticalSpacing="md" horizontalSpacing="lg" highlightOnHover>
                <Table.Thead>
                  <Table.Tr>
                    <Table.Th>Engine</Table.Th>
                    <Table.Th>State</Table.Th>
                    <Table.Th>Identity</Table.Th>
                    <Table.Th>Options</Table.Th>
                    <Table.Th>Fingerprint</Table.Th>
                  </Table.Tr>
                </Table.Thead>
                <Table.Tbody>
                  {engines.data.map((engine) => (
                    <Table.Tr key={engine.id}>
                      <Table.Td>
                        <Group wrap="nowrap">
                          <IconCpu size={19} />
                          <div>
                            <Text fw={650}>{engine.data.display_name}</Text>
                            <Text size="xs" c="dimmed">
                              {engine.data.executable}
                            </Text>
                            {engine.data.component_id === 'uci_arena.engine' &&
                              engine.data.recommended && (
                                <Badge size="xs" color="blue" variant="light" mt={4}>
                                  Preferred UCI Arena engine
                                </Badge>
                              )}
                            {engine.data.default_model && (
                              <Text size="xs" c="dimmed">
                                Model package: {engine.data.default_model.display_name}
                              </Text>
                            )}
                          </div>
                        </Group>
                      </Table.Td>
                      <Table.Td>
                        <StatusPill value={engine.data.observed_state} />
                      </Table.Td>
                      <Table.Td>
                        <Text size="sm">{engine.data.engine_name}</Text>
                        <Text size="xs" c="dimmed">
                          {engine.data.engine_author || 'Unknown author'}
                        </Text>
                      </Table.Td>
                      <Table.Td>{engine.data.discovered_options.length}</Table.Td>
                      <Table.Td>
                        <Code>{engine.data.fingerprint.slice(0, 12)}</Code>
                      </Table.Td>
                    </Table.Tr>
                  ))}
                </Table.Tbody>
              </Table>
            </ScrollArea>
          )}
        </Paper>
      </Stack>

      <Modal opened={opened} onClose={close} title="Add external UCI engine" size="lg">
        <Stack>
          <Text size="sm" c="dimmed">
            Select the executable through the local picker, test it as the Manager service,
            then confirm registration.
          </Text>
          <TextInput
            label="Display name (optional)"
            description="Leave blank to use the name reported by the engine."
            placeholder="Detected automatically"
            value={name}
            onChange={(event) => {
              setName(event.currentTarget.value)
              setManualProbeId('')
            }}
          />
          <PathPickerInput
            label="Executable"
            description="The engine must pass uci, isready, option discovery, and a legal depth-one move."
            placeholder="C:\engines\uci-engine.exe"
            value={executable}
            onChange={(value) => {
              setExecutable(value)
              setManualProbeId('')
            }}
            mode="file"
            filters={[{ name: 'UCI engine executables', patterns: ['*.exe', '*'] }]}
          />
          <PathPickerInput
            label="Working directory"
            description="Defaults to the executable directory."
            value={workingDirectory}
            onChange={(value) => {
              setWorkingDirectory(value)
              setManualProbeId('')
            }}
            mode="directory"
          />
          {probeManual.error && <Alert color="red">{probeManual.error.message}</Alert>}
          {registerManual.error && <Alert color="red">{registerManual.error.message}</Alert>}
          {manualProbeId && (
            <Alert color="green">
              Probe passed. Registration will repeat the probe before saving.
            </Alert>
          )}
          <Group justify="flex-end">
            <Button variant="default" onClick={close}>
              Cancel
            </Button>
            <Button
              variant="default"
              loading={probeManual.isPending}
              disabled={!executable}
              onClick={() => probeManual.mutate()}
            >
              Test engine
            </Button>
            <Button
              loading={registerManual.isPending}
              disabled={!manualProbeId}
              onClick={() => registerManual.mutate()}
            >
              Register
            </Button>
          </Group>
        </Stack>
      </Modal>
    </>
  )
}
