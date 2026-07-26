import { IconCpu, IconPlus, IconRefresh } from '@tabler/icons-react'
import {
  Button,
  Code,
  Group,
  Modal,
  Paper,
  ScrollArea,
  Stack,
  Table,
  Text,
  TextInput,
} from '@mantine/core'
import { useDisclosure } from '@mantine/hooks'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { useState } from 'react'
import { api, makeCommand } from '../api/client'
import type { EngineData } from '../api/types'
import { EmptyState } from '../components/EmptyState'
import { PageHeader } from '../components/PageHeader'
import { StatusPill } from '../components/StatusPill'

export function EnginesPage() {
  const queryClient = useQueryClient()
  const engines = useQuery({
    queryKey: ['resources', 'engine'],
    queryFn: () => api.resources<EngineData>('engine'),
  })
  const [opened, { open, close }] = useDisclosure(false)
  const [name, setName] = useState('')
  const [executable, setExecutable] = useState('')
  const [workingDirectory, setWorkingDirectory] = useState('')
  const register = useMutation({
    mutationFn: () =>
      api.command(
        makeCommand('engine.register', 'engine', {
          display_name: name,
          executable,
          working_directory: workingDirectory || executable.replace(/[\\/][^\\/]+$/, ''),
          arguments: [],
        }),
      ),
    onSuccess: async () => {
      await queryClient.invalidateQueries({ queryKey: ['resources', 'engine'] })
      close()
      setName('')
      setExecutable('')
      setWorkingDirectory('')
    },
  })

  return (
    <>
      <PageHeader
        title="Engines"
        description="Every engine follows the same fingerprinted UCI capability contract."
        actions={
          <Group>
            <Button
              variant="default"
              leftSection={<IconRefresh size={16} />}
              onClick={() => engines.refetch()}
            >
              Refresh
            </Button>
            <Button leftSection={<IconPlus size={16} />} onClick={open}>
              Register engine
            </Button>
          </Group>
        }
      />
      <Paper withBorder className="panel" p={0}>
        {!engines.data?.length ? (
          <EmptyState
            title="No engines registered"
            detail="Add LC0, Stockfish, LatticeKnight, or any other compliant UCI executable. Registration includes a live protocol probe."
            action="Register first engine"
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
      <Modal opened={opened} onClose={close} title="Register UCI engine" size="lg">
        <Stack>
          <Text size="sm" c="dimmed">
            The manager will launch this executable directly, discover its UCI options, and
            require a legal depth-one move before saving it.
          </Text>
          <TextInput
            label="Display name"
            placeholder="LatticeKnight"
            value={name}
            onChange={(event) => setName(event.currentTarget.value)}
          />
          <TextInput
            label="Executable"
            placeholder="C:\engines\latticeknight.exe"
            value={executable}
            onChange={(event) => setExecutable(event.currentTarget.value)}
          />
          <TextInput
            label="Working directory"
            description="Defaults to the executable directory."
            value={workingDirectory}
            onChange={(event) => setWorkingDirectory(event.currentTarget.value)}
          />
          {register.error && <Text c="red">{register.error.message}</Text>}
          <Group justify="flex-end">
            <Button variant="default" onClick={close}>
              Cancel
            </Button>
            <Button
              loading={register.isPending}
              disabled={!name || !executable}
              onClick={() => register.mutate()}
            >
              Probe and register
            </Button>
          </Group>
        </Stack>
      </Modal>
    </>
  )
}
