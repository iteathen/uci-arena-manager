import {
  Alert,
  Badge,
  Button,
  Checkbox,
  Divider,
  Fieldset,
  Group,
  MultiSelect,
  NumberInput,
  Select,
  SimpleGrid,
  Stack,
  Switch,
  Text,
  TextInput,
  Title,
} from '@mantine/core'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { useState } from 'react'
import {
  controlLocalBotAccount,
  getLocalBotOptions,
  updateLocalBotOptions,
} from '../api/client'
import type { BotAccountAction, BotOptions, BotOptionsUpdate } from '../api/types'
import {
  changeOutboundBehavior,
  needsOutboundConfirmation,
  needsRatedConfirmation,
  outboundBehavior,
  schedulerAuthorityState,
} from './botOperationsModel'

const speeds = ['ultraBullet', 'bullet', 'blitz', 'rapid', 'classical', 'correspondence']
const modes = ['casual', 'rated']

function editableOptions(options: BotOptions): BotOptionsUpdate {
  return {
    capacity: {
      total_slots: options.capacity.total_slots,
      startup_parallelism: options.capacity.startup_parallelism,
      human: { ...options.capacity.human },
      bot: { ...options.capacity.bot },
    },
    inbound_challenges: { ...options.inbound_challenges },
    outbound_challenge: { ...options.outbound_challenge },
    scheduler: { ...options.scheduler },
    opponent_selection: { ...options.opponent_selection },
  }
}

function numberValue(value: string | number, fallback: number) {
  return typeof value === 'number' && Number.isFinite(value) ? value : fallback
}

export function BotOperationsPanel({
  installed,
  navigate,
}: {
  installed: boolean
  navigate: (page: string) => void
}) {
  const queryClient = useQueryClient()
  const options = useQuery({
    queryKey: ['bot-options'],
    queryFn: getLocalBotOptions,
    enabled: installed,
    retry: false,
    refetchOnWindowFocus: false,
  })
  const account = useQuery({
    queryKey: ['bot-account'],
    queryFn: async () => {
      const [owners, reservation, scheduler] = await Promise.all([
        controlLocalBotAccount('owner_list'),
        controlLocalBotAccount('reservation_status'),
        controlLocalBotAccount('scheduler_status'),
      ])
      return { owners, reservation, scheduler }
    },
    enabled: installed,
    retry: false,
    refetchOnWindowFocus: false,
  })
  const [draft, setForm] = useState<BotOptionsUpdate | null>(null)
  const [confirmRated, setConfirmRated] = useState(false)
  const [confirmOutbound, setConfirmOutbound] = useState(false)
  const [ownerName, setOwnerName] = useState('')
  const [opponent, setOpponent] = useState('')
  const [reservationGames, setReservationGames] = useState<number | string>(1)
  const [reservationMinutes, setReservationMinutes] = useState<number | string>(30)

  const form = draft ?? (options.data ? editableOptions(options.data) : null)

  const save = useMutation({
    mutationFn: () => {
      if (!form) throw new Error('BOT options have not loaded')
      return updateLocalBotOptions(form, confirmRated, confirmOutbound)
    },
    onSuccess: (result) => {
      queryClient.setQueryData(['bot-options'], result)
      setForm(null)
      setConfirmRated(false)
      setConfirmOutbound(false)
    },
  })
  const accountControl = useMutation({
    mutationFn: ({ action, parameters = {} }: {
      action: BotAccountAction
      parameters?: Record<string, unknown>
    }) => controlLocalBotAccount(action, parameters),
    onSuccess: async () => {
      setOwnerName('')
      setOpponent('')
      await account.refetch()
    },
  })

  const ratedConfirmationRequired = Boolean(
    options.data && form && needsRatedConfirmation(options.data, form),
  )
  const outboundConfirmationRequired = Boolean(
    options.data && form && needsOutboundConfirmation(options.data, form),
  )
  const saveBlocked = !form ||
    (ratedConfirmationRequired && !confirmRated) ||
    (outboundConfirmationRequired && !confirmOutbound)
  const reservation = account.data?.reservation.reservation
  const schedulerPaused = account.data?.scheduler.scheduler?.paused
  const schedulerState = schedulerAuthorityState(
    options.data?.scheduler.enabled,
    schedulerPaused,
  )

  if (!installed) {
    return (
      <Alert color="blue" title="Install the Lichess Bot adapter first">
        Account controls and provider-owned options become available after the verified adapter is installed.
      </Alert>
    )
  }

  return (
    <Stack gap="lg">
      <div>
        <Group justify="space-between" align="flex-start">
          <div>
            <Title order={3}>Account and scheduler</Title>
            <Text size="sm" c="dimmed">
              Durable owners, one reserved opponent, and scheduler pause state are stored by the adapter.
            </Text>
          </div>
          <Button variant="subtle" size="xs" onClick={() => navigate('configuration')}>
            Related setup
          </Button>
        </Group>
        {account.isLoading && <Text size="sm" mt="sm">Loading local account state…</Text>}
        {account.error && (
          <Alert color="blue" mt="sm" title="Desktop helper required">
            {account.error.message}
          </Alert>
        )}
        {account.data && (
          <Stack mt="md" gap="md">
            <Fieldset legend="Owners">
              <Stack gap="sm">
                {account.data.owners.owners?.length ? (
                  account.data.owners.owners.map((owner) => (
                    <Group key={owner.username} justify="space-between">
                      <Text size="sm">{owner.username}</Text>
                      <Button
                        size="xs"
                        variant="subtle"
                        color="red"
                        loading={accountControl.isPending}
                        onClick={() => accountControl.mutate({
                          action: 'owner_remove',
                          parameters: { username: owner.username },
                        })}
                      >
                        Remove
                      </Button>
                    </Group>
                  ))
                ) : (
                  <Text size="sm" c="dimmed">No owners are allowed to reserve games.</Text>
                )}
                <Group align="flex-end">
                  <TextInput
                    label="Lichess username"
                    value={ownerName}
                    onChange={(event) => setOwnerName(event.currentTarget.value)}
                    placeholder="AccountName"
                    style={{ flex: 1 }}
                  />
                  <Button
                    loading={accountControl.isPending}
                    disabled={!ownerName.trim()}
                    onClick={() => accountControl.mutate({
                      action: 'owner_add',
                      parameters: { username: ownerName.trim() },
                    })}
                  >
                    Add owner
                  </Button>
                </Group>
              </Stack>
            </Fieldset>
            <Fieldset legend="Reserved match">
              <Stack gap="sm">
                {reservation ? (
                  <Alert color="teal" title={`Reserved for ${reservation.opponent}`}>
                    {reservation.games_remaining} game(s) remain. This reservation expires at{' '}
                    {new Date(reservation.expires_at).toLocaleString()}.
                  </Alert>
                ) : (
                  <Text size="sm" c="dimmed">No opponent currently has the reserved human slot.</Text>
                )}
                <SimpleGrid cols={{ base: 1, sm: 3 }}>
                  <Select
                    label="Opponent"
                    value={opponent}
                    onChange={(value) => setOpponent(value ?? '')}
                    data={(account.data.owners.owners ?? []).map((owner) => ({
                      value: owner.username,
                      label: owner.username,
                    }))}
                    placeholder={account.data.owners.owners?.length
                      ? 'Select an allowed owner'
                      : 'Add an owner above first'}
                    searchable
                    clearable
                    disabled={!account.data.owners.owners?.length || Boolean(reservation)}
                  />
                  <NumberInput
                    label="Games"
                    min={1}
                    max={100}
                    value={reservationGames}
                    onChange={setReservationGames}
                  />
                  <NumberInput
                    label="Expires after (minutes)"
                    min={1}
                    max={1440}
                    value={reservationMinutes}
                    onChange={setReservationMinutes}
                  />
                </SimpleGrid>
                <Group justify="flex-end">
                  <Button
                    variant="default"
                    disabled={!reservation}
                    loading={accountControl.isPending}
                    onClick={() => accountControl.mutate({ action: 'reservation_cancel' })}
                  >
                    Cancel reservation
                  </Button>
                  <Button
                    disabled={!opponent || Boolean(reservation)}
                    loading={accountControl.isPending}
                    onClick={() => accountControl.mutate({
                      action: 'reservation_create',
                      parameters: {
                        opponent,
                        games: numberValue(reservationGames, 1),
                        minutes: numberValue(reservationMinutes, 30),
                      },
                    })}
                  >
                    Reserve games
                  </Button>
                </Group>
              </Stack>
            </Fieldset>
            <Fieldset legend="Scheduler authority">
              <Group justify="space-between">
                <Group>
                  <Badge color={
                    schedulerState === 'active'
                      ? 'teal'
                      : schedulerState === 'paused'
                        ? 'orange'
                        : 'gray'
                  }>
                    {schedulerState.toUpperCase()}
                  </Badge>
                  <Text size="sm">
                    {schedulerState === 'disabled'
                      ? 'Active play is disabled in provider options; incoming challenges remain separately controlled.'
                      : 'Pausing stops new outbound scheduling; it does not terminate active games.'}
                  </Text>
                </Group>
                <Button
                  variant="default"
                  loading={accountControl.isPending}
                  disabled={schedulerState === 'checking' || schedulerState === 'disabled'}
                  onClick={() => accountControl.mutate({
                    action: schedulerPaused ? 'scheduler_resume' : 'scheduler_pause',
                  })}
                >
                  {schedulerPaused ? 'Resume scheduler' : 'Pause scheduler'}
                </Button>
              </Group>
            </Fieldset>
          </Stack>
        )}
        {accountControl.error && <Alert color="red" mt="sm">{accountControl.error.message}</Alert>}
      </div>

      <Divider />

      <div>
        <Group justify="space-between" align="flex-start">
          <div>
            <Title order={3}>Provider-owned options</Title>
            <Text size="sm" c="dimmed">
              Typed controls update the verified adapter atomically. Engine, model, timing, paths, and credentials are not editable here.
            </Text>
          </div>
          <Group gap="xs">
            <Button variant="subtle" size="xs" onClick={() => navigate('reports')}>Timing evidence</Button>
            <Button variant="subtle" size="xs" onClick={() => navigate('engines')}>Engine</Button>
          </Group>
        </Group>
        {options.isLoading && <Text size="sm" mt="sm">Loading verified adapter options…</Text>}
        {options.error && (
          <Alert color="blue" mt="sm" title="Options unavailable">
            {options.error.message}
          </Alert>
        )}
        {form && options.data && (
          <Stack mt="md" gap="md">
            <Fieldset legend="Capacity">
              <Stack gap="sm">
                <Alert color={options.data.capacity.evidence.qualified ? 'teal' : 'orange'}>
                  Capacity evidence: {options.data.capacity.evidence.status}; qualified maximum{' '}
                  {options.data.capacity.evidence.maximum_concurrency ?? 'unavailable'}.
                </Alert>
                <SimpleGrid cols={{ base: 1, sm: 2, lg: 3 }}>
                  <NumberInput label="Total slots (human + bot)" readOnly value={form.capacity.total_slots} />
                  <NumberInput label="Parallel engine startups" min={1} max={options.data.capacity.evidence.maximum_concurrency ?? 64} value={form.capacity.startup_parallelism}
                    onChange={(value) => setForm({ ...form, capacity: { ...form.capacity, startup_parallelism: numberValue(value, form.capacity.startup_parallelism) } })} />
                  <NumberInput label="Human slots" min={form.capacity.bot.slots === 0 ? 1 : 0} max={(options.data.capacity.evidence.maximum_concurrency ?? 64) - form.capacity.bot.slots} value={form.capacity.human.slots}
                    onChange={(value) => { const slots = numberValue(value, form.capacity.human.slots); setForm({ ...form, capacity: { ...form.capacity, total_slots: slots + form.capacity.bot.slots, human: { ...form.capacity.human, slots } } }) }} />
                  <NumberInput label="Bot slots" min={form.capacity.human.slots === 0 ? 1 : 0} max={(options.data.capacity.evidence.maximum_concurrency ?? 64) - form.capacity.human.slots} value={form.capacity.bot.slots}
                    onChange={(value) => { const slots = numberValue(value, form.capacity.bot.slots); setForm({ ...form, capacity: { ...form.capacity, total_slots: form.capacity.human.slots + slots, bot: { ...form.capacity.bot, slots, target_active: Math.min(form.capacity.bot.target_active, slots) } } }) }} />
                  <NumberInput label="Target active bot games" min={0} max={form.capacity.bot.slots} value={form.capacity.bot.target_active}
                    onChange={(value) => setForm({ ...form, capacity: { ...form.capacity, bot: { ...form.capacity.bot, target_active: numberValue(value, form.capacity.bot.target_active) } } })} />
                </SimpleGrid>
                <Group>
                  <Switch label="Human slots may borrow unused bot capacity" checked={form.capacity.human.allow_borrowing}
                    onChange={(event) => setForm({ ...form, capacity: { ...form.capacity, human: { ...form.capacity.human, allow_borrowing: event.currentTarget.checked } } })} />
                  <Switch label="Accept public human challenges" checked={form.capacity.human.accept_public}
                    onChange={(event) => setForm({ ...form, capacity: { ...form.capacity, human: { ...form.capacity.human, accept_public: event.currentTarget.checked } } })} />
                </Group>
              </Stack>
            </Fieldset>
            <Fieldset legend="Inbound game types and connection policy">
              <Stack gap="sm">
                <Switch label="Accept challenges from bots" checked={form.inbound_challenges.accept_bots}
                  onChange={(event) => setForm({ ...form, inbound_challenges: { ...form.inbound_challenges, accept_bots: event.currentTarget.checked } })} />
                <SimpleGrid cols={{ base: 1, sm: 2 }}>
                  <NumberInput label="Minimum base time (seconds)" min={0} max={86400} value={form.inbound_challenges.min_base}
                    onChange={(value) => setForm({ ...form, inbound_challenges: { ...form.inbound_challenges, min_base: numberValue(value, form.inbound_challenges.min_base) } })} />
                  <NumberInput label="Maximum base time (seconds)" min={0} max={86400} value={form.inbound_challenges.max_base}
                    onChange={(value) => setForm({ ...form, inbound_challenges: { ...form.inbound_challenges, max_base: numberValue(value, form.inbound_challenges.max_base) } })} />
                  <NumberInput label="Minimum increment (seconds)" min={0} max={300} value={form.inbound_challenges.min_increment}
                    onChange={(value) => setForm({ ...form, inbound_challenges: { ...form.inbound_challenges, min_increment: numberValue(value, form.inbound_challenges.min_increment) } })} />
                  <NumberInput label="Maximum increment (seconds)" min={0} max={300} value={form.inbound_challenges.max_increment}
                    onChange={(value) => setForm({ ...form, inbound_challenges: { ...form.inbound_challenges, max_increment: numberValue(value, form.inbound_challenges.max_increment) } })} />
                </SimpleGrid>
                <MultiSelect label="Accepted speeds" data={speeds} value={form.inbound_challenges.speeds}
                  onChange={(value) => setForm({ ...form, inbound_challenges: { ...form.inbound_challenges, speeds: value } })} />
                <MultiSelect label="Accepted modes" data={modes} value={form.inbound_challenges.modes}
                  onChange={(value) => setForm({ ...form, inbound_challenges: { ...form.inbound_challenges, modes: value } })} />
              </Stack>
            </Fieldset>
            <Fieldset legend="Outbound challenges">
              <Stack gap="sm">
                <Group>
                  <Switch label="Rated games" checked={form.outbound_challenge.rated}
                    onChange={(event) => setForm({ ...form, outbound_challenge: { ...form.outbound_challenge, rated: event.currentTarget.checked } })} />
                </Group>
                <SimpleGrid cols={{ base: 1, sm: 2, lg: 4 }}>
                  <Select
                    label="Play behavior"
                    description="Choose whether to seek games or only wait for incoming challenges."
                    data={[
                      { value: 'wait', label: 'Wait for incoming challenges' },
                      {
                        value: 'active',
                        label: 'Actively seek bot games',
                        disabled: form.capacity.bot.slots === 0,
                      },
                    ]}
                    value={outboundBehavior(form)}
                    onChange={(value) => value && setForm(changeOutboundBehavior(
                      form,
                      value as 'wait' | 'active',
                    ))}
                  />
                  <Select label="Preferred color" data={['random', 'white', 'black']} value={form.outbound_challenge.color}
                    onChange={(value) => value && setForm({ ...form, outbound_challenge: { ...form.outbound_challenge, color: value as 'random' | 'white' | 'black' } })} />
                  <NumberInput label="Clock base (seconds)" min={0} max={86400} value={form.outbound_challenge.clock_limit}
                    onChange={(value) => setForm({ ...form, outbound_challenge: { ...form.outbound_challenge, clock_limit: numberValue(value, form.outbound_challenge.clock_limit) } })} />
                  <NumberInput label="Clock increment (seconds)" min={0} max={300} value={form.outbound_challenge.clock_increment}
                    onChange={(value) => setForm({ ...form, outbound_challenge: { ...form.outbound_challenge, clock_increment: numberValue(value, form.outbound_challenge.clock_increment) } })} />
                </SimpleGrid>
              </Stack>
            </Fieldset>
            <Fieldset legend="Scheduler timing and opponent selection">
              <SimpleGrid cols={{ base: 1, sm: 2, lg: 3 }}>
                <NumberInput label="Discovery interval (seconds)" min={15} max={3600} value={form.scheduler.discovery_interval_seconds}
                  onChange={(value) => setForm({ ...form, scheduler: { ...form.scheduler, discovery_interval_seconds: numberValue(value, form.scheduler.discovery_interval_seconds) } })} />
                <NumberInput label="Challenge interval (seconds)" min={1} max={300} value={form.scheduler.challenge_interval_seconds}
                  onChange={(value) => setForm({ ...form, scheduler: { ...form.scheduler, challenge_interval_seconds: numberValue(value, form.scheduler.challenge_interval_seconds) } })} />
                <NumberInput label="Reservation retry (seconds)" min={5} max={600} value={form.scheduler.reservation_retry_seconds}
                  onChange={(value) => setForm({ ...form, scheduler: { ...form.scheduler, reservation_retry_seconds: numberValue(value, form.scheduler.reservation_retry_seconds) } })} />
                <NumberInput label="Challenge timeout (seconds)" min={10} max={300} value={form.scheduler.challenge_timeout_seconds}
                  onChange={(value) => setForm({ ...form, scheduler: { ...form.scheduler, challenge_timeout_seconds: numberValue(value, form.scheduler.challenge_timeout_seconds) } })} />
                <NumberInput label="Games per opponent / day" min={1} max={100} value={form.opponent_selection.max_games_per_opponent_per_day}
                  onChange={(value) => setForm({ ...form, opponent_selection: { ...form.opponent_selection, max_games_per_opponent_per_day: numberValue(value, form.opponent_selection.max_games_per_opponent_per_day) } })} />
                <NumberInput label="Timeout cooldown (minutes)" min={1} max={1440} value={form.opponent_selection.timeout_cooldown_minutes}
                  onChange={(value) => setForm({ ...form, opponent_selection: { ...form.opponent_selection, timeout_cooldown_minutes: numberValue(value, form.opponent_selection.timeout_cooldown_minutes) } })} />
                <NumberInput label="Decline cooldown (minutes)" min={1} max={10080} value={form.opponent_selection.decline_cooldown_minutes}
                  onChange={(value) => setForm({ ...form, opponent_selection: { ...form.opponent_selection, decline_cooldown_minutes: numberValue(value, form.opponent_selection.decline_cooldown_minutes) } })} />
                <NumberInput label="Rating target" min={400} max={4000} value={form.opponent_selection.rating_target}
                  onChange={(value) => setForm({ ...form, opponent_selection: { ...form.opponent_selection, rating_target: numberValue(value, form.opponent_selection.rating_target) } })} />
                <NumberInput label="Rating spread" min={50} max={2000} value={form.opponent_selection.rating_spread}
                  onChange={(value) => setForm({ ...form, opponent_selection: { ...form.opponent_selection, rating_spread: numberValue(value, form.opponent_selection.rating_spread) } })} />
              </SimpleGrid>
            </Fieldset>
            {ratedConfirmationRequired && (
              <Checkbox
                color="orange"
                checked={confirmRated}
                onChange={(event) => setConfirmRated(event.currentTarget.checked)}
                label="I understand that this enables rated Lichess games and may affect the account rating."
              />
            )}
            {outboundConfirmationRequired && (
              <Checkbox
                color="orange"
                checked={confirmOutbound}
                onChange={(event) => setConfirmOutbound(event.currentTarget.checked)}
                label="I understand that this authorizes the bot to send automatic outbound challenges."
              />
            )}
            {save.error && <Alert color="red">{save.error.message}</Alert>}
            {save.isSuccess && <Alert color="green">Adapter options were validated and saved atomically.</Alert>}
            <Group justify="flex-end">
              <Button variant="default" onClick={() => setForm(null)}>
                Reset unsaved changes
              </Button>
              <Button loading={save.isPending} disabled={saveBlocked} onClick={() => save.mutate()}>
                Validate and save options
              </Button>
            </Group>
          </Stack>
        )}
      </div>
    </Stack>
  )
}
