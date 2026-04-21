export interface ComputerUseInputAPI {
  moveMouse(x: number, y: number, smooth?: boolean): Promise<void>
  key(name: string, action: 'press' | 'release'): Promise<void>
  keys(parts: string[]): Promise<void>
  mouseLocation(): Promise<{ x: number; y: number }>
  typeText(text: string): Promise<void>
  mouseButton(
    button: 'left' | 'right' | 'middle',
    action: 'click' | 'press' | 'release',
    count?: 1 | 2 | 3,
  ): Promise<void>
  mouseScroll(amount: number, axis: 'vertical' | 'horizontal'): Promise<void>
  getFrontmostAppInfo(): { bundleId?: string; appName?: string } | null
}

export type ComputerUseInput =
  | {
      isSupported: false
    }
  | ({
      isSupported: true
    } & ComputerUseInputAPI)

declare const input: ComputerUseInput
export default input
