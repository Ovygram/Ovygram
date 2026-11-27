# Working rules for Ovygram

- Mandatory UI rule: every Ovygram-owned dialog and window uses Telegram's
  native UI components and theme palette, including plugin installation,
  warnings and confirmations. Do not introduce unstyled Qt/OS message boxes.
  Plugin descriptions must remain complete, wrapping and scrolling as needed.
- Mandatory localization rule: every user-facing string added or changed in
  Ovygram (C++ UI, Python runtime, and plugin UI/examples) must always include
  both Russian and English translations (with English as the fallback for any
  other language), regardless of the language of the user's request. Use
  `Ovygram::Tr` / `Ovygram::TrCount` / `Ovygram::I18n::TrValue` in C++,
  `ovygram_i18n.tr` in the Python runtime, and `desktop.i18n` / `self.tr(...)`
  (with English fallback) in plugins. Verify changed UI in the offline preview
  with both `--lang ru` and `--lang en`.
- The authorized scope includes both `/workspace/ovygram-info/plugin-distribution.md`
  and the complete SDK plan in `/workspace/ovygram-info/README.md`.
- Windows builds run only through GitHub Actions and only after the owner
  explicitly commands a build. Do not run local Windows CMake/compiler/native
  smoke builds, or dispatch a workflow merely because a change is ready.
- Linux builds do not require GitHub Actions. Do not add a Linux Actions build
  workflow as a plan requirement. The owner authorizes the standalone Linux UI
  preview and its dependencies in Codex Cloud for UI work. Full Linux client
  builds require a separate explicit owner request. Python tests and source
  checks may run normally.
- For every task that creates or changes a window, build and open the relevant
  scenario in the [offline UI preview](docs/ui-preview.md). Exercise the
  changed controls, inspect the rendered result, and include screenshots in
  the final answer. Add a preview scenario when introducing a new window.
  For plugin UI, use a temporary profile and sample data through the isolated
  SDK host. Keep the native theme roles, including one primary action per
  dialog and scrollable content only when it exceeds the available height.
  Use the [reusable UI prompt](docs/ui-design-prompt.txt) for new UI briefs.
- Official packages require the exact literal author `Ovygram`. Case variants
  remain reserved but cannot receive `official` without a corrected file.
- Settings/data/handlers belong to installation_id. A separate plugin permission
  model is not part of the current scope; do not introduce permission grants/UI.
- Plugin code runs only in the plugin host process (`ovygram_host.py`), never
  in the client: the manager in `ovygram_runtime.py` must not import plugin
  code or libraries. On Linux the host starts only inside the sandbox of
  `ovygram_sandbox.py`. Application-owned windows/tabs are registered with
  `Ovygram::ProtectSurface`. The design and what is still open are in
  `docs/protected-ui-architecture.md`; the Windows restrictions in
  `ovygram_sandbox_windows.py` stay unverified until they have run on Windows.
- Registry/index URLs will be supplied at deployment. Keep them configurable;
  their absence is not an implementation blocker. Pin the generated public key
  in the client. Never commit the private signing key or put it in client files.
- Keep upstream and submodules pinned to the selected official release.
  The owner requested the update to Telegram Desktop v7.4.3 on 2026-11-13;
  preserve existing Ovygram changes while merging that release.
- Do not report full plan completion from unit-test counts. Track source
  implementation, executed tests, and pending GitHub/manual validation separately.
