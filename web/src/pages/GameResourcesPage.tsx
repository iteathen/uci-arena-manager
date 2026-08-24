import {
  IconChevronLeft,
  IconChevronRight,
  IconExternalLink,
  IconPlayerPlay,
  IconRefresh,
} from '@tabler/icons-react'
import {
  Alert,
  Badge,
  Button,
  Group,
  Paper,
  SegmentedControl,
  SimpleGrid,
  Slider,
  Stack,
  Text,
} from '@mantine/core'
import { useQuery } from '@tanstack/react-query'
import { useMemo, useState } from 'react'
import { api } from '../api/client'
import type {
  EngineData,
  GameData,
  ProviderGameComponent,
  ProviderGameSummary,
  Resource,
} from '../api/types'
import { Chessboard } from '../components/Chessboard'
import { EmptyState } from '../components/EmptyState'
import { PageHeader } from '../components/PageHeader'
import { StatusPill } from '../components/StatusPill'

type GameSource = 'all' | 'local' | 'lichess'

function formatClock(milliseconds: number) {
  const seconds = Math.max(0, Math.ceil(milliseconds / 1000))
  const minutes = Math.floor(seconds / 60)
  return `${minutes}:${String(seconds % 60).padStart(2, '0')}`
}

function engineLabel(engines: Map<string, string>, id: string) {
  return engines.get(id) ?? `${id.slice(0, 13)}…`
}

function componentLabel(component: ProviderGameComponent | null) {
  return component
    ? `${component.component_id}@${component.version}`
    : 'unavailable historical evidence'
}

function ReplayButtons({
  shownPly,
  maximumPly,
  setPly,
}: {
  shownPly: number
  maximumPly: number
  setPly: (value: number) => void
}) {
  return (
    <Group justify="center" mt="md">
      <Button
        variant="default"
        leftSection={<IconChevronLeft size={16} />}
        disabled={shownPly === 0}
        onClick={() => setPly(Math.max(0, shownPly - 1))}
      >
        Previous ply
      </Button>
      <Button
        variant="default"
        rightSection={<IconChevronRight size={16} />}
        disabled={shownPly === maximumPly}
        onClick={() => setPly(Math.min(maximumPly, shownPly + 1))}
      >
        Next ply
      </Button>
    </Group>
  )
}

function LocalGameCard({
  game,
  engines,
  replay,
  navigate,
}: {
  game: Resource<GameData>
  engines: Map<string, string>
  replay: boolean
  navigate: (page: string) => void
}) {
  const maximumPly = Math.min(game.data.uci_moves.length, game.data.per_ply_fen.length)
  const [ply, setPly] = useState(maximumPly)
  const shownPly = replay ? Math.min(ply, maximumPly) : maximumPly
  const fen = shownPly === 0
    ? game.data.starting_fen
    : game.data.per_ply_fen[shownPly - 1] ?? game.data.starting_fen
  const evaluation = shownPly > 0 ? game.data.evaluations_cp[shownPly - 1] : null
  const variation = shownPly > 0 ? game.data.principal_variations[shownPly - 1] : undefined

  return (
    <Paper withBorder className="panel">
      <Stack gap="md">
        <Group justify="space-between" align="flex-start">
          <div>
            <Text fw={750}>{game.data.display_name}</Text>
            <Text size="xs" c="dimmed" ff="monospace">
              Game {game.id.slice(0, 13)}… · competition {game.data.competition_id.slice(0, 13)}…
            </Text>
          </div>
          <Group gap="xs">
            <Badge color="indigo" variant="light">Local</Badge>
            <Badge variant="light">{game.data.variant}</Badge>
            <StatusPill value={game.data.lifecycle} />
          </Group>
        </Group>
        <SimpleGrid cols={{ base: 1, md: 2 }} spacing="lg">
          <Chessboard fen={fen} />
          <Stack gap="sm">
            <Group justify="space-between">
              <Text fw={650}>{engineLabel(engines, game.data.white_engine.id)}</Text>
              <Text ff="monospace">{formatClock(game.data.clocks.white_ms)}</Text>
            </Group>
            <Group justify="space-between">
              <Text fw={650}>{engineLabel(engines, game.data.black_engine.id)}</Text>
              <Text ff="monospace">{formatClock(game.data.clocks.black_ms)}</Text>
            </Group>
            <Text size="sm">
              Round {game.data.round} · board {game.data.board} · ply {shownPly}/{game.data.uci_moves.length}
            </Text>
            <Text size="sm">Result {game.data.result} · {game.data.termination.replaceAll('_', ' ')}</Text>
            <Text size="sm" c="dimmed">
              Evaluation {evaluation == null ? 'unavailable' : `${evaluation > 0 ? '+' : ''}${evaluation} cp`}
            </Text>
            <Text size="sm" c="dimmed" ff="monospace">
              PV {variation?.length ? variation.join(' ') : 'unavailable'}
            </Text>
            {game.data.failure_detail && <Alert color="red">{game.data.failure_detail}</Alert>}
            <Button
              variant="subtle"
              size="xs"
              component="a"
              href="#page=tournaments"
              onClick={(event) => {
                event.preventDefault()
                navigate('tournaments')
              }}
            >
              Open owning competition
            </Button>
          </Stack>
        </SimpleGrid>
        {replay && (
          <Stack gap="xs">
            <Slider
              aria-label={`Replay ply for ${game.data.display_name}`}
              min={0}
              max={maximumPly}
              step={1}
              value={shownPly}
              onChange={setPly}
              marks={[{ value: 0, label: 'Start' }, { value: maximumPly, label: `${maximumPly}` }]}
              disabled={maximumPly === 0}
            />
            <ReplayButtons shownPly={shownPly} maximumPly={maximumPly} setPly={setPly} />
          </Stack>
        )}
      </Stack>
    </Paper>
  )
}

function ProviderGameCard({ game, replay }: { game: ProviderGameSummary; replay: boolean }) {
  const archiveComplete =
    game.archive_completeness === 'complete' || game.archive_completeness === 'reconciled'
  const [replayOpen, setReplayOpen] = useState(false)
  const [ply, setPly] = useState<number | null>(null)
  const detail = useQuery({
    queryKey: ['provider-game', 'lichess_bot', game.game_id],
    queryFn: () => api.providerGame('lichess_bot', game.game_id),
    enabled: replay && archiveComplete && replayOpen,
    retry: false,
  })
  const archivedGame = detail.data?.availability === 'available' ? detail.data.game : null
  const maximumPly = archivedGame?.replay.length ?? 0
  const shownPly = Math.min(ply ?? maximumPly, maximumPly)
  const fen = archivedGame
    ? shownPly === 0
      ? archivedGame.initial_fen
      : archivedGame.replay[shownPly - 1]?.after_fen
    : null
  const completenessColor = game.archive_completeness === 'summary_only'
    ? 'orange'
    : archiveComplete
      ? 'green'
      : 'blue'

  return (
    <Paper withBorder className="panel">
      <Stack gap="md">
        <Group justify="space-between" align="flex-start">
          <div>
            <Text fw={750}>Lichess vs {game.opponent || 'unknown opponent'}</Text>
            <Text size="xs" c="dimmed" ff="monospace">
              Game {game.game_id} · cursor {game.cursor} · {new Date(game.started_at).toLocaleString()}
            </Text>
          </div>
          <Group gap="xs">
            <Badge color="cyan" variant="light">Lichess</Badge>
            <Badge color={completenessColor} variant="light">
              {game.archive_completeness.replaceAll('_', ' ')}
            </Badge>
            <StatusPill value={game.status} />
          </Group>
        </Group>
        <SimpleGrid cols={{ base: 1, md: 2 }} spacing="lg">
          <Stack gap="xs">
            <Text size="sm">Color {game.color ?? 'unavailable'} · {game.rated ? 'rated' : 'casual'}</Text>
            <Text size="sm">{game.speed ?? 'unknown speed'} · {game.clock_limit}+{game.clock_increment}</Text>
            <Text size="sm">Ply {game.plies} · result {game.result ?? 'in progress'}</Text>
            <Text size="sm" c="dimmed">Provider {componentLabel(game.provider_component)}</Text>
            <Text size="sm" c="dimmed">Engine {componentLabel(game.engine_component)}</Text>
          </Stack>
          <Stack gap="sm" align="flex-start">
            {game.archive_completeness === 'summary_only' && (
              <Alert color="orange" title="Replay unavailable">
                This Provider record is summary-only ({game.incomplete_reason?.replaceAll('_', ' ') ?? 'historical evidence unavailable'}). No position, move, or clock data is inferred.
              </Alert>
            )}
            {replay && archiveComplete && (
              <Button
                variant="default"
                leftSection={<IconPlayerPlay size={16} />}
                loading={detail.isFetching}
                onClick={() => {
                  setPly(null)
                  setReplayOpen((open) => !open)
                }}
              >
                {replayOpen ? 'Close replay' : 'Load Provider replay'}
              </Button>
            )}
            <Button
              component="a"
              href={`https://lichess.org/${game.game_id}`}
              target="_blank"
              rel="noreferrer"
              variant="subtle"
              rightSection={<IconExternalLink size={15} />}
            >
              Open on Lichess
            </Button>
          </Stack>
        </SimpleGrid>
        {detail.error && <Alert color="red">{detail.error.message}</Alert>}
        {replayOpen && detail.data?.availability === 'unavailable' && (
          <Alert color="orange">Provider replay capability is unavailable.</Alert>
        )}
        {replayOpen && archivedGame && fen && (
          <SimpleGrid cols={{ base: 1, md: 2 }} spacing="lg">
            <Chessboard fen={fen} />
            <Stack gap="sm">
              <Text fw={650}>Provider-authored archive</Text>
              <Text size="sm">Ply {shownPly}/{maximumPly}</Text>
              <Text size="sm" c="dimmed">
                Runtime provenance {archivedGame.runtime_provenance?.sha256 ?? 'unavailable historical evidence'}
              </Text>
              <Text size="sm" c="dimmed">
                Positions are rendered from archived FEN snapshots; Manager does not reconstruct chess state.
              </Text>
            </Stack>
            <Stack gap="xs" style={{ gridColumn: '1 / -1' }}>
              <Slider
                aria-label={`Replay ply for Lichess game ${game.game_id}`}
                min={0}
                max={maximumPly}
                step={1}
                value={shownPly}
                onChange={setPly}
                marks={[{ value: 0, label: 'Start' }, { value: maximumPly, label: `${maximumPly}` }]}
                disabled={maximumPly === 0}
              />
              <ReplayButtons shownPly={shownPly} maximumPly={maximumPly} setPly={setPly} />
            </Stack>
          </SimpleGrid>
        )}
      </Stack>
    </Paper>
  )
}

export function GameResourcesPage({
  mode,
  navigate,
  providerEnabled,
}: {
  mode: 'live' | 'history'
  navigate: (page: string) => void
  providerEnabled: boolean
}) {
  const [source, setSource] = useState<GameSource>('all')
  const [providerCursorTrails, setProviderCursorTrails] = useState({
    live: [0],
    history: [0],
  })
  const providerCursors = providerCursorTrails[mode]
  const setProviderCursors = (update: (cursors: number[]) => number[]) => {
    setProviderCursorTrails((trails) => ({
      ...trails,
      [mode]: update(trails[mode]),
    }))
  }
  const providerBefore = providerCursors.at(-1) ?? 0
  const games = useQuery({
    queryKey: ['resources', 'game'],
    queryFn: () => api.resources<GameData>('game'),
  })
  const engines = useQuery({
    queryKey: ['resources', 'engine'],
    queryFn: () => api.resources<EngineData>('engine'),
  })
  const providerGames = useQuery({
    queryKey: ['provider-games', 'lichess_bot', mode, providerBefore],
    queryFn: () => api.providerGames('lichess_bot', mode, providerBefore, 100),
    enabled: providerEnabled && source !== 'local',
    retry: false,
    refetchInterval: mode === 'live' ? 5_000 : false,
    refetchIntervalInBackground: false,
  })
  const engineNames = useMemo(
    () => new Map((engines.data ?? []).map((engine) => [engine.id, engine.data.display_name])),
    [engines.data],
  )
  const localGames = (games.data ?? []).filter((game) => mode === 'live'
    ? game.data.lifecycle === 'running' || game.data.lifecycle === 'interrupted'
    : ['completed', 'cancelled', 'failed'].includes(game.data.lifecycle))
  const providerPage = providerGames.data?.availability === 'available'
    ? providerGames.data.page
    : null
  const visible = [
    ...(source !== 'lichess'
      ? localGames.map((game) => ({ source: 'local' as const, timestamp: Date.parse(game.updated_at), game }))
      : []),
    ...(source !== 'local'
      ? (providerPage?.games ?? []).map((game) => ({ source: 'lichess' as const, timestamp: game.started_at, game }))
      : []),
  ].sort((left, right) => right.timestamp - left.timestamp)
  const title = mode === 'live' ? 'Live games' : 'Game history & replay'
  const loading = games.isLoading || engines.isLoading ||
    (providerEnabled && source !== 'local' && providerGames.isLoading)

  return (
    <>
      <PageHeader
        title={title}
        description={mode === 'live'
          ? 'Manager-owned local games and bounded Provider-owned Lichess activity.'
          : 'Durable local replay and Provider-authored Lichess archive evidence.'}
        actions={(
          <Button
            variant="default"
            leftSection={<IconRefresh size={16} />}
            loading={games.isFetching || engines.isFetching || providerGames.isFetching}
            onClick={() => {
              const refreshes: Array<Promise<unknown>> = [games.refetch(), engines.refetch()]
              if (providerEnabled && source !== 'local') refreshes.push(providerGames.refetch())
              void Promise.all(refreshes)
            }}
          >
            Refresh
          </Button>
        )}
      />
      <Paper withBorder p="sm" mb="lg">
        <Group justify="space-between">
          <Text size="sm" fw={650}>Source</Text>
          <SegmentedControl
            value={source}
            onChange={(value) => {
              setSource(value as GameSource)
              setProviderCursors(() => [0])
            }}
            data={[
              { label: 'All', value: 'all' },
              { label: 'Local', value: 'local' },
              { label: 'Lichess', value: 'lichess', disabled: !providerEnabled },
            ]}
          />
        </Group>
      </Paper>
      {(games.error || engines.error || providerGames.error) && (
        <Alert color="red" mb="lg">
          {(games.error ?? engines.error ?? providerGames.error)?.message}
        </Alert>
      )}
      {providerEnabled && source !== 'local' && providerGames.data?.availability === 'unavailable' && (
        <Alert color="orange" mb="lg" title="Provider game archive unavailable">
          The installed Provider does not publish the receipt-verified game operations. Local games remain available.
        </Alert>
      )}
      {!loading && !visible.length ? (
        <EmptyState
          title={mode === 'live' ? 'No games are active' : 'No terminal games yet'}
          detail={mode === 'live'
            ? 'Start a saved competition or enable the Lichess Provider to see current games.'
            : 'Completed local games and retained Provider history appear here.'}
          action="Open match planner"
          onAction={() => navigate('planner')}
        />
      ) : (
        <Stack gap="lg">
          {visible.map((entry) => entry.source === 'local'
            ? (
                <LocalGameCard
                  key={`local:${entry.game.id}`}
                  game={entry.game}
                  engines={engineNames}
                  replay={mode === 'history'}
                  navigate={navigate}
                />
              )
            : (
                <ProviderGameCard
                  key={`lichess:${entry.game.game_id}`}
                  game={entry.game}
                  replay={mode === 'history'}
                />
              ))}
          {providerEnabled && source !== 'local' && providerPage && (
            <Group justify="center">
              <Button
                variant="default"
                leftSection={<IconChevronLeft size={16} />}
                disabled={providerCursors.length === 1}
                onClick={() => setProviderCursors((cursors) => cursors.slice(0, -1))}
              >
                Previous Provider page
              </Button>
              <Text size="sm" c="dimmed">Provider page {providerCursors.length}</Text>
              <Button
                variant="default"
                rightSection={<IconChevronRight size={16} />}
                disabled={!providerPage.has_more || providerPage.next_cursor === 0}
                onClick={() => setProviderCursors((cursors) => [
                  ...cursors,
                  providerPage.next_cursor,
                ])}
              >
                Next Provider page
              </Button>
            </Group>
          )}
        </Stack>
      )}
    </>
  )
}
