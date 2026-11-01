#pragma once

namespace standaloneview
{
// QueryGameSymbolStructMember (API 114).
constexpr int RequiredMetaHookAPIVersion = 114;
bool          InstallEngineHooks();
void          UninstallEngineHooks();
bool          InstallClientHooks(struct cl_exportfuncs_s* pExportFunc);
void          UninstallClientHooks();
} // namespace standaloneview
