import type { ToolResultBlockParam } from '@anthropic-ai/sdk/resources/index.mjs'
import React from 'react'
import { MessageResponse } from 'src/components/MessageResponse.js'
import { FallbackToolUseErrorMessage } from '../../components/FallbackToolUseErrorMessage.js'
import { Text } from '../../ink.js'
import { TOOL_SUMMARY_MAX_LENGTH } from '../../constants/toolLimits.js'
import { truncate } from '../../utils/format.js'
import type { ScriptToolOutput } from './types.js'

export function userFacingName(): string {
  return 'Script'
}

export function renderToolUseMessage(
  input: Partial<{ code: string; description: string }>,
  { verbose }: { verbose: boolean },
): React.ReactNode {
  if (input.description) {
    return verbose
      ? input.description
      : truncate(input.description, TOOL_SUMMARY_MAX_LENGTH)
  }
  if (!input.code) return null

  const preview = input.code.split('\n')[0]?.trim() ?? ''
  return `script: "${truncate(preview, 60)}"`
}

export function renderToolResultMessage(output: ScriptToolOutput): React.ReactNode {
  const duration = output.duration_ms
  const durationStr = duration >= 1000 ? `${(duration / 1000).toFixed(1)}s` : `${duration}ms`

  if (output.timed_out) {
    return (
      <MessageResponse>
        <Text color="yellow">Timed out after {durationStr}</Text>
        {output.stderr && (
          <Text color="yellow"> {truncate(output.stderr, 200)}</Text>
        )}
      </MessageResponse>
    )
  }

  if (output.stderr && output.result === undefined) {
    return (
      <MessageResponse>
        <Text color="error">Script error ({durationStr}): </Text>
        <Text color="error">{truncate(output.stderr, 300)}</Text>
      </MessageResponse>
    )
  }

  if (output.stderr) {
    return (
      <MessageResponse>
        <Text>Completed in {durationStr}</Text>
        <Text dimColor> (warnings: {truncate(output.stderr, 200)})</Text>
      </MessageResponse>
    )
  }

  return (
    <MessageResponse height={1}>
      <Text>Completed in {durationStr}</Text>
    </MessageResponse>
  )
}

export function renderToolUseErrorMessage(
  result: ToolResultBlockParam['content'],
  { verbose }: { verbose: boolean },
): React.ReactNode {
  if (!verbose && typeof result === 'string') {
    return (
      <MessageResponse>
        <Text color="error">Script execution failed</Text>
      </MessageResponse>
    )
  }
  return <FallbackToolUseErrorMessage result={result} verbose={verbose} />
}

export function getToolUseSummary(
  input: Partial<{ code: string; description: string }> | undefined,
): string | null {
  if (!input) return null
  if (input.description) return truncate(input.description, TOOL_SUMMARY_MAX_LENGTH)
  if (!input.code) return null
  return truncate(input.code.split('\n')[0]?.trim() ?? '', TOOL_SUMMARY_MAX_LENGTH)
}
