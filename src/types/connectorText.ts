export type ConnectorTextBlock = {
  type: 'connector_text'
  connector_text: string
}

export type ConnectorTextDelta = {
  type: 'connector_text_delta'
  connector_text_delta: string
}

export function isConnectorTextBlock(value: unknown): value is ConnectorTextBlock {
  if (typeof value !== 'object' || value === null) {
    return false
  }
  const block = value as Partial<ConnectorTextBlock>
  return block.type === 'connector_text' && typeof block.connector_text === 'string'
}
