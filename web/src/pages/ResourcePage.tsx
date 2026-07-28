import { IconDownload, IconRefresh } from '@tabler/icons-react'
import {
  Button,
  Group,
  Paper,
  ScrollArea,
  Table,
  Text,
} from '@mantine/core'
import { useQuery } from '@tanstack/react-query'
import { api } from '../api/client'
import type { ResourceKind } from '../api/types'
import { EmptyState } from '../components/EmptyState'
import { PageHeader } from '../components/PageHeader'
import { StatusPill } from '../components/StatusPill'

export function ResourcePage({
  title,
  description,
  kind,
}: {
  title: string
  description: string
  kind: ResourceKind
}) {
  const resources = useQuery({
    queryKey: ['resources', kind],
    queryFn: () => api.resources(kind),
  })
  return (
    <>
      <PageHeader
        title={title}
        description={description}
        actions={
          <Group>
            {kind === 'report' && (
              <Button variant="default" leftSection={<IconDownload size={16} />}>
                Export manifest
              </Button>
            )}
            <Button
              variant="default"
              leftSection={<IconRefresh size={16} />}
              onClick={() => resources.refetch()}
            >
              Refresh
            </Button>
          </Group>
        }
      />
      <Paper withBorder className="panel" p={0}>
        {!resources.data?.length ? (
          <EmptyState
            title={`No ${title.toLowerCase()} yet`}
            detail="Resources appear here as soon as they are created through the shared command contract."
          />
        ) : (
          <ScrollArea>
            <Table verticalSpacing="md" horizontalSpacing="lg">
              <Table.Thead>
                <Table.Tr>
                  <Table.Th>Name</Table.Th>
                  <Table.Th>State</Table.Th>
                  <Table.Th>Revision</Table.Th>
                  <Table.Th>Updated</Table.Th>
                  <Table.Th>ID</Table.Th>
                </Table.Tr>
              </Table.Thead>
              <Table.Tbody>
                {resources.data.map((resource) => (
                  <Table.Tr key={resource.id}>
                    <Table.Td fw={650}>
                      {String(resource.data.display_name ?? resource.data.name ?? resource.kind)}
                    </Table.Td>
                    <Table.Td>
                      <StatusPill
                        value={String(
                          resource.data.observed_state ??
                            resource.data.lifecycle ??
                            resource.data.status ??
                            'recorded',
                        )}
                      />
                    </Table.Td>
                    <Table.Td>{resource.revision}</Table.Td>
                    <Table.Td>{new Date(resource.updated_at).toLocaleString()}</Table.Td>
                    <Table.Td>
                      <Text size="xs" ff="monospace">
                        {resource.id.slice(0, 13)}…
                      </Text>
                    </Table.Td>
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
