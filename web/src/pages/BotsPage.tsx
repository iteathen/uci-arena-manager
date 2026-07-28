import {
  IconRobot,
  IconPlus,
  IconRefresh,
  IconSettings,
  IconTrash,
  IconUser,
  IconCalendarEvent,
  IconShield,
  IconPlayerPlay,
  IconPlayerPause,
  IconPlayerStop,
} from '@tabler/icons-react'
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
  NumberInput,
  Checkbox,
} from '@mantine/core'
import { useDisclosure } from '@mantine/hooks'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { useState } from 'react'
import { api, makeCommand } from '../api/client'
import { EmptyState } from '../components/EmptyState'
import { PageHeader } from '../components/PageHeader'
import { PathPickerInput } from '../components/PathPickerInput'
import { StatusPill } from '../components/StatusPill'

export function BotsPage() {
  const queryClient = useQueryClient()
  const bots = useQuery({
    queryKey: ['resources', 'bot'],
    queryFn: () => api.resources<any>('bot'),
  })

  // Modals
  const [addOpened, addModal] = useDisclosure(false)
  const [policyOpened, policyModal] = useDisclosure(false)
  const [ownersOpened, ownersModal] = useDisclosure(false)
  const [reserveOpened, reserveModal] = useDisclosure(false)

  // Selected bot state
  const [selectedBot, setSelectedBot] = useState<any>(null)

  // Add / configure state
  const [name, setName] = useState('')
  const [tokenPath, setTokenPath] = useState('')
  const [expectedUsername, setExpectedUsername] = useState('')
  const [probedData, setProbedData] = useState<any>(null)

  // Policy state
  const [casualOnly, setCasualOnly] = useState(false)
  const [minRating, setMinRating] = useState<number | ''>('')
  const [maxRating, setMaxRating] = useState<number | ''>('')

  // Owners state
  const [ownerHandle, setOwnerHandle] = useState('')

  // Reservations state
  const [duration, setDuration] = useState<number>(3600)
  const [reserveReason, setReserveReason] = useState('')

  const probeBot = useMutation({
    mutationFn: () =>
      api.command(
        makeCommand('bot.probe', 'bot', {
          display_name: name || 'Probing Lichess BOT',
          token_path: tokenPath,
          expected_username: expectedUsername,
        }, false, selectedBot?.id),
      ),
    onSuccess: (result: any) => {
      setProbedData(result.resource?.data?.status_projection || result.error || { ok: true })
    },
  })

  const registerBot = useMutation({
    mutationFn: () => {
      const params = {
        display_name: name,
        token_path: tokenPath,
        expected_username: expectedUsername,
      }
      return api.command(
        selectedBot
          ? makeCommand('bot.configure', 'bot', params, false, selectedBot.id, selectedBot.revision)
          : makeCommand('bot.register', 'bot', params),
      )
    },
    onSuccess: async () => {
      await queryClient.invalidateQueries({ queryKey: ['resources', 'bot'] })
      addModal.close()
      setName('')
      setTokenPath('')
      setExpectedUsername('')
      setProbedData(null)
      setSelectedBot(null)
    },
  })

  const controlBot = useMutation({
    mutationFn: ({ id, action, params = {} }: { id: string; action: string; params?: any }) =>
      api.command(makeCommand(`bot.${action}`, 'bot', params, false, id)),
    onSuccess: async () => {
      await queryClient.invalidateQueries({ queryKey: ['resources', 'bot'] })
    },
  })

  const removeBot = useMutation({
    mutationFn: (bot: any) =>
      api.command(makeCommand('resource.remove', 'bot', {}, false, bot.id, bot.revision)),
    onSuccess: async () => {
      await queryClient.invalidateQueries({ queryKey: ['resources', 'bot'] })
    },
  })

  const openAdd = () => {
    setSelectedBot(null)
    setName('')
    setTokenPath('')
    setExpectedUsername('')
    setProbedData(null)
    addModal.open()
  }

  const openReconnect = (bot: any) => {
    setSelectedBot(bot)
    setName(bot.data.display_name)
    setTokenPath(bot.data.token_path)
    setExpectedUsername(bot.data.expected_username)
    setProbedData(null)
    addModal.open()
  }

  const openPolicy = (bot: any) => {
    setSelectedBot(bot)
    setCasualOnly(bot.data.challenge_policy?.casual_only || false)
    setMinRating(bot.data.challenge_policy?.min_rating || '')
    setMaxRating(bot.data.challenge_policy?.max_rating || '')
    policyModal.open()
  }

  const savePolicy = () => {
    const policy = {
      casual_only: casualOnly,
      min_rating: minRating,
      max_rating: maxRating,
    }
    registerBot.mutateAsync() // uses configure_bot which will probe and save
  }

  const openOwners = (bot: any) => {
    setSelectedBot(bot)
    setOwnerHandle('')
    ownersModal.open()
  }

  const openReserve = (bot: any) => {
    setSelectedBot(bot)
    setDuration(3600)
    setReserveReason('')
    reserveModal.open()
  }

  const refresh = async () => {
    await bots.refetch()
  }

  return (
    <>
      <PageHeader
        title="Lichess Bots"
        description="Add, reconnect, and configure existing Lichess BOT accounts securely."
        actions={
          <Group>
            <Button
              variant="default"
              leftSection={<IconRefresh size={16} />}
              onClick={refresh}
            >
              Refresh
            </Button>
            <Button leftSection={<IconPlus size={16} />} onClick={openAdd}>
              Connect Lichess BOT
            </Button>
          </Group>
        }
      />

      <Paper withBorder className="panel" p={0}>
        {!bots.data?.length ? (
          <EmptyState
            title="No connected bots yet"
            detail="Connect an existing Lichess BOT account using a secure, path-only token reference."
            action="Connect Lichess BOT"
            onAction={openAdd}
          />
        ) : (
          <ScrollArea>
            <Table verticalSpacing="md" horizontalSpacing="lg" highlightOnHover>
              <Table.Thead>
                <Table.Tr>
                  <Table.Th>Bot account</Table.Th>
                  <Table.Th>State</Table.Th>
                  <Table.Th>Lichess ID / Title</Table.Th>
                  <Table.Th>Active games / Slots</Table.Th>
                  <Table.Th>Ownerhandles</Table.Th>
                  <Table.Th>Controls</Table.Th>
                </Table.Tr>
              </Table.Thead>
              <Table.Tbody>
                {bots.data.map((bot: any) => (
                  <Table.Tr key={bot.id}>
                    <Table.Td fw={650}>
                      <Group wrap="nowrap">
                        <IconRobot size={20} />
                        <div>
                          <Text fw={700}>{bot.data.display_name}</Text>
                          <Text size="xs" c="dimmed">
                            {bot.data.token_path}
                          </Text>
                        </div>
                      </Group>
                    </Table.Td>
                    <Table.Td>
                      <StatusPill value={bot.data.observed_state} />
                    </Table.Td>
                    <Table.Td>
                      <Group gap="xs">
                        <Code>{bot.data.lichess_account_id}</Code>
                        <Badge size="xs" color="indigo" variant="light">
                          {bot.data.title}
                        </Badge>
                      </Group>
                    </Table.Td>
                    <Table.Td>
                      <Text size="sm">
                        Games: {bot.data.status_projection?.active_games || 0}
                      </Text>
                      <Text size="xs" c="dimmed">
                        Available slots: {bot.data.status_projection?.available_slots || 0}
                      </Text>
                    </Table.Td>
                    <Table.Td>
                      <Group gap="xs">
                        {bot.data.owner_list?.map((owner: string) => (
                          <Badge size="xs" variant="default" key={owner}>
                            {owner}
                          </Badge>
                        ))}
                        {!bot.data.owner_list?.length && <Text size="xs" c="dimmed">No owners</Text>}
                      </Group>
                    </Table.Td>
                    <Table.Td>
                      <Group gap="xs">
                        {bot.data.observed_state === 'running' ? (
                          <>
                            <Button
                              size="xs"
                              variant="light"
                              leftSection={<IconPlayerPause size={12} />}
                              onClick={() => controlBot.mutate({ id: bot.id, action: 'pause' })}
                            >
                              Pause
                            </Button>
                            <Button
                              size="xs"
                              color="red"
                              variant="light"
                              leftSection={<IconPlayerStop size={12} />}
                              onClick={() => controlBot.mutate({ id: bot.id, action: 'stop' })}
                            >
                              Stop
                            </Button>
                          </>
                        ) : (
                          <Button
                            size="xs"
                            leftSection={<IconPlayerPlay size={12} />}
                            onClick={() => controlBot.mutate({ id: bot.id, action: 'start' })}
                          >
                            Start
                          </Button>
                        )}
                        <Button
                          size="xs"
                          variant="default"
                          leftSection={<IconSettings size={12} />}
                          onClick={() => openReconnect(bot)}
                        >
                          Configure
                        </Button>
                        <Button
                          size="xs"
                          variant="default"
                          color="violet"
                          leftSection={<IconShield size={12} />}
                          onClick={() => openPolicy(bot)}
                        >
                          Matchmaking
                        </Button>
                        <Button
                          size="xs"
                          variant="default"
                          leftSection={<IconUser size={12} />}
                          onClick={() => openOwners(bot)}
                        >
                          Owners
                        </Button>
                        <Button
                          size="xs"
                          variant="default"
                          leftSection={<IconCalendarEvent size={12} />}
                          onClick={() => openReserve(bot)}
                        >
                          Reserve
                        </Button>
                        <Button
                          size="xs"
                          variant="light"
                          color="red"
                          leftSection={<IconTrash size={12} />}
                          onClick={() => removeBot.mutate(bot)}
                        >
                          Remove
                        </Button>
                      </Group>
                    </Table.Td>
                  </Table.Tr>
                ))}
              </Table.Tbody>
            </Table>
          </ScrollArea>
        )}
      </Paper>

      {/* Add / Reconnect Lichess BOT modal */}
      <Modal
        opened={addOpened}
        onClose={addModal.close}
        title={selectedBot ? 'Configure Lichess BOT Account' : 'Connect Existing Lichess BOT Account'}
        size="lg"
      >
        <Stack>
          <Alert color="indigo">
            The manager stores only a path-only token-file reference. Token contents are never read,
            hashed, saved, printed, or returned to the UI.
          </Alert>

          <TextInput
            label="Display name"
            placeholder="Lichess BOT Account"
            value={name}
            onChange={(event) => {
              setName(event.currentTarget.value)
              setProbedData(null)
            }}
          />

          <PathPickerInput
            label="Lichess token file reference"
            description="Select the text file containing the Lichess API BOT token."
            placeholder="C:\Users\you\.config\uci-arena\lichess-token.txt"
            value={tokenPath}
            onChange={(value) => {
              setTokenPath(value)
              setProbedData(null)
            }}
            mode="file"
            filters={[{ name: 'Token reference files', patterns: ['*.txt', '*.token', '*'] }]}
          />

          <TextInput
            label="Expected Lichess username"
            placeholder="YourBOTUsername"
            value={expectedUsername}
            onChange={(event) => {
              setExpectedUsername(event.currentTarget.value)
              setProbedData(null)
            }}
          />

          {probeBot.error && <Alert color="red">{probeBot.error.message}</Alert>}
          {registerBot.error && <Alert color="red">{registerBot.error.message}</Alert>}
          {probedData && (
            <Alert color={probedData.id ? 'green' : 'red'}>
              {probedData.id ? (
                `Probe passed. Lichess BOT Account: ${probedData.username} (ID: ${probedData.id}, Title: ${probedData.title})`
              ) : (
                `Probe failed. Identity verification failed.`
              )}
            </Alert>
          )}

          <Group justify="flex-end">
            <Button variant="default" onClick={addModal.close}>
              Cancel
            </Button>
            <Button
              variant="default"
              loading={probeBot.isPending}
              disabled={!name || !tokenPath}
              onClick={() => probeBot.mutate()}
            >
              Verify token & identity
            </Button>
            <Button
              loading={registerBot.isPending}
              disabled={!probedData || !probedData.id}
              onClick={() => registerBot.mutate()}
            >
              {selectedBot ? 'Save configuration' : 'Connect'}
            </Button>
          </Group>
        </Stack>
      </Modal>

      {/* Challenge Matchmaking Policy modal */}
      <Modal opened={policyOpened} onClose={policyModal.close} title="Configure Matchmaking Policy">
        <Stack>
          <Checkbox
            label="Accept casual challenges only"
            checked={casualOnly}
            onChange={(event) => setCasualOnly(event.currentTarget.checked)}
          />
          <NumberInput
            label="Minimum opponent rating"
            value={minRating}
            onChange={(val) => setMinRating(val === '' ? '' : Number(val))}
          />
          <NumberInput
            label="Maximum opponent rating"
            value={maxRating}
            onChange={(val) => setMaxRating(val === '' ? '' : Number(val))}
          />
          <Group justify="flex-end">
            <Button variant="default" onClick={policyModal.close}>
              Cancel
            </Button>
            <Button onClick={savePolicy}>Save Policy</Button>
          </Group>
        </Stack>
      </Modal>

      {/* Owner Handles modal */}
      <Modal opened={ownersOpened} onClose={ownersModal.close} title="Manage Owner Handles">
        <Stack>
          <Group align="flex-end">
            <TextInput
              label="Add Owner Handle"
              placeholder="operator_username"
              style={{ flex: 1 }}
              value={ownerHandle}
              onChange={(event) => setOwnerHandle(event.currentTarget.value)}
            />
            <Button
              onClick={() => {
                controlBot.mutate({
                  id: selectedBot.id,
                  action: 'owner_add',
                  params: { owner_handle: ownerHandle },
                })
                setOwnerHandle('')
              }}
            >
              Add
            </Button>
          </Group>
          <Text fw={700}>Owners list:</Text>
          {selectedBot?.data?.owner_list?.map((owner: string) => (
            <Group justify="space-between" key={owner}>
              <Text>{owner}</Text>
              <Button
                size="xs"
                color="red"
                variant="light"
                onClick={() =>
                  controlBot.mutate({
                    id: selectedBot.id,
                    action: 'owner_remove',
                    params: { owner_handle: owner },
                  })
                }
              >
                Remove
              </Button>
            </Group>
          ))}
          {!selectedBot?.data?.owner_list?.length && (
            <Text size="sm" c="dimmed">No owners added yet.</Text>
          )}
        </Stack>
      </Modal>

      {/* Temporary Reservations modal */}
      <Modal opened={reserveOpened} onClose={reserveModal.close} title="Manage Temporary Reservations">
        <Stack>
          <Group align="flex-end">
            <NumberInput
              label="Reservation duration (seconds)"
              value={duration}
              onChange={(val) => setDuration(val === '' ? 3600 : Number(val))}
              style={{ flex: 1 }}
            />
            <TextInput
              label="Reason"
              placeholder="Testing Match"
              style={{ flex: 1 }}
              value={reserveReason}
              onChange={(event) => setReserveReason(event.currentTarget.value)}
            />
            <Button
              onClick={() => {
                controlBot.mutate({
                  id: selectedBot.id,
                  action: 'reserve',
                  params: { duration_seconds: duration, reason: reserveReason },
                })
              }}
            >
              Reserve
            </Button>
          </Group>

          <Group justify="space-between">
            <Button
              variant="default"
              onClick={() =>
                controlBot.mutate({ id: selectedBot.id, action: 'reserve_status' })
              }
            >
              Check reservation status
            </Button>
            <Button
              color="red"
              variant="light"
              onClick={() =>
                controlBot.mutate({ id: selectedBot.id, action: 'reserve_cancel' })
              }
            >
              Cancel active reservation
            </Button>
          </Group>

          <Text fw={700}>Active reservations / leases:</Text>
          <Code block>{JSON.stringify(selectedBot?.data?.reservations || null, null, 2)}</Code>
        </Stack>
      </Modal>
    </>
  )
}
