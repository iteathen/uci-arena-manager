import { IconRefresh } from '@tabler/icons-react'
import { Alert, Anchor, Button, Code, Paper, ScrollArea, Table, Text } from '@mantine/core'
import { useQuery } from '@tanstack/react-query'
import { api } from '../api/client'
import { EmptyState } from '../components/EmptyState'
import { PageHeader } from '../components/PageHeader'
import { resourcePageForKind } from '../components/resourceNavigation'

export function AuditPage() {
  const events = useQuery({
    queryKey: ['events', 'audit'],
    queryFn: () => api.events(0),
  })

  return (
    <>
      <PageHeader
        title="Audit"
        description="Authenticated command correlation and durable state transitions."
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
        {!events.data?.length ? (
          <EmptyState
            title="No durable events yet"
            detail="Successful resource mutations appear here through the Manager event contract."
          />
        ) : (
          <ScrollArea>
            <Table verticalSpacing="sm" horizontalSpacing="lg">
              <Table.Thead>
                <Table.Tr>
                  <Table.Th>Time</Table.Th>
                  <Table.Th>Transition</Table.Th>
                  <Table.Th>Resource</Table.Th>
                  <Table.Th>Correlation</Table.Th>
                  <Table.Th>Cursor</Table.Th>
                </Table.Tr>
              </Table.Thead>
              <Table.Tbody>
                {[...events.data].reverse().map((event) => (
                  <Table.Tr key={event.event_id}>
                    <Table.Td>{new Date(event.timestamp).toLocaleString()}</Table.Td>
                    <Table.Td fw={650}>{event.kind}</Table.Td>
                    <Table.Td>
                      {event.resource ? (
                        <Anchor
                          size="sm"
                          href={`#page=${resourcePageForKind(event.resource.kind)}`}
                        >
                          {event.resource.kind} · {event.resource.id.slice(0, 13)}…
                        </Anchor>
                      ) : (
                        <Text size="sm" c="dimmed">system</Text>
                      )}
                    </Table.Td>
                    <Table.Td>
                      {event.correlation_id ? (
                        <Code>{event.correlation_id.slice(0, 18)}…</Code>
                      ) : (
                        <Text size="sm" c="dimmed">—</Text>
                      )}
                    </Table.Td>
                    <Table.Td>{event.cursor}</Table.Td>
                  </Table.Tr>
                ))}
              </Table.Tbody>
            </Table>
          </ScrollArea>
        )}
      </Paper>
    </>
  )
}
