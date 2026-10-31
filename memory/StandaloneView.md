---
title: StandaloneView
type: note
permalink: standaloneview/standalone-view
tags:
- standaloneview
- v_renderview
- gamedata
- architecture
---

# StandaloneView

Plugin from [MetaHookSv issue #942](https://github.com/MetaHookSv/MetaHookSv/issues/942):
other plugins render extra 3D views through the engine's own `V_RenderView`, without
depending on Renderer. It replaces the in-Renderer approach of Renderer PR #18.

## Source entry points

- `include/Interface/IStandaloneView.h`: `IStandaloneView` (consumer callbacks),
  `IStandaloneViewDrawfuncs` (entity draws, passed to the triangle callbacks) and
  `IStandaloneViewManager` (`StandaloneViewManager_API_003`). The issue named both
  classes `IStandaloneView`; the manager was renamed. API_001 (triangle callbacks
  without parameters) was never released; API_002 (drawfuncs without bone sources,
  lighting or `SetupStudioPose`) had no consumers. Neither is exported any more.
- `src/StandaloneViewManager.cpp`: registry, render loop, `V_CalcRefdef` and triangle
  routing, behind `standaloneview::EngineBridge` / `ClientBridge` for tests.
- `src/EmptyWorld.cpp`: `EmptyWorldScope`, the empty worldmodel for frames without a map.
- `src/privatehook.cpp`: gamedata resolution, inline hooks (engine, and the Sven client's
  portal manager), client export replacement, and the engine-side entity draws behind
  `IStandaloneViewDrawfuncs` (`EntityDrawScope`, `DrawStudioModel`).
- `src/plugins.cpp`: `IPluginsV4` lifecycle and the interface export.

## Render flow

`VGui_ViewportPaintBackground` hook counts its depth and calls the original, which
calls `V_RenderView`. The `V_RenderView` hook renders views only inside that call,
when not already rendering views, with at least one view registered, and outside
demo playback; then it always calls the original for the main view.

Per frame: `ClientStateScope` forces `cls.state = 5`, `cls.signon = 2`;
`EmptyWorldScope` installs the empty world unless the client was active (checked before
forcing) with both `cl.worldmodel` and `r_worldentity.model` set, and restores both
pointers afterwards; each registered
active view gets `OnBeforeRenderView`, the original `V_RenderView`,
`OnAfterRenderView`. During the pass `V_CalcRefdef` calls only the view's
`CalcRefdef` and forces `onlyClientDraw = 1`, `nextView = 0`; the triangle exports
call only the view's callbacks.

## Decisions

- **Views before the main view.** Each `V_RenderView` zeroes `r_soundOrigin`
  (restored only by passes without `onlyClientDraw`), calls `CL_SetDemoViewInfo`
  when recording, and `V_GetRefParams` writes `cl.viewangles` / `simorg` /
  `punchangle` back. The main view, last, restores them. Renderer's
  `R_RenderView_SvEngine` flushes its final buffer when `viewIdx == 0`, i.e. at
  the start of every `V_RenderView`; rendering views first means that flush
  removes view leftovers instead of the main view.
- **Paint-background gate.** BulletPhysics calls `V_RenderView` from `HUD_Frame`
  to refresh the refdef while its `R_RenderView` hook suppresses drawing; without
  the gate the views would run a wasted pass there.
- **Client `V_CalcRefdef` skipped for views.** Its bob, punch decay, pitch drift and
  step smoothing advance per call.
- **Demo playback skipped.** `CL_GetDemoViewInfo` overwrites the refdef after
  `ClientDLL_CalcRefdef`. `gEngfuncs.pDemoAPI->IsPlayingback()` is the check;
  `cls.demoplayback` is not published.
- **No clearing between views (user decision, 2026-10-21).** The issue's
  `R_Clear` call was dropped: svencoop-10257 and hl-10210 inline it into
  `R_RenderView`, so there is no function to publish. Each `R_RenderView` still
  clears depth (and color with `gl_clear`) at its start; consumers clear their
  viewport themselves.
- **Triangle callbacks (user decision).** With `onlyClientDraws`, neither the engine
  nor Renderer draws the world, `cl_visedicts` or the trans list; only
  `ClientDLL_DrawNormalTriangles` / `DrawTransparentTriangles` run, so the interface
  carries `DrawNormalTriangles` / `DrawTransparentTriangles`.
- **Empty world (user decision).** A static `model_t` (`mod_brush`) whose `nodes`
  and `leafs` point to one `CONTENTS_EMPTY` leaf with `numleafs = 0`:
  `Mod_PointInLeaf` returns at the root, `Mod_LeafPVS` returns novis for
  `leafs[0]`, and `R_MarkLeaves` marks nothing. Restored right after the views.
- **No unregistering from callbacks (user decision, 2026-10-22).** While views
  render (`IsRenderingStandaloneViews()`), `UnregisterStandaloneView` calls the
  injected `SysError` and keeps the view. The render loop iterates `m_Views` by
  index up to the size at the start of the pass, so registering from a callback is
  allowed and takes effect next frame; the routed callbacks need no
  "still registered" checks. Earlier code copied the list and re-checked
  registration before every callback.
- **Entity draws (user decision, 2026-10-22).** `IStandaloneViewDrawfuncs` is passed
  to `DrawNormalTriangles` / `DrawTransparentTriangles` and valid only during them
  (`m_bDrawingViewTriangles`); a call outside them, or with a NULL entity, NULL model
  or another model type, calls the injected `SysError`. `DrawBrushModel` /
  `DrawSpriteModel` call the engine's `R_DrawBrushModel` / `R_DrawSpriteModel`.
  The engine has no `R_DrawStudioModel` (studio entities go through `pStudioAPI`), so
  `DrawStudioModel` follows the engine's entity lists (user-supplied shape):
  `StudioDrawPlayer(flags, GetPlayerState(index - 1))` for players, otherwise
  `StudioDrawModel(flags)`. `pStudioAPI` and `engine_studio_api_t` are captured by
  wrapping the client's `HUD_GetStudioModelInterface` (the engine keeps its default
  interface when the client returns 0, and the captured `**ppinterface` sees that).
  `flags = STUDIO_RENDER` without `STUDIO_EVENTS`: a preview must not fire sounds,
  muzzle flashes or attachment writes. Each draw sets `currententity` and `r_blend`
  (1 for `kRenderNormal`, `CL_FxBlend / 255` otherwise, skip at 0) and restores them;
  sprites also get `r_entorigin` from attachment `body` of entity `skin`
  (`R_GetAttachmentPoint` is unpublished). Glow sprites are not faded
  (`GlowBlend` is inlined in Sven).
- **Load order.** `LoadClient` runs in reverse `plugins.lst` order; listing
  StandaloneView first makes its export wrappers outermost.
- **API_003 breaks API_002 (user decision, 2026-10-27).** The plugin had no
  consumers, so the studio preview functions were folded into
  `IStandaloneViewDrawfuncs` instead of a derived `IStandaloneViewDrawfuncsV003`:
  `DrawStudioModel(ent, boneSource, lighting)` replaces both the old
  `DrawStudioModel(ent)` and PR #3's `DrawStudioModelEx` (they used the same
  `STUDIO_RENDER` flags), and `SetupStudioPose` is appended. Only API_003 is exported.
- **Studio previews.** `SetupStudioPose` evaluates with flags 0;
  `DrawStudioModel` renders without events. A private `MOVETYPE_FOLLOW` source
  is re-evaluated before each follower because merging consumes the last saved
  bones. The manager scopes per-draw `StudioLighting`; the host hooks the
  engine's `StudioSetupLighting` function, not a table entry cached by the client.
  Failed lighting-hook installation warns and retains world lighting. Retain the
  hook across client recaptures, replace it when the engine target changes and
  remove/reset it in `UninstallEngineHooks`.
- **Empty-world studio visibility.** Force `r_novis.value` to 1 only while the
  empty world is installed and restore the previous value before the main view;
  do not change it for loaded-world standalone passes.
- **Loading world replaced too.** `CL_RegisterResources` sets `cl.worldmodel`, then
  `ContinueLoadingProgressBar` redraws a frame, and only then `R_NewMap` sets
  `r_worldentity.model` (HLND2T `cl_parse.c`, `gl_rmisc.c`). A pass in that window hit
  Renderer's `R_RenderView: NULL worldmodel` Sys_Error; any frame of a non-active client
  now uses the empty world.
- **Sven client portals skipped during a pass.** SvEngine's `V_RenderView` calls the
  client portal manager (engine global, vtable +0xC) before `ClientDLL_CalcRefdef`.
  The client's entry lazily creates the manager, calls `ClientPortalManager_ResetAll`
  on creation, then `ClientPortalManager_RenderPortals`. StandaloneView hooks both
  (client gamedata, svencoop-10257/8948 only, gated on `SCClientDLL001`) and returns
  early while a view is current. Main-menu crashes without the hooks: `RenderPortals`
  dereferences map state; `ResetAll` calls glew pointers the client initializes only on
  map load (call to address 0 without Renderer; Renderer hooks `ResetAll` itself).
  The skipped `ResetAll` must still clear the one-shot studio/first-person byte
  `ClientPortalManager.m_bStudioRenderPlayerPending`: the constructor leaves its
  initialization to `ResetAll`, and Studio consumes it even at the menu. A garbage
  true value takes a path that dereferences the absent local player. Only this
  byte is initialized; do not run the native GL cleanup or zero the constructed
  manager's containers.
  `HUD_VidInit` calls the full native `ResetAll` unconditionally per map.
  Query the member offset from the matched client module with
  `QueryGameSymbolStructMember` (MetaHook API 114); do not resolve it as an
  address or fall back to a fixed layout. Query it before installing either
  portal hook, fail closed with module/build/CRC diagnostics on errors, and
  clear the cached offset on client teardown so recapture queries it again.

## Lessons

- **MetaHook defers inline hooks requested in `LoadEngine` / `LoadClient`.** The
  trampoline (`*pOrginalCall`) is written at `MH_TransactionHookCommit`, after all
  plugins returned. Copying it into a bridge at install time stored NULL: the view pass
  and main view never rendered (black screen). Read the trampoline at call time
  (`CallOriginalRenderView`, portal hooks); `hooks_test` defers its fake commit to
  catch this.
- **HUD sprites need the first `HUD_VidInit`.** `SPR_Load` returns 0 until
  `ClientDLL_HudVidInit` runs `SPR_Init`; TriAPI sprite textures are unavailable at the
  main menu. Under Renderer, TriAPI ignores textures bound with `glBindTexture`.

## Engine facts (IDA, 2026-10-21)

- Sven client 10257: portal vtable slot +0xC → `sub_10011020` → `sub_1004AD40`
  (creates the manager in `dword_1063C800` via `sub_1004D9B0`, which calls
  `ResetAll` at 0x1004dcbb, then `RenderPortals` at RVA 0x4e4f0); `HUD_VidInit` →
  `sub_1004ACC0` → `ResetAll`. The engine-side portal pointer is not published.

- svencoop-10257 `V_RenderView` (RVA 0xcdf60): `cmp cls.state, 5`, `cmp cls.signon, 2`;
  its only caller is `VGui_ViewportPaintBackground(int*)`; it calls `R_RenderView(viewnum)`.
- hl-10210 `V_RenderView`: additionally gates on `con_forcedup`, which is only read
  (`Key_Event`, `V_RenderView`) and never written in that build. The HLND2T GL source
  likewise never assigns it.
- Both builds inline `R_Clear` into `R_RenderView`.
- Catalog (11 Windows identities) publishes `V_RenderView`,
  `VGui_ViewportPaintBackground`, `cls.state`, `cls.signon`, `cl_worldmodel`,
  `r_worldentity`, `R_DrawBrushModel`, `R_DrawSpriteModel`, `CL_FxBlend`,
  `currententity`, `r_blend`, `r_entorigin`; not `R_Clear`, `ClientDLL_CalcRefdef`,
  `cls.demoplayback`, `R_GetAttachmentPoint`.
- Sven client 10257 `CGameStudioRenderer::StudioDrawPlayer` returns 0 in first person
  when `currententity == GetLocalPlayer()`, the local player is the view entity and no
  portal renders. Without Renderer a standalone view therefore has to draw a copy of
  the local player entity; Renderer replaces that path and draws it.

## Renderer interaction

- Renderer does not hook `V_RenderView`; its `R_RenderView` hook runs inside each pass.
- `onlyClientDraws` passes skip world, entities, particles, view model, fog,
  post-processing and deferred lighting, and are alpha-composited into
  `s_FinalBufferFBO`, still bound after the pass.
- `R_RenderWaterPass` does not check `onlyClientDraws`: with the empty world
  `R_GetWorldSurfaceModel` returns NULL (the model is not in `mod_known`) and is
  dereferenced; with a real world water references accumulate per view. Renderer
  needs an early return for `onlyClientDraws` there (Renderer branch
  `fix/onlyclientdraws-water-pass`).
- Renderer hooks `ClientPortalManager_RenderPortals` and `ResetAll` itself; the
  StandaloneView hooks chain with them.
- Renderer reimplements `R_DrawBrushModel` / `R_DrawSpriteModel` but only replaced
  their engine callers, so `IStandaloneViewDrawfuncs` reached the engine's GL code.
  Renderer branch `feat/hook-entity-draws` inline-hooks both engine functions to its
  own implementations. Studio draws already go through Renderer's
  `StudioDrawPlayer` / `StudioDrawModel` hooks.

## Verification

- 2026-10-27: interface merge (API_002 retired, no V003 subclass): Win32 Debug and
  Release builds and all three CTest suites passed in both configurations;
  `standaloneview_interface` checks the API_003 export and rejects API_001/API_002.
  clang-format 23.1.3 checked all 11 owned files. Not verified in game.
- 2026-10-26: API_003 follow-up: full MSVC Win32 Debug/Release builds and all
  three CTest suites passed. `standaloneview_interface` loads the built DLL and
  checks API_002/API_003 singleton exports, rejected versions and manager calls
  across the DLL boundary. Hook tests cover both triangle callbacks, per-follower
  bone-source evaluation, pose failure, zero blend, player slots, engine fallback,
  lighting isolation, hook failure/retry, client recapture, target replacement
  and engine teardown/reinstall. Manager tests cover fractional `r_novis`
  restoration and an unavailable cvar. Both installed payloads validated against
  all 11 gamedata snapshots; clang-format 23.1.3 checked all 11 owned files.
  Mutations removing source evaluation, lighting override, lighting-hook teardown
  or `r_novis` restoration each failed the regression tests. API_003 has not yet
  been verified in game.
- 2026-10-21: standalone Debug/Release build with gamedata sync and validation
  (11 snapshots), CTest `standaloneview_manager` / `standaloneview_hooks` passed
  in both configurations, `format-check` clean (10 files). A mutation that renders
  views after the main view made the manager test fail.
- 2026-10-21: portal hooks and the loading-world fix covered by `hooks_test` /
  `manager_test`; mutations (no portal skip, no `ResetAll` skip, old NULL-only world
  check) each made the tests fail.
- 2026-10-21 in game, Sven Co-op 10257 with a scratch consumer (256x256 view, red
  TriAPI quad at the center, green `glScissor` clear at a corner, `glReadPixels` in
  `OnAfterRenderView`), callbacks counted once per frame each:
  - without Renderer: main menu (GL-texture quad) red; `map stadium4` red; `disconnect`
    back to the menu without crash;
  - with Renderer (+ water fix): main menu corner green (pass composited; untextured
    TriAPI draws nothing there); in map center red, corner green, main view intact
    with no view leftovers; map load, `disconnect`, reload without crash.
- 2026-10-22: `IStandaloneViewDrawfuncs` covered by `manager_test` (dispatch from both
  callbacks, rejection outside them, wrong type, NULL model) and `hooks_test`
  (`currententity` / `r_blend` / `r_entorigin` set and restored, attachment origin,
  `STUDIO_RENDER` flags, player-state slot); mutations of the guard and the restore
  were caught.
- 2026-10-22 in game, Sven Co-op 10257, `map stadium4`, Debug builds, scratch consumer
  drawing the local player, `models/w_medkit.mdl`, `sprites/flare1.spr` and brush
  submodel `*13` (12x12x12) from `DrawNormalTriangles`:
  - with Renderer (+ `feat/hook-entity-draws`): all four visible;
  - without Renderer: medkit, sprite and brush visible; the local player entity itself
    is skipped by the Sven client (see engine facts).
  The first probe picked submodel `*5` (2080x2080x768) and placed the camera inside it,
  so nothing was visible; pick small submodels for such probes.
- Not verified: Half-Life 25th anniversary build, BulletPhysics third person, demo
  playback, Release build in game, `IStandaloneViewDrawfuncs` from
  `DrawTransparentTriangles` and transparent render modes in game.
