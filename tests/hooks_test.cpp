#include <metahook.h>
#include <cl_entity.h>
#include <com_model.h>
#include <const.h>
#include <r_studioint.h>
#include <ref_params.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "StandaloneViewManager.h"
#include "privatehook.h"

metahook_api_t* g_pMetaHookAPI = nullptr;
cl_enginefunc_t gEngfuncs      = {};

namespace
{
using RenderViewFn      = void (*)();
using PaintBackgroundFn = void (*)(int*);
using RenderPortalsFn   = void(__fastcall*)(void*, int, ref_params_s*);
using ResetAllFn        = int(__fastcall*)(void*, int);
using StudioLightingFn  = void (*)(alight_t*);

void EngineStudioLighting(alight_t* light);
void AlternateStudioLighting(alight_t* light);
void CallStudioLighting();

int                    failures       = 0;
int                    installs       = 0;
int                    resolves       = 0;
int                    engineResolves = 0;
int                    failInstall    = 0;
int                    mainRenders    = 0;
int                    paints         = 0;
int                    portalRenders  = 0;
int                    portalResets   = 0;
bool                   svenClient     = false;
const char*            missingSymbol  = nullptr;
char                   error[1024]    = {};
std::vector<uintptr_t> removed;
RenderViewFn           hookedRenderView       = nullptr;
PaintBackgroundFn      hookedPaintBackground  = nullptr;
RenderPortalsFn        hookedRenderPortals    = nullptr;
ResetAllFn             hookedResetAll         = nullptr;
StudioLightingFn       hookedStudioLighting   = nullptr;
uintptr_t              studioLightingHandle   = 0;
int                    lightingCalls          = 0;
int                    alternateLightingCalls = 0;
int                    lightingWarnings       = 0;
alight_t               observedLighting       = {};
float                  observedDirection[3]   = {};
cl_entity_t*           savedBoneEntity        = nullptr;
cl_entity_t*           expectedBoneSource     = nullptr;
cl_entity_t*           failedPoseEntity       = nullptr;
cl_entity_t*           nullPlayerStateEntity  = nullptr;
bool                   bonesPrimed            = true;
bool                   zeroFxBlend            = false;

int         clientState  = 1;
int         clientSignon = 0;
model_t*    worldModel   = nullptr;
cl_entity_t worldEntity  = {};

// Engine draw globals and what the fake draw functions observed.
constexpr int FxBlend       = 51;
cl_entity_t*  currentEntity = nullptr;
float         blend         = 0.25f;
float         entOrigin[3]  = {};
struct DrawCall
{
    const char*                           function;
    cl_entity_t*                          current;
    float                                 blend;
    float                                 origin[3];
    int                                   flags;
    void*                                 player;
    const standaloneview::StudioLighting* lighting;
};
std::vector<DrawCall> drawCalls;

void RecordDraw(const char* function, int flags = 0, void* player = nullptr)
{
    DrawCall call{function, currentEntity, blend, {entOrigin[0], entOrigin[1], entOrigin[2]}, flags, player, StandaloneViewManager().CurrentStudioLighting()};
    drawCalls.push_back(call);
}

void Check(const char* label, int expected, int actual)
{
    if (expected != actual)
    {
        std::printf("FAIL %s: expected %d, got %d\n", label, expected, actual);
        ++failures;
    }
}

unsigned char          portalManager[1024] = {};
uint32_t               portalMemberOffset  = 44;
int                    memberQueries       = 0;
mh_gamesymbol_status_t memberStatus        = MH_GAMESYMBOL_OK;
constexpr const char*  PortalMemberSymbol  = "ClientPortalManager.m_bStudioRenderPlayerPending";
// When set, the fake V_RenderView draws the client triangles like the engine.
cl_exportfuncs_t* engineExports = nullptr;

// Sven's V_RenderView renders the client portals before the client V_CalcRefdef; the
// client resets its portal manager when it creates it on that first use.
void __cdecl OriginalRenderView()
{
    if (svenClient && hookedRenderPortals && hookedResetAll)
    {
        hookedResetAll(&portalManager, 0);
        ref_params_t refdef = {};
        hookedRenderPortals(&portalManager, 0, &refdef);
    }
    if (engineExports)
    {
        engineExports->HUD_DrawNormalTriangles();
        engineExports->HUD_DrawTransparentTriangles();
    }
    ++mainRenders;
}
void __fastcall OriginalRenderPortals(void*, int, ref_params_s*) { ++portalRenders; }
int __fastcall  OriginalResetAll(void*, int)
{
    ++portalResets;
    return 1;
}
void __cdecl EngineDrawBrush(cl_entity_t*) { RecordDraw("brush"); }
void __cdecl EngineDrawSprite(cl_entity_t*) { RecordDraw("sprite"); }
int __cdecl  EngineFxBlend(cl_entity_t*) { return zeroFxBlend ? 0 : FxBlend; }

void __cdecl OriginalPaintBackground(int*)
{
    ++paints;
    hookedRenderView();
}

PVOID           EngineBase() { return reinterpret_cast<void*>(0x10000000); }
PVOID           ClientBase() { return reinterpret_cast<void*>(0x20000000); }
IBaseInterface* ClientInterface(const char* name, int*)
{
    static IBaseInterface svenClientInterface;
    return svenClient && std::strcmp(name, "SCClientDLL001") == 0 ? &svenClientInterface : nullptr;
}
CreateInterfaceFn      ClientFactory() { return ClientInterface; }
DWORD                  EngineBuild() { return 10257; }
mh_gamesymbol_status_t CRC64(PVOID, uint64_t* value)
{
    *value = 0x1234;
    return MH_GAMESYMBOL_OK;
}
const char* Status(mh_gamesymbol_status_t) { return "SYMBOL_NOT_FOUND"; }
void        SysError(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vsnprintf(error, sizeof(error), format, args);
    va_end(args);
}

struct ExpectedSymbol
{
    const char*          name;
    mh_gamesymbol_kind_t kind;
    void*                address;
};

const ExpectedSymbol symbols[] = {
    {"V_RenderView", MH_GAMESYMBOL_KIND_FUNCTION, reinterpret_cast<void*>(OriginalRenderView)},
    {"VGui_ViewportPaintBackground", MH_GAMESYMBOL_KIND_FUNCTION, reinterpret_cast<void*>(OriginalPaintBackground)},
    {"cls.state", MH_GAMESYMBOL_KIND_GLOBAL, &clientState},
    {"cls.signon", MH_GAMESYMBOL_KIND_GLOBAL, &clientSignon},
    {"cl_worldmodel", MH_GAMESYMBOL_KIND_GLOBAL, &worldModel},
    {"r_worldentity", MH_GAMESYMBOL_KIND_GLOBAL, &worldEntity},
    {"R_DrawBrushModel", MH_GAMESYMBOL_KIND_FUNCTION, reinterpret_cast<void*>(EngineDrawBrush)},
    {"R_DrawSpriteModel", MH_GAMESYMBOL_KIND_FUNCTION, reinterpret_cast<void*>(EngineDrawSprite)},
    {"CL_FxBlend", MH_GAMESYMBOL_KIND_FUNCTION, reinterpret_cast<void*>(EngineFxBlend)},
    {"currententity", MH_GAMESYMBOL_KIND_GLOBAL, &currentEntity},
    {"r_blend", MH_GAMESYMBOL_KIND_GLOBAL, &blend},
    {"r_entorigin", MH_GAMESYMBOL_KIND_GLOBAL, entOrigin},
};
constexpr int EngineSymbolCount = sizeof(symbols) / sizeof(symbols[0]);

const ExpectedSymbol clientSymbols[] = {
    {"ClientPortalManager_RenderPortals", MH_GAMESYMBOL_KIND_FUNCTION, reinterpret_cast<void*>(OriginalRenderPortals)},
    {"ClientPortalManager_ResetAll", MH_GAMESYMBOL_KIND_FUNCTION, reinterpret_cast<void*>(OriginalResetAll)},
};

bool IsClientAddress(void* address)
{
    for (const auto& symbol : clientSymbols)
    {
        if (address == symbol.address)
            return true;
    }
    return false;
}

mh_gamesymbol_status_t Resolve(PVOID base, const char* symbol, mh_gamesymbol_kind_t kind, PVOID* address)
{
    ++resolves;
    if (missingSymbol && std::strcmp(symbol, missingSymbol) == 0)
        return MH_GAMESYMBOL_SYMBOL_NOT_FOUND;
    for (const auto& expected : clientSymbols)
    {
        if (std::strcmp(symbol, expected.name) == 0)
        {
            Check("resolve real client base", 1, base == ClientBase());
            Check(expected.name, expected.kind, kind);
            *address = expected.address;
            return MH_GAMESYMBOL_OK;
        }
    }
    ++engineResolves;
    Check("resolve real engine base", 1, base == EngineBase());
    for (const auto& expected : symbols)
    {
        if (std::strcmp(symbol, expected.name) == 0)
        {
            Check(expected.name, expected.kind, kind);
            *address = expected.address;
            return MH_GAMESYMBOL_OK;
        }
    }
    Check("unexpected symbol", 0, 1);
    return MH_GAMESYMBOL_SYMBOL_NOT_FOUND;
}

mh_gamesymbol_status_t QueryStructMember(PVOID base, const char* symbol, uint32_t* offset)
{
    ++memberQueries;
    Check("query real client base", 1, base == ClientBase());
    Check("query portal studio member", 0, std::strcmp(symbol, PortalMemberSymbol));
    if (memberStatus == MH_GAMESYMBOL_OK)
        *offset = portalMemberOffset;
    return memberStatus;
}

// Inside LoadEngine MetaHook defers inline hooks: the trampoline is written on commit.
std::vector<std::pair<void**, void*>> pendingOriginals;

void CommitHooks()
{
    for (const auto& pending : pendingOriginals)
        *pending.first = pending.second;
    pendingOriginals.clear();
}

hook_t* InlineHook(void* address, void* replacement, void** original)
{
    ++installs;
    const bool lighting = address == reinterpret_cast<void*>(EngineStudioLighting) ||
        address == reinterpret_cast<void*>(AlternateStudioLighting);
    if (!lighting && !IsClientAddress(address))
        Check("resolve every engine symbol before any engine installation", 1,
              engineResolves >= EngineSymbolCount && engineResolves % EngineSymbolCount == 0);
    else if (!lighting)
    {
        const int clientResolves = resolves - engineResolves;
        Check("resolve every client symbol before any client installation", 1, clientResolves > 0 && clientResolves % 2 == 0);
        Check("query member before any portal hook installation", 1, memberQueries > 0);
    }
    if (failInstall == installs)
        return nullptr;
    pendingOriginals.emplace_back(original, address);
    if (lighting)
    {
        hookedStudioLighting = reinterpret_cast<StudioLightingFn>(replacement);
        studioLightingHandle = static_cast<uintptr_t>(installs);
    }
    else if (address == clientSymbols[0].address)
        hookedRenderPortals = reinterpret_cast<RenderPortalsFn>(replacement);
    else if (address == clientSymbols[1].address)
        hookedResetAll = reinterpret_cast<ResetAllFn>(replacement);
    else if (address == symbols[1].address)
        hookedPaintBackground = reinterpret_cast<PaintBackgroundFn>(replacement);
    else
        hookedRenderView = reinterpret_cast<RenderViewFn>(replacement);
    return reinterpret_cast<hook_t*>(static_cast<uintptr_t>(installs));
}

BOOL UnHook(hook_t* handle)
{
    removed.push_back(reinterpret_cast<uintptr_t>(handle));
    if (reinterpret_cast<uintptr_t>(handle) == studioLightingHandle)
    {
        hookedStudioLighting = nullptr;
        studioLightingHandle = 0;
    }
    return TRUE;
}

class CountingView : public IStandaloneView
{
public:
    bool IsActive() const override { return true; }
    void CalcRefdef(ref_params_s*) override { ++calcs; }
    void OnBeforeRenderView() override
    {
        ++renders;
        if (unregisterInBefore)
            StandaloneViewManager().UnregisterStandaloneView(this);
    }
    void OnAfterRenderView() override {}
    void DrawNormalTriangles(IStandaloneViewDrawfuncs*) override { ++draws; }
    void DrawTransparentTriangles(IStandaloneViewDrawfuncs*) override { ++draws; }

    int renders = 0;
    int calcs   = 0;
    int draws   = 0;

    bool unregisterInBefore = false;
};

int  clientCalcs = 0;
int  clientDraws = 0;
void ClientCalcRefdef(ref_params_s*) { ++clientCalcs; }
void ClientDraw() { ++clientDraws; }

void Reset()
{
    standaloneview::UninstallEngineHooks();
    standaloneview::UninstallClientHooks();
    StandaloneViewManager().ClearViews();
    installs = resolves = engineResolves = failInstall = mainRenders = paints = portalRenders = portalResets = 0;
    svenClient                                                                                               = false;
    portalMemberOffset                                                                                       = 44;
    memberQueries                                                                                            = 0;
    memberStatus                                                                                             = MH_GAMESYMBOL_OK;
    missingSymbol                                                                                            = nullptr;
    error[0]                                                                                                 = '\0';
    removed.clear();
    pendingOriginals.clear();
    hookedRenderView      = nullptr;
    hookedPaintBackground = nullptr;
    hookedRenderPortals   = nullptr;
    hookedResetAll        = nullptr;
    clientCalcs = clientDraws = 0;
    hookedStudioLighting      = nullptr;
    studioLightingHandle      = 0;
    lightingCalls = alternateLightingCalls = lightingWarnings = 0;
    observedLighting                                          = {};
    savedBoneEntity = expectedBoneSource = failedPoseEntity = nullPlayerStateEntity = nullptr;
    bonesPrimed                                                                     = true;
    zeroFxBlend                                                                     = false;
    drawCalls.clear();
    engineExports = nullptr;
}

void Success()
{
    Reset();
    Check("install succeeds", 1, standaloneview::InstallEngineHooks());
    Check("install two hooks", 2, installs);
    Check("repeated install succeeds", 1, standaloneview::InstallEngineHooks());
    Check("repeated install adds no hooks", 2, installs);
    CommitHooks();

    CountingView view;
    StandaloneViewManager().RegisterStandaloneView(&view);
    hookedRenderView();
    Check("V_RenderView outside paint background renders the main view", 1, mainRenders);
    Check("V_RenderView outside paint background skips views", 0, view.renders);

    int extents[4] = {};
    hookedPaintBackground(extents);
    Check("paint background calls the original", 1, paints);
    Check("paint background renders the view", 1, view.renders);
    Check("view and main view both reach the engine", 3, mainRenders);
    Check("cls.state restored", 1, clientState);

    error[0]                = '\0';
    view.unregisterInBefore = true;
    hookedPaintBackground(extents);
    Check("unregistering from a callback reaches MetaHook SysError", 1,
          std::strstr(error, "UnregisterStandaloneView") != nullptr);
    view.unregisterInBefore = false;
    StandaloneViewManager().UnregisterStandaloneView(&view);

    standaloneview::UninstallEngineHooks();
    Check("both removed", 2, static_cast<int>(removed.size()));
    if (removed.size() == 2)
    {
        Check("paint background removed first", 2, static_cast<int>(removed[0]));
        Check("V_RenderView removed second", 1, static_cast<int>(removed[1]));
    }
    standaloneview::UninstallEngineHooks();
    Check("uninstall is idempotent", 2, static_cast<int>(removed.size()));
}

cl_exportfuncs_t ClientExportTable()
{
    cl_exportfuncs_t exports             = {};
    exports.V_CalcRefdef                 = ClientCalcRefdef;
    exports.HUD_DrawNormalTriangles      = ClientDraw;
    exports.HUD_DrawTransparentTriangles = ClientDraw;
    return exports;
}

void ClientExports()
{
    Reset();
    auto exports = ClientExportTable();
    Check("client install succeeds", 1, standaloneview::InstallClientHooks(&exports));
    Check("non-Sven client installs no inline hook", 0, installs);
    Check("V_CalcRefdef replaced", 1, exports.V_CalcRefdef != ClientCalcRefdef);
    Check("HUD_DrawNormalTriangles replaced", 1, exports.HUD_DrawNormalTriangles != ClientDraw);
    Check("HUD_DrawTransparentTriangles replaced", 1, exports.HUD_DrawTransparentTriangles != ClientDraw);

    ref_params_t refdef = {};
    exports.V_CalcRefdef(&refdef);
    exports.HUD_DrawNormalTriangles();
    exports.HUD_DrawTransparentTriangles();
    Check("V_CalcRefdef forwards to the client", 1, clientCalcs);
    Check("triangle exports forward to the client", 2, clientDraws);

    standaloneview::UninstallClientHooks();
    exports.V_CalcRefdef(&refdef);
    Check("uninstalled client hooks stop forwarding", 1, clientCalcs);
}

void SvenPortals(uint32_t offset)
{
    Reset();
    portalMemberOffset = offset;
    svenClient         = true;
    auto exports       = ClientExportTable();
    Check("Sven client install succeeds", 1, standaloneview::InstallClientHooks(&exports));
    Check("Sven client hooks RenderPortals and ResetAll", 2, installs);
    Check("repeated Sven client install succeeds", 1, standaloneview::InstallClientHooks(&exports));
    Check("repeated Sven client install adds no hook", 2, installs);
    Check("repeated client install reuses member query", 1, memberQueries);
    Check("engine install succeeds", 1, standaloneview::InstallEngineHooks());
    CommitHooks();

    ref_params_t refdef = {};
    hookedRenderPortals(&portalManager, 0, &refdef);
    Check("RenderPortals forwards outside a view pass", 1, portalRenders);
    Check("ResetAll forwards outside a view pass", 1, hookedResetAll(&portalManager, 0));
    Check("ResetAll reaches the client outside a view pass", 1, portalResets);

    CountingView view;
    StandaloneViewManager().RegisterStandaloneView(&view);
    int extents[4] = {};
    portalRenders  = 0;
    portalResets   = 0;
    std::memset(portalManager, 0xA5, sizeof(portalManager));
    hookedPaintBackground(extents);
    Check("view renders", 1, view.renders);
    Check("view pass and main view reach the engine", 2, mainRenders);
    Check("portals render only for the main view", 1, portalRenders);
    Check("portal manager resets only for the main view", 1, portalResets);
    for (size_t i = 0; i < sizeof(portalManager); ++i)
        Check("preview reset only clears the queried byte", i == portalMemberOffset ? 0 : 0xA5, portalManager[i]);

    const auto engineHooks = static_cast<int>(removed.size());
    standaloneview::UninstallClientHooks();
    Check("portal hooks removed", engineHooks + 2, static_cast<int>(removed.size()));
    standaloneview::UninstallClientHooks();
    Check("client uninstall is idempotent", engineHooks + 2, static_cast<int>(removed.size()));
    standaloneview::UninstallEngineHooks();
}

void ClientFailures()
{
    for (const auto& symbol : clientSymbols)
    {
        Reset();
        svenClient    = true;
        missingSymbol = symbol.name;
        auto exports  = ClientExportTable();
        Check("missing client symbol fails", 0, standaloneview::InstallClientHooks(&exports));
        Check("missing client symbol installs nothing", 0, installs);
        Check("missing client symbol leaves exports", 1, exports.V_CalcRefdef == ClientCalcRefdef);
        Check("error names client symbol", 1, std::strstr(error, symbol.name) != nullptr);
        Check("error names client module", 1, std::strstr(error, "module client") != nullptr);
    }
    for (int failed = 1; failed <= 2; ++failed)
    {
        Reset();
        svenClient   = true;
        failInstall  = failed;
        auto exports = ClientExportTable();
        Check("client hook failure fails", 0, standaloneview::InstallClientHooks(&exports));
        Check("client hook failure rolls back prior hooks", failed - 1, static_cast<int>(removed.size()));
        Check("client hook failure leaves exports", 1, exports.V_CalcRefdef == ClientCalcRefdef);
        Check("client hook failure diagnostic", 1, std::strstr(error, "InlineHook failed") != nullptr);
    }
    Reset();
    svenClient   = true;
    auto exports = ClientExportTable();
    Check("client install can retry after failure", 1, standaloneview::InstallClientHooks(&exports));
}

void PortalMemberFailures()
{
    for (const auto status : {MH_GAMESYMBOL_SYMBOL_NOT_FOUND, MH_GAMESYMBOL_KIND_MISMATCH, MH_GAMESYMBOL_GAMEDATA_UNAVAILABLE})
    {
        Reset();
        svenClient   = true;
        memberStatus = status;
        auto exports = ClientExportTable();
        Check("member failure rejects client hooks", 0, standaloneview::InstallClientHooks(&exports));
        Check("member failure installs no hooks", 0, installs);
        Check("member failure preserves exports", 1, exports.V_CalcRefdef == ClientCalcRefdef);
        Check("member failure names member", 1, std::strstr(error, PortalMemberSymbol) != nullptr);
        memberStatus = MH_GAMESYMBOL_OK;
        Check("member query can retry", 1, standaloneview::InstallClientHooks(&exports));
        Check("retry queries member again", 2, memberQueries);
    }

    Reset();
    svenClient                                  = true;
    g_pMetaHookAPI->QueryGameSymbolStructMember = nullptr;
    auto exports                                = ClientExportTable();
    Check("missing member API rejects client hooks", 0, standaloneview::InstallClientHooks(&exports));
    Check("missing member API installs no hooks", 0, installs);
    Check("missing member API preserves exports", 1, exports.V_CalcRefdef == ClientCalcRefdef);
    Check("missing member API diagnostic", 1, std::strstr(error, "QueryGameSymbolStructMember unavailable") != nullptr);
    g_pMetaHookAPI->QueryGameSymbolStructMember = QueryStructMember;
}

int StudioDrawModel(int flags)
{
    RecordDraw("studio", flags);
    if (currentEntity == failedPoseEntity)
        return 0;
    if (currentEntity->curstate.movetype == MOVETYPE_FOLLOW &&
        savedBoneEntity != expectedBoneSource)
        bonesPrimed = false;
    savedBoneEntity = currentEntity;
    if (flags & STUDIO_RENDER)
        CallStudioLighting();
    return 1;
}
int StudioDrawPlayer(int flags, entity_state_s* player)
{
    RecordDraw("player", flags, player);
    if (flags & STUDIO_RENDER)
        CallStudioLighting();
    return 1;
}

constexpr int         MaxClients = 2;
entity_state_t        playerStates[MaxClients];
r_studio_interface_t  clientStudio          = {};
r_studio_interface_t* engineStudioInterface = nullptr; // the engine's pStudioAPI
engine_studio_api_t   engineStudioApi       = {};
cl_entity_t           attachmentOwner       = {};

entity_state_t* GetPlayerState(int index) { return currentEntity == nullPlayerStateEntity ? nullptr : &playerStates[index]; }
int             GetMaxClients() { return MaxClients; }
cl_entity_t*    GetEntityByIndex(int index) { return index == 7 ? &attachmentOwner : nullptr; }

int ClientGetStudioModelInterface(int, r_studio_interface_t** ppinterface, engine_studio_api_t*)
{
    *ppinterface = &clientStudio;
    return 1;
}

int DeclineStudioModelInterface(int, r_studio_interface_t**, engine_studio_api_t*)
{
    return 0;
}

void EngineStudioLighting(alight_t* light)
{
    ++lightingCalls;
    if (!light)
        return;
    observedLighting = *light;
    if (light->plightvec)
        std::copy_n(light->plightvec, 3, observedDirection);
}

void AlternateStudioLighting(alight_t* light)
{
    ++alternateLightingCalls;
    EngineStudioLighting(light);
}

void LightingWarning(const char* format, ...)
{
    if (std::strstr(format, "StudioSetupLighting hook failed"))
        ++lightingWarnings;
}

void CallStudioLighting()
{
    float    direction[3] = {0, 0, -2};
    alight_t light{};
    light.ambientlight = 17;
    light.shadelight   = 29;
    light.color[0]     = 0.2f;
    light.color[1]     = 0.4f;
    light.color[2]     = 0.6f;
    light.plightvec    = direction;
    if (hookedStudioLighting)
        hookedStudioLighting(&light);
    else if (engineStudioApi.StudioSetupLighting)
        engineStudioApi.StudioSetupLighting(&light);
}

class DrawingView : public IStandaloneView
{
public:
    bool IsActive() const override { return true; }
    void CalcRefdef(ref_params_s*) override {}
    void OnBeforeRenderView() override {}
    void OnAfterRenderView() override {}
    void DrawNormalTriangles(IStandaloneViewDrawfuncs* pDrawfuncs) override
    {
        for (auto ent : brushes)
            pDrawfuncs->DrawBrushModel(ent);
        for (auto ent : sprites)
            pDrawfuncs->DrawSpriteModel(ent);
        for (auto ent : studios)
            pDrawfuncs->DrawStudioModel(ent, nullptr, nullptr);
    }
    void DrawTransparentTriangles(IStandaloneViewDrawfuncs*) override {}

    std::vector<cl_entity_t*> brushes;
    std::vector<cl_entity_t*> sprites;
    std::vector<cl_entity_t*> studios;
};

void EntityDraws()
{
    Reset();
    Check("engine install succeeds", 1, standaloneview::InstallEngineHooks());
    auto exports                        = ClientExportTable();
    exports.HUD_GetStudioModelInterface = ClientGetStudioModelInterface;
    Check("client install succeeds", 1, standaloneview::InstallClientHooks(&exports));
    Check("HUD_GetStudioModelInterface replaced", 1, exports.HUD_GetStudioModelInterface != ClientGetStudioModelInterface);
    CommitHooks();
    // As the engine does in ClientDLL_CheckStudioInterface.
    Check("studio interface forwarded", 1, exports.HUD_GetStudioModelInterface(STUDIO_INTERFACE_VERSION, &engineStudioInterface, &engineStudioApi));
    Check("client studio interface reaches the engine", 1, engineStudioInterface == &clientStudio);

    model_t brushModel  = {};
    model_t spriteModel = {};
    model_t studioModel = {};
    brushModel.type     = mod_brush;
    spriteModel.type    = mod_sprite;
    studioModel.type    = mod_studio;

    cl_entity_t brush                = {};
    brush.model                      = &brushModel;
    cl_entity_t transSprite          = {};
    transSprite.model                = &spriteModel;
    transSprite.curstate.rendermode  = kRenderTransAdd;
    transSprite.curstate.skin        = 7;
    transSprite.curstate.body        = 2;
    transSprite.origin[0]            = 100;
    attachmentOwner.attachment[1][0] = 5;
    attachmentOwner.attachment[1][1] = 6;
    attachmentOwner.attachment[1][2] = 7;
    cl_entity_t studio               = {};
    studio.model                     = &studioModel;
    cl_entity_t player               = {};
    player.model                     = &studioModel;
    player.player                    = 1;
    player.index                     = 2;
    cl_entity_t invalidPlayer        = player;
    invalidPlayer.index              = MaxClients + 1;

    DrawingView view;
    view.brushes = {&brush};
    view.sprites = {&transSprite};
    view.studios = {&studio, &player, &invalidPlayer};
    StandaloneViewManager().RegisterStandaloneView(&view);

    cl_entity_t previous = {};
    currentEntity        = &previous;
    blend                = 0.25f;
    entOrigin[0]         = 9;
    int extents[4]       = {};
    engineExports        = &exports;
    hookedPaintBackground(extents);
    engineExports = nullptr;

    Check("valid entities draw once each", 4, static_cast<int>(drawCalls.size()));
    if (drawCalls.size() == 4)
    {
        Check("brush dispatches to R_DrawBrushModel", 0, std::strcmp(drawCalls[0].function, "brush"));
        Check("brush is the current entity", 1, drawCalls[0].current == &brush);
        Check("normal render mode draws opaque", 1, drawCalls[0].blend == 1.0f);

        Check("sprite dispatches to R_DrawSpriteModel", 0, std::strcmp(drawCalls[1].function, "sprite"));
        Check("sprite is the current entity", 1, drawCalls[1].current == &transSprite);
        Check("transparent render mode uses CL_FxBlend", 1, drawCalls[1].blend == FxBlend / 255.0f);
        Check("sprite follows its attachment", 1,
              drawCalls[1].origin[0] == 5 && drawCalls[1].origin[1] == 6 && drawCalls[1].origin[2] == 7);

        Check("studio dispatches to StudioDrawModel", 0, std::strcmp(drawCalls[2].function, "studio"));
        Check("studio is the current entity", 1, drawCalls[2].current == &studio);
        Check("studio renders without events", STUDIO_RENDER, drawCalls[2].flags);

        Check("player dispatches to StudioDrawPlayer", 0, std::strcmp(drawCalls[3].function, "player"));
        Check("player state of the entity's slot", 1, drawCalls[3].player == &playerStates[1]);
        Check("player renders without events", STUDIO_RENDER, drawCalls[3].flags);
    }
    Check("current entity restored", 1, currentEntity == &previous);
    Check("r_blend restored", 1, blend == 0.25f);
    Check("r_entorigin restored", 9, static_cast<int>(entOrigin[0]));
    Check("valid draws report nothing", 0, static_cast<int>(std::strlen(error)));

    StandaloneViewManager().UnregisterStandaloneView(&view);
    standaloneview::UninstallClientHooks();
    standaloneview::UninstallEngineHooks();
    drawCalls.clear();
}

struct StudioOperation
{
    cl_entity_t*                          entity;
    cl_entity_t*                          source;
    const standaloneview::StudioLighting* lighting;
    bool                                  poseOnly;
};

class PreviewView : public CountingView
{
public:
    void DrawNormalTriangles(IStandaloneViewDrawfuncs* drawfuncs) override
    {
        if (!transparent)
            Draw(drawfuncs);
    }
    void DrawTransparentTriangles(IStandaloneViewDrawfuncs* drawfuncs) override
    {
        if (transparent)
            Draw(drawfuncs);
    }

    std::vector<StudioOperation> operations;
    std::vector<bool>            poseResults;
    bool                         transparent = false;

private:
    void Draw(IStandaloneViewDrawfuncs* drawfuncs)
    {
        for (const auto& op : operations)
        {
            if (op.poseOnly)
                poseResults.push_back(drawfuncs->SetupStudioPose(op.entity, op.source));
            else
                drawfuncs->DrawStudioModel(op.entity, op.source, op.lighting);
        }
    }
};

cl_exportfuncs_t InstallPreviewClient(HUD_STUDIO_INTERFACE_FUNC getStudio = ClientGetStudioModelInterface)
{
    Check("preview engine hooks install", 1, standaloneview::InstallEngineHooks());
    auto exports                        = ClientExportTable();
    exports.HUD_GetStudioModelInterface = getStudio;
    Check("preview client hooks install", 1, standaloneview::InstallClientHooks(&exports));
    CommitHooks();
    engineStudioInterface = nullptr;
    exports.HUD_GetStudioModelInterface(STUDIO_INTERFACE_VERSION, &engineStudioInterface, &engineStudioApi);
    return exports;
}

void RenderPreview(cl_exportfuncs_t& exports)
{
    int extents[4] = {};
    engineExports  = &exports;
    hookedPaintBackground(extents);
    engineExports = nullptr;
}

void StudioPreviewPasses()
{
    for (bool transparent : {false, true})
    {
        Reset();
        engineStudioApi.StudioSetupLighting = nullptr;
        auto    exports                     = InstallPreviewClient();
        model_t model{};
        model.type = mod_studio;
        cl_entity_t source{}, entity{}, weapon{}, previous{};
        source.model = entity.model = weapon.model = &model;
        source.curstate.rendermode                 = kRenderTransAdd;
        entity.curstate.movetype = weapon.curstate.movetype = MOVETYPE_FOLLOW;
        expectedBoneSource                                  = &source;
        zeroFxBlend                                         = true;
        PreviewView view;
        view.transparent = transparent;
        view.operations  = {{&entity, &source, nullptr, true}, {&weapon, &source, nullptr, true}, {&entity, &source, nullptr, false}, {&weapon, &source, nullptr, false}};
        StandaloneViewManager().RegisterStandaloneView(&view);
        currentEntity = &previous;
        blend         = 0.25f;
        RenderPreview(exports);
        Check("both follower poses succeed", 1, view.poseResults == std::vector<bool>{true, true});
        Check("each pose and draw evaluates its source again", 8, static_cast<int>(drawCalls.size()));
        Check("follower bones always consume their own source", 1, bonesPrimed);
        if (drawCalls.size() == 8)
        {
            for (size_t i = 0; i < drawCalls.size(); ++i)
            {
                Check("source precedes every follower", 1,
                      drawCalls[i].current == (i % 2 == 0 ? &source : (i % 4 == 1 ? &entity : &weapon)));
                Check("poses and sources use zero flags; draws render without events",
                      i == 5 || i == 7 ? STUDIO_RENDER : 0, drawCalls[i].flags);
            }
            Check("invisible source still evaluates its pose", 1, drawCalls[0].blend == 0.0f);
        }
        Check("studio pass restores currententity", 1, currentEntity == &previous);
        Check("studio pass restores r_blend", 1, blend == 0.25f);

        drawCalls.clear();
        view.poseResults.clear();
        view.operations  = {{&entity, &source, nullptr, true}, {&entity, &source, nullptr, false}};
        failedPoseEntity = &source;
        RenderPreview(exports);
        Check("failed source pose is reported", 1, view.poseResults == std::vector<bool>{false});
        Check("failed source prevents both follower passes", 2, static_cast<int>(drawCalls.size()));
        Check("failed source restores entity", 1, currentEntity == &previous && blend == 0.25f);

        failedPoseEntity = nullptr;
        source.model     = nullptr;
        drawCalls.clear();
        view.poseResults.clear();
        RenderPreview(exports);
        Check("missing source model skips the pose", 1, view.poseResults == std::vector<bool>{false});
        Check("missing source model skips every studio call", 0, static_cast<int>(drawCalls.size()));
        model_t brush{};
        brush.type   = mod_brush;
        source.model = &brush;
        view.poseResults.clear();
        RenderPreview(exports);
        Check("wrong source type skips the pose", 1, view.poseResults == std::vector<bool>{false});
        Check("wrong source type skips every studio call", 0, static_cast<int>(drawCalls.size()));

        entity.curstate.movetype = 0;
        view.operations          = {{&entity, nullptr, nullptr, true}, {&entity, nullptr, nullptr, false}};
        view.poseResults.clear();
        RenderPreview(exports);
        Check("null source evaluates the entity itself", 1, view.poseResults == std::vector<bool>{true});
        Check("null source adds no extra studio calls", 2, static_cast<int>(drawCalls.size()));

        entity.curstate.rendermode = kRenderTransAdd;
        drawCalls.clear();
        view.poseResults.clear();
        RenderPreview(exports);
        Check("invisible entity still supports pose evaluation", 1, view.poseResults == std::vector<bool>{true});
        Check("zero blend suppresses only the render pass", 1, static_cast<int>(drawCalls.size()));
        Check("zero blend restores entity state", 1, currentEntity == &previous && blend == 0.25f);

        entity.curstate.rendermode = kRenderNormal;
        entity.player              = 1;
        entity.index               = 2;
        drawCalls.clear();
        view.poseResults.clear();
        RenderPreview(exports);
        Check("player pose succeeds", 1, view.poseResults == std::vector<bool>{true});
        Check("player pose and draw dispatch", 2, static_cast<int>(drawCalls.size()));
        if (drawCalls.size() == 2)
        {
            Check("player pose uses StudioDrawPlayer", 0, std::strcmp(drawCalls[0].function, "player"));
            Check("player pose has no render or events", 0, drawCalls[0].flags);
            Check("player uses its own slot", 1, drawCalls[1].player == &playerStates[1]);
            Check("player draw has no events", STUDIO_RENDER, drawCalls[1].flags);
        }
        nullPlayerStateEntity = &entity;
        drawCalls.clear();
        view.poseResults.clear();
        RenderPreview(exports);
        Check("missing player state skips the pose", 1, view.poseResults == std::vector<bool>{false});
        Check("missing player state skips the draw", 0, static_cast<int>(drawCalls.size()));
        nullPlayerStateEntity = nullptr;
        entity.index          = MaxClients + 1;
        view.poseResults.clear();
        RenderPreview(exports);
        Check("invalid player slot skips the pose", 1, view.poseResults == std::vector<bool>{false});
        Check("invalid player slot skips the draw", 0, static_cast<int>(drawCalls.size()));
        Check("studio calls leave no lighting scope behind", 1, StandaloneViewManager().CurrentStudioLighting() == nullptr);
        StandaloneViewManager().UnregisterStandaloneView(&view);
    }
}

void StudioFallback()
{
    Reset();
    engineStudioApi.StudioSetupLighting = nullptr;
    auto    exports                     = InstallPreviewClient(DeclineStudioModelInterface);
    model_t model{};
    model.type = mod_studio;
    cl_entity_t entity{};
    entity.model = &model;
    PreviewView view;
    view.operations = {{&entity, nullptr, nullptr, true}, {&entity, nullptr, nullptr, false}};
    StandaloneViewManager().RegisterStandaloneView(&view);
    RenderPreview(exports);
    Check("missing studio interface returns false", 1, view.poseResults == std::vector<bool>{false});
    Check("missing studio interface draws nothing", 0, static_cast<int>(drawCalls.size()));
    // The engine fills its fallback slot after the client export returns 0.
    engineStudioInterface = &clientStudio;
    view.poseResults.clear();
    RenderPreview(exports);
    Check("late engine fallback is read at call time", 1, view.poseResults == std::vector<bool>{true});
    Check("late engine fallback poses and draws", 2, static_cast<int>(drawCalls.size()));
    StandaloneViewManager().UnregisterStandaloneView(&view);
}

void StudioLightingHooks()
{
    Reset();
    engineStudioApi.StudioSetupLighting = EngineStudioLighting;
    auto exports                        = InstallPreviewClient();
    Check("lighting hook installed after studio capture", 3, installs);
    CommitHooks();
    Check("lighting detour is available", 1, hookedStudioLighting != nullptr);
    if (!hookedStudioLighting)
        return;
    CallStudioLighting();
    Check("main-view lighting forwards unchanged", 1,
          observedLighting.ambientlight == 17 && observedLighting.shadelight == 29 &&
              observedLighting.color[0] == 0.2f && observedDirection[2] == -2.0f);
    hookedStudioLighting(nullptr);
    Check("null lighting input is forwarded", 2, lightingCalls);

    exports.HUD_GetStudioModelInterface(STUDIO_INTERFACE_VERSION, &engineStudioInterface, &engineStudioApi);
    Check("repeated capture does not add a lighting hook", 3, installs);
    model_t model{};
    model.type = mod_studio;
    cl_entity_t entity{}, source{};
    entity.model = source.model = &model;
    standaloneview::StudioLighting lighting;
    lighting.ambientlight = 70;
    lighting.shadelight   = 40;
    lighting.color[0]     = 0.5f;
    lighting.direction[0] = 0;
    lighting.direction[1] = 2;
    PreviewView view;
    view.operations = {{&entity, &source, &lighting, false}};
    StandaloneViewManager().RegisterStandaloneView(&view);
    RenderPreview(exports);
    Check("per-draw lighting reaches the real detour", 1,
          observedLighting.ambientlight == 70 && observedLighting.shadelight == 40 &&
              observedLighting.color[0] == 0.5f && observedDirection[0] == 0 &&
              observedDirection[1] == 1 && observedDirection[2] == 0);
    Check("lighting rides both source and follower only within the draw", 1,
          drawCalls.size() == 2 && drawCalls[0].lighting == &lighting && drawCalls[1].lighting == &lighting);
    Check("lighting ends after the draw", 1, StandaloneViewManager().CurrentStudioLighting() == nullptr);
    CallStudioLighting();
    Check("main view keeps world lighting after preview", 17, observedLighting.ambientlight);

    lighting.enabled = false;
    RenderPreview(exports);
    Check("disabled lighting is full ambient", 1,
          observedLighting.ambientlight == 255 && observedLighting.shadelight == 0 && observedLighting.color[0] == 1);
    view.operations[0].lighting = nullptr;
    RenderPreview(exports);
    Check("null per-draw lighting keeps world lighting", 17, observedLighting.ambientlight);

    standaloneview::UninstallClientHooks();
    Check("client reload retains engine lighting hook", 1, hookedStudioLighting != nullptr && removed.empty());
    auto reloaded                        = ClientExportTable();
    reloaded.HUD_GetStudioModelInterface = ClientGetStudioModelInterface;
    Check("client hooks reinstall", 1, standaloneview::InstallClientHooks(&reloaded));
    reloaded.HUD_GetStudioModelInterface(STUDIO_INTERFACE_VERSION, &engineStudioInterface, &engineStudioApi);
    Check("client recapture reuses engine hook", 3, installs);
    view.operations[0].lighting = &lighting;
    RenderPreview(reloaded);
    Check("lighting still works after client reload", 255, observedLighting.ambientlight);

    const auto oldHandle                = studioLightingHandle;
    engineStudioApi.StudioSetupLighting = AlternateStudioLighting;
    reloaded.HUD_GetStudioModelInterface(STUDIO_INTERFACE_VERSION, &engineStudioInterface, &engineStudioApi);
    Check("changed lighting target removes old hook", 1, removed.size() == 1 && removed[0] == oldHandle);
    Check("changed lighting target installs replacement", 4, installs);
    CommitHooks();
    CallStudioLighting();
    Check("replacement trampoline reaches new target", 1, alternateLightingCalls);
    standaloneview::UninstallEngineHooks();
    Check("engine teardown removes lighting and both view hooks", 4, static_cast<int>(removed.size()));
    Check("engine teardown clears lighting detour", 1, hookedStudioLighting == nullptr);
    standaloneview::UninstallEngineHooks();
    Check("engine teardown remains idempotent", 4, static_cast<int>(removed.size()));
    Check("engine hooks reinstall after teardown", 1, standaloneview::InstallEngineHooks());
    reloaded.HUD_GetStudioModelInterface(STUDIO_INTERFACE_VERSION, &engineStudioInterface, &engineStudioApi);
    Check("same lighting target is hooked again after teardown", 7, installs);
    CommitHooks();
    StandaloneViewManager().UnregisterStandaloneView(&view);
    standaloneview::UninstallClientHooks();
    standaloneview::UninstallEngineHooks();
    engineStudioApi.StudioSetupLighting = nullptr;
}

void StudioLightingFailure()
{
    Reset();
    engineStudioApi.StudioSetupLighting = EngineStudioLighting;
    failInstall                         = 3;
    auto exports                        = InstallPreviewClient();
    Check("lighting hook failure reports a warning", 1, lightingWarnings);
    Check("lighting hook failure leaves no detour", 1, hookedStudioLighting == nullptr);
    model_t model{};
    model.type = mod_studio;
    cl_entity_t entity{};
    entity.model = &model;
    standaloneview::StudioLighting lighting;
    PreviewView                    view;
    view.operations = {{&entity, nullptr, &lighting, false}};
    StandaloneViewManager().RegisterStandaloneView(&view);
    RenderPreview(exports);
    Check("lighting hook failure still draws the model", 1, static_cast<int>(drawCalls.size()));
    Check("lighting hook failure falls back to world lighting", 17, observedLighting.ambientlight);
    failInstall = 0;
    exports.HUD_GetStudioModelInterface(STUDIO_INTERFACE_VERSION, &engineStudioInterface, &engineStudioApi);
    Check("failed lighting hook can retry on recapture", 4, installs);
    CommitHooks();
    RenderPreview(exports);
    Check("retry restores per-draw lighting", 96, observedLighting.ambientlight);
    StandaloneViewManager().UnregisterStandaloneView(&view);
    standaloneview::UninstallClientHooks();
    standaloneview::UninstallEngineHooks();
    engineStudioApi.StudioSetupLighting = nullptr;
}

void Failures()
{
    for (const auto& symbol : symbols)
    {
        Reset();
        missingSymbol = symbol.name;
        Check("missing symbol fails", 0, standaloneview::InstallEngineHooks());
        Check("missing symbol installs nothing", 0, installs);
        Check("error names symbol", 1, std::strstr(error, symbol.name) != nullptr);
        Check("error contains CRC64", 1, std::strstr(error, "0000000000001234") != nullptr);
        Check("error contains build", 1, std::strstr(error, "10257") != nullptr);
        Check("error contains status", 1, std::strstr(error, "SYMBOL_NOT_FOUND") != nullptr);
    }
    for (int failed = 1; failed <= 2; ++failed)
    {
        Reset();
        failInstall = failed;
        Check("hook failure fails installation", 0, standaloneview::InstallEngineHooks());
        Check("hook failure rolls back prior hooks", failed - 1, static_cast<int>(removed.size()));
        standaloneview::UninstallEngineHooks();
        Check("rollback leaves no hooks", failed - 1, static_cast<int>(removed.size()));
        Check("hook failure diagnostic", 1, std::strstr(error, "InlineHook failed") != nullptr);
    }
    Reset();
    Check("install can retry after failure", 1, standaloneview::InstallEngineHooks());
    standaloneview::UninstallEngineHooks();
}
} // namespace

int main()
{
    metahook_api_t api              = {};
    api.GetEngineBase               = EngineBase;
    api.GetClientBase               = ClientBase;
    api.GetClientFactory            = ClientFactory;
    api.GetEngineBuildnum           = EngineBuild;
    api.GetModuleCRC64              = CRC64;
    api.GetGameSymbolStatusString   = Status;
    api.SysError                    = SysError;
    api.ResolveGameSymbol           = Resolve;
    api.QueryGameSymbolStructMember = QueryStructMember;
    api.InlineHook                  = InlineHook;
    api.UnHook                      = UnHook;
    g_pMetaHookAPI                  = &api;

    gEngfuncs.GetMaxClients        = GetMaxClients;
    gEngfuncs.GetEntityByIndex     = GetEntityByIndex;
    gEngfuncs.Con_Printf           = LightingWarning;
    clientStudio.version           = STUDIO_INTERFACE_VERSION;
    clientStudio.StudioDrawModel   = StudioDrawModel;
    clientStudio.StudioDrawPlayer  = StudioDrawPlayer;
    engineStudioApi.GetPlayerState = GetPlayerState;
    Success();
    ClientExports();
    for (uint32_t offset : {0u, 44u, 488u, 700u})
        SvenPortals(offset);
    ClientFailures();
    PortalMemberFailures();
    EntityDraws();
    StudioPreviewPasses();
    StudioFallback();
    StudioLightingHooks();
    StudioLightingFailure();
    Failures();
    std::printf("StandaloneView hooks: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
