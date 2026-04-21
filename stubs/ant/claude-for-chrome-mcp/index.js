import { Server } from '@modelcontextprotocol/sdk/server/index.js'
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from '@modelcontextprotocol/sdk/types.js'

const STUB_MESSAGE =
  'Claude in Chrome is unavailable in this build because the private @ant package is not installed.'

export const BROWSER_TOOLS = [
  { name: 'javascript_tool' },
  { name: 'read_page' },
  { name: 'find' },
  { name: 'form_input' },
  { name: 'computer' },
  { name: 'navigate' },
  { name: 'resize_window' },
  { name: 'gif_creator' },
  { name: 'upload_image' },
  { name: 'get_page_text' },
  { name: 'tabs_context_mcp' },
  { name: 'tabs_create_mcp' },
  { name: 'update_plan' },
  { name: 'read_console_messages' },
  { name: 'read_network_requests' },
  { name: 'shortcuts_list' },
  { name: 'shortcuts_execute' },
]

export function createClaudeForChromeMcpServer(context) {
  const server = new Server(
    {
      name: context?.serverName ?? 'claude-in-chrome-stub',
      version: '0.0.0-stub',
    },
    {
      capabilities: {
        tools: {},
      },
    },
  )

  server.setRequestHandler(ListToolsRequestSchema, async () => ({
    tools: BROWSER_TOOLS.map(tool => ({
      name: tool.name,
      description: STUB_MESSAGE,
      inputSchema: {
        type: 'object',
        properties: {},
        additionalProperties: true,
      },
    })),
  }))

  server.setRequestHandler(CallToolRequestSchema, async ({ params }) => ({
    isError: true,
    content: [
      {
        type: 'text',
        text: `${params?.name ?? 'claude_in_chrome'}: ${STUB_MESSAGE}`,
      },
    ],
  }))

  return server
}
