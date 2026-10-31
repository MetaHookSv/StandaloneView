#include <metahook.h>

#include "StandaloneViewManager.h"
#include "privatehook.h"

metahook_api_t*  g_pMetaHookAPI = nullptr;
mh_interface_t*  g_pInterface   = nullptr;
mh_enginesave_t* g_pMetaSave    = nullptr;
cl_enginefunc_t  gEngfuncs      = {};

void IPluginsV4::Init(metahook_api_t* api, mh_interface_t* interfaces, mh_enginesave_t* save)
{
    g_pMetaHookAPI = api;
    g_pInterface   = interfaces;
    g_pMetaSave    = save;
    if (interfaces->MetaHookAPIVersion < standaloneview::RequiredMetaHookAPIVersion)
        api->SysError("[StandaloneView] MetaHook API %d or newer is required.", standaloneview::RequiredMetaHookAPIVersion);
}

void IPluginsV4::LoadEngine(cl_enginefunc_t* engineFuncs)
{
    gEngfuncs = *engineFuncs;
    standaloneview::InstallEngineHooks();
}

void IPluginsV4::LoadClient(cl_exportfuncs_t* pExportFunc)
{
    standaloneview::InstallClientHooks(pExportFunc);
}

void IPluginsV4::ExitGame(int)
{
    StandaloneViewManager().ClearViews();
    standaloneview::UninstallClientHooks();
    standaloneview::UninstallEngineHooks();
}

void IPluginsV4::Shutdown()
{
    StandaloneViewManager().ClearViews();
    standaloneview::UninstallClientHooks();
    standaloneview::UninstallEngineHooks();
}

const char* IPluginsV4::GetVersion() { return "1.0.0"; }

EXPOSE_SINGLE_INTERFACE(IPluginsV4, IPluginsV4, METAHOOK_PLUGIN_API_VERSION_V4);
EXPOSE_SINGLE_INTERFACE_GLOBALVAR(CStandaloneViewManager, IStandaloneViewManager, STANDALONEVIEW_MANAGER_INTERFACE_VERSION, StandaloneViewManager());
