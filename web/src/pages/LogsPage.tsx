import { IconRefresh, IconSearch } from '@tabler/icons-react'
import { Alert, Anchor, Button, Group, Paper, Select, Text, TextInput } from '@mantine/core'
import { useQuery } from '@tanstack/react-query'
import { useVirtualizer } from '@tanstack/react-virtual'
import { useMemo, useRef, useState } from 'react'
import { api } from '../api/client'
import { EmptyState } from '../components/EmptyState'
import { logResourceTypeOptions } from '../components/logModel'
import { PageHeader } from '../components/PageHeader'
import { resourcePageForKind } from '../components/resourceNavigation'

export function LogsPage() {
  const parent = useRef<HTMLDivElement>(null)
  const [query, setQuery] = useState('')
  const [source, setSource] = useState<string | null>('all')
  const events = useQuery({
    queryKey: ['events', 'logs'],
    queryFn: () => api.events(0),
  })
  const lines = useMemo(
    () =>
      [...(events.data ?? [])]
        .reverse()
        .filter((event) => {
          if (source !== 'all' && event.resource?.kind !== source) return false
          if (!query.trim()) return true
          const searchable = `${event.kind} ${event.stream} ${event.resource?.kind ?? ''} ${
            event.resource?.id ?? ''
          }`.toLowerCase()
          return searchable.includes(query.trim().toLowerCase())
        }),
    [events.data, query, source],
  )
  const virtualizer = useVirtualizer({
    count: lines.length,
    getScrollElement: () => parent.current,
    estimateSize: () => 46,
  })

  return (
    <>
      <PageHeader
        title="Logs"
        description="Durable Manager events. Provider output remains bounded by its public contract."
        actions={
          <Button
            variant="default"
            leftSection={<IconRefresh size={16} />}
            loading={events.isFetching}
            onClick={() => events.refetch()}
          >
            Refresh
          </Button>
        }
      />
      {events.error && (
        <Alert color="red" mb="lg">
          {events.error.message}
        </Alert>
      )}
      <Paper withBorder className="panel" p={0}>
        <Group p="md" className="log-toolbar">
          <TextInput
            leftSection={<IconSearch size={15} />}
            placeholder="Filter transitions"
            value={query}
            onChange={(event) => setQuery(event.currentTarget.value)}
          />
          <Select
            label="Resource type"
            aria-label="Resource type"
            value={source}
            onChange={setSource}
            data={[...logResourceTypeOptions]}
          />
        </Group>
        {!lines.length ? (
          <EmptyState
            title={events.data?.length ? 'No matching events' : 'No durable events yet'}
            detail="Change the filter or perform an operation through the shared Manager command contract."
          />
        ) : (
          <div ref={parent} className="log-viewport">
            <div style={{ height: virtualizer.getTotalSize(), position: 'relative' }}>
              {virtualizer.getVirtualItems().map((row) => {
                const event = lines[row.index]
                return (
                  <div
                    className="log-line"
                    key={event.event_id}
                    style={{ transform: `translateY(${row.start}px)` }}
                  >
                    <Text size="xs" c="dimmed" ff="monospace">
                      {new Date(event.timestamp).toISOString()}
                    </Text>
                    <Text size="xs" className="log-level level-info">
                      EVENT
                    </Text>
                    {event.resource ? (
                      <Anchor
                        size="xs"
                        ff="monospace"
                        href={`#page=${resourcePageForKind(event.resource.kind)}`}
                      >
                        {event.resource.kind}
                      </Anchor>
                    ) : (
                      <Text size="xs" c="dimmed" ff="monospace">{event.stream}</Text>
                    )}
                    <Text size="sm" ff="monospace">
                      {event.kind}
                    </Text>
                  </div>
                )
              })}
            </div>
          </div>
        )}
      </Paper>
    </>
  )
}
