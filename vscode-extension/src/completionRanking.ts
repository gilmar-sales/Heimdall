interface RankedCompletion {
    label: string | { label: string };
    filterText?: string;
    sortText?: string;
}

export function preserveCompletionRanking<T extends RankedCompletion>(items: readonly T[], prefix: string): T[] | undefined {
    if (items.length === 0 || !items.every(item => /^heimdall:\d{10}$/.test(item.sortText ?? ''))) {
        return undefined;
    }
    return items.map(item => {
        const label = typeof item.label === 'string' ? item.label : item.label.label;
        return { ...item, filterText: prefix ? `${prefix}_${item.filterText ?? label}` : item.filterText };
    });
}
