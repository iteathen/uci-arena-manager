import { ActionIcon, TextInput, Tooltip } from '@mantine/core'
import { IconFolderOpen } from '@tabler/icons-react'
import { useState } from 'react'
import { pickLocalPath } from '../api/client'
import type { LocalPathFilter } from '../api/client'

interface PathPickerInputProps {
  label: string
  value: string
  onChange: (value: string) => void
  mode: 'file' | 'directory'
  title?: string
  description?: string
  placeholder?: string
  filters?: LocalPathFilter[]
  disabled?: boolean
}

export function PathPickerInput({
  label,
  value,
  onChange,
  mode,
  title,
  description,
  placeholder,
  filters,
  disabled = false,
}: PathPickerInputProps) {
  const [error, setError] = useState('')
  const [loading, setLoading] = useState(false)

  const browse = async () => {
    setLoading(true)
    setError('')
    try {
      const selected = await pickLocalPath({
        mode,
        title: title ?? `Select ${label}`,
        filters,
      })
      if (selected) onChange(selected)
    } catch (reason) {
      setError(reason instanceof Error ? reason.message : 'Native browse dialog failed')
    } finally {
      setLoading(false)
    }
  }

  return (
    <TextInput
      label={label}
      description={description}
      placeholder={placeholder}
      value={value}
      disabled={disabled}
      error={error || undefined}
      onChange={(event) => onChange(event.currentTarget.value)}
      rightSectionPointerEvents="all"
      rightSection={
        <Tooltip label={`Browse for ${label}`} withArrow>
          <ActionIcon
            aria-label={`Browse for ${label}`}
            variant="subtle"
            loading={loading}
            disabled={disabled}
            onClick={browse}
          >
            <IconFolderOpen size={17} />
          </ActionIcon>
        </Tooltip>
      }
    />
  )
}
