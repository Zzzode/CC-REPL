/**
 * Shared types/constants for file persistence.
 *
 * This source snapshot is missing the original `types.ts`; we keep the
 * contracts minimal and stable so headless CLI startup can import
 * `filePersistence.ts` successfully.
 */

export type TurnStartTime = number

export type PersistedFile = {
  filename: string
  file_id: string
}

export type FailedPersistence = {
  filename: string
  error: string
}

export type FilesPersistedEventData = {
  files: PersistedFile[]
  failed: FailedPersistence[]
}

// Session file layout: {cwd}/{sessionId}/outputs
export const OUTPUTS_SUBDIR = 'outputs'

// Guardrail against pathological upload storms in BYOC mode.
export const FILE_COUNT_LIMIT = 1000

// Parallel upload worker count for Files API persistence.
export const DEFAULT_UPLOAD_CONCURRENCY = 8
