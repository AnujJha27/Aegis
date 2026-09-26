# Aegis agent instructions

## Approval flow

- When the user explicitly approves a proposed design or says to implement it, proceed through the plan and code without asking for redundant spec, plan, or execution-method approval.
- Treat specs and plans as implementation artifacts after that approval, not new gates.
- Ask again only when scope materially changes, a destructive action is required, credentials are needed, or a security-sensitive decision is unresolved.

## Working style

- Keep changes modular and commit coherent milestones regularly.
- Prefer one focused integration check over a pile of helper tests.
- Preserve the existing local-first architecture and avoid speculative abstractions.
