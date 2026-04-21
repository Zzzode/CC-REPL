import { Server } from '@modelcontextprotocol/sdk/server/index.js'
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from '@modelcontextprotocol/sdk/types.js'

export const DEFAULT_GRANT_FLAGS = {
  clipboardRead: false,
  clipboardWrite: false,
  systemKeyCombos: false,
}

export const API_RESIZE_PARAMS = {
  maxWidth: 1366,
  maxHeight: 768,
}

const STUB_MESSAGE =
  'Computer Use is unavailable in this build because private @ant native packages are not installed.'

const TOOL_NAMES = [
  'screenshot',
  'zoom',
  'request_access',
  'left_click',
  'right_click',
  'middle_click',
  'double_click',
  'triple_click',
  'type',
  'key',
  'hold_key',
  'scroll',
  'left_click_drag',
  'open_application',
  'mouse_move',
  'left_mouse_down',
  'left_mouse_up',
  'cursor_position',
  'list_granted_applications',
  'read_clipboard',
  'write_clipboard',
  'wait',
  'computer_batch',
]

export function targetImageSize(width, height) {
  const safeW = Number.isFinite(width) ? Math.max(1, Math.round(width)) : 1
  const safeH = Number.isFinite(height) ? Math.max(1, Math.round(height)) : 1
  return [safeW, safeH]
}

export function buildComputerUseTools(_capabilities, _coordinateMode, _apps) {
  return TOOL_NAMES.map(name => ({
    name,
    description: STUB_MESSAGE,
    inputSchema: {
      type: 'object',
      properties: {},
      additionalProperties: true,
    },
  }))
}

export function createComputerUseMcpServer(adapter, coordinateMode) {
  const server = new Server(
    {
      name: adapter?.serverName ?? 'computer-use-stub',
      version: '0.0.0-stub',
    },
    {
      capabilities: {
        tools: {},
      },
    },
  )

  server.setRequestHandler(ListToolsRequestSchema, async () => ({
    tools: buildComputerUseTools(adapter?.executor?.capabilities, coordinateMode),
  }))

  server.setRequestHandler(CallToolRequestSchema, async ({ params }) => ({
    isError: true,
    content: [
      {
        type: 'text',
        text: `${params?.name ?? 'computer_use'}: ${STUB_MESSAGE}`,
      },
    ],
  }))

  return server
}

export function bindSessionContext(_adapter, _coordinateMode, _ctx) {
  return async name => ({
    isError: true,
    telemetry: { error_kind: 'unavailable_stub' },
    content: [
      {
        type: 'text',
        text: `${name}: ${STUB_MESSAGE}`,
      },
    ],
  })
}
