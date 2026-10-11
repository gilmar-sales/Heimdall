import * as path from 'node:path';

// Languages the extension activates for; rulers are applied per language so
// unrelated file types never gain a C++ column guide.
export const CPP_LANGUAGES = ['c', 'cpp', 'cuda', 'objective-c', 'objective-cpp'] as const;

export type CppLanguage = (typeof CPP_LANGUAGES)[number];

// The formatter default when a workspace opted into Heimdall but its config
// does not set `format.column-limit` (mirrors the engine default).
export const FORMATTER_DEFAULT_COLUMN_LIMIT = 100;

export interface RulerFileSystem {
    existsSync(candidate: string): boolean;
    readFileSync(candidate: string, encoding: 'utf8'): string;
}

// `format.column-limit` of one `.heimdall.json` document. Anything missing or
// invalid yields undefined so callers fall back to the formatter default.
export function parseColumnLimit(jsonText: string): number | undefined {
    let document: unknown;
    try {
        document = JSON.parse(jsonText) as unknown;
    } catch {
        return undefined;
    }
    return columnLimitOfDocument(document);
}

function columnLimitOfDocument(document: unknown): number | undefined {
    if (typeof document !== 'object' || document === null) {
        return undefined;
    }
    const format: unknown = (document as Record<string, unknown>)['format'];
    if (typeof format !== 'object' || format === null) {
        return undefined;
    }
    const limit: unknown = (format as Record<string, unknown>)['column-limit'];
    if (typeof limit !== 'number' || !Number.isFinite(limit) || limit < 0) {
        return undefined;
    }
    return Math.floor(limit);
}

interface ChainEntry {
    root: boolean;
    limit: number | undefined;
}

function readChainEntry(file: string, fileSystem: RulerFileSystem): ChainEntry | undefined {
    let document: unknown;
    try {
        document = JSON.parse(fileSystem.readFileSync(file, 'utf8')) as unknown;
    } catch {
        // An unreadable config must not freeze the ruler; keep climbing.
        return undefined;
    }
    if (typeof document !== 'object' || document === null) {
        return undefined;
    }
    const record = document as Record<string, unknown>;
    return { root: record['root'] === true, limit: columnLimitOfDocument(record) };
}

// Nearest `.heimdall.json` above `startDir`, if any. Only its presence is
// reported; inheritance is resolved separately by `resolveColumnLimit`.
export function findHeimdallConfig(startDir: string, fileSystem: RulerFileSystem): string | undefined {
    let directory = path.resolve(startDir);
    for (let climbs = 0; climbs < 64; ++climbs) {
        const file = path.join(directory, '.heimdall.json');
        if (fileSystem.existsSync(file)) {
            return file;
        }
        const parent = path.dirname(directory);
        if (parent === directory) {
            break;
        }
        directory = parent;
    }
    return undefined;
}

// Effective `format.column-limit` for `startDir`, mirroring config inheritance:
// the nearest definition wins, climbing stops after a `"root": true` file.
export function resolveColumnLimit(startDir: string, fileSystem: RulerFileSystem): number | undefined {
    let directory = path.resolve(startDir);
    let limit: number | undefined;
    let defined = false;
    for (let climbs = 0; climbs < 64; ++climbs) {
        const file = path.join(directory, '.heimdall.json');
        if (fileSystem.existsSync(file)) {
            const entry = readChainEntry(file, fileSystem);
            if (entry !== undefined) {
                if (!defined && entry.limit !== undefined) {
                    limit = entry.limit;
                    defined = true;
                }
                if (entry.root) {
                    break;
                }
            }
        }
        const parent = path.dirname(directory);
        if (parent === directory) {
            break;
        }
        directory = parent;
    }
    return defined ? limit : undefined;
}

// Rulers for a resolved limit: undefined keeps the packaged default, zero
// (wrapping disabled) clears the guide, otherwise a single vertical line.
export function rulersForColumnLimit(limit: number | undefined): number[] | undefined {
    if (limit === undefined) {
        return undefined;
    }
    return limit === 0 ? [] : [limit];
}

export function equalArrays(first: readonly number[] | undefined, second: readonly number[] | undefined): boolean {
    if (first === undefined || second === undefined) {
        return first === undefined && second === undefined;
    }
    return first.length === second.length && first.every((value, index) => value === second[index]);
}
