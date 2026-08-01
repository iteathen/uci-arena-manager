import { IconTrash } from '@tabler/icons-react'
import { Alert, Button, Group, Modal, Paper, Progress, SimpleGrid, Stack, Text } from '@mantine/core'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { useState } from 'react'
import { api, makeCommand } from '../api/client'
import type { MatchPlanData, Resource } from '../api/types'
import { StatusPill } from './StatusPill'
import { availableCompetitionActions, type LifecycleAction } from './competitionModel'

type Competition = Resource<MatchPlanData>

export function CompetitionList({
  emptyAction,
  navigate,
}: {
  emptyAction?: { label: string; onClick: () => void }
  navigate: (page: string) => void
}) {
  const queryClient = useQueryClient()
  const [pendingRemoval, setPendingRemoval] = useState<Competition | null>(null)
  const matches = useQuery({
    queryKey: ['resources', 'match_plan'],
    queryFn: () => api.resources<MatchPlanData>('match_plan'),
  })
  const tournaments = useQuery({
    queryKey: ['resources', 'tournament'],
    queryFn: () => api.resources<MatchPlanData>('tournament'),
  })
  const mutation = useMutation({
    mutationFn: ({ resource, action }: { resource: Competition; action: LifecycleAction }) => {
      const prefix = resource.kind === 'match_plan' ? 'match' : 'tournament'
      return api.command(
        makeCommand(
          `${prefix}.${action}`,
          resource.kind,
          {},
          false,
          resource.id,
          resource.revision,
        ),
      )
    },
    onSuccess: async () => {
      await queryClient.invalidateQueries({ queryKey: ['resources'] })
    },
  })
  const removal = useMutation({
    mutationFn: (resource: Competition) => api.command(
      makeCommand(
        'resource.remove',
        resource.kind,
        {},
        false,
        resource.id,
        resource.revision,
      ),
    ),
    onSuccess: async () => {
      setPendingRemoval(null)
      await queryClient.invalidateQueries({ queryKey: ['resources'] })
    },
  })
  const competitions = [...(matches.data ?? []), ...(tournaments.data ?? [])].sort(
    (left, right) => right.updated_at.localeCompare(left.updated_at),
  )

  if (matches.isLoading || tournaments.isLoading) {
    return <Text c="dimmed">Loading saved competitions…</Text>
  }
  if (matches.error || tournaments.error) {
    return <Alert color="red">{(matches.error ?? tournaments.error)?.message}</Alert>
  }
  if (!competitions.length) {
    return (
      <Paper withBorder className="panel">
        <Stack align="flex-start">
          <Text fw={700}>No saved competitions</Text>
          <Text size="sm" c="dimmed">
            Create a match or tournament plan, then start it here.
          </Text>
          {emptyAction && <Button onClick={emptyAction.onClick}>{emptyAction.label}</Button>}
        </Stack>
      </Paper>
    )
  }

  return (
    <Stack gap="md">
      {mutation.error && <Alert color="red">{mutation.error.message}</Alert>}
      {removal.error && <Alert color="red">{removal.error.message}</Alert>}
      <SimpleGrid cols={{ base: 1, xl: 2 }}>
        {competitions.map((resource) => {
          const progress = resource.data.progress
          const actions = availableCompetitionActions(resource.data.lifecycle)
          const totalGames = resource.data.planned_total_games ?? progress?.total_games ?? resource.data.pairings.length
          const latestBye = resource.data.swiss_byes?.at(-1)
          const sprt = resource.data.sprt
          return (
            <Paper withBorder className="panel" key={`${resource.kind}:${resource.id}`}>
              <Stack gap="sm">
                <Group justify="space-between" align="flex-start">
                  <div>
                    <Text fw={750}>{resource.data.display_name}</Text>
                    <Text size="xs" c="dimmed">
                      {resource.data.format.replaceAll('_', ' ')} · {resource.data.engine_ids.length}{' '}
                      participants · {totalGames} scheduled games
                    </Text>
                    <Text size="xs" c="dimmed" ff="monospace">
                      {resource.kind} {resource.id.slice(0, 13)}… · created{' '}
                      {new Date(resource.created_at).toLocaleString()}
                    </Text>
                  </div>
                  <StatusPill value={resource.data.lifecycle} />
                </Group>
                <Progress value={progress?.percentage ?? 0} aria-label="Competition progress" />
                <Group justify="space-between" gap="xs">
                  <Text size="sm" c="dimmed">
                    {progress?.completed_games ?? 0} completed · {progress?.active_games ?? 0} active ·{' '}
                    {progress?.queued_games ?? totalGames} queued /{' '}
                    {progress?.total_games ?? totalGames} total
                  </Text>
                  <Text size="xs" c="dimmed">
                    {progress?.current_round ? `Round ${progress.current_round} · ` : ''}
                    Updated {new Date(progress?.updated_at ?? resource.updated_at).toLocaleString()}
                  </Text>
                </Group>
                {typeof resource.data.failure_detail === 'string' &&
                  resource.data.failure_detail.length > 0 && (
                    <Alert color="red">{resource.data.failure_detail}</Alert>
                  )}
                {sprt && (
                  <Alert color={sprt.verdict ? 'teal' : 'blue'}>
                    SPRT {sprt.verdict ? `decision: ${sprt.verdict.replace('_', ' ')}` : 'in progress'} ·{' '}
                    LLR {Number(sprt.llr ?? 0).toFixed(3)} · {sprt.games} admitted games
                  </Alert>
                )}
                {resource.data.format === 'swiss' && (
                  <Text size="sm" c="dimmed">
                    {resource.data.rounds} rounds planned
                    {latestBye ? ` · Round ${latestBye.round} bye: ${latestBye.engine_id}` : ' · no byes awarded'}
                  </Text>
                )}
                <Group gap="xs">
                  <Button size="xs" variant="subtle" component="a" href="#page=planner" onClick={(event) => { event.preventDefault(); navigate('planner') }}>Planner controls</Button>
                  <Button size="xs" variant="subtle" component="a" href="#page=engines" onClick={(event) => { event.preventDefault(); navigate('engines') }}>Engines</Button>
                  <Button size="xs" variant="subtle" component="a" href="#page=reports" onClick={(event) => { event.preventDefault(); navigate('reports') }}>Reports</Button>
                </Group>
                {actions.length > 0 && (
                  <Group justify="flex-end">
                    {resource.data.lifecycle === 'planned' && (
                      <Button
                        variant="default"
                        color="red"
                        leftSection={<IconTrash size={16} />}
                        disabled={mutation.isPending || removal.isPending}
                        onClick={() => setPendingRemoval(resource)}
                      >
                        Remove plan
                      </Button>
                    )}
                    {actions.map((action) => (
                      <Button
                        key={action}
                        variant={action === 'cancel' ? 'default' : 'filled'}
                        color={action === 'cancel' ? 'red' : undefined}
                        loading={
                          mutation.isPending &&
                          mutation.variables?.resource.id === resource.id &&
                          mutation.variables.action === action
                        }
                        disabled={mutation.isPending}
                        onClick={() => mutation.mutate({ resource, action })}
                      >
                        {action[0].toUpperCase() + action.slice(1)}
                      </Button>
                    ))}
                  </Group>
                )}
              </Stack>
            </Paper>
          )
        })}
      </SimpleGrid>
      <Modal
        opened={pendingRemoval !== null}
        onClose={() => !removal.isPending && setPendingRemoval(null)}
        title="Remove planned competition?"
        centered
      >
        <Stack>
          <Text size="sm">
            This removes only the never-started plan. Started competitions and game history cannot be removed here.
          </Text>
          {pendingRemoval && (
            <Text size="xs" c="dimmed" ff="monospace">
              {pendingRemoval.data.display_name} · {pendingRemoval.id}
            </Text>
          )}
          <Group justify="flex-end">
            <Button variant="default" disabled={removal.isPending} onClick={() => setPendingRemoval(null)}>
              Keep plan
            </Button>
            <Button
              color="red"
              loading={removal.isPending}
              disabled={!pendingRemoval}
              onClick={() => pendingRemoval && removal.mutate(pendingRemoval)}
            >
              Remove plan
            </Button>
          </Group>
        </Stack>
      </Modal>
    </Stack>
  )
}
