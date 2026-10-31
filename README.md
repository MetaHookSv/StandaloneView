# StandaloneView

MetaHookSV plugin that lets other plugins render additional 3D views ("standalone
views") through the engine's own `V_RenderView`, for example a model preview in a
menu. It does not depend on the Renderer plugin and works with or without it.

## How it works

The engine calls `V_RenderView` once per frame from `VGui_ViewportPaintBackground`,
including at the main menu, where it returns early because `cls.state` /
`cls.signon` are not active. StandaloneView hooks both functions. Inside the
paint-background call, before the main view, it:

1. forces `cls.state = ca_active` and `cls.signon = SIGNONS`;
2. installs an empty brush worldmodel into `cl.worldmodel` / `r_worldentity.model`
   unless the client was already active with both set, because `R_RenderView`
   rejects a NULL world. While a map loads, the engine sets `cl.worldmodel` and
   redraws the loading screen before `R_NewMap` sets `r_worldentity.model` and the
   renderer prepares the world, so a loading world is replaced as well. While the
   empty world is installed, `r_novis` is temporarily forced to `1` to prevent the
   client's studio visibility test from rejecting preview models;
3. calls the original `V_RenderView` once per active view: `OnBeforeRenderView`,
   the render pass, `OnAfterRenderView`;
4. restores the world, `r_novis` and the client state, then lets the main view render
   as usual. A loaded world's `r_novis` is left untouched.

During a view's pass the client export `V_CalcRefdef` calls only the view's
`CalcRefdef`, then forces `onlyClientDraw = 1` and `nextView = 0`; the engine
therefore draws neither the world nor its entities. `HUD_DrawNormalTriangles` /
`HUD_DrawTransparentTriangles` call only the view's triangle callbacks, where the
view draws its own content with TriAPI, OpenGL or the `IStandaloneViewDrawfuncs`
passed to the callback. The client's own implementations are not called for
standalone views.

Views render before the main view because each `V_RenderView` call resets the
sound listener, records the demo view info and writes the refdef back into client
state; the main view then restores all of them. Renderer's SvEngine path also
flushes its final buffer at the start of every `V_RenderView` call.

Sven Co-op's engine lets the client render its portals at the start of every
`V_RenderView`. With the Sven Co-op client, StandaloneView also hooks
`ClientPortalManager::RenderPortals` and `ClientPortalManager::ResetAll` and skips
both during a standalone pass: the portals belong to the main view, and at the main
menu the client has neither a map nor its OpenGL entry points, which it loads with
the first map. `HUD_VidInit` resets the portal manager again for every map.

## Interface

`include/Interface/IStandaloneView.h` declares `IStandaloneView`, implemented by
the consumer, `IStandaloneViewDrawfuncs`, passed to its triangle callbacks, and
`IStandaloneViewManager`, exposed as `StandaloneViewManager_API_003` from
`StandaloneView.dll`:

```cpp
auto module = GetModuleHandleA("StandaloneView.dll");
if (module)
{
    auto factory = Sys_GetFactory((HINTERFACEMODULE)module);
    auto manager = factory ? static_cast<IStandaloneViewManager*>(
        factory(STANDALONEVIEW_MANAGER_INTERFACE_VERSION, nullptr)) : nullptr;
    if (manager)
        manager->RegisterStandaloneView(&g_MyView);
}
```

API_003 added studio pose evaluation, private bone sources and per-draw lighting
directly to `IStandaloneViewDrawfuncs`, breaking the drawfuncs vtable; the earlier
`StandaloneViewManager_API_001` / `_API_002` had no consumers and are no longer
exported.

Consumers should treat the plugin as optional and unregister their views before
the view objects are destroyed. Usage rules:

- All callbacks run on the main thread, inside the engine's `V_RenderView`, while
  `cls.state` / `cls.signon` are forced active.
- `CalcRefdef` sets `vieworg`, `viewangles` and `viewport`. Leave the `cl_*`,
  `simorg`, `punchangle` and similar fields untouched: the engine writes them back
  into client state. The field of view is not part of the interface.
- The pass draws into the viewport of the framebuffer bound at the time, which is
  the engine back buffer or Renderer's final buffer. Copy the result out in
  `OnAfterRenderView`. StandaloneView does not clear it afterwards. With Renderer an
  `onlyClientDraw` pass is alpha-composited over what the framebuffer already holds,
  so clear your viewport after copying it if later views or the main menu must not
  see it.
- Views are skipped during demo playback, which replaces the refdef with the
  recorded view.
- HUD sprites (`SPR_Load`) are unavailable until the first map loads, so TriAPI
  sprite textures cannot be used at the main menu. With Renderer, TriAPI does not
  sample textures bound with `glBindTexture` either; draw there with your own
  OpenGL resources.
- `IStandaloneViewDrawfuncs` draws one entity with the installed renderer:
  `DrawBrushModel` and `DrawSpriteModel` call the engine's `R_DrawBrushModel` /
  `R_DrawSpriteModel`; `DrawStudioModel` calls the client's `StudioDrawPlayer` for
  player entities, with the state of player slot `index - 1`, and `StudioDrawModel`
  otherwise, with `STUDIO_RENDER` only, so no animation events fire. Each call sets
  `currententity` and `r_blend` like the engine's entity lists, `1` for
  `kRenderNormal` and `CL_FxBlend / 255` otherwise, skips the entity when that is
  `0`, and restores both afterwards. A sprite with `curstate.body` 1-4 is drawn at
  that attachment of entity `curstate.skin`. Glow sprites are not faded by
  visibility. Only use the pointer until the callback returns; calling it at any
  other time, or with an entity whose model is missing or of another type, is a
  fatal error.
- In first person Sven Co-op's client skips `StudioDrawPlayer` for the local player
  entity itself, unless a renderer such as Renderer replaces that path. Draw a copy
  of the entity to show the local player.
- Never call `UnregisterStandaloneView` from an `IStandaloneView` callback, for any
  view: StandaloneView reports it as a fatal error. Unregister, then destroy, outside
  StandaloneView callbacks, e.g. from the code that closes the owning UI. Views
  registered from a callback render from the next frame.
- List `StandaloneView.dll` before other plugins in `plugins.lst`. `LoadClient`
  runs in reverse list order, so the first plugin wraps the client exports last and
  its wrappers run first: other plugins' `V_CalcRefdef` then never change a
  standalone view's refdef.

### Studio previews

The studio functions of `IStandaloneViewDrawfuncs` take an optional bone source and,
for draws, an optional lighting. For example, inside a triangle callback:

```cpp
standaloneview::StudioLighting lighting;
pDrawfuncs->DrawStudioModel(&previewEntity, &animationSource, &lighting);
```

- `SetupStudioPose(entity, boneSource)` evaluates the pose with flags `0`: no
  meshes, visibility culling or animation events. It returns whether evaluation
  succeeded. The consumer can read the engine's `StudioGetBoneTransform` results
  immediately afterwards, until the next studio call.
- `DrawStudioModel(entity, boneSource, lighting)` renders with `STUDIO_RENDER`,
  never `STUDIO_EVENTS`, as described above.
- Pass `nullptr` for `boneSource` to use the entity's own animation. For a private
  `MOVETYPE_FOLLOW` entity, pass its animation source explicitly: it need not have
  a slot in the engine entity array. The source pose is evaluated again before
  every follower, because studio bone merging consumes the last saved bones.
  An invalid source model or failed source evaluation returns `false` from
  `SetupStudioPose` and skips `DrawStudioModel`.
- A non-null `StudioLighting` overrides ambient light, shade, color and the
  world-space light-ray direction for that draw only. The direction is normalized;
  a zero vector falls back to `{-1, 0, 0}`. `enabled = false` requests full ambient
  lighting. Pass `nullptr` to retain world lighting. If the lighting hook cannot
  be installed, the plugin warns and the model still renders with world lighting.

The lighting hook survives client interface recaptures but is removed when engine
hooks are uninstalled.

With Renderer, a standalone pass with an empty world additionally requires
Renderer to skip its water pass for `onlyClientDraws` views, and
`IStandaloneViewDrawfuncs` requires Renderer to route the engine's
`R_DrawBrushModel` / `R_DrawSpriteModel` to its own implementations.

## Installation

Copy `StandaloneView.dll` to `<mod>/metahook/plugins/` and the complete
`standaloneview` catalog directory to `<mod>/metahook/gamedata/standaloneview/`. Add
`StandaloneView.dll` as the first entry of `<mod>/metahook/configs/plugins.lst`.
MetaHook API 114 or newer is required (`QueryGameSymbolStructMember`). Sven client
gamedata must include `ClientPortalManager.m_bStudioRenderPlayerPending`; missing
or incompatible member records fail initialization without a hardcoded fallback.

The shipped catalog covers Half-Life builds 3248, 3266, 3329, 3647, 4554, 6153,
8684, 10210; Sven Co-op builds 8948, 10257; and Cry of Fear build 5936. Unknown
binaries or missing required symbols fail with a diagnostic.

## Build and test

Windows, Visual Studio 2022 with MSVC x86, CMake 3.21+, and Python 3.8+ are required.

```powershell
cmake -S . -B build -A Win32 -DSTANDALONEVIEW_BUILD_TESTS=ON -DCMAKE_INSTALL_PREFIX=install
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release
```

Repeat with `Debug` for the other configuration. `METAHOOK_SOURCE_PATH` selects a
local, read-only SDK tree; otherwise the SDK is fetched from its `main` branch.
`STANDALONEVIEW_SYNC_GAMEDATA` defaults ON and validates all 11 catalog snapshots
before building. With it OFF, a previously generated catalog must be supplied for
installation. Build artifacts and synchronized catalogs stay outside Git.

Standalone Visual Studio F5 deployment is optional:

```powershell
cmake -S . -B build/launch -A Win32 -DMETAHOOKSV_ENABLE_LAUNCH_GAME=ON
```

The shared LaunchGame module supports `METAHOOKSV_GAME_DIRECTORY`,
`METAHOOKSV_GAME_APPID`, and `METAHOOKSV_GAME_MOD`; deployment requires an existing
MetaHook installation. See the [aggregator documentation](https://github.com/MetaHookSv/MetaHookSv#debugging).

Regression tests compile the production manager, empty world and hook installation
code against the SDK. They model the engine's `V_RenderView` and cover render
order, forced and restored client state, the empty world, the paint-background
gate, demo playback, reentrancy, the unregister-during-a-pass contract, export routing,
entity draws through `IStandaloneViewDrawfuncs`, studio source/follower ordering
and flags in both triangle callbacks, pose failure and engine fallback,
per-draw lighting isolation, lighting-hook failure/retry/recapture/teardown,
`r_novis` restoration and installation rollback, including MetaHook committing
inline hooks only after `LoadEngine` / `LoadClient` return. The full build also
loads the resulting DLL to verify the API_003 factory export, the rejected retired
versions and the manager vtable across the DLL boundary. These tests do not replace in-game verification.

## References

- [MetaHookSv issue #942](https://github.com/MetaHookSv/MetaHookSv/issues/942)
- [Renderer pull request #18](https://github.com/MetaHookSv/Renderer/pull/18), the
  in-Renderer approach this plugin replaces

SDK components retain their own licensing terms.
