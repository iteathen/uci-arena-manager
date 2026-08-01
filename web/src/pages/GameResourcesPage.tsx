import { IconChevronLeft, IconChevronRight, IconRefresh } from '@tabler/icons-react'
import {
  Alert,
  Badge,
  Button,
  Group,
  Paper,
  SimpleGrid,
  Slider,
  Stack,
  Text,
} from '@mantine/core'
import { useQuery } from '@tanstack/react-query'
import { useEffect, useMemo, useState } from 'react'
import { api } from '../api/client'
import type { EngineData, GameData, Resource } from '../api/types'
import { Chessboard } from '../components/Chessboard'
import { EmptyState } from '../components/EmptyState'
import { PageHeader } from '../components/PageHeader'
import { StatusPill } from '../components/StatusPill'

function formatClock(milliseconds: number) {
  const seconds = Math.max(0, Math.ceil(milliseconds / 1000))
  const minutes = Math.floor(seconds / 60)
  return `${minutes}:${String(seconds % 60).padStart(2, '0')}`
}

function engineLabel(engines: Map<string, string>, id: string) {
  return engines.get(id) ?? `${id.slice(0, 13)}…`
}

function GameCard({
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
  useEffect(() => setPly(maximumPly), [maximumPly])
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
            <Group justify="center" mt="md">
              <Button
                variant="default"
                leftSection={<IconChevronLeft size={16} />}
                disabled={shownPly === 0}
                onClick={() => setPly((value) => Math.max(0, value - 1))}
              >
                Previous ply
              </Button>
              <Button
                variant="default"
                rightSection={<IconChevronRight size={16} />}
                disabled={shownPly === maximumPly}
                onClick={() => setPly((value) => Math.min(maximumPly, value + 1))}
              >
                Next ply
              </Button>
            </Group>
          </Stack>
        )}
      </Stack>
    </Paper>
  )
}

export function GameResourcesPage({
  mode,
  navigate,
}: {
  mode: 'live' | 'history'
  navigate: (page: string) => void
}) {
  const games = useQuery({
    queryKey: ['resources', 'game'],
    queryFn: () => api.resources<GameData>('game'),
  })
  const engines = useQuery({
    queryKey: ['resources', 'engine'],
    queryFn: () => api.resources<EngineData>('engine'),
  })
  const engineNames = useMemo(
    () => new Map((engines.data ?? []).map((engine) => [engine.id, engine.data.display_name])),
    [engines.data],
  )
  const visible = (games.data ?? [])
    .filter((game) => mode === 'live'
      ? game.data.lifecycle === 'running' || game.data.lifecycle === 'interrupted'
      : ['completed', 'cancelled', 'failed'].includes(game.data.lifecycle))
    .sort((left, right) => right.updated_at.localeCompare(left.updated_at))
  const title = mode === 'live' ? 'Live games' : 'Game history & replay'

  return (
    <>
      <PageHeader
        title={title}
        description={mode === 'live'
          ? 'Backend-authored positions, clocks, evaluations, and engine state.'
          : 'Replay terminal games from persisted, backend-authoritative FEN snapshots.'}
        actions={(
          <Button
            variant="default"
            leftSection={<IconRefresh size={16} />}
            loading={games.isFetching || engines.isFetching}
            onClick={() => void Promise.all([games.refetch(), engines.refetch()])}
          >
            Refresh
          </Button>
        )}
      />
      {(games.error || engines.error) && (
        <Alert color="red" mb="lg">{(games.error ?? engines.error)?.message}</Alert>
      )}
      {!games.isLoading && !visible.length ? (
        <EmptyState
          title={mode === 'live' ? 'No games are active' : 'No terminal games yet'}
          detail={mode === 'live'
            ? 'Start a saved competition to see its current canonical state here.'
            : 'Completed, cancelled, and failed games appear here as durable history.'}
          action="Open match planner"
          onAction={() => navigate('planner')}
        />
      ) : (
        <Stack gap="lg">
          {visible.map((game) => (
            <GameCard
              key={game.id}
              game={game}
              engines={engineNames}
              replay={mode === 'history'}
              navigate={navigate}
            />
          ))}
        </Stack>
      )}
    </>
  )
}
