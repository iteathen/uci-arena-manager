import { IconSearch } from '@tabler/icons-react'
import { Group, Paper, Select, Text, TextInput } from '@mantine/core'
import { useVirtualizer } from '@tanstack/react-virtual'
import { useRef } from 'react'
import { PageHeader } from '../components/PageHeader'

const previewLines = [
  ['INFO', 'control', 'Control plane initialized; SQLite WAL ready'],
  ['INFO', 'auth', 'Local authenticated session established'],
  ['INFO', 'events', 'Durable event stream awaiting subscribers'],
  ['DEBUG', 'scheduler', 'No queued match plans'],
  ['INFO', 'health', 'Local host health is ready'],
]

export function LogsPage() {
  const parent = useRef<HTMLDivElement>(null)
  const virtualizer = useVirtualizer({
    count: previewLines.length,
    getScrollElement: () => parent.current,
    estimateSize: () => 42,
  })
  return (
    <>
      <PageHeader
        title="Logs"
        description="Structured critical events and bounded, rotated process output."
      />
      <Paper withBorder className="panel" p={0}>
        <Group p="md" className="log-toolbar">
          <TextInput leftSection={<IconSearch size={15} />} placeholder="Filter messages" />
          <Select
            defaultValue="all"
            data={[
              { value: 'all', label: 'All sources' },
              { value: 'control', label: 'Control plane' },
              { value: 'engine', label: 'Engines' },
              { value: 'service', label: 'Services' },
            ]}
          />
        </Group>
        <div ref={parent} className="log-viewport">
          <div style={{ height: virtualizer.getTotalSize(), position: 'relative' }}>
            {virtualizer.getVirtualItems().map((row) => {
              const line = previewLines[row.index]
              return (
                <div
                  className="log-line"
                  key={row.key}
                  style={{ transform: `translateY(${row.start}px)` }}
                >
                  <Text size="xs" c="dimmed" ff="monospace">
                    {new Date().toISOString()}
                  </Text>
                  <Text size="xs" className={`log-level level-${line[0].toLowerCase()}`}>
                    {line[0]}
                  </Text>
                  <Text size="xs" c="dimmed" ff="monospace">
                    {line[1]}
                  </Text>
                  <Text size="sm" ff="monospace">
                    {line[2]}
                  </Text>
                </div>
              )
            })}
          </div>
        </div>
      </Paper>
    </>
  )
}
