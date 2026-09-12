// A one-shot hint for which Settings category to land on, set by whatever
// triggered navigation to Settings (e.g. TopBar's profile chip) and consumed
// once by SettingsView on mount. Deliberately not a reactive store -- nothing
// needs to re-render when this changes, it's read exactly once per
// navigation. Falls back to Settings' own default ("background") whenever
// nothing requested a specific landing category.
let pendingCategory: string | null = null;

export function requestSettingsCategory(category: string): void {
  pendingCategory = category;
}

export function consumePendingSettingsCategory(): string | null {
  const c = pendingCategory;
  pendingCategory = null;
  return c;
}
