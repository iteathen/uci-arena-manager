import { Alert, Button, Group, Modal, Paper, Stack, Text } from '@mantine/core'
import { IconDownload } from '@tabler/icons-react'
import { useMutation } from '@tanstack/react-query'
import { useState } from 'react'
import {
  applyLocalSetup,
  localSetupAvailable,
  planLocalSetup,
  type LocalSetupRequest,
  type LocalSetupResult,
} from '../api/client'

type ComponentId = Extract<
  LocalSetupRequest,
  { operation: 'component' }
>['component_id']

export function ComponentSetupButton({
  componentId,
  label,
  onCompleted,
  available = true,
}: {
  componentId: ComponentId
  label: string
  onCompleted?: () => void | Promise<void>
  available?: boolean
}) {
  const [plan, setPlan] = useState<LocalSetupResult | null>(null)
  const request: LocalSetupRequest = {
    operation: 'component',
    component_id: componentId,
  }
  const prepare = useMutation({
    mutationFn: () => planLocalSetup(request),
    onSuccess: setPlan,
  })
  const apply = useMutation({
    mutationFn: () => applyLocalSetup(request),
    onSuccess: async () => {
      setPlan(null)
      await onCompleted?.()
    },
  })
  const actions = plan?.actions ?? []
  const blockers = Array.isArray(plan?.blockers) ? plan.blockers : []
  const space =
    plan?.space && typeof plan.space === 'object'
      ? (plan.space as Record<string, unknown>)
      : {}

  if (!available) {
    return (
      <Button variant="default" leftSection={<IconDownload size={16} />} disabled>
        Repair Suite Setup to install
      </Button>
    )
  }

  if (!localSetupAvailable()) {
    return (
      <Button variant="default" leftSection={<IconDownload size={16} />} disabled>
        Open desktop app to install
      </Button>
    )
  }

  return (
    <>
      <Button
        leftSection={<IconDownload size={16} />}
        loading={prepare.isPending}
        onClick={() => prepare.mutate()}
      >
        Download and install
      </Button>
      {prepare.error && <Alert color="red" mt="sm">{prepare.error.message}</Alert>}
      {apply.error && <Alert color="red" mt="sm">{apply.error.message}</Alert>}
      <Modal
        opened={Boolean(plan)}
        onClose={() => !apply.isPending && setPlan(null)}
        title={`Install ${label}`}
        size="lg"
      >
        <Stack>
          <Alert color="blue">
            The adjacent signed setup tool resolved this plan from the retained catalog.
          </Alert>
          <Group grow>
            <Paper withBorder p="sm">
              <Text size="xs" c="dimmed">Components</Text>
              <Text fw={700}>{actions.length}</Text>
            </Paper>
            <Paper withBorder p="sm">
              <Text size="xs" c="dimmed">Peak disk impact</Text>
              <Text fw={700}>
                {Number(space.peak_required_bytes ?? 0).toLocaleString()} bytes
              </Text>
            </Paper>
          </Group>
          {actions.map((action, index) => (
            <Paper withBorder p="sm" key={`${String(action.component_id)}-${index}`}>
              <Text fw={650}>
                {String(action.action ?? 'install')} · {String(action.component_id)}
              </Text>
              {Array.isArray(action.dependencies) && action.dependencies.length > 0 && (
                <Text size="xs" c="dimmed">
                  Dependencies: {action.dependencies.map(String).join(', ')}
                </Text>
              )}
            </Paper>
          ))}
          {blockers.map((blocker, index) => (
            <Alert color="red" key={index}>{JSON.stringify(blocker)}</Alert>
          ))}
          <Alert color="orange">
            Administrator confirmation is required. Manager will restart and reconnect
            after the verified transaction.
          </Alert>
          <Group justify="flex-end">
            <Button
              variant="default"
              disabled={apply.isPending}
              onClick={() => setPlan(null)}
            >
              Cancel
            </Button>
            <Button
              loading={apply.isPending}
              disabled={blockers.length > 0}
              onClick={() => apply.mutate()}
            >
              Confirm and install
            </Button>
          </Group>
        </Stack>
      </Modal>
    </>
  )
}
