import { Alert, Button, Center, Stack, Text, Title } from '@mantine/core'
import { Component } from 'react'
import type { ErrorInfo, ReactNode } from 'react'

interface DashboardErrorBoundaryProps {
  children: ReactNode
  reload?: () => void
}

interface DashboardErrorBoundaryState {
  failed: boolean
}

export class DashboardErrorBoundary extends Component<
  DashboardErrorBoundaryProps,
  DashboardErrorBoundaryState
> {
  state: DashboardErrorBoundaryState = { failed: false }

  static getDerivedStateFromError(): DashboardErrorBoundaryState {
    return { failed: true }
  }

  componentDidCatch(error: Error, info: ErrorInfo) {
    console.error('Dashboard render failed', error, info.componentStack)
  }

  render() {
    if (!this.state.failed) return this.props.children

    return (
      <Center mih="100vh" p="xl">
        <Stack maw={560} gap="lg">
          <Title order={1}>Dashboard update did not load</Title>
          <Alert color="orange" title="Manager is still running">
            The installed dashboard changed while this window was open. Reload the dashboard
            to join the current installed version.
          </Alert>
          <Text c="dimmed">
            If reloading does not resolve the problem, reopen UCI Arena Manager and review Logs.
          </Text>
          <Button onClick={this.props.reload ?? (() => window.location.reload())}>
            Reload dashboard
          </Button>
        </Stack>
      </Center>
    )
  }
}

