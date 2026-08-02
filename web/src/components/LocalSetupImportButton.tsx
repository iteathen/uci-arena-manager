import { IconDatabaseImport } from '@tabler/icons-react'
import { Alert, Button, Group, Modal, Paper, Stack, Text } from '@mantine/core'
import { useMutation } from '@tanstack/react-query'
import { useState } from 'react'
import {
  applyLocalSetup,
  localSetupAvailable,
  planLocalSetup,
  type LocalSetupRequest,
  type LocalSetupResult,
} from '../api/client'

type ImportRequest = Extract<LocalSetupRequest, { operation: 'import' }>

export function LocalSetupImportButton({
  assetKind,
  path,
  label,
  onCompleted,
}: {
  assetKind: ImportRequest['asset_kind']
  path: string
  label: string
  onCompleted?: (result: LocalSetupResult) => void | Promise<void>
}) {
  const [plan, setPlan] = useState<LocalSetupResult | null>(null)
  const request: ImportRequest = {
    operation: 'import',
    asset_kind: assetKind,
    path,
  }
  const prepare = useMutation({
    mutationFn: () => planLocalSetup(request),
    onSuccess: setPlan,
  })
  const apply = useMutation({
    mutationFn: () => applyLocalSetup(request),
    onSuccess: async (result) => {
      setPlan(null)
      await onCompleted?.(result)
    },
  })
  const actions = plan?.actions ?? []
  const blockers = Array.isArray(plan?.blockers) ? plan.blockers : []

  if (!localSetupAvailable()) {
    return (
      <Button variant="default" leftSection={<IconDatabaseImport size={16} />} disabled>
        Open desktop app to save
      </Button>
    )
  }

  return (
    <>
      <Button
        variant="default"
        leftSection={<IconDatabaseImport size={16} />}
        loading={prepare.isPending}
        disabled={!path}
        onClick={() => prepare.mutate()}
      >
        {label}
      </Button>
      {prepare.error && <Alert color="red" mt="sm">{prepare.error.message}</Alert>}
      {apply.error && <Alert color="red" mt="sm">{apply.error.message}</Alert>}
      <Modal
        opened={Boolean(plan)}
        onClose={() => !apply.isPending && setPlan(null)}
        title={label}
        size="lg"
      >
        <Stack>
          <Alert color="blue">
            The adjacent Installer validates this reference and updates every affected
            consumer as one transaction. The credential file is never opened.
          </Alert>
          {actions.map((action, index) => (
            <Paper withBorder p="sm" key={`${String(action.action)}-${index}`}>
              <Text fw={650}>{String(action.action ?? 'validate and import')}</Text>
              <Text size="xs" c="dimmed">
                {String(action.asset_kind ?? assetKind).replaceAll('_', ' ')}
              </Text>
            </Paper>
          ))}
          {blockers.map((blocker, index) => (
            <Alert color="red" key={index}>{JSON.stringify(blocker)}</Alert>
          ))}
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
              Confirm and save
            </Button>
          </Group>
        </Stack>
      </Modal>
    </>
  )
}
