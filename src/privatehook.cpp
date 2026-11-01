#include <metahook.h>
#include <cl_entity.h>
#include <com_model.h>
#include <const.h>
#include <demo_api.h>
#include <r_studioint.h>
#include <ref_params.h>
#include <studio.h>
#include <cstring>

#include "StandaloneViewManager.h"
#include "privatehook.h"

static_assert(METAHOOK_API_VERSION >= standaloneview::RequiredMetaHookAPIVersion,
              "StandaloneView requires struct-member gamedata (MetaHook API 114)");

extern cl_enginefunc_t gEngfuncs;

namespace standaloneview
{
namespace
{
using PaintBackgroundFn = void (*)(int* extents);
// ClientPortalManager::RenderPortals and ResetAll are thiscall.
using RenderPortalsFn = void(__fastcall*)(void* pthis, int, ref_params_s* pparams);
using ResetAllFn      = int(__fastcall*)(void* pthis, int);

// Engine functions and globals the entity draws use, resolved with the engine hooks.
struct EngineDraw
{
    void (*R_DrawBrushModel)(cl_entity_t* ent)  = nullptr;
    void (*R_DrawSpriteModel)(cl_entity_t* ent) = nullptr;
    int (*CL_FxBlend)(cl_entity_t* ent)         = nullptr;
    cl_entity_t** currententity                 = nullptr;
    float*        r_blend                       = nullptr;
    float*        r_entorigin                   = nullptr; // vec3_t
};

constexpr float MaxFxBlend        = 255.0f;
constexpr int   EntityAttachments = 4;
constexpr int   VectorComponents  = 3;

EngineDraw engineDraw;
// The engine's pStudioAPI and engine studio API, as passed to HUD_GetStudioModelInterface.
r_studio_interface_t**    studioInterface                 = nullptr;
engine_studio_api_t*      engineStudio                    = nullptr;
HUD_STUDIO_INTERFACE_FUNC originalGetStudioModelInterface = nullptr;

hook_t* renderViewHook                    = nullptr;
hook_t* paintBackgroundHook               = nullptr;
hook_t* renderPortalsHook                 = nullptr;
hook_t* resetAllHook                      = nullptr;
void (*originalRenderView)()              = nullptr;
PaintBackgroundFn originalPaintBackground = nullptr;
RenderPortalsFn   originalRenderPortals   = nullptr;
ResetAllFn        originalResetAll        = nullptr;
uint32_t          portalStudioFlagOffset  = 0;

struct GameModule
{
    const char* name;
    PVOID       base;
};

GameModule EngineModule() { return {"engine", g_pMetaHookAPI->GetEngineBase()}; }
GameModule ClientModule() { return {"client", g_pMetaHookAPI->GetClientBase()}; }

void ReportFailure(const GameModule& module, const char* symbol, const char* reason)
{
    uint64_t   crc64  = 0;
    const auto status = g_pMetaHookAPI->GetModuleCRC64(module.base, &crc64);
    if (status == MH_GAMESYMBOL_OK)
        g_pMetaHookAPI->SysError("[StandaloneView] %s (module %s)\nEngine buildnum: %d\nCRC64: %016llx\nReason: %s",
                                 symbol, module.name, g_pMetaHookAPI->GetEngineBuildnum(), static_cast<unsigned long long>(crc64), reason);
    else
        g_pMetaHookAPI->SysError("[StandaloneView] %s (module %s)\nEngine buildnum: %d\nReason: %s\nCRC64 unavailable: %s",
                                 symbol, module.name, g_pMetaHookAPI->GetEngineBuildnum(), reason, g_pMetaHookAPI->GetGameSymbolStatusString(status));
}

template <typename T>
bool Resolve(const GameModule& module, const char* symbol, mh_gamesymbol_kind_t kind, T** address)
{
    void*      resolved = nullptr;
    const auto status   = g_pMetaHookAPI->ResolveGameSymbol(module.base, symbol, kind, &resolved);
    if (status == MH_GAMESYMBOL_OK)
    {
        *address = reinterpret_cast<T*>(resolved);
        return true;
    }
    ReportFailure(module, symbol, g_pMetaHookAPI->GetGameSymbolStatusString(status));
    return false;
}

bool QueryStructMember(const GameModule& module, const char* symbol, uint32_t* offset)
{
    if (!g_pMetaHookAPI->QueryGameSymbolStructMember)
    {
        ReportFailure(module, symbol, "QueryGameSymbolStructMember unavailable");
        return false;
    }
    const auto status = g_pMetaHookAPI->QueryGameSymbolStructMember(module.base, symbol, offset);
    if (status == MH_GAMESYMBOL_OK)
        return true;
    ReportFailure(module, symbol, g_pMetaHookAPI->GetGameSymbolStatusString(status));
    return false;
}

bool IsDemoPlayback()
{
    return gEngfuncs.pDemoAPI && gEngfuncs.pDemoAPI->IsPlayingback() != 0;
}

// Hooks requested inside LoadEngine are committed after every plugin's LoadEngine
// returned; only then does MetaHook write the trampoline, so read it per call.
void __cdecl CallOriginalRenderView()
{
    originalRenderView();
}

void __cdecl RenderViewHook()
{
    StandaloneViewManager().RenderView();
}

void __cdecl PaintBackgroundHook(int* extents)
{
    StandaloneViewManager().PaintBackground(originalPaintBackground, extents);
}

// SvEngine's V_RenderView lets the Sven client render its portals before every pass.
// A standalone pass must not re-render them, and without a map the client's portal
// state is invalid.
void __fastcall RenderPortalsHook(void* pthis, int, ref_params_s* pparams)
{
    if (StandaloneViewManager().GetCurrentView())
        return;
    originalRenderPortals(pthis, 0, pparams);
}

// The client creates its portal manager on first use and resets it then. A menu
// preview may be that first use, before the client's GL entry points are ready.
// Preserve ResetAll's CPU-side studio flag initialization without calling its GL
// cleanup. The member offset comes from this client's gamedata, never a fallback
// layout. HUD_VidInit still performs the full native reset for every map.
int __fastcall ResetAllHook(void* pthis, int)
{
    if (StandaloneViewManager().GetCurrentView())
    {
        static_cast<unsigned char*>(pthis)[portalStudioFlagOffset] = 0;
        return 0;
    }
    return originalResetAll(pthis, 0);
}

void UninstallPortalHooks()
{
    if (resetAllHook)
    {
        g_pMetaHookAPI->UnHook(resetAllHook);
        resetAllHook = nullptr;
    }
    if (renderPortalsHook)
    {
        g_pMetaHookAPI->UnHook(renderPortalsHook);
        renderPortalsHook = nullptr;
    }
    originalResetAll       = nullptr;
    originalRenderPortals  = nullptr;
    portalStudioFlagOffset = 0;
}

bool InstallPortalHooks()
{
    const auto module               = ClientModule();
    void*      renderPortalsAddress = nullptr;
    void*      resetAllAddress      = nullptr;
    uint32_t   studioFlagOffset     = 0;
    if (!Resolve(module, "ClientPortalManager_RenderPortals", MH_GAMESYMBOL_KIND_FUNCTION, &renderPortalsAddress) ||
        !Resolve(module, "ClientPortalManager_ResetAll", MH_GAMESYMBOL_KIND_FUNCTION, &resetAllAddress) ||
        !QueryStructMember(module, "ClientPortalManager.m_bStudioRenderPlayerPending", &studioFlagOffset))
        return false;

    renderPortalsHook = g_pMetaHookAPI->InlineHook(renderPortalsAddress, reinterpret_cast<void*>(RenderPortalsHook),
                                                   reinterpret_cast<void**>(&originalRenderPortals));
    if (!renderPortalsHook)
    {
        ReportFailure(module, "ClientPortalManager_RenderPortals", "InlineHook failed");
        return false;
    }
    resetAllHook = g_pMetaHookAPI->InlineHook(resetAllAddress, reinterpret_cast<void*>(ResetAllHook),
                                              reinterpret_cast<void**>(&originalResetAll));
    if (!resetAllHook)
    {
        UninstallPortalHooks();
        ReportFailure(module, "ClientPortalManager_ResetAll", "InlineHook failed");
        return false;
    }
    portalStudioFlagOffset = studioFlagOffset;
    return true;
}

bool IsSvenCoopClient()
{
    const auto clientFactory = g_pMetaHookAPI->GetClientFactory();
    return clientFactory && clientFactory("SCClientDLL001", nullptr);
}

// Sets currententity and r_blend the way the engine's entity lists do, then restores them.
class EntityDrawScope
{
public:
    explicit EntityDrawScope(cl_entity_t* ent) :
        m_pSavedEntity(*engineDraw.currententity),
        m_flSavedBlend(*engineDraw.r_blend)
    {
        *engineDraw.currententity = ent;
        *engineDraw.r_blend       = ent->curstate.rendermode == kRenderNormal ? 1.0f : engineDraw.CL_FxBlend(ent) / MaxFxBlend;
    }

    ~EntityDrawScope()
    {
        *engineDraw.currententity = m_pSavedEntity;
        *engineDraw.r_blend       = m_flSavedBlend;
    }

    EntityDrawScope(const EntityDrawScope&)            = delete;
    EntityDrawScope& operator=(const EntityDrawScope&) = delete;

    bool IsVisible() const { return *engineDraw.r_blend > 0; }

private:
    cl_entity_t* m_pSavedEntity;
    float        m_flSavedBlend;
};

void DrawBrushModel(cl_entity_s* ent)
{
    EntityDrawScope scope(ent);
    if (scope.IsVisible())
        engineDraw.R_DrawBrushModel(ent);
}

// A sprite with curstate.body follows attachment body of entity curstate.skin
// (R_GetAttachmentPoint, not published as a symbol).
const float* SpriteOrigin(cl_entity_t* ent)
{
    const int attachment = ent->curstate.body;
    if (attachment <= 0 || attachment > EntityAttachments)
        return ent->origin;

    const auto owner = gEngfuncs.GetEntityByIndex(ent->curstate.skin);
    return owner ? owner->attachment[attachment - 1] : ent->origin;
}

void DrawSpriteModel(cl_entity_s* ent)
{
    EntityDrawScope scope(ent);
    if (!scope.IsVisible())
        return;

    float savedOrigin[VectorComponents];
    memcpy(savedOrigin, engineDraw.r_entorigin, sizeof(savedOrigin));
    memcpy(engineDraw.r_entorigin, SpriteOrigin(ent), sizeof(savedOrigin));
    engineDraw.R_DrawSpriteModel(ent);
    memcpy(engineDraw.r_entorigin, savedOrigin, sizeof(savedOrigin));
}

// The studio passes: an optional private MOVETYPE_FOLLOW bone source is evaluated with
// flags 0 immediately before the entity (StudioMergeBones consumes the LAST StudioSaveBones
// result, not aiment, and each follower replaces that cache; flags 0 also bypasses bbox
// culling for animation-only source models). The draw itself uses STUDIO_RENDER without
// STUDIO_EVENTS: a preview must not fire model events. The per-draw lighting does not
// appear here: it rides the manager's active StudioLighting and the host's
// StudioSetupLighting hook.
bool StudioModelPass(cl_entity_s* ent, cl_entity_s* boneSource, int flags)
{
    if (!studioInterface || !*studioInterface || !(*studioInterface)->StudioDrawModel)
        return false;
    if (!ent || !ent->model || ent->model->type != mod_studio)
        return false;

    if (boneSource)
    {
        if (!boneSource->model || boneSource->model->type != mod_studio)
            return false;
        EntityDrawScope source(boneSource);
        if (!(*studioInterface)->StudioDrawModel(0))
            return false;
    }

    EntityDrawScope scope(ent);
    if ((flags & STUDIO_RENDER) && !scope.IsVisible())
        return false;

    const auto studio = *studioInterface;
    if (!ent->player)
        return studio->StudioDrawModel(flags) != 0;

    const int playerIndex = ent->index - 1;
    if (!engineStudio || playerIndex < 0 || playerIndex >= gEngfuncs.GetMaxClients())
        return false;
    const auto state = engineStudio->GetPlayerState(playerIndex);
    return state && studio->StudioDrawPlayer(flags, state) != 0;
}

bool SetupStudioPose(cl_entity_s* ent, cl_entity_s* boneSource)
{
    return StudioModelPass(ent, boneSource, 0);
}

void DrawStudioModel(cl_entity_s* ent, cl_entity_s* boneSource)
{
    StudioModelPass(ent, boneSource, STUDIO_RENDER);
}

// The engine's StudioSetupLighting and its trampoline. While a studio draw with a
// StudioLighting is in flight, that lighting is folded into the engine's lighting result
// here. The client caches a copy of the studio API table, so the API function itself is
// hooked rather than a table entry the client no longer reads. The function belongs to the
// engine, not the reloadable client DLL; the hook is retained across recaptures.
struct StudioLightBridge
{
    hook_t* hook                = nullptr;
    void (*target)(alight_t*)   = nullptr;
    void (*original)(alight_t*) = nullptr;
};

StudioLightBridge studioLightBridge;

void UninstallStudioLighting()
{
    if (studioLightBridge.hook)
        g_pMetaHookAPI->UnHook(studioLightBridge.hook);
    studioLightBridge = {};
}

void StudioSetupLightingHook(alight_t* light)
{
    const auto* lighting = StandaloneViewManager().CurrentStudioLighting();
    if (!light || !lighting || !studioLightBridge.original)
    {
        if (studioLightBridge.original)
            studioLightBridge.original(light);
        return;
    }

    alight_t preview = *light;
    float    direction[3];
    StandaloneViewManager().AdjustStudioLighting(&preview, direction, *lighting);
    studioLightBridge.original(&preview);
}

void InstallStudioLighting()
{
    if (!engineStudio || !engineStudio->StudioSetupLighting || studioLightBridge.target == engineStudio->StudioSetupLighting)
        return;
    UninstallStudioLighting();
    studioLightBridge.target = engineStudio->StudioSetupLighting;
    studioLightBridge.hook   = g_pMetaHookAPI->InlineHook(reinterpret_cast<void*>(studioLightBridge.target),
                                                          reinterpret_cast<void*>(StudioSetupLightingHook),
                                                          reinterpret_cast<void**>(&studioLightBridge.original));
    if (!studioLightBridge.hook)
    {
        studioLightBridge.target = nullptr;
        gEngfuncs.Con_Printf("[StandaloneView] StudioSetupLighting hook failed; previews draw with the world lighting\n");
    }
}

// The engine keeps its own interface in *ppinterface when the client provides none.
int GetStudioModelInterfaceHook(int version, r_studio_interface_t** ppinterface, engine_studio_api_t* pstudio)
{
    const int result = originalGetStudioModelInterface(version, ppinterface, pstudio);
    studioInterface  = ppinterface;
    engineStudio     = pstudio;
    InstallStudioLighting();
    return result;
}

void CalcRefdefHook(ref_params_s* pparams)
{
    StandaloneViewManager().CalcRefdef(pparams);
}

void DrawNormalTrianglesHook()
{
    StandaloneViewManager().DrawNormalTriangles();
}

void DrawTransparentTrianglesHook()
{
    StandaloneViewManager().DrawTransparentTriangles();
}
} // namespace

bool InstallEngineHooks()
{
    if (renderViewHook && paintBackgroundHook)
        return true;

    const auto   module                 = EngineModule();
    void*        renderViewAddress      = nullptr;
    void*        paintBackgroundAddress = nullptr;
    EngineBridge engine;
    EngineDraw   draw;
    if (!Resolve(module, "R_DrawBrushModel", MH_GAMESYMBOL_KIND_FUNCTION, &draw.R_DrawBrushModel) ||
        !Resolve(module, "R_DrawSpriteModel", MH_GAMESYMBOL_KIND_FUNCTION, &draw.R_DrawSpriteModel) ||
        !Resolve(module, "CL_FxBlend", MH_GAMESYMBOL_KIND_FUNCTION, &draw.CL_FxBlend) ||
        !Resolve(module, "currententity", MH_GAMESYMBOL_KIND_GLOBAL, &draw.currententity) ||
        !Resolve(module, "r_blend", MH_GAMESYMBOL_KIND_GLOBAL, &draw.r_blend) ||
        !Resolve(module, "r_entorigin", MH_GAMESYMBOL_KIND_GLOBAL, &draw.r_entorigin) ||
        !Resolve(module, "V_RenderView", MH_GAMESYMBOL_KIND_FUNCTION, &renderViewAddress) ||
        !Resolve(module, "VGui_ViewportPaintBackground", MH_GAMESYMBOL_KIND_FUNCTION, &paintBackgroundAddress) ||
        !Resolve(module, "cls.state", MH_GAMESYMBOL_KIND_GLOBAL, &engine.clientState) ||
        !Resolve(module, "cls.signon", MH_GAMESYMBOL_KIND_GLOBAL, &engine.clientSignon) ||
        !Resolve(module, "cl_worldmodel", MH_GAMESYMBOL_KIND_GLOBAL, &engine.worldModel) ||
        !Resolve(module, "r_worldentity", MH_GAMESYMBOL_KIND_GLOBAL, &engine.worldEntity))
        return false;

    renderViewHook = g_pMetaHookAPI->InlineHook(renderViewAddress, reinterpret_cast<void*>(RenderViewHook),
                                                reinterpret_cast<void**>(&originalRenderView));
    if (!renderViewHook)
    {
        ReportFailure(module, "V_RenderView", "InlineHook failed");
        return false;
    }
    paintBackgroundHook = g_pMetaHookAPI->InlineHook(paintBackgroundAddress, reinterpret_cast<void*>(PaintBackgroundHook),
                                                     reinterpret_cast<void**>(&originalPaintBackground));
    if (!paintBackgroundHook)
    {
        UninstallEngineHooks();
        ReportFailure(module, "VGui_ViewportPaintBackground", "InlineHook failed");
        return false;
    }

    engine.V_RenderView    = CallOriginalRenderView;
    engine.IsDemoPlayback  = IsDemoPlayback;
    engine.SysError        = g_pMetaHookAPI->SysError;
    engine.DrawBrushModel  = DrawBrushModel;
    engine.DrawSpriteModel = DrawSpriteModel;
    engineDraw             = draw;
    StandaloneViewManager().SetEngine(engine);
    return true;
}

void UninstallEngineHooks()
{
    StandaloneViewManager().ResetEngine();
    UninstallStudioLighting();
    if (paintBackgroundHook)
    {
        g_pMetaHookAPI->UnHook(paintBackgroundHook);
        paintBackgroundHook = nullptr;
    }
    if (renderViewHook)
    {
        g_pMetaHookAPI->UnHook(renderViewHook);
        renderViewHook = nullptr;
    }
    originalPaintBackground = nullptr;
    originalRenderView      = nullptr;
    engineDraw              = {};
}

bool InstallClientHooks(cl_exportfuncs_t* pExportFunc)
{
    if (!renderPortalsHook && IsSvenCoopClient() && !InstallPortalHooks())
        return false;

    ClientBridge client;
    client.V_CalcRefdef             = pExportFunc->V_CalcRefdef;
    client.DrawNormalTriangles      = pExportFunc->HUD_DrawNormalTriangles;
    client.DrawTransparentTriangles = pExportFunc->HUD_DrawTransparentTriangles;
    client.DrawStudioModel          = DrawStudioModel;
    client.SetupStudioPose          = SetupStudioPose;
    StandaloneViewManager().SetClient(client);

    // Without the export the engine never asks; keep it that way.
    originalGetStudioModelInterface = pExportFunc->HUD_GetStudioModelInterface;
    if (originalGetStudioModelInterface)
        pExportFunc->HUD_GetStudioModelInterface = GetStudioModelInterfaceHook;

    pExportFunc->V_CalcRefdef                 = CalcRefdefHook;
    pExportFunc->HUD_DrawNormalTriangles      = DrawNormalTrianglesHook;
    pExportFunc->HUD_DrawTransparentTriangles = DrawTransparentTrianglesHook;
    return true;
}

void UninstallClientHooks()
{
    // The export table belongs to the engine and is torn down with the client;
    // only stop forwarding into the saved entry points.
    StandaloneViewManager().ResetClient();
    UninstallPortalHooks();
    originalGetStudioModelInterface = nullptr;
    studioInterface                 = nullptr;
    engineStudio                    = nullptr;
}
} // namespace standaloneview
