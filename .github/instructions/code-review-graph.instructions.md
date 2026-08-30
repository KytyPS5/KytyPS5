---
applyTo: '**'
description: >-
  Use code-review-graph MCP tools for token-efficient
  codebase exploration and code review.
---

<!-- code-review-graph MCP tools -->
## MCP Tools: code-review-graph

**This project has a knowledge graph. Start with the code-review-graph
MCP tools to narrow scope, then read the source.** The graph is cheaper than scanning files and
gives you structural context (callers, dependents, test coverage) that file search cannot.

### When to use graph tools FIRST

- **Exploring code**: `semantic_search_nodes_tool` or `query_graph_tool`
- **Understanding impact**: `get_impact_radius_tool`
- **Code review**: `detect_changes_tool` + `get_review_context_tool`
- **Finding relationships**: `query_graph_tool` callers_of/callees_of
- **Architecture questions**: `get_architecture_overview_tool`

### Verify in the source

- Narrow scope with the graph, then read the source. Do not change code from graph output alone.
- For any non-trivial change, read the implementation and the relevant tests before concluding.
- Verify the exact source when touching behavior, database logic, migrations, retries, fallbacks,
  recovery, or compatibility code.
- When the graph and the source disagree, the source wins. The graph may be stale or may not
  model that relationship.
- An empty graph result can mean "not indexed" or "not statically visible", not "does not exist".

### Key Tools

| Tool | Use when |
| ------ | ---------- |
| `detect_changes_tool` | Risk-scored change analysis |
| `get_review_context_tool` | Token-efficient source snippets |
| `get_impact_radius_tool` | Blast radius of a change |
| `get_affected_flows_tool` | Impacted execution paths |
| `query_graph_tool` | Trace callers, callees, imports, tests |
| `semantic_search_nodes_tool` | Find functions/classes by keyword |
| `get_architecture_overview_tool` | High-level structure |
| `refactor_tool` | Rename planning, dead code |

### Workflow

1. The graph auto-updates on file changes (via hooks).
2. Use `detect_changes_tool` for code review.
3. Use `get_affected_flows_tool` to understand impact.
4. Use `query_graph_tool` pattern="tests_for" to check coverage.
<!-- /code-review-graph MCP tools -->
