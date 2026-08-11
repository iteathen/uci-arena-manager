import '@mantine/core/styles.css'
import './styles.css'

import { MantineProvider, createTheme } from '@mantine/core'
import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import App from './App'
import { DashboardErrorBoundary } from './components/DashboardErrorBoundary'
import { recoverDashboardGeneration } from './dashboardGenerationRecovery'

window.addEventListener('vite:preloadError', (event) => {
  recoverDashboardGeneration(event, window.sessionStorage, () => window.location.reload())
})

const theme = createTheme({
  primaryColor: 'indigo',
  fontFamily: 'Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif',
  headings: {
    fontFamily: 'Inter, ui-sans-serif, system-ui, sans-serif',
    fontWeight: '720',
  },
  defaultRadius: 'md',
  cursorType: 'pointer',
})

const queryClient = new QueryClient({
  defaultOptions: {
    queries: { staleTime: 3_000, retry: 1, refetchOnWindowFocus: false },
  },
})

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <MantineProvider theme={theme} defaultColorScheme="dark">
      <QueryClientProvider client={queryClient}>
        <DashboardErrorBoundary>
          <App />
        </DashboardErrorBoundary>
      </QueryClientProvider>
    </MantineProvider>
  </StrictMode>,
)
